/*
 * CPU usage plugin to fbpanel
 *
 * Copyright (C) 2004 by Alexandre Pereira da Silva <alexandre.pereira@poli.usp.br>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
/*A little bug fixed by Mykola <mykola@2ka.mipt.ru>:) */

/**
 * @file chart.c
 * @brief Base scrolling chart plugin for fbpanel.
 *
 * Provides the chart_class base used by the cpu, mem2, and net plugins.
 * Not intended to be loaded directly by the user; subplugins call
 * class_get("chart") and delegate to its constructor/destructor.
 *
 * The chart maintains a 2D ring-buffer of pixel heights (c->ticks[row][col])
 * for each data row (e.g., user+sys CPU, TX, RX).  On every add_tick() call
 * the ring advances by one column and the drawing area is invalidated.  The
 * "draw" signal handler renders coloured vertical lines from the bottom up,
 * then overlays a GTK theme frame.
 *
 * chart_class extends plugin_class with two virtual methods:
 *   add_tick(c, val[]) — append a new tick (val[i] in [0.0..1.0] per row).
 *   set_rows(c, num, colors[]) — set the number of data rows and their colours.
 *
 * Config keys: none (all configuration is done by the subplugin).
 *
 * Main widget: c->da = p->pwid (the GtkBgbox directly; minimum 40x25 pixels).
 * The "size-allocate" and "draw" signals on p->pwid drive tick-buffer resizing
 * and rendering respectively.
 *
 * Memory: c->ticks is a 2D array (c->rows x c->w gint values), allocated by
 * chart_alloc_ticks() and freed by chart_free_ticks().  c->gc_cpu is an array
 * of c->rows GdkRGBA values (transfer-full, freed by chart_free_gcs()).
 */


#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>

#include "plugin.h"
#include "panel.h"
#include "gtkbgbox.h"
#include "chart.h"


//#define DEBUGPRN
#include "dbg.h"


static void chart_add_tick(chart_priv *c, float *val);
static void chart_draw(chart_priv *c, cairo_t *cr);
static void chart_size_allocate(GtkWidget *widget, GtkAllocation *a, chart_priv *c);
static gboolean chart_draw_event(GtkWidget *widget, cairo_t *cr, chart_priv *c);

static void chart_alloc_ticks(chart_priv *c);
static void chart_free_ticks(chart_priv *c);
static void chart_alloc_gcs(chart_priv *c, gchar *colors[]);
static void chart_free_gcs(chart_priv *c);

/**
 * chart_add_tick - append one column of data values to the ring buffer.
 * @c:   chart_priv. (transfer none)
 * @val: array of c->rows floats in [0.0..1.0]; clamped to [0..1] internally.
 *
 * Values are scaled to pixel heights (val[i] * c->h) and written at position
 * c->pos, which then advances modulo c->w.  The drawing area is invalidated
 * via gtk_widget_queue_draw().
 */
static void
chart_add_tick(chart_priv *c, float *val)
{
    int i;

    if (!c->ticks)
        return;
    for (i = 0; i < c->rows; i++) {
        if (val[i] < 0)
            val[i] = 0;
        if (val[i] > 1)
            val[i] = 1;
        c->ticks[i][c->pos] = val[i] * c->h;
        DBG("new wval = %uld\n", c->ticks[i][c->pos]);
    }
    c->pos = (c->pos + 1) %  c->w;
    gtk_widget_queue_draw(c->da);

    return;
}

/**
 * chart_draw - render the tick ring buffer as vertical coloured lines.
 * @c:  chart_priv. (transfer none)
 * @cr: Cairo context for the drawing area. (transfer none)
 *
 * Draws columns from left to right; within each column stacks data rows
 * from the bottom up, each row rendered in its GdkRGBA colour.
 */
static void
chart_draw(chart_priv *c, cairo_t *cr)
{
    int j, i, y;

    if (!c->ticks || !c->gc_cpu)
        return;
    for (i = 1; i < c->w-1; i++) {
        y = c->h-2;
        for (j = 0; j < c->rows; j++) {
            int val;

            val = c->ticks[j][(i + c->pos) % c->w];
            if (val) {
                gdk_cairo_set_source_rgba(cr, &c->gc_cpu[j]);
                cairo_move_to(cr, i, y);
                cairo_line_to(cr, i, y - val);
                cairo_stroke(cr);
            }
            y -= val;
        }
    }
    return;
}

/**
 * chart_size_allocate - resize the tick buffer when the widget size changes.
 * @widget: the GtkBgbox drawing area. (transfer none)
 * @a:      new allocation. (transfer none)
 * @c:      chart_priv. (transfer none)
 *
 * Frees and reallocates c->ticks whenever c->w or c->h changes.  Updates
 * the frame rectangle (c->fx/fy/fw/fh) used by chart_draw_event for the
 * theme frame, respecting the transparent flag and panel orientation.
 */
static void
chart_size_allocate(GtkWidget *widget, GtkAllocation *a, chart_priv *c)
{
    if (c->w != a->width || c->h != a->height) {
        chart_free_ticks(c);
        c->w = a->width;
        c->h = a->height;
        chart_alloc_ticks(c);
        c->area.x = 0;
        c->area.y = 0;
        c->area.width = a->width;
        c->area.height = a->height;
        if (c->plugin.panel->transparent) {
            c->fx = 0;
            c->fy = 0;
            c->fw = a->width;
            c->fh = a->height;
        } else if (c->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
            c->fx = 0;
            c->fy = 1;
            c->fw = a->width;
            c->fh = a->height -2;
        } else {
            c->fx = 1;
            c->fy = 0;
            c->fw = a->width -2;
            c->fh = a->height;
        }
    }
    gtk_widget_queue_draw(c->da);
    return;
}


/**
 * chart_draw_event - "draw" signal handler: render chart data and theme frame.
 * @widget: the drawing area. (transfer none)
 * @cr:     Cairo context. (transfer none)
 * @c:      chart_priv. (transfer none)
 *
 * Calls chart_draw() for the data lines then gtk_render_frame() for the
 * GTK theme border.
 *
 * Returns: FALSE (allow further drawing).
 */
static gboolean
chart_draw_event(GtkWidget *widget, cairo_t *cr, chart_priv *c)
{
    GtkStyleContext *ctx;
    chart_draw(c, cr);

    ctx = gtk_widget_get_style_context(widget);
    gtk_render_frame(ctx, cr, c->fx, c->fy, c->fw, c->fh);

    return FALSE;
}

/**
 * chart_alloc_ticks - allocate the 2D tick ring buffer.
 * @c: chart_priv with c->rows and c->w already set. (transfer none)
 *
 * Allocates c->ticks as an array of c->rows pointers, each to a c->w gint
 * array.  Resets c->pos to 0.  No-op if c->w or c->rows is zero.
 */
static void
chart_alloc_ticks(chart_priv *c)
{
    int i;

    if (!c->w || !c->rows)
        return;
    c->ticks = g_new0(gint *, c->rows);
    for (i = 0; i < c->rows; i++) {
        c->ticks[i] = g_new0(gint, c->w);
        if (!c->ticks[i])
            DBG2("can't alloc mem: %p %d\n", c->ticks[i], c->w);
    }
    c->pos = 0;
    return;
}


/**
 * chart_free_ticks - free the 2D tick ring buffer.
 * @c: chart_priv. (transfer none)
 *
 * Frees each row array and then the pointer array.  Sets c->ticks = NULL.
 * No-op if c->ticks is already NULL.
 */
static void
chart_free_ticks(chart_priv *c)
{
    int i;

    if (!c->ticks)
        return;
    for (i = 0; i < c->rows; i++)
        g_free(c->ticks[i]);
    g_free(c->ticks);
    c->ticks = NULL;
    return;
}


/**
 * chart_alloc_gcs - allocate and parse GdkRGBA colour array.
 * @c:      chart_priv with c->rows set. (transfer none)
 * @colors: NULL-terminated array of CSS colour strings (transfer none).
 *
 * Allocates c->gc_cpu as c->rows GdkRGBA values and parses each colour
 * string via gdk_rgba_parse().
 */
static void
chart_alloc_gcs(chart_priv *c, gchar *colors[])
{
    int i;

    c->gc_cpu = g_new0(GdkRGBA, c->rows);
    if (c->gc_cpu) {
        for (i = 0; i < c->rows; i++) {
            gdk_rgba_parse(&c->gc_cpu[i], colors[i]);
        }
    }
    return;
}



/**
 * chart_free_gcs - free the GdkRGBA colour array.
 * @c: chart_priv. (transfer none)
 *
 * Frees c->gc_cpu and sets it to NULL.  No-op if already NULL.
 */
static void
chart_free_gcs(chart_priv *c)
{
    if (c->gc_cpu) {
        g_free(c->gc_cpu);
        c->gc_cpu = NULL;
    }
    return;
}


/**
 * chart_set_rows - set the number of data rows and their colours.
 * @c:      chart_priv. (transfer none)
 * @num:    number of data rows (1..9).
 * @colors: NULL-terminated array of CSS colour strings (transfer none).
 *
 * Frees any existing tick buffer and colour array, then reallocates both
 * for the new row count.  Called by subplugins (cpu, mem2, net) after
 * chart_constructor() returns.
 */
static void
chart_set_rows(chart_priv *c, int num, gchar *colors[])
{
    g_assert(num > 0 && num < 10);
    chart_free_ticks(c);
    chart_free_gcs(c);
    c->rows = num;
    chart_alloc_ticks(c);
    chart_alloc_gcs(c, colors);
    return;
}

/**
 * chart_constructor - initialise the chart drawing area inside p->pwid.
 * @p: plugin_instance. (transfer none)
 *
 * Sets c->da = p->pwid (the GtkBgbox is the drawing surface).  Sets a
 * minimum size of 40x25 pixels.  Connects "size-allocate" and "draw"
 * signals on p->pwid.  The tick buffer and colour array are allocated later
 * by chart_set_rows() called from the subplugin constructor.
 *
 * Returns: 1 on success.
 */
static int
chart_constructor(plugin_instance *p)
{
    chart_priv *c;

    /* must be allocated by caller */
    c = (chart_priv *) p;
    c->rows = 0;
    c->ticks = NULL;
    c->gc_cpu = NULL;
    c->da = p->pwid;

    gtk_widget_set_size_request(c->da, 40, 25);
    g_signal_connect (G_OBJECT (p->pwid), "size-allocate",
          G_CALLBACK (chart_size_allocate), (gpointer) c);

    g_signal_connect_after (G_OBJECT (p->pwid), "draw",
          G_CALLBACK (chart_draw_event), (gpointer) c);

    return 1;
}

/**
 * chart_destructor - free the tick buffer and colour array.
 * @p: plugin_instance. (transfer none)
 *
 * Frees c->ticks and c->gc_cpu.  Signal handlers are automatically
 * disconnected when the p->pwid widget is destroyed by the parent.
 */
static void
chart_destructor(plugin_instance *p)
{
    chart_priv *c = (chart_priv *) p;

    chart_free_ticks(c);
    chart_free_gcs(c);
    return;
}

static chart_class class = {
    .plugin = {
        .type        = "chart",
        .name        = "Chart",
        .description = "Basic chart plugin",
        .priv_size   = sizeof(chart_priv),

        .constructor = chart_constructor,
        .destructor  = chart_destructor,
    },
    .add_tick = chart_add_tick,
    .set_rows = chart_set_rows,
};
static plugin_class *class_ptr = (plugin_class *) &class;
