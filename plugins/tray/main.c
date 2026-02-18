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


typedef struct {
    plugin_instance plugin;
    GtkWidget *box;
    EggTrayManager *tray_manager;
    FbBg *bg;
    gulong sid;
} tray_priv;

static void
tray_bg_changed(FbBg *bg, GtkWidget *widget)
{
    {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        gtk_widget_set_size_request(widget, alloc.width, alloc.height);
    }
    gtk_widget_hide(widget);
    if (gtk_events_pending())
        gtk_main_iteration();
    gtk_widget_show(widget);
    gtk_widget_set_size_request(widget, -1, -1);
    return;
}

static void
tray_added (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    gtk_box_pack_end(GTK_BOX(tr->box), icon, FALSE, FALSE, 0);
    gtk_widget_show(icon);
    gdk_display_sync(gtk_widget_get_display(icon));
    tray_bg_changed(NULL, tr->plugin.pwid);
    return;
}

static void
tray_removed (EggTrayManager *manager, GtkWidget *icon, tray_priv *tr)
{
    DBG("del icon\n");
    tray_bg_changed(NULL, tr->plugin.pwid);
    return;
}

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

static void
message_cancelled (EggTrayManager *manager, GtkWidget *icon, glong id,
    void *data)
{
    return;
}

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
