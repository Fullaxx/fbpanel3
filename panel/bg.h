/**
 * @file bg.h
 * @brief FbBg — root-pixmap background monitor and cache.
 *
 * FbBg is a GObject singleton that tracks the X11 root pixmap
 * (set by wallpaper-setter programs via the _XROOTPMAP_ID property)
 * and provides CPU-side cairo_image_surface_t slices for any widget
 * that needs a "see-through" transparent background.
 *
 * SINGLETON PATTERN
 * -----------------
 * Use fb_bg_get_for_display() to obtain the singleton.  Each caller
 * receives a reference; call g_object_unref() when done.  The singleton
 * is re-created from scratch after the last unref.
 *
 * BACKGROUND CHANGE NOTIFICATION
 * --------------------------------
 * When the wallpaper changes, call fb_bg_notify_changed_bg().  This
 * emits the "changed" GObject signal, which connected GtkBgbox widgets
 * use to refresh their cached background slices.
 *
 * SURFACE OWNERSHIP
 * -----------------
 * fb_bg_get_xroot_pix_for_win()  → (transfer full) caller owns the surface;
 * fb_bg_get_xroot_pix_for_area() → (transfer full) caller owns the surface.
 * Both return cairo_image_surface_t (CPU-side, not tied to an X drawable).
 * The caller must call cairo_surface_destroy() when done.
 *
 * See also: docs/MEMORY_MODEL.md §3 (cairo surface lifecycle).
 */

/*
 * fb-background-monitor.h:
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
 *	Mark McLoughlin <mark@skynet.ie>
 */

#ifndef __FB_BG_H__
#define __FB_BG_H__

/* FIXME: this needs to be made multiscreen aware
 *        panel_bg_get should take
 *        a GdkScreen argument.
 */

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <X11/Xlib.h>

/** GObject type macro for FbBg. */
#define FB_TYPE_BG         (fb_bg_get_type ())

/** Cast @o to FbBg*, checking the type. */
#define FB_BG(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o),      \
				       FB_TYPE_BG,        \
				       FbBg))
/** Cast @k to FbBgClass*, checking the type. */
#define FB_BG_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k),         \
				       FB_TYPE_BG,        \
				       FbBgClass))
/** Return TRUE if @o is an FbBg instance. */
#define FB_IS_BG(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o),      \
				       FB_TYPE_BG))
/** Return TRUE if @k is the FbBg class. */
#define FB_IS_BG_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k),         \
				       FB_TYPE_BG))
/** Return the FbBgClass for instance @o. */
#define FB_BG_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o),       \
				       FB_TYPE_BG,        \
				       FbBgClass))

typedef struct _FbBgClass FbBgClass;
typedef struct _FbBg      FbBg;

/**
 * fb_bg_get_type - return the GType for FbBg.
 *
 * Registers the type on first call via g_type_register_static().
 */
GType             fb_bg_get_type             (void);

/**
 * fb_bg_new - allocate a new FbBg instance.
 *
 * Prefer fb_bg_get_for_display() for normal use.
 * This function is exposed mainly for the singleton implementation.
 *
 * Returns: (transfer full) new FbBg; caller must g_object_unref().
 */
FbBg             *fb_bg_new                  (void);

/**
 * fb_bg_get_xroot_pix_for_win - get a background slice matching @widget's area.
 * @bg:     FbBg instance (must not be NULL).
 * @widget: GTK widget whose screen position determines the crop rectangle.
 *          Must be realized (has a GDK window).
 *
 * Translates the widget's window coordinates to root-window coordinates,
 * then crops that rectangle from the cached root pixmap image.
 *
 * Returns: (transfer full) new cairo_image_surface_t (CAIRO_FORMAT_RGB24)
 *   containing the background slice, or NULL if:
 *   - no root pixmap is set (_XROOTPMAP_ID not present), or
 *   - XGetGeometry on the widget window fails, or
 *   - the widget has zero/one-pixel dimensions.
 *   Caller must cairo_surface_destroy() the returned surface.
 */
cairo_surface_t  *fb_bg_get_xroot_pix_for_win(FbBg *bg, GtkWidget *widget);

/**
 * fb_bg_get_xroot_pix_for_area - get a background slice for an arbitrary area.
 * @bg:     FbBg instance (must not be NULL).
 * @x:      Root-window X coordinate of the top-left corner.
 * @y:      Root-window Y coordinate of the top-left corner.
 * @width:  Width of the requested area in pixels.
 * @height: Height of the requested area in pixels.
 *
 * Returns: (transfer full) new cairo_image_surface_t (CAIRO_FORMAT_RGB24),
 *   or NULL if no root pixmap is set.
 *   Caller must cairo_surface_destroy() the returned surface.
 */
cairo_surface_t  *fb_bg_get_xroot_pix_for_area(FbBg *bg, gint x, gint y, gint width, gint height);

/**
 * fb_bg_get_xrootpmap - return the cached X11 root Pixmap ID.
 * @bg: FbBg instance.
 *
 * Returns: the Pixmap XID from _XROOTPMAP_ID, or None if unset.
 *   This is an X11 server-side object; do not cairo_surface_destroy() it.
 */
Pixmap            fb_bg_get_xrootpmap        (FbBg *bg);

/**
 * fb_bg_notify_changed_bg - emit the "changed" signal on @bg.
 * @bg: FbBg instance.
 *
 * Call this when the wallpaper changes (e.g., from the X11 PropertyNotify
 * handler for _XROOTPMAP_ID).  The signal handler invalidates the internal
 * cache and re-fetches the root pixmap ID; all connected GtkBgbox widgets
 * then refresh their background slices.
 */
void              fb_bg_notify_changed_bg    (FbBg *bg);

/**
 * fb_bg_get_for_display - obtain the FbBg singleton for the default display.
 *
 * First call: allocates a new FbBg and returns it with refcount == 1.
 * Subsequent calls: increments the refcount and returns the same instance.
 *
 * Returns: (transfer full) the singleton FbBg; caller must g_object_unref()
 *   when done.  The singleton is re-created from scratch after the last unref
 *   (fb_bg_finalize sets default_bg = NULL).
 */
FbBg             *fb_bg_get_for_display      (void);

#endif /* __FB_BG_H__ */
