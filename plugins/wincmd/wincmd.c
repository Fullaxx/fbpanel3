/**
 * @file wincmd.c
 * @brief "Show Desktop" button plugin for fbpanel.
 *
 * Displays a single button (icon or image) that performs window management
 * actions on the current desktop:
 *   LMB (button 1): toggle-iconify all non-dock, non-desktop, non-splash
 *                   windows.  If all are iconified/shaded, raises them;
 *                   otherwise iconifies them all.
 *   MMB (button 2): toggle-shade all such windows (add/remove _NET_WM_STATE_SHADED).
 *
 * Window lists are obtained via _NET_CLIENT_LIST (shade) or
 * _NET_CLIENT_LIST_STACKING (iconify).  Dock, desktop, and splash window
 * types are skipped.  Only windows on the current virtual desktop (or
 * sticky windows with desktop == -1) are affected.
 *
 * Config keys (all transfer-none xconf strings):
 *   Button1  (enum: none/iconify/shade, default iconify) — unused in logic;
 *            button 1 is hardcoded to toggle-iconify.
 *   Button2  (enum: none/iconify/shade, default shade)   — similarly advisory.
 *   Icon     (str, optional) — icon theme name for fb_button_new().
 *   Image    (str, optional) — file path for fb_button_new(); "~" expanded
 *            via expand_tilda() (returns transfer-full, freed in constructor).
 *   tooltip  (str, optional) — markup tooltip on the button.
 *
 * Main widget: fb_button GtkButton packed into p->pwid.
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkx.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "gtkbgbox.h"
//#define DEBUGPRN
#include "dbg.h"


typedef struct {
    plugin_instance plugin;
    int button1; /**< Action for button 1 (read from config; not enforced). */
    int button2; /**< Action for button 2 (read from config; not enforced). */
    int action1; /**< Unused. */
    int action2; /**< Current shade toggle state (0=unshaded, 1=shaded). */
} wincmd_priv;

enum { WC_NONE, WC_ICONIFY, WC_SHADE };


xconf_enum wincmd_enum[] = {
    { .num = WC_NONE, .str = "none" },
    { .num = WC_ICONIFY, .str = "iconify" },
    { .num = WC_SHADE, .str = "shade" },
    { .num = 0, .str = NULL },
};

/**
 * toggle_shaded - add or remove _NET_WM_STATE_SHADED from all eligible windows.
 * @wc:     wincmd_priv. (transfer none)
 * @action: non-zero to shade (STATE_ADD), zero to unshade (STATE_REMOVE).
 *
 * Fetches _NET_CLIENT_LIST via get_xaproperty() (transfer-full, XFree'd).
 * Iterates windows on the current virtual desktop, skips dock/desktop/splash
 * types, and sends _NET_WM_STATE ClientMessages.
 */
static void
toggle_shaded(wincmd_priv *wc, guint32 action)
{
    Window *win = NULL;
    int num, i;
    guint32 tmp2, dno;
    net_wm_window_type nwwt;

    win = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST,
        XA_WINDOW, &num);
    if (!win)
	return;
    if (!num)
        goto end;
    dno = get_net_current_desktop();
    DBG("wincmd: #desk=%d\n", dno);
    for (i = 0; i < num; i++) {
        int skip;

        tmp2 = get_net_wm_desktop(win[i]);
        DBG("wincmd: win=0x%x dno=%d...", win[i], tmp2);
        if ((tmp2 != -1) && (tmp2 != dno)) {
            DBG("skip - not cur desk\n");
            continue;
        }
        get_net_wm_window_type(win[i], &nwwt);
        skip = (nwwt.dock || nwwt.desktop || nwwt.splash);
        if (skip) {
            DBG("skip - omnipresent window type\n");
            continue;
        }
        Xclimsg(win[i], a_NET_WM_STATE,
              action ? a_NET_WM_STATE_ADD : a_NET_WM_STATE_REMOVE,
              a_NET_WM_STATE_SHADED, 0, 0, 0);
        DBG("ok\n");
    }

 end:
    XFree(win);
    return;
}

/**
 * toggle_iconify - raise all iconified windows or iconify all visible ones.
 * @wc: wincmd_priv. (transfer none)
 *
 * Fetches _NET_CLIENT_LIST_STACKING via get_xaproperty() (transfer-full,
 * XFree'd).  Builds a local list (transfer-full g_new, g_free'd) of eligible
 * windows on the current desktop.  If all are hidden or shaded, maps them all
 * via XMapWindow(); otherwise iconifies them all via XIconifyWindow().
 */
/* if all windows are iconified then open all,
 * if any are open then iconify 'em */
static void
toggle_iconify(wincmd_priv *wc)
{
    Window *win, *awin;
    int num, i, j, dno, raise;
    guint32 tmp;
    net_wm_window_type nwwt;
    net_wm_state nws;

    win = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST_STACKING,
            XA_WINDOW, &num);
    if (!win)
	return;
    if (!num)
        goto end;
    awin = g_new(Window, num);
    dno = get_net_current_desktop();
    raise = 1;
    for (j = 0, i = 0; i < num; i++) {
        tmp = get_net_wm_desktop(win[i]);
        DBG("wincmd: win=0x%x dno=%d...", win[i], tmp);
        if ((tmp != -1) && (tmp != dno))
            continue;

        get_net_wm_window_type(win[i], &nwwt);
        tmp = (nwwt.dock || nwwt.desktop || nwwt.splash);
        if (tmp)
            continue;

        get_net_wm_state(win[i], &nws);
        raise = raise && (nws.hidden || nws.shaded);;
        awin[j++] = win[i];
    }
    while (j-- > 0) {
        if (raise)
            XMapWindow (GDK_DPY, awin[j]);
        else
            XIconifyWindow(GDK_DPY, awin[j],
                DefaultScreen(GDK_DPY));
    }

    g_free(awin);
 end:
    XFree(win);
    return;
}

/**
 * clicked - button-press-event handler for the wincmd button.
 * @widget: the fb_button GtkButton. (transfer none)
 * @event:  button press event. (transfer none)
 * @data:   wincmd_priv pointer. (transfer none)
 *
 * Button 1 calls toggle_iconify(); button 2 calls toggle_shaded() with an
 * alternating action2 value.
 *
 * Returns: FALSE (event not consumed; allow further processing).
 */
static gint
clicked (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    wincmd_priv *wc = (wincmd_priv *) data;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (event->button == 1) {
        toggle_iconify(wc);
    } else if (event->button == 2) {
        wc->action2 = 1 - wc->action2;
        toggle_shaded(wc, wc->action2);
        DBG("wincmd: shade all\n");
    } else {
        DBG("wincmd: unsupported command\n");
    }

    return FALSE;
}

/**
 * wincmd_destructor - no-op; all cleanup is handled by widget destruction.
 * @p: plugin_instance. (transfer none)
 */
static void
wincmd_destructor(plugin_instance *p)
{
    return;
}

/**
 * wincmd_constructor - create the show-desktop button.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads config keys.  "Image" (if present) is passed through expand_tilda()
 * to expand "~" (returns transfer-full, freed at end).  Creates an fb_button
 * from the icon/image, connects "button_press_event", adds it to p->pwid.
 * Sets the background to BG_INHERIT on transparent panels.
 *
 * Returns: 1 on success.
 */
static int
wincmd_constructor(plugin_instance *p)
{
    gchar *tooltip, *fname, *iname;
    wincmd_priv *wc;
    GtkWidget *button;
    int w, h;

    wc = (wincmd_priv *) p;
    tooltip = fname = iname = NULL;
    wc->button1 = WC_ICONIFY;
    wc->button2 = WC_SHADE;
    XCG(p->xc, "Button1", &wc->button1, enum, wincmd_enum);
    XCG(p->xc, "Button2", &wc->button2, enum, wincmd_enum);
    XCG(p->xc, "Icon", &iname, str);
    XCG(p->xc, "Image", &fname, str);
    XCG(p->xc, "tooltip", &tooltip, str);
    fname = expand_tilda(fname); /* returns transfer-full; freed below */

    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        w = -1;
        h = p->panel->max_elem_height;
    } else {
        w = p->panel->max_elem_height;
        h = -1;
    }
    button = fb_button_new(iname, fname, w, h, 0x202020, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(button), 0);
    g_signal_connect(G_OBJECT(button), "button_press_event",
          G_CALLBACK(clicked), (gpointer)wc);

    gtk_widget_show(button);
    gtk_container_add(GTK_CONTAINER(p->pwid), button);
    if (p->panel->transparent)
        gtk_bgbox_set_background(button, BG_INHERIT,
            p->panel->tintcolor, p->panel->alpha);

    g_free(fname); /* correct: expand_tilda returned transfer-full */
    if (tooltip)
        gtk_widget_set_tooltip_markup(button, tooltip);
    /* tooltip is transfer-none (XCG str); do NOT g_free it */

    return 1;
}


static plugin_class class = {
    .count       = 0,
    .type        = "wincmd",
    .name        = "Show desktop",
    .version     = "1.0",
    .description = "Show Desktop button",
    .priv_size   = sizeof(wincmd_priv),


    .constructor = wincmd_constructor,
    .destructor = wincmd_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
