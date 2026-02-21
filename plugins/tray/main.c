/**
 * @file main.c
 * @brief System tray plugin — fbpanel integration for EggTrayManager.
 *
 * Wraps EggTrayManager to display a row of system-tray (notification area)
 * icons on the panel.  Each docked application embeds its icon via the
 * XEMBED protocol; the plugin receives the resulting GtkSocket widget and
 * packs it into a GtkBar.
 *
 * PLUGIN LIFECYCLE
 * ----------------
 * 1. tray_constructor() calls class_get("tray") to register this instance
 *    (prevents duplicate tray plugins from both attempting XEMBED ownership).
 * 2. A GtkBar is created and added to plug->pwid (the plugin's GtkBgbox).
 * 3. FbBg "changed" signal is connected to trigger a resize when the root
 *    window background changes.
 * 4. If another systray manager is already running on the screen, the plugin
 *    returns early with tr->tray_manager == NULL (tray icons cannot be embedded
 *    but the plugin widget still exists).
 * 5. Otherwise, EggTrayManager takes the _NET_SYSTEM_TRAY_S{n} X selection and
 *    begins accepting dock requests from systray applications.
 *
 * ICON WIDGET LIFECYCLE
 * ---------------------
 * - EggTrayManager "tray_icon_added": tray_added() receives a GtkSocket.
 *   The socket is packed into tr->box via gtk_box_pack_end(), shown, and the
 *   display is synchronised to let the embedding complete.
 * - EggTrayManager "tray_icon_removed": tray_removed() triggers a resize.
 *   The socket itself is destroyed by the plug_removed / unmanage path inside
 *   EggTrayManager.
 *
 * BALLOON MESSAGES
 * ----------------
 * - EggTrayManager "message_sent": message_sent() displays a fixed tooltip
 *   near the icon position using fixed_tip_show().
 * - EggTrayManager "message_cancelled": message_cancelled() is a no-op stub.
 *
 * DIMENSION MANAGEMENT
 * --------------------
 * tray_size_alloc() is connected to plug->pwid "size-allocate".  It computes
 * how many icon slots fit in the available dimension
 * (height for horizontal panel, width for vertical), and updates the GtkBar
 * dimension to wrap icons into multiple rows/columns.
 *
 * TRAY PRIVATE FIELDS
 * -------------------
 * - plugin: must be first (plugin framework casts between plugin_instance* and tray_priv*).
 * - box:    GtkBar (transfer none; owned by plug->pwid GTK container).
 * - tray_manager: EggTrayManager (transfer full, g_object_unref in destructor);
 *                 NULL if another manager was already running.
 * - bg:     FbBg singleton reference (transfer full, g_object_unref in destructor).
 * - sid:    GSignal handler ID for FbBg "changed"; disconnected in destructor.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "bg.h"
#include "gtkbgbox.h"
#include "gtkbar.h"

#include "eggtraymanager.h"
#include "fixedtip.h"


//#define DEBUGPRN
#include "dbg.h"


/**
 * tray_priv - system tray plugin private state.
 *
 * Embedded as first field of the plugin allocation so that
 * plugin_instance* and tray_priv* are interchangeable.
 */
typedef struct {
    plugin_instance plugin;         /**< Must be first; plugin framework accesses this. */
    GtkWidget *box;                 /**< GtkBar containing embedded GtkSocket icons;
                                     *   owned by plug->pwid GTK container. */
    EggTrayManager *tray_manager;   /**< XEMBED systray manager; (transfer full).
                                     *   NULL if another manager was already running
                                     *   when this plugin was constructed. */
    FbBg *bg;                       /**< FbBg singleton ref (transfer full, g_object_unref).
                                     *   Signals background changes for icon resize. */
    gulong sid;                     /**< GSignal handler ID for FbBg "changed" signal;
                                     *   disconnected in tray_destructor. */
} tray_priv;

/**
 * tray_bg_changed - FbBg "changed" signal handler.
 * @bg:     FbBg instance (unused). (transfer none)
 * @widget: The plugin's pwid GtkBgbox. (transfer none)
 *
 * Queues a resize on the plugin widget so tray icons repaint against the
 * new background.  In the GTK2 port this used hide+show; in GTK3 a
 * queue_resize suffices.
 */
static void
tray_bg_changed(FbBg *bg, GtkWidget *widget)
{
    /* GTK2 used hide+show+gtk_main_iteration() to force a synchronous
     * reflow after the wallpaper changed.  In GTK3 queue_resize suffices. */
    gtk_widget_queue_resize(widget);
    return;
}

/**
 * tray_added - EggTrayManager "tray_icon_added" signal handler.
 * @manager: EggTrayManager that received the dock request. (transfer none)
 * @icon:    GtkSocket wrapping the embedded systray application window. (transfer none)
 * @tr:      Tray plugin instance. (transfer none)
 *
 * Packs the new socket at the end of tr->box (right side for horizontal
 * panels), shows it, and synchronises the display to complete XEMBED embedding.
 * Then triggers a resize of the plugin widget via tray_bg_changed() so the
 * GtkBar dimension is updated.
 */
static void
tray_added (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    gtk_box_pack_end(GTK_BOX(tr->box), icon, FALSE, FALSE, 0);
    gtk_widget_show(icon);
    gdk_display_sync(gtk_widget_get_display(icon));
    tray_bg_changed(NULL, tr->plugin.pwid);
    return;
}

/**
 * tray_removed - EggTrayManager "tray_icon_removed" signal handler.
 * @manager: EggTrayManager. (transfer none)
 * @icon:    The GtkSocket being removed. (transfer none)
 * @tr:      Tray plugin instance. (transfer none)
 *
 * Triggers a resize so the GtkBar recalculates available icon slots after
 * the icon is removed.  The socket itself is destroyed by EggTrayManager's
 * plug_removed handler.
 */
static void
tray_removed (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    DBG("del icon\n");
    tray_bg_changed(NULL, tr->plugin.pwid);
    return;
}

/**
 * message_sent - EggTrayManager "message_sent" signal handler.
 * @manager: EggTrayManager. (transfer none)
 * @icon:    GtkSocket of the message sender. (transfer none)
 * @text:    Markup text for the balloon message. (transfer none)
 * @id:      Message identifier (opaque to the tray manager).
 * @timeout: Display timeout in milliseconds (ignored by fixed_tip_show).
 * @data:    User data (unused).
 *
 * Displays a fixed tooltip near the bottom of the primary monitor, positioned
 * just above the icon's X position.  Uses fixed_tip_show() with strut=bottom
 * of screen - 50px.
 *
 * Note: "FIXME multihead" — the position assumes a single primary monitor.
 */
static void
message_sent (EggTrayManager *manager, GtkWidget *icon, const char *text,
    glong id, glong timeout, void *data)
{
    /* FIXME multihead */
    int x, y;

    gdk_window_get_origin (gtk_widget_get_window(icon), &x, &y);
    {
        GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);
        fixed_tip_show (0, x, y, FALSE, geom.y + geom.height - 50, text);
    }
    return;
}

/**
 * message_cancelled - EggTrayManager "message_cancelled" signal handler.
 * @manager: EggTrayManager. (transfer none)
 * @icon:    GtkSocket of the cancelling application. (transfer none)
 * @id:      Identifier of the message to cancel.
 * @data:    User data (unused).
 *
 * Currently a no-op stub.  fixed_tip_hide() is not called here because
 * the balloon is identified by icon, not by message id, and the existing
 * fixed_tip implementation has no per-message tracking.
 */
static void
message_cancelled (EggTrayManager *manager, GtkWidget *icon, glong id,
    void *data)
{
    return;
}

/**
 * tray_destructor - plugin destructor; tears down the systray.
 * @p: Plugin instance. (transfer none)
 *
 * Teardown sequence:
 *  1. Disconnect FbBg "changed" signal handler (tr->sid).
 *  2. g_object_unref(tr->bg).
 *  3. If tr->tray_manager is set: g_object_unref triggers finalize, which
 *     calls egg_tray_manager_unmanage() — releases the _NET_SYSTEM_TRAY_S{n}
 *     X selection and removes the GDK window filter.
 *  4. fixed_tip_hide() to destroy any visible balloon tooltip.
 *
 * Note: tr->box is owned by the plug->pwid GTK container and is destroyed
 * when the container is destroyed by the plugin framework.
 */
static void
tray_destructor(plugin_instance *p)
{
    tray_priv *tr = (tray_priv *) p;

    g_signal_handler_disconnect(tr->bg, tr->sid);
    g_object_unref(tr->bg);
    /* Make sure we drop the manager selection */
    if (tr->tray_manager)
        g_object_unref(G_OBJECT(tr->tray_manager));
    fixed_tip_hide();
    return;
}


/**
 * tray_size_alloc - "size-allocate" signal handler for the plugin's pwid.
 * @widget: The plugin's pwid GtkBgbox. (transfer none)
 * @a:      New allocation. (transfer none)
 * @tr:     Tray plugin instance. (transfer none)
 *
 * Computes how many icon slots of size max_elem_height fit in the current
 * allocation dimension (height for horizontal panels, width for vertical),
 * and updates the GtkBar dimension so icons wrap into the correct number of
 * rows or columns.
 */
static void
tray_size_alloc(GtkWidget *widget, GtkAllocation *a,
    tray_priv *tr)
{
    int dim, size;

    size = tr->plugin.panel->max_elem_height;
    if (tr->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        dim = a->height / size;
    else
        dim = a->width / size;
    DBG("width=%d height=%d iconsize=%d -> dim=%d\n",
        a->width, a->height, size, dim);
    gtk_bar_set_dimension(GTK_BAR(tr->box), dim);
    return;
}


/**
 * tray_constructor - plugin constructor; sets up the systray.
 * @p: Plugin instance (also usable as tray_priv*). (transfer none)
 *
 * Initialisation sequence:
 *  1. class_get("tray") — registers this as the active tray instance;
 *     prevents a second tray plugin from also attempting XEMBED ownership.
 *  2. Connect "size-allocate" on plug->pwid to tray_size_alloc.
 *  3. Create a GtkBar (horizontal or vertical, spacing=0, row/col size =
 *     max_elem_height); centre-align it in pwid.
 *  4. Acquire FbBg singleton and connect "changed" signal.
 *  5. If egg_tray_manager_check_running() finds an existing systray manager
 *     on the screen: set tr->tray_manager = NULL, log a warning, and return
 *     early.  The plugin exists but has no XEMBED capability.
 *  6. Otherwise: create EggTrayManager, call egg_tray_manager_manage_screen()
 *     to take the _NET_SYSTEM_TRAY_S{n} selection, and connect the four
 *     EggTrayManager signals.
 *
 * Returns: 1 on success (even if another manager is already running).
 */
static int
tray_constructor(plugin_instance *p)
{
    tray_priv *tr;
    GdkScreen *screen;

    tr = (tray_priv *) p;
    class_get("tray");
    g_signal_connect(G_OBJECT(p->pwid), "size-allocate",
        (GCallback) tray_size_alloc, tr);
    tr->box = gtk_bar_new(p->panel->orientation, 0,
        p->panel->max_elem_height, p->panel->max_elem_height);
    gtk_widget_set_halign(tr->box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tr->box, GTK_ALIGN_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(tr->box), 0);
    gtk_container_add(GTK_CONTAINER(p->pwid), tr->box);
    gtk_widget_show_all(tr->box);
    tr->bg = fb_bg_get_for_display();
    tr->sid = g_signal_connect(tr->bg, "changed",
        G_CALLBACK(tray_bg_changed), p->pwid);

    screen = gtk_widget_get_screen(p->panel->topgwin);

    if (egg_tray_manager_check_running(screen)) {
        tr->tray_manager = NULL;
        ERR("tray: another systray already running\n");
        return 1;
    }
    tr->tray_manager = egg_tray_manager_new ();
    if (!egg_tray_manager_manage_screen (tr->tray_manager, screen))
        g_printerr("tray: can't get the system tray manager selection\n");

    g_signal_connect(tr->tray_manager, "tray_icon_added",
        G_CALLBACK(tray_added), tr);
    g_signal_connect(tr->tray_manager, "tray_icon_removed",
        G_CALLBACK(tray_removed), tr);
    g_signal_connect(tr->tray_manager, "message_sent",
        G_CALLBACK(message_sent), tr);
    g_signal_connect(tr->tray_manager, "message_cancelled",
        G_CALLBACK(message_cancelled), tr);

    gtk_widget_show_all(tr->box);
    return 1;

}


static plugin_class class = {
    .count       = 0,
    .type        = "tray",
    .name        = "System tray",
    .version     = "1.0",
    .description = "System tray aka Notification Area",
    .priv_size   = sizeof(tray_priv),

    .constructor = tray_constructor,
    .destructor = tray_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
