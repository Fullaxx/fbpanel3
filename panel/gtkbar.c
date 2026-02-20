/**
 * @file gtkbar.c
 * @brief GtkBar — fixed-size grid container for task buttons (implementation).
 *
 * GtkBar subclasses GtkBox and overrides three vfuncs:
 *   - get_preferred_width / get_preferred_height: delegate to gtk_bar_compute_size
 *   - size_allocate: MUST call parent first (CSS gadget), then places children
 *     in a rows × cols grid respecting child_width and child_height limits.
 *
 * LAYOUT ALGORITHM
 * ----------------
 * Given N visible children and bar->dimension D:
 *   Horizontal: rows = min(D,N); cols = ceil(N/rows)
 *   Vertical:   cols = min(D,N); rows = ceil(N/cols)
 *
 * Each child gets:
 *   width  = min(available_width  / cols, bar->child_width)
 *   height = min(available_height / rows, bar->child_height)
 * clamped to >= 1 in each dimension.
 *
 * Children are arranged left-to-right, then wrapping to the next row
 * (horizontal) or next column (vertical).
 *
 * EMPTY BAR
 * ---------
 * When N == 0, gtk_bar_compute_size returns 2 × 2 (GTK minimum).  This
 * means GtkBar reports a 2 px minimum to its parent.  If the containing
 * GtkWindow is allowed to shrink, it will, causing the panel to collapse
 * and producing "Negative content height" warnings.  The fix (v8.3.24) is
 * to call gtk_widget_set_size_request(topgwin, -1, p->ah) so the window
 * has a hard minimum height floor.
 *
 * PARENT-CLASS size_allocate REQUIREMENT (v8.3.7)
 * ------------------------------------------------
 * GTK3 uses an internal "CSS gadget" node for each widget.  The gadget's
 * geometry is updated only when the parent class size_allocate is called.
 * GtkBar::size_allocate therefore calls
 *     GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation)
 * as its first action.  Omitting this call causes:
 *     Gtk-WARNING: Drawing a gadget with negative dimensions.
 * See docs/GTK_WIDGET_LIFECYCLE.md §3 and §5.
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

#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"

/** Unused upper guard on child size — kept for reference, not enforced. */
#define MAX_CHILD_SIZE 150

static void gtk_bar_class_init    (GtkBarClass   *klass);
static void gtk_bar_get_preferred_width  (GtkWidget *widget, gint *minimum, gint *natural);
static void gtk_bar_get_preferred_height (GtkWidget *widget, gint *minimum, gint *natural);
static void gtk_bar_size_allocate (GtkWidget *widget, GtkAllocation  *allocation);
//static gint gtk_bar_expose        (GtkWidget *widget, GdkEventExpose *event);

/** Needed for the rows/cols ceiling calculation; declared here to satisfy -Wimplicit. */
float ceilf(float x);

/** Cached parent class pointer; set in gtk_bar_class_init. */
static GtkBoxClass *parent_class = NULL;

/**
 * gtk_bar_get_type - return the GType for GtkBar, registering it on first call.
 *
 * Registers GtkBar as a static subtype of GTK_TYPE_BOX.
 */
GType
gtk_bar_get_type (void)
{
    static GType bar_type = 0;

    if (!bar_type)
    {
        static const GTypeInfo bar_info =
            {
                sizeof (GtkBarClass),
                NULL,		/* base_init */
                NULL,		/* base_finalize */
                (GClassInitFunc) gtk_bar_class_init,
                NULL,		/* class_finalize */
                NULL,		/* class_data */
                sizeof (GtkBar),
                0,		/* n_preallocs */
                NULL
            };

        bar_type = g_type_register_static (GTK_TYPE_BOX, "GtkBar",
              &bar_info, 0);
    }

    return bar_type;
}

/**
 * gtk_bar_class_init - initialise GtkBarClass.
 * @class: Class struct to fill.
 *
 * Caches the parent class pointer and overrides the three GtkWidget vfuncs
 * that implement GtkBar's grid layout.
 */
static void
gtk_bar_class_init (GtkBarClass *class)
{
    GtkWidgetClass *widget_class;

    parent_class = g_type_class_peek_parent (class);
    widget_class = (GtkWidgetClass*) class;

    widget_class->get_preferred_width  = gtk_bar_get_preferred_width;
    widget_class->get_preferred_height = gtk_bar_get_preferred_height;
    widget_class->size_allocate = gtk_bar_size_allocate;
    //widget_class->expose_event = gtk_bar_expose;
}


/**
 * gtk_bar_new - create a new GtkBar widget.
 * @orient:       GTK_ORIENTATION_HORIZONTAL or GTK_ORIENTATION_VERTICAL.
 * @spacing:      Pixel gap between children.
 * @child_height: Maximum height per child cell; clamped to >= 1.
 * @child_width:  Maximum width per child cell; clamped to >= 1.
 *
 * Initialises dimension to 1 (single row/column); use gtk_bar_set_dimension()
 * to allow more rows/columns.
 *
 * Returns: (transfer full) new GtkBar widget.
 */
GtkWidget*
gtk_bar_new(GtkOrientation orient, gint spacing,
    gint child_height, gint child_width)
{
    GtkBar *bar;

    bar = g_object_new (GTK_TYPE_BAR, NULL);
    gtk_box_set_spacing(GTK_BOX(bar), spacing);
    bar->orient = orient;
    bar->child_width = MAX(1, child_width);
    bar->child_height = MAX(1, child_height);
    bar->dimension = 1;
    return (GtkWidget *)bar;
}

/**
 * gtk_bar_set_dimension - update the maximum row/column count.
 * @bar:       GtkBar instance.
 * @dimension: New value; clamped to >= 1.
 *
 * Triggers gtk_widget_queue_resize() only when the value actually changes,
 * to avoid unnecessary relayout cycles.
 */
void
gtk_bar_set_dimension(GtkBar *bar, gint dimension)
{
    dimension = MAX(1, dimension);
    if (bar->dimension != dimension) {
        bar->dimension = MAX(1, dimension);
        gtk_widget_queue_resize(GTK_WIDGET(bar));
    }
}

/**
 * gtk_bar_get_dimension - return the current row/column dimension.
 * @bar: GtkBar instance.
 *
 * Returns: bar->dimension (always >= 1).
 */
gint gtk_bar_get_dimension(GtkBar *bar)
{
    return bar->dimension;
}

/**
 * gtk_bar_compute_size - compute the preferred requisition for the grid layout.
 * @widget:      GtkBar widget.
 * @requisition: Output; set to the preferred width × height.
 *
 * Counts visible children, then computes the rows × cols grid using the
 * dimension limit.  Each cell contributes child_width × child_height pixels
 * plus one-pixel separators between cells.
 *
 * Edge case: when there are no visible children, returns 2 × 2 (GTK minimum).
 * This is the trigger for the v8.3.24 "Negative content height" fix — the
 * 2 px minimum propagates up to GtkWindow which may shrink below the desired
 * panel height unless gtk_widget_set_size_request() provides a floor.
 *
 * Note: calls gtk_widget_get_preferred_size() on each visible child as a
 * side effect; this is intentional — label layout depends on it to
 * correctly compute its natural size before allocation.
 */
static void
gtk_bar_compute_size(GtkWidget *widget, GtkRequisition *requisition)
{
    GtkBar *bar = GTK_BAR(widget);
    GList *children, *l;
    gint nvis_children, rows, cols, dim;

    nvis_children = 0;
    children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        if (gtk_widget_get_visible(child)) {
            GtkRequisition child_req;
            /* Do not remove child request — label layout depends on it. */
            gtk_widget_get_preferred_size(child, &child_req, NULL);
            nvis_children++;
        }
    }
    g_list_free(children);

    DBG("nvis_children=%d\n", nvis_children);
    if (!nvis_children) {
        /* No visible children: report GTK minimum (2×2).
         * The containing GtkWindow must have a size_request floor set to
         * prevent it from shrinking — see v8.3.24 fix in panel.c. */
        requisition->width = 2;
        requisition->height = 2;
        return;
    }
    dim = MIN(bar->dimension, nvis_children);
    if (bar->orient == GTK_ORIENTATION_HORIZONTAL) {
        rows = dim;
        cols = (gint) ceilf((float) nvis_children / rows);
    } else {
        cols = dim;
        rows = (gint) ceilf((float) nvis_children / cols);
    }

    /* Each cell is child_width × child_height; single-pixel gap between cells */
    requisition->width  = bar->child_width  * cols + (cols - 1);
    requisition->height = bar->child_height * rows + (rows - 1);
    DBG("width=%d, height=%d\n", requisition->width, requisition->height);
}

/**
 * gtk_bar_get_preferred_width - GtkWidget::get_preferred_width override.
 * @widget:  GtkBar widget.
 * @minimum: Set to the computed preferred width.
 * @natural: Set to the same value (no separate natural width).
 */
static void
gtk_bar_get_preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{
    GtkRequisition req;
    gtk_bar_compute_size(widget, &req);
    *minimum = *natural = req.width;
}

/**
 * gtk_bar_get_preferred_height - GtkWidget::get_preferred_height override.
 * @widget:  GtkBar widget.
 * @minimum: Set to the computed preferred height.
 * @natural: Set to the same value.
 */
static void
gtk_bar_get_preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{
    GtkRequisition req;
    gtk_bar_compute_size(widget, &req);
    *minimum = *natural = req.height;
}

/**
 * gtk_bar_size_allocate - GtkWidget::size_allocate override.
 * @widget:     GtkBar widget.
 * @allocation: Rectangle assigned by the parent container.
 *
 * MUST call GTK_WIDGET_CLASS(parent_class)->size_allocate first to update
 * the GTK3 CSS gadget node.  Omitting this call causes the gadget to have
 * stale (possibly negative) dimensions, producing:
 *     Gtk-WARNING: Drawing a gadget with negative dimensions.
 * (Fixed in v8.3.7.)
 *
 * After the parent call, divides @allocation among visible children in a
 * rows × cols grid.  Child cells are clamped to [1, child_width] × [1,
 * child_height].  Children are placed left-to-right, wrapping at @cols.
 *
 * Note: must NOT call gtk_widget_queue_draw() from within size_allocate;
 * GTK3 queues a redraw automatically when the allocation changes.
 */
static void
gtk_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GtkBar *bar;
    GList *children, *l;
    GtkAllocation child_allocation;
    gint nvis_children, tmp, rows, cols, dim;

    DBG("a.w=%d  a.h=%d\n", allocation->width, allocation->height);

    /* Call parent first: allocates the CSS gadget node and sets the widget
     * allocation. Skipping this caused "Drawing a gadget with negative
     * dimensions (node box owner GtkBar)" + double-free on resize in GTK3. */
    GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation);

    bar = GTK_BAR(widget);
    nvis_children = 0;
    children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (l = children; l; l = l->next) {
        if (gtk_widget_get_visible(GTK_WIDGET(l->data)))
            nvis_children++;
    }
    /* No gtk_widget_queue_draw — GTK3 queues it automatically after
     * allocation changes; calling it here from size_allocate is illegal. */
    dim = MIN(bar->dimension, nvis_children);
    if (nvis_children == 0) {
        g_list_free(children);
        return;
    }
    if (bar->orient == GTK_ORIENTATION_HORIZONTAL) {
        rows = dim;
        cols = (gint) ceilf((float) nvis_children / rows);
    } else {
        cols = dim;
        rows = (gint) ceilf((float) nvis_children / cols);
    }
    DBG("rows=%d cols=%d\n", rows, cols);
    tmp = allocation->width - (cols - 1);
    child_allocation.width = MIN(tmp / cols, bar->child_width);
    tmp = allocation->height - (rows - 1);
    child_allocation.height = MIN(tmp / rows, bar->child_height);

    /* Clamp to at least 1 pixel in each dimension */
    if (child_allocation.width < 1)
        child_allocation.width = 1;
    if (child_allocation.height < 1)
        child_allocation.height = 1;
    DBG("child alloc: width=%d height=%d\n",
        child_allocation.width,
        child_allocation.height);

    child_allocation.x = allocation->x;
    child_allocation.y = allocation->y;
    tmp = 0;
    for (l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        if (gtk_widget_get_visible(child)) {
            DBG("allocate x=%d y=%d\n", child_allocation.x, child_allocation.y);
            gtk_widget_size_allocate(child, &child_allocation);
            tmp++;
            if (tmp == cols) {
                /* End of row: move to next row */
                child_allocation.x = allocation->x;
                child_allocation.y += child_allocation.height;
                tmp = 0;
            } else {
                /* Advance to next column */
                child_allocation.x += child_allocation.width;
            }
        }
    }
    g_list_free(children);
    return;
}
