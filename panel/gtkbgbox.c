/* GTK - The GIMP Toolkit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/.
 */

#include <string.h>
#include "gtkbgbox.h"
#include "bg.h"
#include <gdk/gdk.h>
#include <glib.h>
#include <glib-object.h>


//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    cairo_surface_t *pixmap;
    guint32 tintcolor;
    gint alpha;
    int bg_type;
    FbBg *bg;
    gulong sid;
} GtkBgboxPrivate;

G_DEFINE_TYPE_WITH_CODE(GtkBgbox, gtk_bgbox, GTK_TYPE_BIN, G_ADD_PRIVATE(GtkBgbox))

static void gtk_bgbox_class_init    (GtkBgboxClass *klass);
static void gtk_bgbox_init          (GtkBgbox *bgbox);
static void gtk_bgbox_realize       (GtkWidget *widget);
static void gtk_bgbox_get_preferred_width  (GtkWidget *widget, gint *minimum, gint *natural);
static void gtk_bgbox_get_preferred_height (GtkWidget *widget, gint *minimum, gint *natural);
static void gtk_bgbox_size_allocate (GtkWidget *widget, GtkAllocation *allocation);
static void gtk_bgbox_style_updated (GtkWidget *widget);
static gboolean gtk_bgbox_configure_event(GtkWidget *widget, GdkEventConfigure *e);
static gboolean gtk_bgbox_draw      (GtkWidget *widget, cairo_t *cr);

static void gtk_bgbox_finalize (GObject *object);

static void gtk_bgbox_set_bg_root(GtkWidget *widget, GtkBgboxPrivate *priv);
static void gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv);
static void gtk_bgbox_bg_changed(FbBg *bg, GtkWidget *widget);

static GtkBinClass *parent_class = NULL;

static void
gtk_bgbox_class_init (GtkBgboxClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

    parent_class = g_type_class_peek_parent (class);

    widget_class->realize              = gtk_bgbox_realize;
    widget_class->get_preferred_width  = gtk_bgbox_get_preferred_width;
    widget_class->get_preferred_height = gtk_bgbox_get_preferred_height;
    widget_class->size_allocate        = gtk_bgbox_size_allocate;
    widget_class->style_updated        = gtk_bgbox_style_updated;
    widget_class->configure_event      = gtk_bgbox_configure_event;
    widget_class->draw                 = gtk_bgbox_draw;

    object_class->finalize = gtk_bgbox_finalize;
}

static void
gtk_bgbox_init (GtkBgbox *bgbox)
{
    GtkBgboxPrivate *priv;

    gtk_widget_set_has_window(GTK_WIDGET(bgbox), TRUE);

    priv = gtk_bgbox_get_instance_private(bgbox);
    priv->bg_type = BG_NONE;
    priv->sid = 0;
    return;
}

GtkWidget*
gtk_bgbox_new (void)
{
    return g_object_new (GTK_TYPE_BGBOX, NULL);
}

static void
gtk_bgbox_finalize (GObject *object)
{
    GtkBgboxPrivate *priv;

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(object));
    if (priv->pixmap) {
        cairo_surface_destroy(priv->pixmap);
        priv->pixmap = NULL;
    }
    if (priv->sid) {
        g_signal_handler_disconnect(priv->bg, priv->sid);
        priv->sid = 0;
    }
    if (priv->bg) {
        g_object_unref(priv->bg);
        priv->bg = NULL;
    }
    return;
}

static void
gtk_bgbox_realize (GtkWidget *widget)
{
    GtkBgboxPrivate *priv;
    GdkWindowAttr attributes;
    GtkAllocation allocation;
    GdkWindow *window;
    gint attributes_mask;

    gtk_widget_add_events(widget,
        GDK_BUTTON_MOTION_MASK |
        GDK_BUTTON_PRESS_MASK  |
        GDK_BUTTON_RELEASE_MASK |
        GDK_ENTER_NOTIFY_MASK  |
        GDK_LEAVE_NOTIFY_MASK  |
        GDK_STRUCTURE_MASK);

    /* GTK3: gtk_widget_real_realize() asserts !has_window, so we cannot call
     * the parent class realize when has_window==TRUE.  Create the GDK child
     * window ourselves, following the GtkLayout / GtkDrawingArea pattern. */
    gtk_widget_set_realized(widget, TRUE);

    gtk_widget_get_allocation(widget, &allocation);
    attributes.window_type  = GDK_WINDOW_CHILD;
    attributes.x            = allocation.x;
    attributes.y            = allocation.y;
    attributes.width        = allocation.width;
    attributes.height       = allocation.height;
    attributes.wclass       = GDK_INPUT_OUTPUT;
    attributes.visual       = gtk_widget_get_visual(widget);
    attributes.event_mask   = gtk_widget_get_events(widget);
    attributes_mask         = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;

    window = gdk_window_new(gtk_widget_get_parent_window(widget),
                            &attributes, attributes_mask);
    gtk_widget_register_window(widget, window);
    gtk_widget_set_window(widget, window);

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    if (priv->bg_type == BG_NONE)
        gtk_bgbox_set_background(widget, BG_STYLE, 0, 0);
    return;
}


static void
gtk_bgbox_style_updated (GtkWidget *widget)
{
    GtkBgboxPrivate *priv;

    GTK_WIDGET_CLASS(parent_class)->style_updated(widget);
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    if (gtk_widget_get_realized(widget) && gtk_widget_get_has_window(widget)) {
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }
    return;
}

/* gtk discards configure_event for GTK_WINDOW_CHILD. too pitty */
static  gboolean
gtk_bgbox_configure_event (GtkWidget *widget, GdkEventConfigure *e)
{
    DBG("geom: size (%d, %d). pos (%d, %d)\n", e->width, e->height, e->x, e->y);
    return FALSE;
}

static void
gtk_bgbox_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    guint border = gtk_container_get_border_width(GTK_CONTAINER(widget));
    gint child_min = 0, child_nat = 0;

    if (child && gtk_widget_get_visible(child))
        gtk_widget_get_preferred_width(child, &child_min, &child_nat);

    *minimum = child_min + border * 2;
    *natural = child_nat + border * 2;
}

static void
gtk_bgbox_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    guint border = gtk_container_get_border_width(GTK_CONTAINER(widget));
    gint child_min = 0, child_nat = 0;

    if (child && gtk_widget_get_visible(child))
        gtk_widget_get_preferred_height(child, &child_min, &child_nat);

    *minimum = child_min + border * 2;
    *natural = child_nat + border * 2;
}

/* calls with same allocation are usually refer to exactly same background
 * and we just skip them for optimization reason.
 * so if you see artifacts or unupdated background - reallocate bg on every call
 */
static void
gtk_bgbox_size_allocate (GtkWidget *widget, GtkAllocation *wa)
{
    GtkAllocation ca, old_alloc;
    GtkBgboxPrivate *priv;
    int same_alloc;
    guint border;
    GtkWidget *child;

    gtk_widget_get_allocation(widget, &old_alloc);
    same_alloc = !memcmp(&old_alloc, wa, sizeof(*wa));
    DBG("same alloc = %d\n", same_alloc);
    DBG("x=%d y=%d w=%d h=%d\n", wa->x, wa->y, wa->width, wa->height);

    gtk_widget_set_allocation(widget, wa);
    border = gtk_container_get_border_width(GTK_CONTAINER(widget));
    ca.x = border;
    ca.y = border;
    ca.width  = MAX (wa->width  - border * 2, 0);
    ca.height = MAX (wa->height - border * 2, 0);

    if (gtk_widget_get_realized(widget) && gtk_widget_get_has_window(widget)
          && !same_alloc) {
        priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
        DBG("move resize pos=%d,%d geom=%dx%d\n", wa->x, wa->y, wa->width, wa->height);
        gdk_window_move_resize(gtk_widget_get_window(widget), wa->x, wa->y, wa->width, wa->height);
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }

    child = gtk_bin_get_child(GTK_BIN(widget));
    if (child)
        gtk_widget_size_allocate(child, &ca);
    return;
}


static gboolean
gtk_bgbox_draw(GtkWidget *widget, cairo_t *cr)
{
    GtkBgboxPrivate *priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    if (priv->pixmap) {
        cairo_set_source_surface(cr, priv->pixmap, 0, 0);
        cairo_paint(cr);
    }
    if (priv->alpha) {
        GdkRGBA rgba;
        rgba.red   = ((priv->tintcolor >> 16) & 0xff) / 255.0;
        rgba.green = ((priv->tintcolor >>  8) & 0xff) / 255.0;
        rgba.blue  = ((priv->tintcolor      ) & 0xff) / 255.0;
        rgba.alpha = 1.0;
        gdk_cairo_set_source_rgba(cr, &rgba);
        cairo_paint_with_alpha(cr, (double)priv->alpha / 255.0);
    }
    GTK_WIDGET_CLASS(parent_class)->draw(widget, cr);
    return FALSE;
}

static void
gtk_bgbox_bg_changed(FbBg *bg, GtkWidget *widget)
{
    GtkBgboxPrivate *priv;

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    if (gtk_widget_get_realized(widget) && gtk_widget_get_has_window(widget)) {
        gtk_bgbox_set_background(widget, priv->bg_type, priv->tintcolor, priv->alpha);
    }
    return;
}

void
gtk_bgbox_set_background(GtkWidget *widget, int bg_type, guint32 tintcolor, gint alpha)
{
    GtkBgboxPrivate *priv;

    if (!(GTK_IS_BGBOX (widget)))
        return;

    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    DBG("widget=%p bg_type old:%d new:%d\n", widget, priv->bg_type, bg_type);
    if (priv->pixmap) {
        cairo_surface_destroy(priv->pixmap);
        priv->pixmap = NULL;
    }
    priv->bg_type = bg_type;
    if (priv->bg_type == BG_STYLE) {
        /* Let GTK3 handle background via CSS/style context */
        if (priv->sid) {
            g_signal_handler_disconnect(priv->bg, priv->sid);
            priv->sid = 0;
        }
        if (priv->bg) {
            g_object_unref(priv->bg);
            priv->bg = NULL;
        }
    } else {
        if (!priv->bg)
            priv->bg = fb_bg_get_for_display();
        if (!priv->sid)
            priv->sid = g_signal_connect(G_OBJECT(priv->bg), "changed", G_CALLBACK(gtk_bgbox_bg_changed), widget);

        if (priv->bg_type == BG_ROOT) {
            priv->tintcolor = tintcolor;
            priv->alpha = alpha;
            gtk_bgbox_set_bg_root(widget, priv);
        } else if (priv->bg_type == BG_INHERIT) {
            gtk_bgbox_set_bg_inherit(widget, priv);
        }
    }
    gtk_widget_queue_draw(widget);

    DBG("queue draw all %p\n", widget);
    return;
}

static void
gtk_bgbox_set_bg_root(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    priv->pixmap = fb_bg_get_xroot_pix_for_win(priv->bg, widget);
    if (!priv->pixmap)
        DBG("no root pixmap was found\n");
    return;
}

static void
gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    return;
}
