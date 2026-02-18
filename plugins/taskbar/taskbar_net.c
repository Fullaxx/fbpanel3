#include "taskbar_priv.h"

gboolean use_net_active = FALSE;

/*****************************************************
 * handlers for NET actions                          *
 *****************************************************/


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



void
tb_net_current_desktop(GtkWidget *widget, taskbar_priv *tb)
{
    tb->cur_desk = get_net_current_desktop();
    tb_display(tb);
    return;
}


void
tb_net_number_of_desktops(GtkWidget *widget, taskbar_priv *tb)
{
    tb->desk_num = get_net_number_of_desktops();
    tb_display(tb);
    return;
}


/* set new active window. if that happens to be us, then remeber
 * current focus to use it for iconify command */
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

GdkFilterReturn
tb_event_filter( XEvent *xev, GdkEvent *event, taskbar_priv *tb)
{

    //RET(GDK_FILTER_CONTINUE);
    g_assert(tb != NULL);
    if (xev->type == PropertyNotify )
        tb_propertynotify(tb, xev);
    return GDK_FILTER_CONTINUE;
}

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
