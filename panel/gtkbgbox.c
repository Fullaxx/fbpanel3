/**
 * @file gtkbgbox.c
 * @brief GtkBgbox — background-painting GtkBin subclass (implementation).
 *
 * GtkBgbox is the primary visible container in the panel.  It is used for:
 *   - panel->bbox: the panel background behind all plugins.
 *   - each plugin->pwid: the per-plugin container.
 *
 * PRIVATE STATE (GtkBgboxPrivate)
 * --------------------------------
 * @pixmap:    cairo_image_surface_t holding the current background image.
 *             NULL when not in use (BG_STYLE mode or no root pixmap).
 *             Owned by GtkBgbox; destroyed in gtk_bgbox_set_background()
 *             (before refresh) and in gtk_bgbox_finalize().
 * @tintcolor: 0xRRGGBB tint colour applied in BG_ROOT mode.
 * @alpha:     Tint opacity 0–255 applied over the root pixmap slice.
 * @bg_type:   Current background mode (BG_NONE/BG_STYLE/BG_ROOT/BG_INHERIT).
 * @bg:        FbBg singleton reference; non-NULL only in BG_ROOT/BG_INHERIT
 *             modes.  Obtained via fb_bg_get_for_display() (transfer full);
 *             released with g_object_unref() in finalize or on mode switch.
 * @sid:       GLib signal handler ID for the FbBg "changed" signal.
 *             Disconnected in finalize or on switch to BG_STYLE.
 *
 * REALIZE — manual GDK window creation
 * --------------------------------------
 * Because has_window = TRUE, GTK3's default realize would assert false.
 * gtk_bgbox_realize() creates the GDK child window manually, exactly as
 * GtkLayout and GtkDrawingArea do.  After creation it calls
 * gtk_bgbox_set_background() to establish the initial BG_STYLE mode.
 *
 * SIZE ALLOCATE — parent NOT called
 * -----------------------------------
 * gtk_bgbox_size_allocate does NOT call parent_class->size_allocate.
 * GtkBin::size_allocate would allocate to the child, but GtkBgbox handles
 * child allocation itself (inset by border_width).  The parent call would
 * double-allocate — harmlessly but wastefully.  This is intentional and
 * correct; the CSS gadget trap does not apply here because GtkBin does not
 * own a CSS gadget in the same way GtkBox does.
 *
 * DRAW — layered painting
 * -------------------------
 * gtk_bgbox_draw paints three layers in order:
 *   1. priv->pixmap (root pixmap slice) or CSS background fallback.
 *   2. Colour tint (priv->tintcolor + priv->alpha) via cairo_paint_with_alpha.
 *   3. GTK3 child rendering via GTK_WIDGET_CLASS(parent_class)->draw().
 *
 * See also: docs/GTK_WIDGET_LIFECYCLE.md §2, docs/MEMORY_MODEL.md §3.
 */

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

/**
 * GtkBgboxPrivate - private per-instance background state.
 *
 * @pixmap:    Cached background cairo_image_surface_t for this widget's area.
 *             (transfer full) owned by GtkBgbox.  NULL = not set / BG_STYLE.
 * @tintcolor: Tint colour 0xRRGGBB used in BG_ROOT mode.
 * @alpha:     Tint alpha 0–255; 0 means no tint overlay.
 * @bg_type:   Active background mode (BG_* enum from gtkbgbox.h).
 * @bg:        FbBg singleton; non-NULL only when bg_type is BG_ROOT or
 *             BG_INHERIT.  (transfer full) — released via g_object_unref().
 * @sid:       g_signal_connect() ID for FbBg "changed"; 0 when disconnected.
 */
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

/** Cached parent class pointer; set in gtk_bgbox_class_init. */
static GtkBinClass *parent_class = NULL;

/**
 * gtk_bgbox_class_init - initialise GtkBgboxClass.
 * @class: Class struct to fill.
 *
 * Overrides the GtkWidget and GObject vfuncs needed for custom background
 * painting, manual GDK window management, and private-state cleanup.
 */
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

/**
 * gtk_bgbox_init - instance initialiser for GtkBgbox.
 * @bgbox: Newly allocated GtkBgbox instance.
 *
 * Sets has_window = TRUE (required for custom GDK window creation in realize)
 * and initialises private state to BG_NONE with no signal connection.
 */
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

/**
 * gtk_bgbox_new - allocate a new GtkBgbox widget.
 *
 * Returns: (transfer full) new GtkBgbox as a GtkWidget*.
 */
GtkWidget*
gtk_bgbox_new (void)
{
    return g_object_new (GTK_TYPE_BGBOX, NULL);
}

/**
 * gtk_bgbox_finalize - GObject finalize handler.
 * @object: GObject being finalized (a GtkBgbox instance).
 *
 * Releases the cairo background surface, disconnects the FbBg "changed"
 * signal, and unrefs the FbBg singleton.  Called by GObject when the last
 * reference to the widget is dropped (after GTK destroy).
 *
 * Cleanup order matters:
 *   1. cairo_surface_destroy(pixmap)  — must be first; pixmap may hold an
 *      indirect reference to X resources.
 *   2. g_signal_handler_disconnect    — stop callbacks before unreffing bg.
 *   3. g_object_unref(bg)             — may trigger fb_bg_finalize() and
 *      set default_bg = NULL if this is the last holder.
 */
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

/**
 * gtk_bgbox_realize - GtkWidget::realize override.
 * @widget: GtkBgbox widget being realized.
 *
 * Creates the GDK child window manually because has_window = TRUE prevents
 * calling GTK3's default realize (which asserts !has_window).  Follows the
 * GtkLayout / GtkDrawingArea pattern:
 *   1. gtk_widget_set_realized(TRUE)
 *   2. gdk_window_new() with the widget's allocation and visual
 *   3. gtk_widget_register_window() + gtk_widget_set_window()
 *
 * After creation, applies an initial BG_STYLE background so the widget is
 * visible even before the caller sets a specific mode.
 *
 * Note: the event mask is set before window creation so the GDK window
 * inherits it automatically.
 */
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


/**
 * gtk_bgbox_style_updated - GtkWidget::style_updated override.
 * @widget: GtkBgbox widget.
 *
 * Calls the parent style_updated to propagate the CSS change, then
 * refreshes the background slice if the widget is realized and has a window.
 * This ensures the panel re-reads root-pixmap or CSS colours after a theme
 * change.
 */
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

/**
 * gtk_bgbox_configure_event - GtkWidget::configure_event override (no-op).
 * @widget: GtkBgbox widget.
 * @e:      Configure event (position/size change of the GDK window).
 *
 * GTK discards configure events for GDK_WINDOW_CHILD windows (only
 * top-level windows normally receive them).  This stub exists for debugging
 * purposes — the DBG() call logs the event geometry in debug builds.
 *
 * Returns: FALSE (event not consumed; propagate normally).
 */
static  gboolean
gtk_bgbox_configure_event (GtkWidget *widget, GdkEventConfigure *e)
{
    DBG("geom: size (%d, %d). pos (%d, %d)\n", e->width, e->height, e->x, e->y);
    return FALSE;
}

/**
 * gtk_bgbox_get_preferred_width - GtkWidget::get_preferred_width override.
 * @widget:  GtkBgbox widget.
 * @minimum: Set to child's minimum width + border*2.
 * @natural: Set to child's natural width + border*2.
 *
 * Queries the single GtkBin child (if visible) and adds the container's
 * border_width on both sides.
 */
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

/**
 * gtk_bgbox_get_preferred_height - GtkWidget::get_preferred_height override.
 * @widget:  GtkBgbox widget.
 * @minimum: Set to child's minimum height + border*2.
 * @natural: Set to child's natural height + border*2.
 */
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

/**
 * gtk_bgbox_size_allocate - GtkWidget::size_allocate override.
 * @widget: GtkBgbox widget.
 * @wa:     Allocation rectangle assigned by the parent container.
 *
 * Does NOT call parent_class->size_allocate (intentional — see file docblock).
 *
 * When the allocation changes and the widget is realized:
 *   1. Moves and resizes the GDK window to match.
 *   2. Refreshes the background slice (gtk_bgbox_set_background) so the
 *      wallpaper crop matches the new position.
 *
 * Allocates the single GtkBin child within the content area (wa inset by
 * border_width on each side, clamped to >= 0).
 *
 * Note: allocation comparison is done with memcmp to skip the expensive
 * background refresh when the allocation is unchanged (optimisation).
 * Artifacts from stale backgrounds would appear if the wallpaper changes
 * without an allocation change — the FbBg "changed" signal handles that
 * case via gtk_bgbox_bg_changed().
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


/**
 * gtk_bgbox_draw - GtkWidget::draw override.
 * @widget: GtkBgbox widget.
 * @cr:     Cairo context for the widget's GDK window.
 *
 * Paints three layers in order:
 *   1. Background image (priv->pixmap root-pixmap slice), or CSS fallback
 *      when priv->pixmap is NULL and bg_type == BG_ROOT (no wallpaper set).
 *   2. Colour tint (priv->tintcolor as RGB + priv->alpha as opacity) via
 *      cairo_paint_with_alpha — only when priv->alpha != 0.
 *   3. GTK3 child widget rendering via parent_class->draw().
 *
 * Returns: FALSE (event not consumed; allows further drawing).
 */
static gboolean
gtk_bgbox_draw(GtkWidget *widget, cairo_t *cr)
{
    GtkBgboxPrivate *priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    if (priv->pixmap) {
        cairo_set_source_surface(cr, priv->pixmap, 0, 0);
        cairo_paint(cr);
    } else if (priv->bg_type == BG_ROOT) {
        /* No root pixmap (wallpaper not set).  Fall back to the CSS background
         * so the panel is visible rather than transparent/black.  The caller
         * (panel.c) applies a dark fallback rule at PRIORITY_FALLBACK so a
         * desktop theme can still override it. */
        GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
        gtk_render_background(ctx, cr, 0, 0,
            gtk_widget_get_allocated_width(widget),
            gtk_widget_get_allocated_height(widget));
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

/**
 * gtk_bgbox_bg_changed - FbBg "changed" signal handler.
 * @bg:     FbBg singleton that emitted the signal.
 * @widget: GtkBgbox instance connected to this handler.
 *
 * Called when the root pixmap changes (wallpaper replaced).  Refreshes the
 * background slice so the new wallpaper appears in the panel.
 */
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

/**
 * gtk_bgbox_set_background - set or change the background mode and refresh.
 * @widget:    A GtkBgbox instance.  No-op if not GTK_IS_BGBOX().
 * @bg_type:   New background mode (BG_NONE/BG_STYLE/BG_ROOT/BG_INHERIT).
 * @tintcolor: Tint colour 0xRRGGBB (stored and used in BG_ROOT mode).
 * @alpha:     Tint alpha 0–255 (stored and used in BG_ROOT mode).
 *
 * Always destroys priv->pixmap before switching mode (prevents stale slices).
 *
 * BG_STYLE: disconnects and unrefs FbBg; GTK CSS takes over.
 *
 * BG_ROOT / BG_INHERIT:
 *   - Acquires FbBg singleton if not already held (fb_bg_get_for_display).
 *   - Connects the "changed" signal (sid) if not already connected.
 *   - For BG_ROOT: calls gtk_bgbox_set_bg_root() to fill priv->pixmap.
 *   - For BG_INHERIT: calls gtk_bgbox_set_bg_inherit() (currently a stub).
 *
 * Queues a full redraw (gtk_widget_queue_draw) at the end.
 */
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

/**
 * gtk_bgbox_set_bg_root - fill priv->pixmap with the root pixmap slice.
 * @widget: GtkBgbox widget (must be realized for XGetGeometry to succeed).
 * @priv:   Private state; priv->bg must be a valid FbBg instance.
 *
 * Delegates to fb_bg_get_xroot_pix_for_win() which translates the widget's
 * window position to root coordinates and crops the cached root pixmap image.
 * The returned surface is (transfer full) and stored in priv->pixmap.
 *
 * If no root pixmap is set, priv->pixmap remains NULL; gtk_bgbox_draw()
 * falls back to CSS rendering in that case.
 */
static void
gtk_bgbox_set_bg_root(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    priv->pixmap = fb_bg_get_xroot_pix_for_win(priv->bg, widget);
    if (!priv->pixmap)
        DBG("no root pixmap was found\n");
    return;
}

/**
 * gtk_bgbox_set_bg_inherit - stub for BG_INHERIT mode.
 * @widget: GtkBgbox widget.
 * @priv:   Private state (unused).
 *
 * Currently a no-op.  Intended to copy the parent widget's background
 * into priv->pixmap.  See BUG-004 in docs/BUGS_AND_ISSUES.md.
 */
static void
gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));

    return;
}
