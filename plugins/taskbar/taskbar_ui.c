/**
 * @file taskbar_ui.c
 * @brief Taskbar plugin — task button creation, cairo drawing, and event callbacks.
 *
 * BUTTON STRUCTURE (per task)
 * ---------------------------
 * tk_build_gui() creates the following widget hierarchy for each task:
 *
 *   GtkButton (tk->button)
 *     +-- "draw" signal: tk_button_draw (cairo custom rendering)
 *     +-- "button_release_event": tk_callback_button_release_event
 *     +-- "button_press_event":   tk_callback_button_press_event
 *     +-- "enter" / "leave":      tk_callback_enter / leave
 *     +-- "drag-motion" / "drag-leave": tk_callback_drag_motion/leave
 *     +-- "scroll-event" (optional):    tk_callback_scroll_event
 *     |
 *     +-- icons_only: GtkImage (tk->image) added directly to button
 *     +-- !icons_only: GtkBox (horizontal, spacing 1)
 *           +-- GtkImage (tk->image)
 *           +-- GtkLabel (tk->label, PANGO_ELLIPSIZE_END)
 *
 * The button is packed into tb->bar (GtkBar) via gtk_box_pack_start.
 *
 * CAIRO CUSTOM RENDERING
 * ----------------------
 * tk_button_draw() connects to the "draw" signal and returns TRUE to suppress
 * GTK3's default button drawing.  It renders a gradient-filled rounded rectangle
 * (radius 3px) with a thin border, then calls gtk_container_propagate_draw on
 * the button's child widget so the icon and label are still drawn.
 *
 * Three gradient stops:
 *   GTK_STATE_FLAG_ACTIVE (focused):  dark gray gradient (0.44 -> 0.35)
 *   GTK_STATE_FLAG_PRELIGHT (hover):  light gray gradient (0.75 -> 0.60)
 *   Normal:                           medium gray gradient (0.67 -> 0.53)
 *
 * CLICK HANDLING
 * --------------
 * LMB (button 1):
 *   - Iconified window: unmap/raise it (XMapRaised or _NET_ACTIVE_WINDOW).
 *   - Focused or recently-clicked (ptk) window: iconify it (XIconifyWindow).
 *   - Otherwise: raise it (tk_raise_window).
 *
 * MMB (button 2): toggle _NET_WM_STATE_SHADED via _NET_WM_STATE ClientMessage.
 *
 * RMB (button 3): show the task context menu (tb->menu) at pointer position.
 *   Ctrl+RMB: propagate to the panel's bar for the panel context menu.
 *   The discard_release_event flag prevents the release after Ctrl+RMB from
 *   accidentally triggering the task menu.
 *
 * DRAG-OVER ACTIVATION
 * --------------------
 * When a drag enters a task button, a DRAG_ACTIVE_DELAY ms timeout is started
 * (delay_active_win) that raises the window, allowing the user to drag content
 * to a different window.  The timeout is cancelled on drag-leave.
 *
 * DEFERRED BAR DIMENSION UPDATE
 * ------------------------------
 * taskbar_size_alloc() must not call gtk_bar_set_dimension from inside a
 * size-allocate signal handler.  Instead it stores the new dimension value
 * in tb->pending_dim and schedules taskbar_apply_dim via g_idle_add.
 * The idle ID is saved in tb->pending_dim_id so it can be removed in the
 * destructor if it has not yet fired.
 */

#include <math.h>
#include "taskbar_priv.h"

/**
 * tk_button_draw - cairo custom rendering for a task button.
 * @widget:    The GtkButton being drawn.
 * @cr:        Cairo context for the current draw cycle.
 * @user_data: Unused.
 *
 * Draws a gradient-filled rounded rectangle with a 1px border.  Uses the
 * widget's current GTK state flags to choose the gradient colours.  After
 * filling, propagates the draw to the child widget (icon + label box).
 *
 * Returns TRUE to suppress GTK's default button rendering.
 */
static gboolean
tk_button_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    GtkStateFlags state;
    int w, h;
    double r1, r2, r3;
    cairo_pattern_t *pat;
    GtkWidget *child;

    w = gtk_widget_get_allocated_width(widget);
    h = gtk_widget_get_allocated_height(widget);
    state = gtk_widget_get_state_flags(widget);

    pat = cairo_pattern_create_linear(0, 0, 0, h);
    if (state & GTK_STATE_FLAG_ACTIVE) {
        /* focused window: darker pressed look */
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.44, 0.44, 0.44);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.35, 0.35, 0.35);
    } else if (state & GTK_STATE_FLAG_PRELIGHT) {
        /* hover: slightly lighter */
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.75, 0.75, 0.75);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.60, 0.60, 0.60);
    } else {
        /* normal: light gray */
        cairo_pattern_add_color_stop_rgb(pat, 0, 0.67, 0.67, 0.67);
        cairo_pattern_add_color_stop_rgb(pat, 1, 0.53, 0.53, 0.53);
    }

    /* Rounded rectangle fill */
    r1 = 3.0;
    cairo_new_path(cr);
    cairo_arc(cr,   r1,   r1, r1,  M_PI,       -M_PI / 2.0);
    cairo_arc(cr, w-r1,   r1, r1, -M_PI / 2.0,  0.0);
    cairo_arc(cr, w-r1, h-r1, r1,  0.0,          M_PI / 2.0);
    cairo_arc(cr,   r1, h-r1, r1,  M_PI / 2.0,  M_PI);
    cairo_close_path(cr);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);

    /* Border */
    r2 = 0.33;
    cairo_set_source_rgb(cr, r2, r2, r2);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Label text colour: dark on light-gray background */
    r3 = 0.07;
    cairo_set_source_rgb(cr, r3, r3, r3);

    /* Draw the button's child widget (HBox with icon + label) */
    child = gtk_bin_get_child(GTK_BIN(widget));
    if (child)
        gtk_container_propagate_draw(GTK_CONTAINER(widget), child, cr);

    return TRUE; /* suppress GTK's default button rendering */
}

/**
 * tk_callback_leave - restore button state on mouse leave.
 * @widget: The task button.
 * @tk:     The associated task.
 *
 * Sets the button state to focused_state if the window is focused, or
 * normal_state otherwise (hover state is implicitly cleared by GTK on leave).
 */
static void
tk_callback_leave( GtkWidget *widget, task *tk)
{
    gtk_widget_set_state_flags(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state, TRUE);
    return;
}


/**
 * tk_callback_enter - update button state on mouse enter.
 * @widget: The task button.
 * @tk:     The associated task.
 *
 * Restores the correct (focused or normal) state on enter.  The prelight
 * gradient is applied by the cairo draw handler based on GTK state flags;
 * this callback ensures the focused/unfocused distinction is preserved.
 */
static void
tk_callback_enter( GtkWidget *widget, task *tk )
{
    gtk_widget_set_state_flags(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state, TRUE);
    return;
}

/**
 * delay_active_win - idle callback to raise a window after drag-over delay.
 * @tk: Task whose window should be raised.
 *
 * Called by the DRAG_ACTIVE_DELAY timeout installed in tk_callback_drag_motion.
 * Raises the window and clears tb->dnd_activate.
 *
 * Returns: FALSE (one-shot; do not repeat).
 */
static gboolean
delay_active_win(task* tk)
{
    tk_raise_window(tk, CurrentTime);
    tk->tb->dnd_activate = 0;
    return FALSE;
}

/**
 * tk_callback_drag_motion - start drag-over activation timer.
 * @widget:       The task button.
 * @drag_context: GDK drag context.
 * @x, @y:        Pointer position within the button.
 * @time:         Event timestamp.
 * @tk:           The associated task.
 *
 * Installs a DRAG_ACTIVE_DELAY ms one-shot timeout to raise the window
 * (allowing the user to drag content into it).  If a timeout is already
 * pending, does nothing.  Returns TRUE with drag status 0 to accept the drag.
 */
static gboolean
tk_callback_drag_motion( GtkWidget *widget,
      GdkDragContext *drag_context,
      gint x, gint y,
      guint time, task *tk)
{
    /* prevent excessive motion notification */
    if (!tk->tb->dnd_activate) {
        tk->tb->dnd_activate = g_timeout_add(DRAG_ACTIVE_DELAY,
              (GSourceFunc)delay_active_win, tk);
    }
    gdk_drag_status (drag_context,0,time);
    return TRUE;
}

/**
 * tk_callback_drag_leave - cancel drag-over activation timer.
 * @widget:       The task button.
 * @drag_context: GDK drag context (unused).
 * @time:         Event timestamp (unused).
 * @tk:           The associated task.
 *
 * Removes the pending DRAG_ACTIVE_DELAY timeout if the pointer leaves the
 * button before the delay fires.
 */
static void
tk_callback_drag_leave (GtkWidget *widget,
      GdkDragContext *drag_context,
      guint time, task *tk)
{
    if (tk->tb->dnd_activate) {
        g_source_remove(tk->tb->dnd_activate);
        tk->tb->dnd_activate = 0;
    }
    return;
}


/**
 * tk_callback_scroll_event - handle mouse wheel scroll on a task button.
 * @widget: The task button.
 * @event:  The scroll event.
 * @tk:     The associated task.
 *
 * Scroll up:   raise/unmap the window (gdk_window_show or XMapRaised + XSetInputFocus).
 * Scroll down: iconify the window (XIconifyWindow).
 * XSync is called after Xlib operations.
 *
 * Returns TRUE (event consumed).
 */
static gint
tk_callback_scroll_event (GtkWidget *widget, GdkEventScroll *event, task *tk)
{
    if (event->direction == GDK_SCROLL_UP) {
        GdkWindow *gdkwindow;

        gdkwindow = gdk_x11_window_lookup_for_display(gdk_display_get_default(), tk->win);
        if (gdkwindow)
            gdk_window_show (gdkwindow);
        else
            XMapRaised (GDK_DPY, tk->win);
        XSetInputFocus (GDK_DPY, tk->win, RevertToNone, CurrentTime);
        DBG("XMapRaised  %x\n", tk->win);
    } else if (event->direction == GDK_SCROLL_DOWN) {
        DBG("tb->ptk = %x\n", (tk->tb->ptk) ? tk->tb->ptk->win : 0);
        XIconifyWindow (GDK_DPY, tk->win, DefaultScreen(GDK_DPY));
        DBG("XIconifyWindow %x\n", tk->win);
    }

    XSync (GDK_DPY, False);
    return TRUE;
}


/**
 * tk_callback_button_press_event - handle button-press on a task button.
 * @widget: The task button.
 * @event:  The button-press event.
 * @tk:     The associated task.
 *
 * Ctrl+RMB: propagate the event to tb->bar (for the panel context menu) and
 * set tb->discard_release_event to prevent the release from also triggering
 * the task menu.  Returns TRUE to stop further propagation.
 *
 * All other press events: return FALSE (let GTK handle them normally; the
 * actual action is in the release handler).
 */
static gboolean
tk_callback_button_press_event(GtkWidget *widget, GdkEventButton *event,
    task *tk)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        tk->tb->discard_release_event = 1;
        gtk_propagate_event(tk->tb->bar, (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}


/**
 * tk_callback_button_release_event - handle button-release on a task button.
 * @widget: The task button.
 * @event:  The button-release event.
 * @tk:     The associated task.
 *
 * Discards the release if discard_release_event is set (set by Ctrl+RMB press).
 * Discards if the pointer is outside the button's allocation (click drag-out).
 *
 * LMB release:
 *   - Iconified: raise/unmap the window.
 *   - Focused or == ptk: iconify the window.
 *   - Otherwise: tk_raise_window.
 *
 * MMB release: toggle _NET_WM_STATE_SHADED.
 *
 * RMB release: show the task context menu (gtk_menu_popup_at_pointer).
 *
 * XSync after all Xlib calls.
 *
 * Returns: TRUE if the event was handled; FALSE to pass to GTK.
 */
static gboolean
tk_callback_button_release_event(GtkWidget *widget, GdkEventButton *event,
    task *tk)
{

    if (event->type == GDK_BUTTON_RELEASE && tk->tb->discard_release_event) {
        tk->tb->discard_release_event = 0;
        return TRUE;
    }
    {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        if ((event->type != GDK_BUTTON_RELEASE) ||
            (event->x < 0 || event->x >= alloc.width ||
             event->y < 0 || event->y >= alloc.height))
            return FALSE;
    }
    DBG("win=%x\n", tk->win);
    if (event->button == 1) {
        if (tk->iconified)    {
            if(use_net_active) {
                Xclimsg(tk->win, a_NET_ACTIVE_WINDOW, 2, event->time, 0, 0, 0);
            } else {
                GdkWindow *gdkwindow;

                gdkwindow = gdk_x11_window_lookup_for_display(gdk_display_get_default(), tk->win);
                if (gdkwindow)
                    gdk_window_show (gdkwindow);
                else
                    XMapRaised (GDK_DPY, tk->win);
                XSync (GDK_DPY, False);
                DBG("XMapRaised  %x\n", tk->win);
            }
        } else {
            DBG("tb->ptk = %x\n", (tk->tb->ptk) ? tk->tb->ptk->win : 0);
            if (tk->focused || tk == tk->tb->ptk) {
                //tk->iconified = 1;
                XIconifyWindow (GDK_DPY, tk->win,
                    DefaultScreen(GDK_DPY));
                DBG("XIconifyWindow %x\n", tk->win);
            } else {
                tk_raise_window( tk, event->time );
            }
        }
    } else if (event->button == 2) {
        Xclimsg(tk->win, a_NET_WM_STATE,
            2 /*a_NET_WM_STATE_TOGGLE*/,
            a_NET_WM_STATE_SHADED,
            0, 0, 0);
    } else if (event->button == 3) {
        /*
        XLowerWindow (GDK_DPY, tk->win);
        DBG("XLowerWindow %x\n", tk->win);
        */
        tk->tb->menutask = tk;
        gtk_menu_popup_at_pointer(GTK_MENU(tk->tb->menu), (GdkEvent *)event);

    }
    XSync (GDK_DPY, False);
    return TRUE;
}


/**
 * tk_update - update visibility and state of a single task button.
 * @key: Hash table key (unused; task is iterated by g_hash_table_foreach).
 * @tk:  Task to update.
 * @tb:  Taskbar instance.
 *
 * If task_visible returns true: set button state (focused or normal),
 * queue a redraw, show the button, and update the tooltip.
 * Otherwise: hide the button.
 */
static void
tk_update(gpointer key, task *tk, taskbar_priv *tb)
{
    g_assert ((tb != NULL) && (tk != NULL));
    if (task_visible(tb, tk)) {
        gtk_widget_set_state_flags(tk->button,
              (tk->focused) ? tb->focused_state : tb->normal_state, TRUE);
        gtk_widget_queue_draw(tk->button);
        //_gtk_button_set_depressed(GTK_BUTTON(tk->button), tk->focused);
        gtk_widget_show(tk->button);

        if (tb->tooltips) {
            gtk_widget_set_tooltip_text(tk->button, tk->name);
        }
        return;
    }
    gtk_widget_hide(tk->button);
    return;
}

/**
 * tk_display - refresh the display of a single task.
 * @tb: Taskbar instance.
 * @tk: Task to refresh.
 */
void
tk_display(taskbar_priv *tb, task *tk)
{
    tk_update(NULL, tk, tb);
    return;
}

/**
 * tb_display - refresh the display of all tasks.
 * @tb: Taskbar instance.
 *
 * Calls tk_update for every task in the hash table via g_hash_table_foreach.
 * No-op if tb->wins is NULL (no _NET_CLIENT_LIST has been received yet).
 */
void
tb_display(taskbar_priv *tb)
{
    if (tb->wins)
        g_hash_table_foreach(tb->task_list, (GHFunc) tk_update, (gpointer) tb);
    return;

}

/**
 * tk_build_gui - create the button widget for a newly-added task.
 * @tb: Taskbar instance.
 * @tk: Task whose button is to be created.
 *
 * 1. If the window is not a GDK-tracked panel window (FBPANEL_WIN), installs
 *    XSelectInput for PropertyChangeMask + StructureNotifyMask, then wraps it
 *    in a GdkWindow (gdk_x11_window_foreign_new_for_display) and installs the
 *    per-window GDK filter (tb_event_filter).
 * 2. Creates the GtkButton and connects all event callbacks.
 * 3. Loads the task icon (tk_update_icon with a=None).
 * 4. In icons_only mode: adds the GtkImage directly to the button.
 *    Otherwise: creates an HBox with GtkImage + GtkLabel (PANGO_ELLIPSIZE_END).
 * 5. Packs the button into tb->bar; hides it if not currently visible.
 * 6. Starts the flash animation if tk->urgency is set.
 */
void
tk_build_gui(taskbar_priv *tb, task *tk)
{
    GtkWidget *w1;

    g_assert ((tb != NULL) && (tk != NULL));

    /* NOTE
     * 1. the extended mask is sum of taskbar and pager needs
     * see bug [ 940441 ] pager loose track of windows
     *
     * Do not change event mask to gtk windows spwaned by this gtk client
     * this breaks gtk internals */
    if (!FBPANEL_WIN(tk->win)) {
        XSelectInput(GDK_DPY, tk->win,
                PropertyChangeMask | StructureNotifyMask);
        /* Attach a per-window GDK filter instead of the deprecated global
         * gdk_window_add_filter(NULL,...).  tb_event_filter only handles
         * PropertyNotify on tracked windows; root-window events go through
         * fbev signals and do not need this filter. */
        tk->gdkwin = gdk_x11_window_foreign_new_for_display(
                gdk_display_get_default(), tk->win);
        if (tk->gdkwin)
            gdk_window_add_filter(tk->gdkwin,
                    (GdkFilterFunc)tb_event_filter, tb);
    }

    /* button */
    tk->button = gtk_button_new();
    /* gtk_button_new() has no child in GTK3; halign is set per-widget on
     * tk->image and tk->label below after they are created */
    g_signal_connect(G_OBJECT(tk->button), "draw",
        G_CALLBACK(tk_button_draw), NULL);
    gtk_widget_show(tk->button);
    gtk_container_set_border_width(GTK_CONTAINER(tk->button), 0);
    gtk_widget_add_events (tk->button, GDK_BUTTON_RELEASE_MASK
            | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(tk->button), "button_release_event",
          G_CALLBACK(tk_callback_button_release_event), (gpointer)tk);
    g_signal_connect(G_OBJECT(tk->button), "button_press_event",
           G_CALLBACK(tk_callback_button_press_event), (gpointer)tk);
    g_signal_connect_after (G_OBJECT (tk->button), "leave",
          G_CALLBACK (tk_callback_leave), (gpointer) tk);
    g_signal_connect_after (G_OBJECT (tk->button), "enter",
          G_CALLBACK (tk_callback_enter), (gpointer) tk);
    gtk_drag_dest_set( tk->button, 0, NULL, 0, 0);
    g_signal_connect (G_OBJECT (tk->button), "drag-motion",
          G_CALLBACK (tk_callback_drag_motion), (gpointer) tk);
    g_signal_connect (G_OBJECT (tk->button), "drag-leave",
          G_CALLBACK (tk_callback_drag_leave), (gpointer) tk);
    if (tb->use_mouse_wheel)
        g_signal_connect_after(G_OBJECT(tk->button), "scroll-event",
              G_CALLBACK(tk_callback_scroll_event), (gpointer)tk);

    /* pix */
    tk_update_icon(tb, tk, None);
    w1 = tk->image = gtk_image_new_from_pixbuf(tk->pixbuf);
    gtk_widget_set_halign(tk->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tk->image, GTK_ALIGN_CENTER);

    if (!tb->icons_only) {
        w1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
        gtk_container_set_border_width(GTK_CONTAINER(w1), 0);
        gtk_box_pack_start(GTK_BOX(w1), tk->image, FALSE, FALSE, 0);
        tk->label = gtk_label_new(tk->iconified ? tk->iname : tk->name);
        gtk_label_set_ellipsize(GTK_LABEL(tk->label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(tk->label, GTK_ALIGN_START);
        gtk_widget_set_valign(tk->label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(w1), tk->label, TRUE, TRUE, 0);
    }

    gtk_container_add (GTK_CONTAINER (tk->button), w1);
    gtk_box_pack_start(GTK_BOX(tb->bar), tk->button, FALSE, TRUE, 0);
    gtk_widget_set_can_focus(tk->button, FALSE);
    gtk_widget_set_can_default(tk->button, FALSE);

    gtk_widget_show_all(tk->button);
    if (!task_visible(tb, tk)) {
        gtk_widget_hide(tk->button);
    }

    if (tk->urgency) {
        /* Flash button for window with urgency hint */
        tk_flash_window(tk);
    }
    return;
}

/**
 * menu_close_window - send WM_DELETE_WINDOW to close the task's window.
 * @widget: Menu item (unused).
 * @tb:     Taskbar instance; tb->menutask is the target window.
 *
 * Sends a WM_PROTOCOLS/WM_DELETE_WINDOW ClientMessage via Xclimsgwm.
 * This is a polite close request; the window may ignore it.
 */
static void
menu_close_window(GtkWidget *widget, taskbar_priv *tb)
{
    DBG("win %x\n", tb->menutask->win);
    XSync (GDK_DPY, 0);
    //XKillClient(GDK_DPY, tb->menutask->win);
    Xclimsgwm(tb->menutask->win, a_WM_PROTOCOLS, a_WM_DELETE_WINDOW);
    XSync (GDK_DPY, 0);
    return;
}


/**
 * menu_raise_window - raise the task's window to the top of the stack.
 * @widget: Menu item (unused).
 * @tb:     Taskbar instance.
 */
static void
menu_raise_window(GtkWidget *widget, taskbar_priv *tb)
{
    DBG("win %x\n", tb->menutask->win);
    XMapRaised(GDK_DPY, tb->menutask->win);
    return;
}


/**
 * menu_iconify_window - iconify (minimise) the task's window.
 * @widget: Menu item (unused).
 * @tb:     Taskbar instance.
 */
static void
menu_iconify_window(GtkWidget *widget, taskbar_priv *tb)
{
    DBG("win %x\n", tb->menutask->win);
    XIconifyWindow (GDK_DPY, tb->menutask->win,
        DefaultScreen(GDK_DPY));
    return;
}

/**
 * send_to_workspace - move the task's window to a specific virtual desktop.
 * @widget: Menu item; the destination desktop index is stored in object data "num".
 * @iii:    Unused (GtkMenuItem signal passes this as the first data arg).
 * @tb:     Taskbar instance.
 *
 * Sends a _NET_WM_DESKTOP ClientMessage with the destination desktop index.
 * The "num" object data is set when building the submenu in tb_make_menu.
 * ALL_WORKSPACES (0xFFFFFFFF) makes the window sticky.
 */
static void
send_to_workspace(GtkWidget *widget, void *iii, taskbar_priv *tb)
{
    int dst_desktop;


    dst_desktop = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "num"));
    DBG("win %x -> %d\n",  (unsigned int)tb->menutask->win, dst_desktop);
    Xclimsg(tb->menutask->win, a_NET_WM_DESKTOP, dst_desktop, 0, 0, 0, 0);

    return;
}

/**
 * tb_update_desktops_names - refresh tb->desk_names from _NET_DESKTOP_NAMES.
 * @tb: Taskbar instance.
 *
 * Re-reads tb->desk_namesno from the WM and tb->desk_names from the root
 * window's _NET_DESKTOP_NAMES property.  The old desk_names (if any) is
 * g_strfreev'd before the new one is assigned.
 */
static void
tb_update_desktops_names(taskbar_priv *tb)
{
    tb->desk_namesno = get_net_number_of_desktops();
    if (tb->desk_names)
        g_strfreev(tb->desk_names);
    tb->desk_names = get_utf8_property_list(GDK_ROOT_WINDOW(),
        a_NET_DESKTOP_NAMES, &(tb->desk_namesno));
    return;
}

/**
 * tb_make_menu - (re)build the task context menu.
 * @widget: Unused (may be NULL; called from both FbEv signal and directly).
 * @tb:     Taskbar instance.
 *
 * Builds a GtkMenu with:
 *   - Raise / Iconify actions
 *   - "Move to workspace" submenu with one item per desktop
 *     (item for "All workspaces" at the bottom)
 *   - Separator
 *   - Close
 *
 * Desktop names are re-read from _NET_DESKTOP_NAMES.  If a desktop has no
 * name, an empty string is used.
 *
 * The old tb->menu is gtk_widget_destroy'd before the new one is installed.
 * The new menu is NOT shown until gtk_menu_popup_at_pointer is called.
 */
void
tb_make_menu(GtkWidget *widget, taskbar_priv *tb)
{
    GtkWidget *mi, *menu, *submenu;
    gchar *buf;
    int i;

    menu = gtk_menu_new ();

    mi = gtk_menu_item_new_with_label(_("Raise"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_raise_window, tb);
    gtk_widget_show (mi);

    mi = gtk_menu_item_new_with_label(_("Iconify"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_iconify_window, tb);
    gtk_widget_show (mi);

    tb_update_desktops_names(tb);
    submenu = gtk_menu_new();
    for (i = 0; i < tb->desk_num; i++) {
        buf = g_strdup_printf("%d  %s", i + 1,
            (i < tb->desk_namesno) ? tb->desk_names[i] : "");
        mi = gtk_menu_item_new_with_label(buf);
        g_object_set_data(G_OBJECT(mi), "num", GINT_TO_POINTER(i));
        gtk_menu_shell_append (GTK_MENU_SHELL (submenu), mi);
        g_signal_connect(G_OBJECT(mi), "button_press_event",
            (GCallback)send_to_workspace, tb);
        g_free(buf);
    }
    mi = gtk_menu_item_new_with_label(_("All workspaces"));
    g_object_set_data(G_OBJECT(mi), "num", GINT_TO_POINTER(ALL_WORKSPACES));
    g_signal_connect(mi, "activate",
        (GCallback)send_to_workspace, tb);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), mi);
    gtk_widget_show_all(submenu);

    mi = gtk_menu_item_new_with_label(_("Move to workspace"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), submenu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    mi = gtk_separator_menu_item_new();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    mi = gtk_menu_item_new_with_label(_("Close"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)menu_close_window, tb);
    gtk_widget_show (mi);

    if (tb->menu)
        gtk_widget_destroy(tb->menu);
    tb->menu = menu;
}

/**
 * taskbar_apply_dim - idle callback to apply a deferred GtkBar dimension change.
 * @tb: Taskbar instance.
 *
 * Applies the pending dimension (tb->pending_dim) to tb->bar via
 * gtk_bar_set_dimension, then clears tb->pending_dim_id.
 *
 * Returns: G_SOURCE_REMOVE (one-shot; remove after firing).
 */
static gboolean
taskbar_apply_dim(taskbar_priv *tb)
{
    tb->pending_dim_id = 0;
    gtk_bar_set_dimension(GTK_BAR(tb->bar), tb->pending_dim);
    return G_SOURCE_REMOVE;
}

/**
 * taskbar_size_alloc - handle size-allocate signal on the plugin widget.
 * @widget: The plugin widget (p->pwid).
 * @a:      The new allocation.
 * @tb:     Taskbar instance.
 *
 * Computes the number of task button rows (horizontal panels) or columns
 * (vertical panels) that fit in the new allocation, then schedules a deferred
 * dimension update via g_idle_add (taskbar_apply_dim).
 *
 * Calling gtk_bar_set_dimension directly from a size-allocate handler would
 * re-enter GTK layout; the idle callback avoids this.
 */
static void
taskbar_size_alloc(GtkWidget *widget, GtkAllocation *a,
    taskbar_priv *tb)
{
    int dim;

    if (tb->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        dim = a->height / tb->task_height_max;
    else
        dim = a->width / tb->task_width_max;
    DBG("width=%d height=%d task_height_max=%d -> dim=%d\n",
        a->width, a->height, tb->task_height_max, dim);

    /* Defer gtk_bar_set_dimension (which calls gtk_widget_queue_resize) to an
     * idle callback.  Calling queue_resize from within a size-allocate signal
     * handler asks GTK to redo layout while layout is already running, which
     * can produce stale allocations in GTK3. */
    tb->pending_dim = dim;
    if (!tb->pending_dim_id)
        tb->pending_dim_id = g_idle_add((GSourceFunc)taskbar_apply_dim, tb);
    return;
}

/**
 * taskbar_build_gui - construct the taskbar's widget tree and connect FbEv signals.
 * @p: Plugin instance.
 *
 * 1. Connects "size-allocate" on p->pwid (for deferred dimension updates).
 * 2. Creates tb->bar (GtkBar) with the panel orientation, spacing,
 *    task_height_max, and task_width_max.
 * 3. Adds tb->bar to p->pwid.
 * 4. Loads the fallback icon from default.xpm into tb->gen_pixbuf.
 * 5. Connects FbEv signals: current_desktop, active_window, number_of_desktops,
 *    client_list, desktop_names, number_of_desktops (for menu rebuild).
 * 6. Reads initial cur_desk and desk_num from EWMH.
 * 7. Builds the initial task context menu (tb_make_menu).
 */
void
taskbar_build_gui(plugin_instance *p)
{
    taskbar_priv *tb = (taskbar_priv *) p;

    g_signal_connect(G_OBJECT(p->pwid), "size-allocate",
        (GCallback) taskbar_size_alloc, tb);

    tb->bar = gtk_bar_new(p->panel->orientation, tb->spacing,
        tb->task_height_max, tb->task_width_max);
    gtk_container_set_border_width(GTK_CONTAINER(tb->bar), 0);
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        gtk_widget_set_halign(tb->bar, GTK_ALIGN_START);
        gtk_widget_set_valign(tb->bar, GTK_ALIGN_CENTER);
    } else {
        gtk_widget_set_halign(tb->bar, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(tb->bar, GTK_ALIGN_START);
    }
    gtk_container_add(GTK_CONTAINER(p->pwid), tb->bar);
    gtk_widget_show_all(tb->bar);

    tb->gen_pixbuf = gdk_pixbuf_new_from_xpm_data((const char **)icon_xpm);

    g_signal_connect (G_OBJECT (fbev), "current_desktop",
          G_CALLBACK (tb_net_current_desktop), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "active_window",
          G_CALLBACK (tb_net_active_window), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (tb_net_number_of_desktops), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "client_list",
          G_CALLBACK (tb_net_client_list), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "desktop_names",
          G_CALLBACK (tb_make_menu), (gpointer) tb);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (tb_make_menu), (gpointer) tb);

    tb->desk_num = get_net_number_of_desktops();
    tb->cur_desk = get_net_current_desktop();
    tb->focused = NULL;
    tb->menu = NULL;

    tb_make_menu(NULL, tb);
    gtk_container_set_border_width(GTK_CONTAINER(p->pwid), 0);
    gtk_widget_show_all(tb->bar);
    return;
}
