/**
 * @file bg.c
 * @brief FbBg — root-pixmap background monitor and cache (implementation).
 *
 * FbBg maintains a CPU-side copy of the X11 root pixmap so that multiple
 * GtkBgbox widgets can obtain background slices without round-tripping to
 * the X server on every draw.
 *
 * INTERNAL CACHE DESIGN
 * ----------------------
 * bg->cache          — cairo_image_surface_t holding a full CPU copy of the
 *                      current root pixmap.  NULL when invalid.
 * bg->cache_pixmap   — the Pixmap XID that was used to fill the cache.
 *                      Cache is valid only when bg->pixmap == bg->cache_pixmap.
 *
 * On a wallpaper change (fb_bg_changed):
 *   1. bg->cache is destroyed (set to NULL).
 *   2. bg->pixmap is re-read from _XROOTPMAP_ID.
 *   3. Next call to fb_bg_ensure_cache() refills bg->cache from the new pixmap.
 *
 * SURFACE LIFECYCLE
 * -----------------
 * bg->cache          — owned by FbBg; destroyed in fb_bg_changed() and
 *                      fb_bg_finalize().
 * slices returned by fb_bg_get_xroot_pix_for_win/area — (transfer full) to
 * caller; caller must cairo_surface_destroy() them.
 *
 * See also: docs/MEMORY_MODEL.md §3.
 */

/*
 * fb-background-monitor.c:
 *
 * Copyright (C) 2001, 2002 Ian McKellar <yakk@yakk.net>
 *                     2002 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *      Ian McKellar <yakk@yakk.net>
 *      Mark McLoughlin <mark@skynet.ie>
 */

#include <glib.h>
#include <glib-object.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cairo/cairo-xlib.h>

#include "bg.h"
#include "panel.h"
#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"


/** GObject signal indices for FbBg. */
enum {
    CHANGED,      /**< Emitted when the root pixmap changes. */
    LAST_SIGNAL
};


/**
 * _FbBgClass - GObject class struct for FbBg.
 * @parent_class: GObjectClass; must be first.
 * @changed:      Default handler for the "changed" signal.
 */
struct _FbBgClass {
    GObjectClass   parent_class;
    void         (*changed) (FbBg *monitor);
};

/**
 * _FbBg - GObject instance struct for the background monitor.
 * @parent_instance: GObject base; must be first.
 * @xroot:           X11 root window XID (DefaultRootWindow).
 * @id:              Atom for "_XROOTPMAP_ID" used to query the wallpaper.
 * @gc:              X11 GC for tile-fill operations; always valid after init.
 * @dpy:             X11 Display pointer; valid for the lifetime of FbBg.
 * @pixmap:          Current root Pixmap XID from _XROOTPMAP_ID; None if unset.
 * @cache:           CPU-side cairo_image_surface copy of the root pixmap.
 *                   NULL when invalid.  Owned by FbBg.
 * @cache_pixmap:    bg->pixmap value when @cache was filled; used to detect
 *                   pixmap replacement without a signal (cache invalidation).
 */
struct _FbBg {
    GObject    parent_instance;

    Window   xroot;
    Atom     id;
    GC       gc;
    Display *dpy;
    Pixmap   pixmap;

    /* Root pixmap cache — avoids repeated X11 round-trips per plugin widget */
    cairo_surface_t *cache;        /* full root pixmap as CPU image; NULL = invalid */
    Pixmap           cache_pixmap; /* bg->pixmap value when cache was filled */
};

static void fb_bg_class_init (FbBgClass *klass);
static void fb_bg_init (FbBg *monitor);
static void fb_bg_finalize (GObject *object);
static void fb_bg_changed(FbBg *monitor);
static Pixmap fb_bg_get_xrootpmap_real(FbBg *bg);
static gboolean fb_bg_ensure_cache(FbBg *bg);

static guint signals [LAST_SIGNAL] = { 0 };

/** The process-wide FbBg singleton; NULL after finalize. */
static FbBg *default_bg = NULL;

/**
 * fb_bg_get_type - return the GType for FbBg, registering it on first call.
 */
GType
fb_bg_get_type (void)
{
    static GType object_type = 0;

    if (!object_type) {
        static const GTypeInfo object_info = {
            sizeof (FbBgClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) fb_bg_class_init,
            NULL,           /* class_finalize */
            NULL,           /* class_data */
            sizeof (FbBg),
            0,              /* n_preallocs */
            (GInstanceInitFunc) fb_bg_init,
        };

        object_type = g_type_register_static (
            G_TYPE_OBJECT, "FbBg", &object_info, 0);
    }

    return object_type;
}


/**
 * fb_bg_class_init - initialise the FbBgClass.
 * @klass: Class struct to fill.
 *
 * Registers the "changed" GObject signal and sets fb_bg_changed as the
 * default handler.  Installs fb_bg_finalize as the GObject finalize hook.
 */
static void
fb_bg_class_init (FbBgClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    signals [CHANGED] =
        g_signal_new ("changed",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbBgClass, changed),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    klass->changed = fb_bg_changed;
    object_class->finalize = fb_bg_finalize;
    return;
}

/**
 * fb_bg_init - instance initialiser for FbBg.
 * @bg: Newly allocated FbBg instance.
 *
 * Connects to the default X11 display, interns _XROOTPMAP_ID, reads the
 * current root pixmap, and creates an X11 GC for tile-fill operations.
 *
 * If _XROOTPMAP_ID is not set (no wallpaper setter running), logs a
 * human-readable message — transparent backgrounds will not work.
 */
static void
fb_bg_init (FbBg *bg)
{
    XGCValues  gcv;
    uint mask;

    bg->dpy = GDK_DPY;
    bg->xroot = DefaultRootWindow(bg->dpy);
    bg->id = XInternAtom(bg->dpy, "_XROOTPMAP_ID", False);
    bg->pixmap = fb_bg_get_xrootpmap_real(bg);
    if (bg->pixmap == None)
        g_message("fbpanel: _XROOTPMAP_ID not set — no wallpaper pixmap found.\n"
                  "         Run 'xsetroot -solid <color>' or a wallpaper setter\n"
                  "         for the transparent background effect to work.");
    gcv.ts_x_origin = 0;
    gcv.ts_y_origin = 0;
    gcv.fill_style = FillTiled;
    mask = GCTileStipXOrigin | GCTileStipYOrigin | GCFillStyle;
    if (bg->pixmap != None) {
        gcv.tile = bg->pixmap;
        mask |= GCTile ;
    }
    bg->gc = XCreateGC (bg->dpy, bg->xroot, mask, &gcv) ;
    return;
}


/**
 * fb_bg_new - allocate a new FbBg instance.
 *
 * Returns: (transfer full) new FbBg; caller must g_object_unref().
 */
FbBg *
fb_bg_new()
{
    return g_object_new (FB_TYPE_BG, NULL);
}

/**
 * fb_bg_finalize - GObject finalize handler for FbBg.
 * @object: GObject being finalized (an FbBg instance).
 *
 * Destroys the cairo cache surface, frees the X11 GC, and clears
 * the global default_bg pointer so the singleton can be re-created.
 *
 * Note: sets default_bg = NULL unconditionally.  Since the GTK main
 * loop is single-threaded, no race between finalize and
 * fb_bg_get_for_display() is possible in normal operation.
 */
static void
fb_bg_finalize (GObject *object)
{
    FbBg *bg;

    bg = FB_BG (object);
    if (bg->cache) {
        cairo_surface_destroy(bg->cache);
        bg->cache = NULL;
    }
    XFreeGC(bg->dpy, bg->gc);
    default_bg = NULL;

    return;
}

/**
 * fb_bg_get_xrootpmap - return the cached root Pixmap XID.
 * @bg: FbBg instance.
 *
 * Returns: bg->pixmap (an X11 Pixmap XID), or None if unset.
 *   This is a server-side object; do not destroy it.
 */
Pixmap
fb_bg_get_xrootpmap(FbBg *bg)
{
    return bg->pixmap;
}

/**
 * fb_bg_get_xrootpmap_real - query _XROOTPMAP_ID from the X server.
 * @bg: FbBg instance with dpy, xroot, and id initialised.
 *
 * Calls XGetWindowProperty twice (retry loop with c=2) to handle the
 * case where the property changes between the size query and the data fetch.
 *
 * Returns: Pixmap XID read from the property, or None if the property
 *   is absent or not of type XA_PIXMAP.
 */
static Pixmap
fb_bg_get_xrootpmap_real(FbBg *bg)
{
    Pixmap ret = None;

    if (bg->id)
    {
        int  act_format, c = 2 ;
        u_long  nitems ;
        u_long  bytes_after ;
        u_char *prop = NULL;
        Atom ret_type;

        do
        {
            if (XGetWindowProperty(bg->dpy, bg->xroot, bg->id, 0, 1,
                    False, XA_PIXMAP, &ret_type, &act_format,
                    &nitems, &bytes_after, &prop) == Success)
            {
                if (ret_type == XA_PIXMAP)
                {
                    ret = *((Pixmap *)prop);
                    c = -c ; //to quit loop
                }
                XFree(prop);   /* prop is X11-heap; always XFree, never g_free */
            }
        } while (--c > 0);
    }
    return ret;

}


/**
 * fb_bg_ensure_cache - ensure bg->cache holds the current root pixmap image.
 * @bg: FbBg instance.
 *
 * Cache hit (bg->cache_pixmap == bg->pixmap): returns TRUE immediately.
 * Cache miss: destroys any stale cache, creates a temporary cairo_xlib_surface
 * wrapping the root Pixmap, blits it into a new cairo_image_surface (CPU-side),
 * then destroys the xlib surface.  Sets bg->cache_pixmap = bg->pixmap.
 *
 * The xlib surface is intentionally transient — it exists only during the
 * blit — so callers never need to deal with X drawable lifetimes.
 *
 * Returns: TRUE if bg->cache is valid and ready; FALSE if bg->pixmap is None
 *   or if any X or cairo operation fails.
 */
static gboolean
fb_bg_ensure_cache(FbBg *bg)
{
    cairo_surface_t *xlib_surf;
    guint rpw, rph, rpborder, rpdepth;
    Window dummy;
    int rpx, rpy;
    cairo_t *cr;

    if (bg->pixmap == None)
        return FALSE;

    /* Cache hit: same pixmap ID as when we last filled the cache */
    if (bg->cache && bg->cache_pixmap == bg->pixmap)
        return TRUE;

    /* Cache miss: free stale entry and refill from X server */
    if (bg->cache) {
        cairo_surface_destroy(bg->cache);
        bg->cache = NULL;
    }

    if (!XGetGeometry(bg->dpy, bg->pixmap, &dummy, &rpx, &rpy,
                      &rpw, &rph, &rpborder, &rpdepth)) {
        ERR("XGetGeometry on root pixmap failed\n");
        return FALSE;
    }

    xlib_surf = cairo_xlib_surface_create(
        bg->dpy, bg->pixmap,
        DefaultVisual(bg->dpy, DefaultScreen(bg->dpy)),
        (int)rpw, (int)rph);
    if (cairo_surface_status(xlib_surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(xlib_surf);
        return FALSE;
    }

    bg->cache = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)rpw, (int)rph);
    if (cairo_surface_status(bg->cache) != CAIRO_STATUS_SUCCESS) {
        ERR("cairo_image_surface_create for root pixmap cache failed\n");
        cairo_surface_destroy(xlib_surf);
        cairo_surface_destroy(bg->cache);
        bg->cache = NULL;
        return FALSE;
    }

    cr = cairo_create(bg->cache);
    cairo_set_source_surface(cr, xlib_surf, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(xlib_surf);  /* xlib surface no longer needed */

    bg->cache_pixmap = bg->pixmap;
    DBG("root pixmap cached: %ux%u\n", rpw, rph);
    return TRUE;
}

/**
 * fb_bg_get_xroot_pix_for_area - crop a slice from the cached root pixmap.
 * @bg:     FbBg instance.
 * @x:      Root-window X of the top-left corner.
 * @y:      Root-window Y of the top-left corner.
 * @width:  Slice width in pixels.
 * @height: Slice height in pixels.
 *
 * Returns: (transfer full) new cairo_image_surface_t (CAIRO_FORMAT_RGB24)
 *   containing the requested area, or NULL if no root pixmap is available.
 *   Caller must cairo_surface_destroy() the returned surface.
 */
cairo_surface_t *
fb_bg_get_xroot_pix_for_area(FbBg *bg, gint x, gint y, gint width, gint height)
{
    cairo_surface_t *gbgpix;
    cairo_t *cr;

    if (!fb_bg_ensure_cache(bg))
        return NULL;

    gbgpix = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(gbgpix) != CAIRO_STATUS_SUCCESS) {
        ERR("cairo_image_surface_create failed\n");
        cairo_surface_destroy(gbgpix);
        return NULL;
    }

    cr = cairo_create(gbgpix);
    cairo_set_source_surface(cr, bg->cache, -x, -y);
    cairo_paint(cr);
    cairo_destroy(cr);
    return gbgpix;  /* (transfer full) to caller */
}

/**
 * fb_bg_get_xroot_pix_for_win - crop a background slice matching @widget's area.
 * @bg:     FbBg instance.
 * @widget: Realized GtkWidget whose screen position determines the crop area.
 *
 * Translates the widget's GDK window position to root-window coordinates via
 * XTranslateCoordinates, then crops that rectangle from bg->cache.
 *
 * Returns: (transfer full) new cairo_image_surface_t (CAIRO_FORMAT_RGB24),
 *   or NULL if the root pixmap is unavailable, the widget has zero/one-pixel
 *   dimensions, or XGetGeometry on the widget window fails.
 *   Caller must cairo_surface_destroy() the returned surface.
 */
cairo_surface_t *
fb_bg_get_xroot_pix_for_win(FbBg *bg, GtkWidget *widget)
{
    Window win;
    Window dummy;
    cairo_surface_t *gbgpix;
    guint  width, height, border, depth;
    int  x, y;
    cairo_t *cr;

    if (!fb_bg_ensure_cache(bg))
        return NULL;

    win = GDK_WINDOW_XID(gtk_widget_get_window(widget));
    if (!XGetGeometry(bg->dpy, win, &dummy, &x, &y, &width, &height, &border,
              &depth)) {
        DBG2("XGetGeometry failed\n");
        return NULL;
    }
    if (width <= 1 || height <= 1)
        return NULL;

    XTranslateCoordinates(bg->dpy, win, bg->xroot, 0, 0, &x, &y, &dummy);
    DBG("win=%lx %dx%d%+d%+d\n", win, width, height, x, y);

    gbgpix = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)width, (int)height);
    if (cairo_surface_status(gbgpix) != CAIRO_STATUS_SUCCESS) {
        ERR("cairo_image_surface_create failed\n");
        cairo_surface_destroy(gbgpix);
        return NULL;
    }

    cr = cairo_create(gbgpix);
    cairo_set_source_surface(cr, bg->cache, -x, -y);
    cairo_paint(cr);
    cairo_destroy(cr);
    return gbgpix;  /* (transfer full) to caller */
}


/**
 * fb_bg_changed - default "changed" signal handler.
 * @bg: FbBg instance receiving the signal.
 *
 * Invalidates the CPU-side cache (destroys bg->cache, sets it to NULL)
 * and re-reads the root pixmap XID from _XROOTPMAP_ID.  Updates the X11 GC
 * tile to match the new pixmap.
 *
 * Connected GtkBgbox instances will call fb_bg_get_xroot_pix_for_win() on
 * their next resize or explicit background refresh, which triggers
 * fb_bg_ensure_cache() to refill bg->cache from the new pixmap.
 */
static void
fb_bg_changed(FbBg *bg)
{
    /* Invalidate the cached root pixmap — will be refilled on next request */
    if (bg->cache) {
        cairo_surface_destroy(bg->cache);
        bg->cache = NULL;
    }
    bg->cache_pixmap = None;

    bg->pixmap = fb_bg_get_xrootpmap_real(bg);
    if (bg->pixmap != None) {
        XGCValues  gcv;

        gcv.tile = bg->pixmap;
        XChangeGC(bg->dpy, bg->gc, GCTile, &gcv);
        DBG("changed\n");
    }
    return;
}


/**
 * fb_bg_notify_changed_bg - emit the "changed" signal on @bg.
 * @bg: FbBg instance.
 *
 * Call this from the X11 PropertyNotify handler when _XROOTPMAP_ID changes.
 * The default signal handler (fb_bg_changed) invalidates the cache and
 * re-reads the pixmap; all connected GtkBgbox instances then refresh.
 */
void fb_bg_notify_changed_bg(FbBg *bg)
{
    g_signal_emit (bg, signals [CHANGED], 0);
    return;
}

/**
 * fb_bg_get_for_display - obtain the FbBg singleton for the default display.
 *
 * First call: allocates a new FbBg (refcount 1) and stores it in default_bg.
 * Subsequent calls: increments refcount via g_object_ref() and returns the
 * same instance.
 *
 * Returns: (transfer full) the FbBg singleton.  Caller must g_object_unref()
 *   when done.  After the last unref, fb_bg_finalize() sets default_bg = NULL
 *   so the singleton can be re-created on the next call.
 */
FbBg *fb_bg_get_for_display(void)
{
    if (!default_bg)
        default_bg = fb_bg_new();
    else
        g_object_ref(default_bg);
    return default_bg;
}
