/**
 * @file gtkbgbox.h
 * @brief GtkBgbox — GtkBin subclass with custom background painting.
 *
 * GtkBgbox is the panel's primary container widget.  Every plugin's `pwid`
 * is a GtkBgbox, and the panel's own `bbox` (which fills the top-level
 * GtkWindow) is also a GtkBgbox.
 *
 * BACKGROUND MODES
 * ----------------
 * Set via gtk_bgbox_set_background():
 *
 *   BG_NONE    — transient initial state; no background has been set yet.
 *   BG_STYLE   — GTK3 CSS/style context draws the background (default after
 *                realize).  The FbBg singleton is not referenced.
 *   BG_ROOT    — A slice of the X11 root pixmap (wallpaper) is captured via
 *                FbBg and painted by the draw handler, followed by an
 *                optional colour tint overlay (tintcolor + alpha).
 *   BG_INHERIT — Intended to inherit the parent's background; currently a
 *                stub (no-op). See BUG-004 in docs/BUGS_AND_ISSUES.md.
 *
 * SURFACE OWNERSHIP
 * -----------------
 * GtkBgboxPrivate::pixmap is a cairo_image_surface_t owned by GtkBgbox.
 * It is:
 *   - Created by fb_bg_get_xroot_pix_for_win() on each background refresh.
 *   - Destroyed in gtk_bgbox_set_background() before reassignment.
 *   - Destroyed in gtk_bgbox_finalize().
 *
 * WIDGET WINDOW (has_window = TRUE)
 * ----------------------------------
 * GtkBgbox sets has_window = TRUE in gtk_bgbox_init().  This means it owns
 * its own GDK child window and must create it manually in realize() —
 * GTK3's default realize asserts !has_window.  See docs/GTK_WIDGET_LIFECYCLE.md §2.
 *
 * SIZE ALLOCATION — parent class NOT called
 * ------------------------------------------
 * gtk_bgbox_size_allocate does NOT call parent_class->size_allocate.
 * GtkBin::size_allocate allocates to the child, but GtkBgbox handles
 * child allocation itself (offset by border_width).  Calling the parent
 * would double-allocate the child — harmlessly, but wastefully.
 * This is the inverse of the GtkBar situation: here it is intentional.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
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

#ifndef __GTK_BGBOX_H__
#define __GTK_BGBOX_H__


#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include "bg.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/** GObject type macro for GtkBgbox. */
#define GTK_TYPE_BGBOX              (gtk_bgbox_get_type ())
/** Cast @obj to GtkBgbox*, checking the type. */
#define GTK_BGBOX(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_BGBOX, GtkBgbox))
/** Cast @klass to GtkBgboxClass*, checking the type. */
#define GTK_BGBOX_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_BGBOX, GtkBgboxClass))
/** Return TRUE if @obj is a GtkBgbox instance. */
#define GTK_IS_BGBOX(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_BGBOX))
/** Return TRUE if @klass is the GtkBgbox class. */
#define GTK_IS_BGBOX_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_BGBOX))
/** Return the GtkBgboxClass for instance @obj. */
#define GTK_BGBOX_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_BGBOX, GtkBgboxClass))

typedef struct _GtkBgbox	  GtkBgbox;
typedef struct _GtkBgboxClass  GtkBgboxClass;

/**
 * GtkBgbox - instance struct for the background-painting bin container.
 *
 * @bin: GtkBin base class; must be first (C-style inheritance).
 *
 * All background state is stored in GtkBgboxPrivate (accessed via
 * gtk_bgbox_get_instance_private()).  The public struct intentionally
 * exposes no additional fields.
 */
struct _GtkBgbox
{
    GtkBin bin;
};

/**
 * GtkBgboxClass - class struct for GtkBgbox.
 * @parent_class: GtkBinClass; must be first.
 *
 * No additional virtual methods.
 */
struct _GtkBgboxClass
{
    GtkBinClass parent_class;
};

/**
 * Background mode constants for gtk_bgbox_set_background().
 *
 * BG_NONE    — No background set; transient state during construction.
 * BG_STYLE   — GTK3 CSS style context renders the background.
 *              FbBg is not used; priv->bg is NULL.
 * BG_ROOT    — Root pixmap slice + optional tint via cairo.
 *              Requires a wallpaper setter to have set _XROOTPMAP_ID.
 *              Falls back to CSS rendering if no root pixmap is available.
 * BG_INHERIT — Intended to inherit parent background; currently a stub.
 *              See BUG-004 in docs/BUGS_AND_ISSUES.md.
 * BG_LAST    — Sentinel; must be last.
 */
enum { BG_NONE, BG_STYLE, BG_ROOT, BG_INHERIT, BG_LAST };

/**
 * gtk_bgbox_get_type - return the GType for GtkBgbox, registering on first call.
 */
GType	   gtk_bgbox_get_type (void) G_GNUC_CONST;

/**
 * gtk_bgbox_new - allocate a new GtkBgbox widget.
 *
 * The widget starts with has_window = TRUE and bg_type = BG_NONE.
 * After realize(), bg_type is set to BG_STYLE automatically.
 *
 * Returns: (transfer full) new GtkBgbox as a GtkWidget*.
 */
GtkWidget* gtk_bgbox_new (void);

/**
 * gtk_bgbox_set_background - set or change the background mode.
 * @widget:    A GtkBgbox instance (checked via GTK_IS_BGBOX; no-op if not).
 * @bg_type:   New background mode (BG_NONE / BG_STYLE / BG_ROOT / BG_INHERIT).
 * @tintcolor: Tint colour as 0xRRGGBB (used only for BG_ROOT).
 * @alpha:     Tint opacity 0–255 (0 = no tint; used only for BG_ROOT).
 *
 * Destroys priv->pixmap before switching mode.  For BG_STYLE, disconnects
 * and unrefs the FbBg singleton.  For BG_ROOT / BG_INHERIT, acquires the
 * FbBg singleton (fb_bg_get_for_display()) if not already held, and
 * connects the "changed" signal to refresh the background slice when the
 * wallpaper changes.
 *
 * Queues a redraw after updating.  Safe to call from any widget state
 * (unrealized, realized, visible).
 */
extern void gtk_bgbox_set_background (GtkWidget *widget, int bg_type, guint32 tintcolor, gint alpha);


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __GTK_BGBOX_H__ */
