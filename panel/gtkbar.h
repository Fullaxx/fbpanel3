/**
 * @file gtkbar.h
 * @brief GtkBar — fixed-size grid container for task buttons.
 *
 * GtkBar is a GtkBox subclass used exclusively by the taskbar plugin to
 * arrange task buttons in a constrained grid.  Unlike a plain GtkBox it
 * enforces per-child maximum dimensions (child_width × child_height) and
 * can wrap buttons into multiple rows (horizontal bar) or columns (vertical
 * bar) controlled by the `dimension` field.
 *
 * LAYOUT MODEL
 * ------------
 * Given N visible children and dimension D:
 *   - Horizontal bar: rows = min(D, N); cols = ceil(N / rows)
 *   - Vertical bar:   cols = min(D, N); rows = ceil(N / cols)
 *
 * Each child receives min(available/cols, child_width) × min(available/rows,
 * child_height) pixels.  Children are placed left-to-right, top-to-bottom.
 *
 * When N == 0 the widget requests 2 × 2 pixels (GTK minimum).  This can
 * cause "Negative content height" warnings if the containing GtkWindow is
 * allowed to shrink; see docs/GTK_WIDGET_LIFECYCLE.md §3.
 *
 * CRITICAL: size_allocate PARENT-CLASS CALL
 * ------------------------------------------
 * GtkBar::size_allocate MUST call
 *     GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation)
 * before doing any custom work.  Without this the GTK3 CSS gadget node is
 * not updated, producing:
 *     Gtk-WARNING: Drawing a gadget with negative dimensions.
 * This was fixed in v8.3.7.  See docs/GTK_WIDGET_LIFECYCLE.md §3.
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

#ifndef __GTK_BAR_H__
#define __GTK_BAR_H__


#include <gtk/gtk.h>


#ifdef __cplusplus
//extern "C" {
#endif /* __cplusplus */


/** GObject type macro for GtkBar. */
#define GTK_TYPE_BAR            (gtk_bar_get_type ())
/** Cast @obj to GtkBar*, checking the type. */
#define GTK_BAR(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_BAR, GtkBar))
/** Cast @klass to GtkBarClass*, checking the type. */
#define GTK_BAR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_BAR, GtkBarClass))
/** Return TRUE if @obj is a GtkBar instance. */
#define GTK_IS_BAR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_BAR))
/** Return TRUE if @klass is the GtkBar class. */
#define GTK_IS_BAR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_BAR))
/** Return the GtkBarClass for instance @obj. */
#define GTK_BAR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_BAR, GtkBarClass))


typedef struct _GtkBar       GtkBar;
typedef struct _GtkBarClass  GtkBarClass;

/**
 * GtkBar - instance struct for the task-button grid container.
 *
 * @box:          GtkBox base class; must be first (C-style inheritance).
 * @child_height: Maximum height allocated to each child (pixels).
 *                Set at construction; used in both size computation and
 *                allocation.  Always >= 1.
 * @child_width:  Maximum width allocated to each child (pixels).
 *                Set at construction; always >= 1.
 * @dimension:    Maximum number of rows (horizontal bar) or columns
 *                (vertical bar) the layout uses simultaneously.
 *                Controls how children wrap.  Always >= 1.
 *                Updated via gtk_bar_set_dimension(); triggers a queue_resize.
 * @orient:       GTK_ORIENTATION_HORIZONTAL or GTK_ORIENTATION_VERTICAL.
 *                Set at construction; determines the row/column layout axis.
 */
struct _GtkBar
{
    GtkBox box;
    gint child_height, child_width;
    gint dimension;
    GtkOrientation orient;
};

/**
 * GtkBarClass - class struct for GtkBar.
 * @parent_class: GtkBoxClass; must be first.
 *
 * No additional virtual methods beyond the overridden GtkWidget vfuncs
 * (get_preferred_width, get_preferred_height, size_allocate).
 */
struct _GtkBarClass
{
    GtkBoxClass parent_class;
};


/**
 * gtk_bar_get_type - return the GType for GtkBar, registering it on first call.
 */
GType	   gtk_bar_get_type (void) G_GNUC_CONST;

/**
 * gtk_bar_new - create a new GtkBar widget.
 * @orient:       GTK_ORIENTATION_HORIZONTAL or GTK_ORIENTATION_VERTICAL.
 * @spacing:      Pixel spacing between children (passed to gtk_box_set_spacing).
 * @child_height: Maximum height per child cell (pixels); clamped to >= 1.
 * @child_width:  Maximum width per child cell (pixels); clamped to >= 1.
 *
 * Returns: (transfer full) new GtkBar as a GtkWidget*.
 *   The caller (or container) is responsible for eventual destruction.
 */
GtkWidget* gtk_bar_new(GtkOrientation orient,
    gint spacing, gint child_height, gint child_width);

/**
 * gtk_bar_set_dimension - set the maximum row/column count and queue a resize.
 * @bar:       GtkBar instance.
 * @dimension: New maximum row count (horizontal) or column count (vertical).
 *             Clamped to >= 1.  If unchanged, no queue_resize is issued.
 *
 * The taskbar plugin calls this to control how many rows of task buttons
 * are shown simultaneously.
 */
void gtk_bar_set_dimension(GtkBar *bar, gint dimension);

/**
 * gtk_bar_get_dimension - return the current dimension value.
 * @bar: GtkBar instance.
 *
 * Returns: the current bar->dimension (always >= 1).
 */
gint gtk_bar_get_dimension(GtkBar *bar);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* __GTK_BAR_H__ */
