/* Display workspace number, by cmeury@users.sf.net */

/**
 * @file deskno2.c
 * @brief Desktop name plugin (version 2) for fbpanel.
 *
 * Displays the name of the current virtual desktop on a flat GtkButton,
 * using _NET_DESKTOP_NAMES from the WM.  When a named desktop is not
 * available the desktop number (1-based) is used as the label.  Scroll-up
 * or scroll-left moves to the previous desktop; scroll-down or scroll-right
 * moves to the next.  Clicking the button launches `xfce-setting-show
 * workspaces` (hardcoded xfce command).
 *
 * Config keys: none.
 *
 * Signals connected to fbev (global FbEv singleton):
 *   "current_desktop"    -> update_dno (refresh current desk label)
 *   "desktop_names"      -> update_all (re-read all names from WM)
 *   "number_of_desktops" -> update_all (re-read on count change)
 *
 * Main widget: GtkButton (dc->main) packed into p->pwid.
 *
 * Note: the `fmt` field declared in deskno_priv is never read or written
 * (see BUG-026).  dc->dnames is a strv from get_utf8_property_list()
 * (transfer-full, freed via g_strfreev in update_all and destructor).
 * dc->lnames is a freshly allocated strv (transfer-full, freed the same way).
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"


typedef struct {
    plugin_instance plugin;
    GtkWidget  *main;        /**< GtkButton (flat relief) showing desktop name. */
    int         dno;         /**< Current 0-based desktop index. */
    int         dnum;        /**< Total number of desktops. */
    char      **dnames;      /**< Desktop names from WM (transfer-full strv). */
    int         dnames_num;  /**< Length of dnames array. */
    char      **lnames;      /**< Display label per desktop (transfer-full strv). */
    char       *fmt;         /**< Unused field (BUG-026). */
} deskno_priv;

static  void
clicked(GtkWidget *widget, deskno_priv *dc)
{
    (void)system("xfce-setting-show workspaces");
}

/**
 * update_dno - refresh the button label for the current desktop.
 * @widget: unused (FbEv signal data). (transfer none)
 * @dc:     deskno_priv. (transfer none)
 *
 * Called by the "current_desktop" FbEv signal and from update_all().
 */
static  void
update_dno(GtkWidget *widget, deskno_priv *dc)
{
    dc->dno = fb_ev_current_desktop(fbev);
    gtk_button_set_label(GTK_BUTTON(dc->main), dc->lnames[dc->dno]);

    return;
}

/**
 * update_all - re-read desktop names and count from the WM, then refresh.
 * @widget: unused (FbEv signal data). (transfer none)
 * @dc:     deskno_priv. (transfer none)
 *
 * Called by "desktop_names" and "number_of_desktops" FbEv signals and once
 * from the constructor.  Frees the previous dnames/lnames strv arrays and
 * builds new ones.  Desktops without a WM-provided name fall back to their
 * 1-based number.  Calls update_dno() to refresh the visible label.
 */
static  void
update_all(GtkWidget *widget, deskno_priv *dc)
{
    int i;

    dc->dnum = fb_ev_number_of_desktops(fbev);
    if (dc->dnames)
        g_strfreev (dc->dnames);
    if (dc->lnames)
        g_strfreev (dc->lnames);
    dc->dnames = get_utf8_property_list(GDK_ROOT_WINDOW(), a_NET_DESKTOP_NAMES, &(dc->dnames_num));
    dc->lnames = g_new0 (gchar*, dc->dnum + 1);
    for (i = 0; i < MIN(dc->dnum, dc->dnames_num); i++) {
        dc->lnames[i] = g_strdup(dc->dnames[i]);
    }
    for (; i < dc->dnum; i++) {
        dc->lnames[i] = g_strdup_printf("%d", i + 1);
    }
    update_dno(widget, dc);
    return;
}

static gboolean
scroll (GtkWidget *widget, GdkEventScroll *event, deskno_priv *dc)
{
    int dno;

    dno = dc->dno + ((event->direction == GDK_SCROLL_UP) ? (-1) : (+1));
    if (dno < 0)
        dno = dc->dnum - 1;
    else if (dno == dc->dnum)
        dno = 0;
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, dno, 0, 0, 0, 0);
    return TRUE;

}

/**
 * deskno_constructor - create the desktop name button and connect signals.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Creates a flat GtkButton.  Calls update_all() once to populate dnames and
 * lnames from the WM before showing the widget.  Connects three FbEv signals
 * for live updates.
 *
 * Returns: 1 on success.
 */
static int
deskno_constructor(plugin_instance *p)
{
    deskno_priv *dc;
    dc = (deskno_priv *) p;
    dc->main = gtk_button_new_with_label("w");
    gtk_button_set_relief(GTK_BUTTON(dc->main),GTK_RELIEF_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(dc->main), 0);
    g_signal_connect(G_OBJECT(dc->main), "clicked", G_CALLBACK (clicked), (gpointer) dc);
    g_signal_connect(G_OBJECT(dc->main), "scroll-event", G_CALLBACK(scroll), (gpointer) dc);

    update_all(dc->main, dc);

    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    gtk_widget_show_all(p->pwid);

    g_signal_connect (G_OBJECT (fbev), "current_desktop", G_CALLBACK (update_dno), (gpointer) dc);
    g_signal_connect (G_OBJECT (fbev), "desktop_names", G_CALLBACK (update_all), (gpointer) dc);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops", G_CALLBACK (update_all), (gpointer) dc);

    return 1;
}

/**
 * deskno_destructor - disconnect FbEv signals and free name arrays.
 * @p: plugin_instance. (transfer none)
 *
 * Disconnects update_dno and update_all from fbev, then frees dc->dnames
 * and dc->lnames (both are transfer-full strv arrays).
 */
static void
deskno_destructor(plugin_instance *p)
{
    deskno_priv *dc = (deskno_priv *) p;

    /* disconnect ALL handlers matching func and data */
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update_dno, dc);
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update_all, dc);
    if (dc->dnames)
        g_strfreev(dc->dnames);
    if (dc->lnames)
        g_strfreev(dc->lnames);
    return;
}

static plugin_class class = {
    .count       = 0,
    .type        = "deskno2",
    .name        = "Desktop No v2",
    .version     = "0.6",
    .description = "Display workspace number",
    .priv_size   = sizeof(deskno_priv),

    .constructor = deskno_constructor,
    .destructor  = deskno_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
