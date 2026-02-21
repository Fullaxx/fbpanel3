/**
 * @file taskbar_net.c
 * @brief Taskbar plugin — EWMH event handlers and per-window GDK event filter.
 *
 * This file handles the taskbar's response to root-window EWMH property changes
 * (received via FbEv signals) and per-window PropertyNotify events (received
 * via tb_event_filter, a GdkFilterFunc installed on each client GdkWindow).
 *
 * ROOT WINDOW EVENTS (via FbEv signals)
 * ---------------------------------------
 * tb_net_client_list:   _NET_CLIENT_LIST changed -> rebuild task list.
 * tb_net_current_desktop: _NET_CURRENT_DESKTOP changed -> update cur_desk, refresh display.
 * tb_net_number_of_desktops: _NET_NUMBER_OF_DESKTOPS changed -> update desk_num.
 * tb_net_active_window: _NET_ACTIVE_WINDOW changed -> update tb->focused.
 *
 * PER-WINDOW EVENTS (via tb_event_filter / tb_propertynotify)
 * -----------------------------------------------------------
 * a_NET_WM_DESKTOP:     Window moved to another desktop.
 * XA_WM_NAME:           Window title changed.
 * XA_WM_HINTS:          Icon or urgency hint changed.
 * a_NET_WM_STATE:       Window state changed (may cause removal from taskbar).
 * a_NET_WM_ICON:        ARGB icon data updated.
 * a_NET_WM_WINDOW_TYPE: Window type changed (may cause removal from taskbar).
 *
 * STALE TASK REMOVAL
 * ------------------
 * tb_net_client_list uses a two-pass approach:
 *   Pass 1: for each window in the new _NET_CLIENT_LIST:
 *     - If already in task_list: increment tk->refcount.
 *     - If new: create task, insert into hash table (refcount=1).
 *   Pass 2: g_hash_table_foreach_remove with task_remove_stale:
 *     - If refcount was not incremented (== 0 after decrement): remove task.
 *
 * NET ACTIVE WINDOW DETECTION
 * ---------------------------
 * use_net_active is set to TRUE by net_active_detect() if a_NET_ACTIVE_WINDOW
 * appears in the root window's _NET_SUPPORTED list.  When TRUE, window
 * activation sends _NET_ACTIVE_WINDOW ClientMessages; otherwise Xlib calls
 * are used directly (XRaiseWindow + XSetInputFocus).
 */

#include "taskbar_priv.h"

/** TRUE if the WM supports _NET_ACTIVE_WINDOW; set by net_active_detect(). */
gboolean use_net_active = FALSE;

/*****************************************************
 * handlers for NET actions                          *
 *****************************************************/


/**
 * tb_net_client_list - handle _NET_CLIENT_LIST change.
 * @widget: FbEv GObject (unused; signature matches GCallback convention).
 * @tb:     Taskbar instance.
 *
 * Refreshes tb->wins from the root window's _NET_CLIENT_LIST property.
 * Creates task entries for newly-appeared windows (after filtering by
 * accept_net_wm_state and accept_net_wm_window_type), increments refcount
 * for existing tasks, then calls task_remove_stale to delete tasks whose
 * windows are no longer in the list.
 *
 * Ownership: tb->wins is replaced on each call (old value XFree'd first).
 */
void
tb_net_client_list(GtkWidget *widget, taskbar_priv *tb)
{
    int i;
    task *tk;

    if (tb->wins)
        XFree(tb->wins);
    tb->wins = get_xaproperty (GDK_ROOT_WINDOW(),
        a_NET_CLIENT_LIST, XA_WINDOW, &tb->win_num);
    if (!tb->wins)
        return;
    for (i = 0; i < tb->win_num; i++) {
        if ((tk = g_hash_table_lookup(tb->task_list, &tb->wins[i]))) {
            tk->refcount++;
        } else {
            net_wm_window_type nwwt;
            net_wm_state nws;

            get_net_wm_state(tb->wins[i], &nws);
            if (!accept_net_wm_state(&nws, tb->accept_skip_pager))
                continue;
            get_net_wm_window_type(tb->wins[i], &nwwt);
            if (!accept_net_wm_window_type(&nwwt))
                continue;

            tk = g_new0(task, 1);
            tk->refcount = 1;
            tb->num_tasks++;
            tk->win = tb->wins[i];
            tk->tb = tb;
            tk->iconified = nws.hidden;
            tk->desktop = get_net_wm_desktop(tk->win);
            tk->nws = nws;
            tk->nwwt = nwwt;
            if( tb->use_urgency_hint && tk_has_urgency(tk)) {
                tk->urgency = 1;
            }

            tk_build_gui(tb, tk);
            tk_get_names(tk);
            tk_set_names(tk);

            g_hash_table_insert(tb->task_list, &tk->win, tk);
            DBG("adding %08x(%p) %s\n", tk->win,
                FBPANEL_WIN(tk->win), tk->name);
        }
    }

    /* remove windows that arn't in the NET_CLIENT_LIST anymore */
    g_hash_table_foreach_remove(tb->task_list, (GHRFunc) task_remove_stale,
        NULL);
    tb_display(tb);
    return;
}



/**
 * tb_net_current_desktop - handle _NET_CURRENT_DESKTOP change.
 * @widget: FbEv GObject (unused).
 * @tb:     Taskbar instance.
 *
 * Updates tb->cur_desk and refreshes the display to show/hide tasks
 * based on the new active desktop.
 */
void
tb_net_current_desktop(GtkWidget *widget, taskbar_priv *tb)
{
    tb->cur_desk = get_net_current_desktop();
    tb_display(tb);
    return;
}


/**
 * tb_net_number_of_desktops - handle _NET_NUMBER_OF_DESKTOPS change.
 * @widget: FbEv GObject (unused).
 * @tb:     Taskbar instance.
 *
 * Updates tb->desk_num and refreshes the display.
 */
void
tb_net_number_of_desktops(GtkWidget *widget, taskbar_priv *tb)
{
    tb->desk_num = get_net_number_of_desktops();
    tb_display(tb);
    return;
}


/**
 * tb_net_active_window - handle _NET_ACTIVE_WINDOW change.
 * @widget: FbEv GObject (unused).
 * @tb:     Taskbar instance.
 *
 * Reads the new active window from _NET_ACTIVE_WINDOW.  Updates tb->focused
 * and the visual state of the old and new focused tasks.
 *
 * Special case: if the panel window itself became active (the user clicked on
 * the panel), tb->ptk is updated to the previously-focused task so that a
 * subsequent click on that task's button will iconify it (the second-click
 * iconify logic in tk_callback_button_release_event).
 *
 * The _NET_ACTIVE_WINDOW data is (transfer full) XFree'd here after reading.
 */
void
tb_net_active_window(GtkWidget *widget, taskbar_priv *tb)
{
    Window *f;
    task *ntk, *ctk;
    int drop_old, make_new;

    g_assert (tb != NULL);
    drop_old = make_new = 0;
    ctk = tb->focused;
    ntk = NULL;
    f = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
    DBG("FOCUS=%x\n", f ? *f : 0);
    if (!f) {
        drop_old = 1;
        tb->ptk = NULL;
    } else {
        if (*f == tb->topxwin) {
            if (ctk) {
                tb->ptk = ctk;
                drop_old = 1;
            }
        } else {
            tb->ptk = NULL;
            ntk = find_task(tb, *f);
            if (ntk != ctk) {
                drop_old = 1;
                make_new = 1;
            }
        }
        XFree(f);
    }
    if (ctk && drop_old) {
        ctk->focused = 0;
        tb->focused = NULL;
        tk_display(tb, ctk);
        DBG("old focus was dropped\n");
    }
    if (ntk && make_new) {
        ntk->focused = 1;
        tb->focused = ntk;
        tk_display(tb, ntk);
        DBG("new focus was set\n");
    }
    return;
}

/**
 * tb_propertynotify - dispatch a per-window PropertyNotify X event.
 * @tb: Taskbar instance.
 * @ev: The X11 PropertyNotify event.
 *
 * Called by tb_event_filter for PropertyNotify events on client windows
 * (not the root window).  Looks up the task by window ID and dispatches
 * based on the changed atom:
 *
 *   a_NET_WM_DESKTOP  -> update task desktop, refresh display
 *   XA_WM_NAME        -> re-read and re-display window title
 *   XA_WM_HINTS       -> re-read icon (may have just been set after map);
 *                        start/stop flash if urgency hint changed
 *   a_NET_WM_STATE    -> re-check accept filter; remove task if no longer accepted;
 *                        otherwise update iconified state and title
 *   a_NET_WM_ICON     -> refresh ARGB icon
 *   a_NET_WM_WINDOW_TYPE -> re-check accept filter; remove task if window type changed
 */
static void
tb_propertynotify(taskbar_priv *tb, XEvent *ev)
{
    Atom at;
    Window win;

    DBG("win=%x\n", ev->xproperty.window);
    at = ev->xproperty.atom;
    win = ev->xproperty.window;
    if (win != GDK_ROOT_WINDOW()) {
        task *tk = find_task(tb, win);

        if (!tk) RET();
        DBG("win=%x\n", ev->xproperty.window);
        if (at == a_NET_WM_DESKTOP) {
            DBG("NET_WM_DESKTOP\n");
            tk->desktop = get_net_wm_desktop(win);
            tb_display(tb);
        } else if (at == XA_WM_NAME) {
            DBG("WM_NAME\n");
            tk_get_names(tk);
            tk_set_names(tk);
        } else if (at == XA_WM_HINTS)   {
            /* some windows set their WM_HINTS icon after mapping */
            DBG("XA_WM_HINTS\n");
            tk_update_icon (tb, tk, XA_WM_HINTS);
            gtk_image_set_from_pixbuf (GTK_IMAGE(tk->image), tk->pixbuf);
            if (tb->use_urgency_hint) {
                if (tk_has_urgency(tk)) {
                    //tk->urgency = 1;
                    tk_flash_window(tk);
                } else {
                    //tk->urgency = 0;
                    tk_unflash_window(tk);
                }
            }
        } else if (at == a_NET_WM_STATE) {
            net_wm_state nws;

            DBG("_NET_WM_STATE\n");
            get_net_wm_state(tk->win, &nws);
            if (!accept_net_wm_state(&nws, tb->accept_skip_pager)) {
                del_task(tb, tk, 1);
                tb_display(tb);
            } else {
                tk->iconified = nws.hidden;
                tk_set_names(tk);
            }
        } else if (at == a_NET_WM_ICON) {
            DBG("_NET_WM_ICON\n");
            DBG("#0 %d\n", GDK_IS_PIXBUF (tk->pixbuf));
            tk_update_icon (tb, tk, a_NET_WM_ICON);
            DBG("#1 %d\n", GDK_IS_PIXBUF (tk->pixbuf));
            gtk_image_set_from_pixbuf (GTK_IMAGE(tk->image), tk->pixbuf);
            DBG("#2 %d\n", GDK_IS_PIXBUF (tk->pixbuf));
        } else if (at == a_NET_WM_WINDOW_TYPE) {
            net_wm_window_type nwwt;

            DBG("_NET_WM_WINDOW_TYPE\n");
            get_net_wm_window_type(tk->win, &nwwt);
            if (!accept_net_wm_window_type(&nwwt)) {
                del_task(tb, tk, 1);
                tb_display(tb);
            }
        } else {
            DBG("at = %d\n", at);
        }
    }
    return;
}

/**
 * tb_event_filter - GdkFilterFunc for per-client-window X events.
 * @xev:   Raw X11 event.
 * @event: GDK-translated event (unused).
 * @tb:    Taskbar instance.
 *
 * Installed on each task's GdkWindow via gdk_window_add_filter.
 * Passes PropertyNotify events to tb_propertynotify and always returns
 * GDK_FILTER_CONTINUE so GDK continues normal event processing.
 *
 * Returns: GDK_FILTER_CONTINUE always (we observe, never consume events).
 */
GdkFilterReturn
tb_event_filter( XEvent *xev, GdkEvent *event, taskbar_priv *tb)
{

    //RET(GDK_FILTER_CONTINUE);
    g_assert(tb != NULL);
    if (xev->type == PropertyNotify )
        tb_propertynotify(tb, xev);
    return GDK_FILTER_CONTINUE;
}

/**
 * net_active_detect - probe _NET_SUPPORTED for _NET_ACTIVE_WINDOW support.
 *
 * Reads _NET_SUPPORTED from the root window and sets the global use_net_active
 * to TRUE if a_NET_ACTIVE_WINDOW appears in the list.  Called once from
 * taskbar_constructor before any window activation is attempted.
 *
 * The _NET_SUPPORTED data is (transfer full) XFree'd after scanning.
 */
void net_active_detect()
{
    int nitens;
    Atom *data;

    data = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_SUPPORTED, XA_ATOM, &nitens);
    if (!data)
        return;

    while (nitens > 0)
        if(data[--nitens]==a_NET_ACTIVE_WINDOW) {
            use_net_active = TRUE;
            break;
        }

    XFree(data);
}
