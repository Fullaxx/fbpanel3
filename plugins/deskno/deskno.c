// reused dclock.c and variables from pager.c
// 11/23/04 by cmeury@users.sf.net",

/**
 * @file deskno.c
 * @brief Desktop number plugin (version 1) for fbpanel.
 *
 * Displays the current virtual desktop number (1-based) as a bold GtkLabel
 * on a flat GtkButton.  Click or scroll-up/left moves to the previous desktop;
 * scroll-down/right moves to the next.  Left-click cycles to the next desktop.
 *
 * Config keys: none.
 *
 * Signals connected to fbev (global FbEv singleton):
 *   "current_desktop"    -> name_update (updates the label)
 *   "number_of_desktops" -> update      (refreshes desknum)
 *
 * Main widget: GtkButton (dc->main) with GtkLabel (dc->namew) inside,
 * packed into p->pwid.  Both are owned by the parent widget chain.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    plugin_instance plugin;
    GtkWidget *main;   /**< GtkButton (flat relief). */
    GtkWidget *namew;  /**< GtkLabel showing "<b>N</b>" where N is 1-based desk number. */
    int deskno;        /**< Current 0-based desktop index. */
    int desknum;       /**< Total number of desktops. */
} deskno_priv;

/**
 * change_desktop - send _NET_CURRENT_DESKTOP ClientMessage to move by delta.
 * @dc:    deskno_priv instance. (transfer none)
 * @delta: +1 or -1 (wraps around at boundaries).
 */
static void
change_desktop(deskno_priv *dc, int delta)
{
    int newdesk = dc->deskno + delta;

    if (newdesk < 0)
        newdesk = dc->desknum - 1;
    else if (newdesk >= dc->desknum)
        newdesk = 0;
    DBG("%d/%d -> %d\n", dc->deskno, dc->desknum, newdesk);
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, newdesk, 0, 0, 0, 0);
    return;
}

static void
clicked(GtkWidget *widget, deskno_priv *dc)
{
    change_desktop(dc, 1);
}

static gboolean
scrolled(GtkWidget *widget, GdkEventScroll *event, deskno_priv *dc)
{
    change_desktop(dc, (event->direction == GDK_SCROLL_UP
            || event->direction == GDK_SCROLL_LEFT) ? -1 : 1);
    return FALSE;
}

/**
 * name_update - refresh the label with the current desktop number.
 * @widget: unused (GObject signal data). (transfer none)
 * @dc:     deskno_priv. (transfer none)
 *
 * Called by the "current_desktop" FbEv signal and from the constructor.
 *
 * Returns: TRUE.
 */
static gint
name_update(GtkWidget *widget, deskno_priv *dc)
{
    char buffer [15];

    dc->deskno = get_net_current_desktop();
    snprintf(buffer, sizeof(buffer), "<b>%d</b>", dc->deskno + 1);
    gtk_label_set_markup(GTK_LABEL(dc->namew), buffer);
    return TRUE;
}

/**
 * update - refresh desknum from the WM.
 * @widget: unused. (transfer none)
 * @dc:     deskno_priv. (transfer none)
 *
 * Called by the "number_of_desktops" FbEv signal.
 *
 * Returns: TRUE.
 */
static gint
update(GtkWidget *widget, deskno_priv *dc)
{
    dc->desknum = get_net_number_of_desktops();
    return TRUE;
}

/**
 * deskno_constructor - create the desktop number button.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Creates a GtkButton (flat relief) containing a GtkLabel.  Connects
 * "clicked" and "scroll-event" signals on the button, then connects two
 * FbEv signals (current_desktop, number_of_desktops) for live updates.
 * An initial name_update()/update() populates the label immediately.
 *
 * Returns: 1 on success.
 */
static int
deskno_constructor(plugin_instance *p)
{
    deskno_priv *dc;

    dc = (deskno_priv *) p;
    dc->main = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(dc->main), GTK_RELIEF_NONE);
    g_signal_connect(G_OBJECT(dc->main), "clicked", G_CALLBACK(clicked),
        (gpointer) dc);
    g_signal_connect(G_OBJECT(dc->main), "scroll-event", G_CALLBACK(scrolled),
        (gpointer) dc);
    dc->namew = gtk_label_new("ww");
    gtk_container_add(GTK_CONTAINER(dc->main), dc->namew);
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    gtk_widget_show_all(p->pwid);
    name_update(dc->main, dc);
    update(dc->main, dc);
    g_signal_connect(G_OBJECT(fbev), "current_desktop", G_CALLBACK
        (name_update), (gpointer) dc);
    g_signal_connect(G_OBJECT(fbev), "number_of_desktops", G_CALLBACK
        (update), (gpointer) dc);
    return 1;
}

/**
 * deskno_destructor - disconnect FbEv signal handlers.
 * @p: plugin_instance. (transfer none)
 *
 * Disconnects "current_desktop" and "number_of_desktops" handlers from fbev.
 * Widget cleanup is handled by the parent GtkBgbox destruction.
 */
static void
deskno_destructor(plugin_instance *p)
{
  deskno_priv *dc = (deskno_priv *) p;

  g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), name_update, dc);
  g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), update, dc);
  return;
}

static plugin_class class = {
    .type        = "deskno",
    .name        = "Desktop No v1",
    .version     = "0.6",
    .description = "Display workspace number",
    .priv_size   = sizeof(deskno_priv),

    .constructor = deskno_constructor,
    .destructor  = deskno_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
