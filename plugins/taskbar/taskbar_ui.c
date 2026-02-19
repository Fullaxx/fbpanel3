#include <math.h>
#include "taskbar_priv.h"

/* Paint the task button background with cairo, bypassing GTK3 theme/CSS.
 * Connected to the "draw" signal (not "draw-after"), so we run before GTK's
 * default button draw.  We return TRUE to suppress GTK's drawing, then
 * propagate to the child (image+label box) ourselves. */
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

static void
tk_callback_leave( GtkWidget *widget, task *tk)
{
    gtk_widget_set_state_flags(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state, TRUE);
    return;
}


static void
tk_callback_enter( GtkWidget *widget, task *tk )
{
    gtk_widget_set_state_flags(widget,
          (tk->focused) ? tk->tb->focused_state : tk->tb->normal_state, TRUE);
    return;
}

static gboolean
delay_active_win(task* tk)
{
    tk_raise_window(tk, CurrentTime);
    tk->tb->dnd_activate = 0;
    return FALSE;
}

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

void
tk_display(taskbar_priv *tb, task *tk)
{
    tk_update(NULL, tk, tb);
    return;
}

void
tb_display(taskbar_priv *tb)
{
    if (tb->wins)
        g_hash_table_foreach(tb->task_list, (GHFunc) tk_update, (gpointer) tb);
    return;

}

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


static void
menu_raise_window(GtkWidget *widget, taskbar_priv *tb)
{
    DBG("win %x\n", tb->menutask->win);
    XMapRaised(GDK_DPY, tb->menutask->win);
    return;
}


static void
menu_iconify_window(GtkWidget *widget, taskbar_priv *tb)
{
    DBG("win %x\n", tb->menutask->win);
    XIconifyWindow (GDK_DPY, tb->menutask->win,
        DefaultScreen(GDK_DPY));
    return;
}

static void
send_to_workspace(GtkWidget *widget, void *iii, taskbar_priv *tb)
{
    int dst_desktop;


    dst_desktop = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "num"));
    DBG("win %x -> %d\n",  (unsigned int)tb->menutask->win, dst_desktop);
    Xclimsg(tb->menutask->win, a_NET_WM_DESKTOP, dst_desktop, 0, 0, 0, 0);

    return;
}

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

static gboolean
taskbar_apply_dim(taskbar_priv *tb)
{
    tb->pending_dim_id = 0;
    gtk_bar_set_dimension(GTK_BAR(tb->bar), tb->pending_dim);
    return G_SOURCE_REMOVE;
}

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
