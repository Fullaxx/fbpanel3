/**
 * @file pager.c
 * @brief Pager plugin — thumbnail view of virtual desktops.
 *
 * Displays a row (or column) of miniature desktop representations, each showing
 * the current desktop background colour and scaled rectangles for every managed
 * window.  Clicking a thumbnail switches the active desktop.
 *
 * DATA STRUCTURES
 * ---------------
 * - struct task   — per-window state (position, desktop, icon pixbuf, GDK filter)
 * - struct desk   — per-desktop state (GtkDrawingArea, cairo backing surface,
 *                   wallpaper surface, scale factors)
 * - struct pager_priv — plugin private state (desk array, task hash table, config)
 *
 * DRAWING PIPELINE
 * ----------------
 * Each desk has an off-screen cairo_image_surface_t `pix` (RGB24) that is used
 * as a backing buffer.  When a window changes position, desktop, or state,
 * the affected desk(s) are marked dirty.  The next GDK "draw" signal on the
 * GtkDrawingArea triggers desk_draw_event(), which:
 *   1. Calls desk_clear_pixmap() — fills pix with the background colour
 *      (or copies the wallpaper cache gpix).
 *   2. Iterates pg->wins[] in stacking order, calling task_update_pix() for
 *      each task on this desktop.
 *   3. Blits d->pix to the cairo_t provided by GDK.
 *
 * TASK LIFECYCLE
 * --------------
 * do_net_client_list_stacking() rebuilds the task set on every
 * EV_CLIENT_LIST_STACKING event (two-pass stale removal):
 *   - For known windows: increment refcount, update stacking index.
 *   - For new windows: allocate task, install per-window GDK filter,
 *     query desktop/state/geometry/icon.
 *   - task_remove_stale() called via g_hash_table_foreach_remove():
 *     deletes tasks whose refcount was 0 before the increment step.
 *     Note: BUG-023 — task_remove_stale does NOT free t->pixbuf.
 *
 * GDK FILTER
 * ----------
 * pager_event_filter() is installed on each task's GdkWindow (via
 * gdk_window_add_filter).  It handles PropertyNotify (NET_WM_STATE,
 * NET_WM_DESKTOP) and ConfigureNotify (window moves/resizes), marking the
 * relevant desk(s) dirty.  The filter is removed and gdkwin unreffed in
 * task_remove_stale() / task_remove_all().
 *
 * WALLPAPER
 * ---------
 * The wallpaper feature (config: showwallpaper) allocates a FbBg reference and
 * a per-desk gpix surface.  desk_draw_bg() is currently a no-op stub (GTK3 port
 * incomplete), so the wallpaper code path fills pix from gpix, but gpix itself
 * is always blank.  The feature is effectively non-functional but harmless.
 *
 * KNOWN ISSUES
 * ------------
 * BUG-018: task::name and task::iname fields are declared but never written or
 *          freed; they are permanently NULL.
 * BUG-019: scalew/scaleh naming is swapped relative to conventional meaning
 *          (scalew = desk_h/screen_h, scaleh = desk_w/screen_w); harmless
 *          because values are equal when the desk aspect ratio is correct.
 * BUG-020: pg->gen_pixbuf is not g_object_unref'd in pager_destructor.
 * BUG-021: pager_priv::dirty field is declared but never read or written.
 * BUG-022: desk::first is set in desk_new() but never read.
 * BUG-023: task_remove_stale() does not free t->pixbuf; leaks one GdkPixbuf
 *          per task removed during normal operation.
 */

/* pager.c -- pager module of fbpanel project
 *
 * Copyright (C) 2002-2003 Anatoly Asviyan <aanatoly@users.sf.net>
 *                         Joe MacDonald   <joe@deserted.net>
 *
 * This file is part of fbpanel.
 *
 * fbpanel is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * fbpanel is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sawfish; see the file COPYING.   If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>


#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "data/images/default.xpm"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"



/**
 * task - per-managed-window state for the pager.
 *
 * Created in do_net_client_list_stacking() when a new window appears in
 * _NET_CLIENT_LIST_STACKING.  Destroyed by task_remove_stale() (normal
 * operation) or task_remove_all() (destructor).
 *
 * The task struct is stored in the hash table keyed by &task::win.  Because
 * the key pointer points INTO the struct, the key is always valid for the
 * lifetime of the task.
 */
typedef struct _task {
    Window win;             /**< X11 window ID; used as hash table key via &win. */
    GdkWindow *gdkwin;      /**< GDK wrapper for win; holds pager_event_filter.
                             *   g_object_unref'd in task_remove_stale/task_remove_all.
                             *   NULL for fbpanel's own windows (FBPANEL_WIN check). */
    int x, y;               /**< Window position on screen (root-relative, pixels). */
    guint w, h;             /**< Window dimensions on screen (pixels). */
    gint refcount;          /**< Stale-removal counter; see do_net_client_list_stacking. */
    guint stacking;         /**< Position in _NET_CLIENT_LIST_STACKING (z-order index). */
    guint desktop;          /**< Desktop number this window lives on (from _NET_WM_DESKTOP). */
    char *name, *iname;     /**< NOTE: declared but never written; always NULL. (BUG-018) */
    net_wm_state nws;       /**< Parsed _NET_WM_STATE flags (hidden, shaded, skip_pager). */
    net_wm_window_type nwwt;/**< Parsed _NET_WM_WINDOW_TYPE flags (desktop, dock, etc). */
    GdkPixbuf *pixbuf;      /**< Task icon (16x16); (transfer full); may be NULL.
                             *   NOT freed by task_remove_stale (BUG-023);
                             *   freed by task_remove_all at destructor time. */
    unsigned int using_netwm_icon:1; /**< TRUE if pixbuf came from _NET_WM_ICON. */
} task;

typedef struct _desk   desk;
typedef struct _pager_priv  pager_priv;

/** Maximum number of virtual desktops the pager supports. */
#define MAX_DESK_NUM   20

/**
 * desk - per-virtual-desktop thumbnail state.
 *
 * Created by desk_new(); destroyed by desk_free().
 * Owns the GtkDrawingArea `da` (added to pg->box), the off-screen backing
 * surface `pix`, and the wallpaper cache `gpix`.
 */
struct _desk {
    GtkWidget *da;          /**< GtkDrawingArea for this desktop thumbnail.
                             *   Owned by the GTK container pg->box;
                             *   gtk_widget_destroy'd in desk_free(). */
    Pixmap xpix;            /**< X11 Pixmap for wallpaper (stub; never assigned). */
    cairo_surface_t *gpix;  /**< Wallpaper cache surface (CAIRO_FORMAT_RGB24, desk size).
                             *   Allocated in desk_configure_event when pg->wallpaper.
                             *   cairo_surface_destroy'd in desk_configure_event (resize)
                             *   and desk_free(). desk_draw_bg() is currently a no-op. */
    cairo_surface_t *pix;   /**< Off-screen backing buffer (CAIRO_FORMAT_RGB24, desk size).
                             *   Rebuilt on configure_event; blitted to GDK in draw event.
                             *   cairo_surface_destroy'd in desk_configure_event and desk_free(). */
    guint no;               /**< Desktop number (0-based index). */
    guint dirty;            /**< Non-zero: backing surface needs redraw before next paint. */
    guint first;            /**< Set to 1 in desk_new(); never read. (BUG-022) */
    gfloat scalew;          /**< Scale from screen height to desk height (desk_h/screen_h).
                             *   Used to scale task x-pos and width. Naming is swapped
                             *   vs conventional (BUG-019); values are equal for a
                             *   correctly-proportioned desk widget. */
    gfloat scaleh;          /**< Scale from screen width to desk width (desk_w/screen_w).
                             *   Used to scale task y-pos and height. (BUG-019) */
    pager_priv *pg;         /**< Back-pointer to owning pager instance. */
};

/**
 * pager_priv - pager plugin private state.
 *
 * Embedded as the first field of the plugin allocation (plugin_instance plugin
 * is first so casting between plugin_instance* and pager_priv* is safe).
 */
struct _pager_priv {
    plugin_instance plugin;         /**< Must be first; plugin framework accesses this. */
    GtkWidget *box;                 /**< Horizontal or vertical GtkBox holding desk widgets;
                                     *   owned by plug->pwid container. */
    desk *desks[MAX_DESK_NUM];      /**< Array of desk pointers; [0..desknum-1] are valid. */
    guint desknum;                  /**< Number of virtual desktops (from _NET_NUMBER_OF_DESKTOPS). */
    guint curdesk;                  /**< Current desktop index (from _NET_CURRENT_DESKTOP). */
    gint wallpaper;                 /**< Config: show wallpaper thumbnails (showwallpaper). */
    gfloat ratio;                   /**< Monitor aspect ratio (width/height); used to size desks. */
    Window *wins;                   /**< (transfer full, XFree) _NET_CLIENT_LIST_STACKING array.
                                     *   Updated on every EV_CLIENT_LIST_STACKING event. */
    int winnum;                     /**< Number of entries in wins[]. */
    int dirty;                      /**< Declared but never used. (BUG-021) */
    GHashTable* htable;             /**< Window -> task* map; key is &task::win (pointer
                                     *   into task struct; stable for task lifetime). */
    task *focusedtask;              /**< Weak pointer to the currently focused task (from
                                     *   _NET_ACTIVE_WINDOW); NULL if no focus or task gone.
                                     *   NULL'd by task_remove_stale when the task is removed. */
    FbBg *fbbg;                     /**< FbBg singleton ref; (transfer full, g_object_unref).
                                     *   Only acquired when pg->wallpaper is TRUE. */
    gint dah, daw;                  /**< Desk area height and width (pixels); computed from
                                     *   panel dimensions and monitor aspect ratio. */
    GdkPixbuf *gen_pixbuf;          /**< Default icon used for tasks without icons (transfer full).
                                     *   Loaded from default.xpm at construction.
                                     *   NOT freed in pager_destructor (BUG-020). */
};



/**
 * TASK_VISIBLE - test whether a task should be drawn in the pager.
 * @tk: Pointer to a task struct.
 *
 * Returns false (task hidden from pager) when the task has the hidden or
 * skip_pager NET_WM_STATE flags set.
 */
#define TASK_VISIBLE(tk)                            \
 (!( (tk)->nws.hidden || (tk)->nws.skip_pager ))


static void pager_rebuild_all(FbEv *ev, pager_priv *pg);
static void desk_draw_bg(pager_priv *pg, desk *d1);
static GdkFilterReturn pager_event_filter(XEvent *, GdkEvent *, pager_priv *);

static void pager_destructor(plugin_instance *p);

static inline void desk_set_dirty_by_win(pager_priv *p, task *t);
static inline void desk_set_dirty(desk *d);
static inline void desk_set_dirty_all(pager_priv *pg);

#ifdef EXTRA_DEBUG
static pager_priv *cp;

/* debug func to print ids of all managed windows on USR2 signal */
static void
sig_usr(int signum)
{
    int j;
    task *t;

    if (signum != SIGUSR2)
        return;
    ERR("dekstop num=%d cur_desktop=%d\n", cp->desknum, cp->curdesk);
    for (j = 0; j < cp->winnum; j++) {
        if (!(t = g_hash_table_lookup(cp->htable, &cp->wins[j])))
            continue;
        ERR("win=%x desktop=%u\n", (guint) t->win, t->desktop);
    }

}
#endif


/*****************************************************************
 * Task Management Routines                                      *
 *****************************************************************/

/**
 * task_remove_stale - GHRFunc: remove tasks that were not seen in the last
 *                     _NET_CLIENT_LIST_STACKING scan.
 * @win: Hash table key (pointer to Window; unused). (transfer none)
 * @t:   Task to evaluate. (transfer none)
 * @p:   Owning pager instance. (transfer none)
 *
 * Called via g_hash_table_foreach_remove() after incrementing refcount for
 * all windows currently in _NET_CLIENT_LIST_STACKING.  A task with refcount==0
 * before the increment (now 0 after post-decrement) was absent from the list
 * and is removed.
 *
 * Note: does NOT free t->pixbuf (BUG-023).
 *
 * Returns: TRUE to remove and free the hash entry; FALSE to keep.
 */
static gboolean
task_remove_stale(Window *win, task *t, pager_priv *p)
{
    if (t->refcount-- == 0) {
        desk_set_dirty_by_win(p, t);
        if (p->focusedtask == t)
            p->focusedtask = NULL;
        DBG("del %lx\n", t->win);
        if (t->gdkwin) {
            gdk_window_remove_filter(t->gdkwin,
                    (GdkFilterFunc)pager_event_filter, p);
            g_object_unref(t->gdkwin);
        }
        g_free(t);
        return TRUE;
    }
    return FALSE;
}

/**
 * task_remove_all - GHRFunc: unconditionally remove and free a task.
 * @win: Hash table key (pointer to Window; unused). (transfer none)
 * @t:   Task to free. (transfer none)
 * @p:   Owning pager instance. (transfer none)
 *
 * Called via g_hash_table_foreach_remove() during pager_destructor() and
 * pager_rebuild_all() (after desk count change).  Removes the per-window
 * GDK filter, unrefs gdkwin, and frees t->pixbuf if set.
 *
 * Returns: TRUE always (remove all).
 */
static gboolean
task_remove_all(Window *win, task *t, pager_priv *p)
{
    if (t->pixbuf != NULL)
        g_object_unref(t->pixbuf);
    if (t->gdkwin) {
        gdk_window_remove_filter(t->gdkwin,
                (GdkFilterFunc)pager_event_filter, p);
        g_object_unref(t->gdkwin);
    }
    g_free(t);
    return TRUE;
}


/**
 * task_get_sizepos - query and update a task's on-screen position and size.
 * @t: Task to update. (transfer none)
 *
 * Tries XGetWindowAttributes() first (gives root-relative position via
 * XTranslateCoordinates).  Falls back to XGetGeometry() for unmapped/reparented
 * windows.  As a last resort, sets all dimensions to 2.
 *
 * Updates t->x, t->y, t->w, t->h.
 */
static void
task_get_sizepos(task *t)
{
    Window root, junkwin;
    int rx, ry;
    guint dummy;
    XWindowAttributes win_attributes;

    if (!XGetWindowAttributes(GDK_DPY, t->win, &win_attributes)) {
        if (!XGetGeometry (GDK_DPY, t->win, &root, &t->x, &t->y, &t->w, &t->h,
                  &dummy, &dummy)) {
            t->x = t->y = t->w = t->h = 2;
        }

    } else {
        XTranslateCoordinates (GDK_DPY, t->win, win_attributes.root,
              -win_attributes.border_width,
              -win_attributes.border_width,
              &rx, &ry, &junkwin);
        t->x = rx;
        t->y = ry;
        t->w = win_attributes.width;
        t->h = win_attributes.height;
        DBG("win=0x%lx WxH=%dx%d\n", t->win,t->w, t->h);
    }
    return;
}


/**
 * task_update_pix - draw one task's scaled rectangle into a desk's backing surface.
 * @t: Task to draw. (transfer none)
 * @d: Desk whose pix surface to draw into. (transfer none)
 *
 * Skips tasks that are:
 *  - not TASK_VISIBLE (hidden or skip_pager)
 *  - on a different desktop than d->no (unless desktop > desknum, shown on all)
 *  - too small after scaling (w<3 or h<3 pixels)
 *
 * Draws onto d->pix:
 *  1. Filled rectangle in the GTK SELECTED background colour (focused task) or
 *     NORMAL background colour (unfocused).
 *  2. Outlined rectangle in the corresponding foreground colour.
 *  3. If the scaled rectangle is at least 10x10: draws t->pixbuf (or
 *     pg->gen_pixbuf as fallback) centred within the rectangle, scaling the
 *     icon down if the rectangle is smaller than 18x18.
 *
 * For shaded windows, height is clamped to 3 pixels.
 *
 * Note: BUG-019 — scalew and scaleh names are swapped vs conventional usage,
 * but values are equal for a correctly-proportioned desk, so rendering is correct.
 */
static void
task_update_pix(task *t, desk *d)
{
    int x, y, w, h;
    GtkWidget *widget;
    cairo_t *cr;
    GtkStyleContext *ctx;
    GdkRGBA fg_sel, fg_norm, bg_sel, bg_norm;

    g_return_if_fail(d->pix != NULL);
    if (!TASK_VISIBLE(t))
        return;

    if (t->desktop < d->pg->desknum &&
          t->desktop != d->no)
        return;

    x = (gfloat)t->x * d->scalew;
    y = (gfloat)t->y * d->scaleh;
    w = (gfloat)t->w * d->scalew;
    //h = (gfloat)t->h * d->scaleh;
    h = (t->nws.shaded) ? 3 : (gfloat)t->h * d->scaleh;
    if (w < 3 || h < 3)
        return;
    widget = GTK_WIDGET(d->da);

    ctx = gtk_widget_get_style_context(widget);
    gtk_style_context_save(ctx);
    gtk_style_context_set_state(ctx, GTK_STATE_FLAG_SELECTED);
    gtk_style_context_get(ctx, GTK_STATE_FLAG_SELECTED, GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &bg_sel, NULL);
    gtk_style_context_get_color(ctx, GTK_STATE_FLAG_SELECTED, &fg_sel);
    gtk_style_context_set_state(ctx, GTK_STATE_FLAG_NORMAL);
    gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &bg_norm, NULL);
    gtk_style_context_get_color(ctx, GTK_STATE_FLAG_NORMAL, &fg_norm);
    gtk_style_context_restore(ctx);

    cr = cairo_create(d->pix);

    /* filled rectangle with bg color */
    if (d->pg->focusedtask == t)
        gdk_cairo_set_source_rgba(cr, &bg_sel);
    else
        gdk_cairo_set_source_rgba(cr, &bg_norm);
    cairo_rectangle(cr, x+1, y+1, w-1, h-1);
    cairo_fill(cr);

    /* outline rectangle with fg color */
    if (d->pg->focusedtask == t)
        gdk_cairo_set_source_rgba(cr, &fg_sel);
    else
        gdk_cairo_set_source_rgba(cr, &fg_norm);
    cairo_rectangle(cr, x, y, w-1, h);
    cairo_stroke(cr);

    cairo_destroy(cr);

    if (w>=10 && h>=10) {
        GdkPixbuf* source_buf = t->pixbuf;
        if (source_buf == NULL)
            source_buf = d->pg->gen_pixbuf;

        /* determine how much to scale */
        GdkPixbuf* scaled = source_buf;
        int scale = 16;
        int noscale = 1;
        int smallest = ( (w<h) ? w : h );
        if (smallest < 18) {
            noscale = 0;
            scale = smallest - 2;
            if (scale % 2 != 0)
                scale++;

            scaled = gdk_pixbuf_scale_simple(source_buf,
                                    scale, scale,
                                    GDK_INTERP_BILINEAR);
        }

        /* position */
        int pixx = x+((w/2)-(scale/2))+1;
        int pixy = y+((h/2)-(scale/2))+1;

        /* draw pixbuf via cairo */
        cr = cairo_create(d->pix);
        gdk_cairo_set_source_pixbuf(cr, scaled, pixx, pixy);
        cairo_paint(cr);
        cairo_destroy(cr);

        /* free it if its been scaled and its not the default */
        if (!noscale && t->pixbuf != NULL)
            g_object_unref(scaled);
    }
    return;
}


/*****************************************************************
 * Desk Functions                                                *
 *****************************************************************/

/**
 * desk_clear_pixmap - fill a desk's backing surface with the background.
 * @d: Desk to clear. (transfer none)
 *
 * If d->pix is NULL (not yet allocated), returns immediately.
 *
 * When pg->wallpaper is TRUE and d->xpix != None:
 *   Copies gpix (wallpaper cache) to pix.  Then if this is the current
 *   desktop, draws a SELECTED-colour border around the thumbnail.
 *
 * When pg->wallpaper is FALSE (or xpix is None):
 *   Fills pix with SELECTED background colour for the current desktop,
 *   NORMAL background colour for all others.
 *
 * Note: desk_draw_bg() is a no-op in the GTK3 port, so gpix is always blank.
 */
static void
desk_clear_pixmap(desk *d)
{
    GtkWidget *widget;
    GtkStyleContext *ctx;
    GdkRGBA color;
    cairo_t *cr;
    GtkAllocation alloc;

    DBG("d->no=%d\n", d->no);
    if (!d->pix)
        return;
    widget = GTK_WIDGET(d->da);
    gtk_widget_get_allocation(widget, &alloc);
    ctx = gtk_widget_get_style_context(widget);

    if (d->pg->wallpaper && d->xpix != None) {
        /* copy gpix to pix using cairo */
        cr = cairo_create(d->pix);
        cairo_set_source_surface(cr, d->gpix, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
    } else {
        cr = cairo_create(d->pix);
        gtk_style_context_save(ctx);
        if (d->no == d->pg->curdesk) {
            gtk_style_context_set_state(ctx, GTK_STATE_FLAG_SELECTED);
            gtk_style_context_get(ctx, GTK_STATE_FLAG_SELECTED, GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &color, NULL);
        } else {
            gtk_style_context_set_state(ctx, GTK_STATE_FLAG_NORMAL);
            gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &color, NULL);
        }
        gtk_style_context_restore(ctx);
        gdk_cairo_set_source_rgba(cr, &color);
        cairo_rectangle(cr, 0, 0, alloc.width, alloc.height);
        cairo_fill(cr);
        cairo_destroy(cr);
    }
    if (d->pg->wallpaper && d->no == d->pg->curdesk) {
        cr = cairo_create(d->pix);
        gtk_style_context_save(ctx);
        gtk_style_context_set_state(ctx, GTK_STATE_FLAG_SELECTED);
        gtk_style_context_get_color(ctx, GTK_STATE_FLAG_SELECTED, &color);
        gtk_style_context_restore(ctx);
        gdk_cairo_set_source_rgba(cr, &color);
        cairo_rectangle(cr, 0, 0, alloc.width - 1, alloc.height - 1);
        cairo_stroke(cr);
        cairo_destroy(cr);
    }
    return;
}


/**
 * desk_draw_bg - render the desktop wallpaper into a desk's gpix surface.
 * @pg: Pager instance. (transfer none)
 * @d1: Desk to update. (transfer none)
 *
 * STUB: wallpaper rendering is not implemented in the GTK3 port.
 * Returns immediately.
 */
static void
desk_draw_bg(pager_priv *pg, desk *d1)
{
    /* Wallpaper rendering not supported in GTK3 port */
    return;
}



/**
 * desk_set_dirty - mark a desk as needing a redraw and queue a GDK draw.
 * @d: Desk to mark dirty. (transfer none)
 */
static inline void
desk_set_dirty(desk *d)
{
    d->dirty = 1;
    gtk_widget_queue_draw(d->da);
    return;
}

/**
 * desk_set_dirty_all - mark every desk dirty and queue redraws.
 * @pg: Pager instance. (transfer none)
 */
static inline void
desk_set_dirty_all(pager_priv *pg)
{
    int i;
    for (i = 0; i < pg->desknum; i++)
        desk_set_dirty(pg->desks[i]);
    return;
}

/**
 * desk_set_dirty_by_win - mark the desk(s) affected by a task change as dirty.
 * @p: Pager instance. (transfer none)
 * @t: Task that changed. (transfer none)
 *
 * Skips tasks with skip_pager or _NET_WM_WINDOW_TYPE_DESKTOP set.
 * Marks t->desktop if it is a valid desktop index; otherwise marks all desks
 * (for sticky windows where desktop >= desknum).
 */
static inline void
desk_set_dirty_by_win(pager_priv *p, task *t)
{
    if (t->nws.skip_pager || t->nwwt.desktop /*|| t->nwwt.dock || t->nwwt.splash*/ )
        return;
    if (t->desktop < p->desknum)
        desk_set_dirty(p->desks[t->desktop]);
    else
        desk_set_dirty_all(p);
    return;
}

/**
 * desk_draw_event - GDK "draw" signal handler for a desk's GtkDrawingArea.
 * @widget: The GtkDrawingArea. (transfer none)
 * @cr:     Cairo context provided by GDK for this draw cycle. (transfer none)
 * @d:      The desk this drawing area belongs to. (transfer none)
 *
 * If d->dirty: clears the backing surface (desk_clear_pixmap), then calls
 * task_update_pix() for every window in pg->wins[] in stacking order.
 *
 * Blits d->pix onto cr via cairo_set_source_surface + cairo_paint.
 *
 * Returns FALSE (allow further signal handlers; GTK convention for draw).
 */
static gint
desk_draw_event (GtkWidget *widget, cairo_t *cr, desk *d)
{
    DBG("d->no=%d\n", d->no);

    if (d->dirty) {
        pager_priv *pg = d->pg;
        task *t;
        int j;

        d->dirty = 0;
        desk_clear_pixmap(d);
        for (j = 0; j < pg->winnum; j++) {
            if (!(t = g_hash_table_lookup(pg->htable, &pg->wins[j])))
                continue;
            task_update_pix(t, d);
        }
    }
    if (d->pix) {
        cairo_set_source_surface(cr, d->pix, 0, 0);
        cairo_paint(cr);
    }
    return FALSE;
}


/**
 * desk_configure_event - GDK "configure_event" handler; reallocates backing surfaces.
 * @widget: The GtkDrawingArea. (transfer none)
 * @event:  Configure event (contains new size). (transfer none)
 * @d:      The desk. (transfer none)
 *
 * Destroys old d->pix (and d->gpix if wallpaper enabled) and creates new ones
 * sized to the widget's current allocation.
 *
 * Recomputes d->scalew and d->scaleh from the primary monitor geometry:
 *   scalew = alloc.height / monitor_height   (scale for x/width)
 *   scaleh = alloc.width  / monitor_width    (scale for y/height)
 * The naming appears swapped vs convention but the values are equal for a
 * correctly-proportioned desk, so rendering is unaffected (BUG-019).
 *
 * Returns FALSE.
 */
static gint
desk_configure_event (GtkWidget *widget, GdkEventConfigure *event, desk *d)
{
    int w, h;
    GtkAllocation alloc;

    gtk_widget_get_allocation(widget, &alloc);
    w = alloc.width;
    h = alloc.height;

    DBG("d->no=%d %dx%d %dx%d\n", d->no, w, h, d->pg->daw, d->pg->dah);
    if (d->pix)
        cairo_surface_destroy(d->pix);
    if (d->gpix)
        cairo_surface_destroy(d->gpix);
    d->pix = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (d->pg->wallpaper) {
        d->gpix = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        desk_draw_bg(d->pg, d);
    }
    {
        GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);
        d->scalew = (gfloat)h / (gfloat)geom.height;
        d->scaleh = (gfloat)w / (gfloat)geom.width;
    }
    desk_set_dirty(d);
    return FALSE;
}

/**
 * desk_button_press_event - GDK "button_press_event" handler for a desk thumbnail.
 * @widget: The GtkDrawingArea. (transfer none)
 * @event:  Button press event. (transfer none)
 * @d:      The desk. (transfer none)
 *
 * Ctrl+RMB: passes through (returns FALSE) for the panel's right-click menu.
 * Any other button press: sends _NET_CURRENT_DESKTOP ClientMessage to switch
 * to this desktop, returns TRUE (event consumed).
 */
static gint
desk_button_press_event(GtkWidget * widget, GdkEventButton * event, desk *d)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        return FALSE;
    }
    DBG("s=%d\n", d->no);
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, d->no, 0, 0, 0, 0);
    return TRUE;
}

/**
 * desk_new - allocate and initialise a new desk for desktop index @i.
 * @pg: Pager instance. (transfer none)
 * @i:  Desktop index (must be < pg->desknum).
 *
 * Allocates pg->desks[i], creates a GtkDrawingArea sized daw x dah,
 * packs it into pg->box, and connects "draw", "configure_event", and
 * "button_press_event" signals.
 */
static void
desk_new(pager_priv *pg, int i)
{
    desk *d;

    g_assert(i < pg->desknum);
    d = pg->desks[i] = g_new0(desk, 1);
    d->pg = pg;
    d->pix = NULL;
    d->dirty = 0;
    d->first = 1;   /* set but never read (BUG-022) */
    d->no = i;

    d->da = gtk_drawing_area_new();
    gtk_widget_set_size_request(d->da, pg->daw, pg->dah);
    gtk_box_pack_start(GTK_BOX(pg->box), d->da, TRUE, TRUE, 0);
    gtk_widget_add_events (d->da, GDK_EXPOSURE_MASK
          | GDK_BUTTON_PRESS_MASK
          | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect (G_OBJECT (d->da), "draw",
          (GCallback) desk_draw_event, (gpointer)d);
    g_signal_connect (G_OBJECT (d->da), "configure_event",
          (GCallback) desk_configure_event, (gpointer)d);
    g_signal_connect (G_OBJECT (d->da), "button_press_event",
         (GCallback) desk_button_press_event, (gpointer)d);
    gtk_widget_show_all(d->da);
    return;
}

/**
 * desk_free - destroy a desk and free all its resources.
 * @pg: Pager instance. (transfer none)
 * @i:  Desktop index to free.
 *
 * Destroys d->pix and d->gpix (cairo surfaces), calls gtk_widget_destroy on
 * d->da, then g_free's the desk struct.  Clears pg->desks[i] indirectly
 * (caller is responsible for not accessing it after this).
 */
static void
desk_free(pager_priv *pg, int i)
{
    desk *d;

    d = pg->desks[i];
    DBG("i=%d d->no=%d d->da=%p d->pix=%p\n",
          i, d->no, d->da, d->pix);
    if (d->pix)
        cairo_surface_destroy(d->pix);
    if (d->gpix)
        cairo_surface_destroy(d->gpix);
    gtk_widget_destroy(d->da);
    g_free(d);
    return;
}


/*****************************************************************
 * Stuff to grab icons from windows - ripped from taskbar.c      *
 *****************************************************************/

/**
 * _wnck_gdk_pixbuf_get_from_pixmap - convert an X11 Pixmap to a GdkPixbuf.
 * @dest:     Ignored (pass NULL). Kept for API compatibility.
 * @xpixmap:  X11 Pixmap ID to read. (transfer none)
 * @src_x:    Source X offset within the pixmap.
 * @src_y:    Source Y offset within the pixmap.
 * @dest_x:   Ignored.
 * @dest_y:   Ignored.
 * @width:    Width of the region to extract (pixels).
 * @height:   Height of the region to extract (pixels).
 *
 * Uses cairo-xlib to copy the pixmap into an RGB24 image surface, then
 * calls gdk_pixbuf_get_from_surface() to produce a GdkPixbuf.
 *
 * Handles 1-bit bitmaps (icon masks): renders white-on-black so that
 * apply_mask() can use pixel value 0 as transparent, 255 as opaque.
 *
 * Returns: (transfer full) new GdkPixbuf, or NULL on X11 / cairo failure.
 */
static GdkPixbuf*
_wnck_gdk_pixbuf_get_from_pixmap (GdkPixbuf   *dest,
                                  Pixmap       xpixmap,
                                  int          src_x,
                                  int          src_y,
                                  int          dest_x,
                                  int          dest_y,
                                  int          width,
                                  int          height)
{
    Display *dpy = GDK_DPY;
    cairo_surface_t *xlib_surf, *image;
    GdkPixbuf *ret;
    cairo_t *cr;
    Window root_ret;
    int rx, ry;
    unsigned int rw, rh, rborder, depth;

    (void)dest; (void)dest_x; (void)dest_y;

    if (!XGetGeometry(dpy, xpixmap, &root_ret, &rx, &ry, &rw, &rh, &rborder, &depth))
        return NULL;

    image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        return NULL;
    }

    if (depth == 1) {
        /* 1-bit X bitmap (icon mask).  Render white-on-black so that
         * apply_mask() can read black=transparent, white=opaque. */
        xlib_surf = cairo_xlib_surface_create_for_bitmap(
            dpy, xpixmap, DefaultScreenOfDisplay(dpy), (int)rw, (int)rh);
        if (cairo_surface_status(xlib_surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(xlib_surf);
            cairo_surface_destroy(image);
            return NULL;
        }
        cr = cairo_create(image);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_mask_surface(cr, xlib_surf, -src_x, -src_y);
        cairo_destroy(cr);
    } else {
        /* Full-depth colour pixmap (icon image). */
        xlib_surf = cairo_xlib_surface_create(
            dpy, xpixmap,
            DefaultVisual(dpy, DefaultScreen(dpy)),
            (int)rw, (int)rh);
        if (cairo_surface_status(xlib_surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(xlib_surf);
            cairo_surface_destroy(image);
            return NULL;
        }
        cr = cairo_create(image);
        cairo_set_source_surface(cr, xlib_surf, -src_x, -src_y);
        cairo_paint(cr);
        cairo_destroy(cr);
    }

    cairo_surface_destroy(xlib_surf);
    ret = gdk_pixbuf_get_from_surface(image, 0, 0, width, height);
    cairo_surface_destroy(image);
    return ret;
}

/**
 * apply_mask - composite a 1-bit mask into a GdkPixbuf's alpha channel.
 * @pixbuf: Source RGB pixbuf. (transfer none)
 * @mask:   Mask pixbuf (from _wnck_gdk_pixbuf_get_from_pixmap on a 1-bit bitmap).
 *          Pixels with value 0 become transparent; non-zero become opaque.
 *          (transfer none)
 *
 * Returns: (transfer full) new RGBA GdkPixbuf with alpha channel set from @mask.
 *          Caller must g_object_unref the result and the input pixbufs separately.
 */
static GdkPixbuf*
apply_mask (GdkPixbuf *pixbuf,
            GdkPixbuf *mask)
{
  int w, h;
  int i, j;
  GdkPixbuf *with_alpha;
  guchar *src;
  guchar *dest;
  int src_stride;
  int dest_stride;

  w = MIN (gdk_pixbuf_get_width (mask), gdk_pixbuf_get_width (pixbuf));
  h = MIN (gdk_pixbuf_get_height (mask), gdk_pixbuf_get_height (pixbuf));

  with_alpha = gdk_pixbuf_add_alpha (pixbuf, FALSE, 0, 0, 0);

  dest = gdk_pixbuf_get_pixels (with_alpha);
  src = gdk_pixbuf_get_pixels (mask);

  dest_stride = gdk_pixbuf_get_rowstride (with_alpha);
  src_stride = gdk_pixbuf_get_rowstride (mask);

  i = 0;
  while (i < h)
    {
      j = 0;
      while (j < w)
        {
          guchar *s = src + i * src_stride + j * 3;
          guchar *d = dest + i * dest_stride + j * 4;

          /* s[0] == s[1] == s[2], they are 255 if the bit was set, 0
           * otherwise
           */
          if (s[0] == 0)
            d[3] = 0;   /* transparent */
          else
            d[3] = 255; /* opaque */

          ++j;
        }

      ++i;
    }

  return with_alpha;
}

/**
 * free_pixels - GdkPixbuf destroy notify to free a g_new'd pixel buffer.
 * @pixels: Pixel data to free. (transfer full)
 * @data:   Unused user data.
 */
static void
free_pixels (guchar *pixels, gpointer data)
{
    g_free (pixels);
    return;
}

/**
 * argbdata_to_pixdata - convert ARGB gulong array to packed RGBA bytes.
 * @argb_data: Array of @len ARGB values (as returned by XGetWindowProperty
 *             with XA_CARDINAL type). (transfer none)
 * @len:       Number of ARGB entries.
 *
 * Converts each entry: `rgba = (argb << 8) | (argb >> 24)`, then unpacks
 * RGBA bytes in order: R, G, B, A.
 *
 * Returns: (transfer full) newly g_new'd byte array of length len*4;
 *          must be freed by the GdkPixbuf destroy-notify (free_pixels).
 */
static guchar *
argbdata_to_pixdata (gulong *argb_data, int len)
{
    guchar *p, *ret;
    int i;

    ret = p = g_new (guchar, len * 4);
    if (!ret)
        return NULL;
    /* One could speed this up a lot. */
    i = 0;
    while (i < len) {
        guint32 argb;
        guint32 rgba;

        argb = argb_data[i];
        rgba = (argb << 8) | (argb >> 24);

        *p = rgba >> 24;
        ++p;
        *p = (rgba >> 16) & 0xff;
        ++p;
        *p = (rgba >> 8) & 0xff;
        ++p;
        *p = rgba & 0xff;
        ++p;

        ++i;
    }
    return ret;
}

/**
 * get_netwm_icon - load a task icon from _NET_WM_ICON (ARGB format).
 * @tkwin: X11 window to query. (transfer none)
 * @iw:    Desired icon width.
 * @ih:    Desired icon height.
 *
 * Reads XA_CARDINAL _NET_WM_ICON property, validates dimensions are in
 * [16, 256] range, converts ARGB to RGBA via argbdata_to_pixdata, creates a
 * GdkPixbuf, and scales to iw x ih if needed.
 *
 * Returns: (transfer full) new GdkPixbuf scaled to iw x ih, or NULL on failure.
 *          Data array freed by XFree regardless of outcome.
 */
static GdkPixbuf *
get_netwm_icon(Window tkwin, int iw, int ih)
{
    gulong *data;
    GdkPixbuf *ret = NULL;
    int n;
    guchar *p;
    GdkPixbuf *src;
    int w, h;

    data = get_xaproperty(tkwin, a_NET_WM_ICON, XA_CARDINAL, &n);
    if (!data)
        return NULL;

    /* check that data indeed represents icon in w + h + ARGB[] format
     * with 16x16 dimension at least */
    if (n < (16 * 16 + 1 + 1)) {
        ERR("win %lx: icon is too small or broken (size=%d)\n", tkwin, n);
        goto out;
    }
    w = data[0];
    h = data[1];
    /* check that sizes are in 64-256 range */
    if (w < 16 || w > 256 || h < 16 || h > 256) {
        ERR("win %lx: icon size (%d, %d) is not in 64-256 range\n",
            tkwin, w, h);
        goto out;
    }

    DBG("orig  %dx%d dest %dx%d\n", w, h, iw, ih);
    p = argbdata_to_pixdata(data + 2, w * h);
    if (!p)
        goto out;
    src = gdk_pixbuf_new_from_data (p, GDK_COLORSPACE_RGB, TRUE,
        8, w, h, w * 4, free_pixels, NULL);
    if (src == NULL)
        goto out;
    ret = src;
    if (w != iw || h != ih) {
        ret = gdk_pixbuf_scale_simple(src, iw, ih, GDK_INTERP_HYPER);
        g_object_unref(src);
    }

out:
    XFree(data);
    return ret;
}

/**
 * get_wm_icon - load a task icon from WM_HINTS (X11 Pixmap path).
 * @tkwin: X11 window to query. (transfer none)
 * @iw:    Desired icon width.
 * @ih:    Desired icon height.
 *
 * Reads XGetWMHints; if IconPixmapHint is set, converts the X11 Pixmap to
 * a GdkPixbuf via _wnck_gdk_pixbuf_get_from_pixmap + cairo-xlib.  If
 * IconMaskHint is also set, composites the mask via apply_mask().
 * Scales the result to iw x ih using GDK_INTERP_TILES.
 *
 * Returns: (transfer full) new GdkPixbuf scaled to iw x ih, or NULL on failure.
 *          Intermediate pixbufs are g_object_unref'd before returning.
 */
static GdkPixbuf*
get_wm_icon(Window tkwin, int iw, int ih)
{
    XWMHints *hints;
    Pixmap xpixmap = None, xmask = None;
    Window win;
    unsigned int w, h;
    int sd;
    GdkPixbuf *ret, *masked, *pixmap, *mask = NULL;

    hints = XGetWMHints(GDK_DPY, tkwin);
    DBG("\nwm_hints %s\n", hints ? "ok" : "failed");
    if (!hints)
        return NULL;

    if ((hints->flags & IconPixmapHint))
        xpixmap = hints->icon_pixmap;
    if ((hints->flags & IconMaskHint))
        xmask = hints->icon_mask;
    DBG("flag=%ld xpixmap=%lx flag=%ld xmask=%lx\n", (hints->flags & IconPixmapHint), xpixmap,
         (hints->flags & IconMaskHint),  xmask);
    XFree(hints);
    if (xpixmap == None)
        return NULL;

    if (!XGetGeometry (GDK_DPY, xpixmap, &win, &sd, &sd, &w, &h,
              (guint *)&sd, (guint *)&sd)) {
        DBG("XGetGeometry failed for %x pixmap\n", (unsigned int)xpixmap);
        return NULL;
    }
    DBG("tkwin=%x icon pixmap w=%d h=%d\n", tkwin, w, h);
    pixmap = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xpixmap, 0, 0, 0, 0, w, h);
    if (!pixmap)
        return NULL;
    if (xmask != None && XGetGeometry (GDK_DPY, xmask,
              &win, &sd, &sd, &w, &h, (guint *)&sd, (guint *)&sd)) {
        mask = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xmask, 0, 0, 0, 0, w, h);

        if (mask) {
            masked = apply_mask (pixmap, mask);
            g_object_unref (G_OBJECT (pixmap));
            g_object_unref (G_OBJECT (mask));
            pixmap = masked;
        }
    }
    if (!pixmap)
        return NULL;
    ret = gdk_pixbuf_scale_simple (pixmap, iw, ih, GDK_INTERP_TILES);
    g_object_unref(pixmap);

    return ret;
}


/*****************************************************************
 * Netwm/WM Interclient Communication                            *
 *****************************************************************/

/**
 * do_net_active_window - FbEv "active_window" signal handler.
 * @ev: FbEv instance (unused). (transfer none)
 * @p:  Pager instance. (transfer none)
 *
 * Reads _NET_ACTIVE_WINDOW from the root window.  Updates p->focusedtask
 * and marks the previously- and newly-focused desks dirty so they repaint
 * with the correct highlight colour.
 *
 * The data pointer from get_xaproperty is XFree'd after hash lookup.
 */
static void
do_net_active_window(FbEv *ev, pager_priv *p)
{
    Window *fwin;
    task *t;

    fwin = get_xaproperty(GDK_ROOT_WINDOW(), a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
    DBG("win=%lx\n", fwin ? *fwin : 0);
    if (fwin) {
        t = g_hash_table_lookup(p->htable, fwin);
        if (t != p->focusedtask) {
            if (p->focusedtask)
                desk_set_dirty_by_win(p, p->focusedtask);
            p->focusedtask = t;
            if (t)
                desk_set_dirty_by_win(p, t);
        }
        XFree(fwin);
    } else {
        if (p->focusedtask) {
            desk_set_dirty_by_win(p, p->focusedtask);
            p->focusedtask = NULL;
        }
    }
    return;
}

/**
 * do_net_current_desktop - FbEv "current_desktop" signal handler.
 * @ev: FbEv instance (may be NULL when called from pager_rebuild_all). (transfer none)
 * @pg: Pager instance. (transfer none)
 *
 * Updates pg->curdesk and marks old and new current-desktop desk thumbnails
 * dirty; also updates the GtkStateFlags (SELECTED vs NORMAL) on the drawing
 * areas so CSS-based styling reflects the active desktop.
 */
static void
do_net_current_desktop(FbEv *ev, pager_priv *pg)
{
    desk_set_dirty(pg->desks[pg->curdesk]);
    gtk_widget_set_state_flags(pg->desks[pg->curdesk]->da, GTK_STATE_FLAG_NORMAL, TRUE);
    pg->curdesk =  get_net_current_desktop ();
    if (pg->curdesk >= pg->desknum)
        pg->curdesk = 0;
    desk_set_dirty(pg->desks[pg->curdesk]);
    gtk_widget_set_state_flags(pg->desks[pg->curdesk]->da, GTK_STATE_FLAG_SELECTED, TRUE);
    return;
}


/**
 * do_net_client_list_stacking - FbEv "client_list_stacking" signal handler.
 * @ev: FbEv instance (may be NULL when called from pager_rebuild_all). (transfer none)
 * @p:  Pager instance. (transfer none)
 *
 * Refreshes the task set from _NET_CLIENT_LIST_STACKING.  Uses a two-pass
 * stale-removal approach (same as taskbar):
 *
 *   Pass 1 — for each window in the new list:
 *     - Known task: increment refcount; update stacking index if changed.
 *     - New task: allocate, set refcount=1, query desktop/state/geometry/icon,
 *       install pager_event_filter on gdkwin.
 *
 *   Pass 2 — g_hash_table_foreach_remove(task_remove_stale):
 *     tasks whose refcount was 0 before pass 1 are removed.
 *
 * pg->wins is XFree'd and replaced on each call.
 */
static void
do_net_client_list_stacking(FbEv *ev, pager_priv *p)
{
    int i;
    task *t;

    if (p->wins)
        XFree(p->wins);
    p->wins = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST_STACKING,
          XA_WINDOW, &p->winnum);
    if (!p->wins || !p->winnum)
        return;

    /* refresh existing tasks and add new */
    for (i = 0; i < p->winnum; i++) {
        if ((t = g_hash_table_lookup(p->htable, &p->wins[i]))) {
            t->refcount++;
            if (t->stacking != i) {
                t->stacking = i;
                desk_set_dirty_by_win(p, t);
            }
        } else {
            t = g_new0(task, 1);
            t->refcount++;
            t->win = p->wins[i];
            if (!FBPANEL_WIN(t->win)) {
                XSelectInput (GDK_DPY, t->win, PropertyChangeMask | StructureNotifyMask);
                t->gdkwin = gdk_x11_window_foreign_new_for_display(
                        gdk_display_get_default(), t->win);
                if (t->gdkwin)
                    gdk_window_add_filter(t->gdkwin,
                            (GdkFilterFunc)pager_event_filter, p);
            }
            t->desktop = get_net_wm_desktop(t->win);
            get_net_wm_state(t->win, &t->nws);
            get_net_wm_window_type(t->win, &t->nwwt);
            task_get_sizepos(t);
            /* Icon loading: try _NET_WM_ICON first, fall back to WM_HINTS pixmap. */
            t->pixbuf = get_netwm_icon(t->win, 16, 16);
            t->using_netwm_icon = (t->pixbuf != NULL);
            if (!t->using_netwm_icon) {
                t->pixbuf = get_wm_icon(t->win, 16, 16);
            }
            g_hash_table_insert(p->htable, &t->win, t);
            DBG("add %lx\n", t->win);
            desk_set_dirty_by_win(p, t);
        }
    }
    /* pass throu hash table and delete stale windows */
    g_hash_table_foreach_remove(p->htable, (GHRFunc) task_remove_stale, (gpointer)p);
    return;
}


/*****************************************************************
 * Pager Functions                                               *
 *****************************************************************/

/**
 * pager_configurenotify - handle ConfigureNotify from a managed window.
 * @p:  Pager instance. (transfer none)
 * @ev: XConfigureEvent from the per-window GDK filter. (transfer none)
 *
 * Looks up the window in the hash table.  If found, refreshes the task's
 * position and size via task_get_sizepos(), then marks the desk dirty.
 */
static void
pager_configurenotify(pager_priv *p, XEvent *ev)
{
    Window win = ev->xconfigure.window;
    task *t;


    if (!(t = g_hash_table_lookup(p->htable, &win)))
        return;
    DBG("win=0x%lx\n", win);
    task_get_sizepos(t);
    desk_set_dirty_by_win(p, t);
    return;
}

/**
 * pager_propertynotify - handle PropertyNotify from a managed window.
 * @p:  Pager instance. (transfer none)
 * @ev: XPropertyEvent from the per-window GDK filter. (transfer none)
 *
 * Ignores root-window property changes (handled via FbEv signals).
 * Handles _NET_WM_STATE: updates t->nws.
 * Handles _NET_WM_DESKTOP: marks old desk dirty, updates t->desktop.
 * Other atoms are ignored.
 * Always marks the affected desk dirty after state update.
 */
static void
pager_propertynotify(pager_priv *p, XEvent *ev)
{
    Atom at = ev->xproperty.atom;
    Window win = ev->xproperty.window;
    task *t;


    if ((win == GDK_ROOT_WINDOW()) || !(t = g_hash_table_lookup(p->htable, &win)))
        return;

    DBG("window=0x%lx\n", t->win);
    if (at == a_NET_WM_STATE) {
        DBG("event=NET_WM_STATE\n");
        get_net_wm_state(t->win, &t->nws);
    } else if (at == a_NET_WM_DESKTOP) {
        DBG("event=NET_WM_DESKTOP\n");
        desk_set_dirty_by_win(p, t); // to clean up desks where this task was
        t->desktop = get_net_wm_desktop(t->win);
    } else {
        return;
    }
    desk_set_dirty_by_win(p, t);
    return;
}

/**
 * pager_event_filter - per-window GDK event filter for managed windows.
 * @xev:   Raw X11 event. (transfer none)
 * @event: GDK event wrapper (unused). (transfer none)
 * @pg:    Pager instance. (transfer none)
 *
 * Installed on each task's GdkWindow via gdk_window_add_filter().
 * Dispatches PropertyNotify to pager_propertynotify() and
 * ConfigureNotify to pager_configurenotify().
 *
 * Always returns GDK_FILTER_CONTINUE (events are not consumed).
 */
static GdkFilterReturn
pager_event_filter( XEvent *xev, GdkEvent *event, pager_priv *pg)
{
    if (xev->type == PropertyNotify )
        pager_propertynotify(pg, xev);
    else if (xev->type == ConfigureNotify )
        pager_configurenotify(pg, xev);
    return GDK_FILTER_CONTINUE;
}

/**
 * pager_bg_changed - FbBg "changed" signal handler; redraws all desks.
 * @bg: FbBg instance (unused). (transfer none)
 * @pg: Pager instance. (transfer none)
 *
 * Called when the X root window background changes.  Redraws the wallpaper
 * cache (desk_draw_bg) and marks all desks dirty.  desk_draw_bg is a no-op
 * in the GTK3 port, so this only triggers a full repaint.
 */
static void
pager_bg_changed(FbBg *bg, pager_priv *pg)
{
    int i;

    for (i = 0; i < pg->desknum; i++) {
        desk *d = pg->desks[i];
        desk_draw_bg(pg, d);
        desk_set_dirty(d);
    }
    return;
}


/**
 * pager_rebuild_all - FbEv "number_of_desktops" signal handler; rebuilds desk array.
 * @ev: FbEv instance (may be NULL when called directly at construction). (transfer none)
 * @pg: Pager instance. (transfer none)
 *
 * Re-reads _NET_NUMBER_OF_DESKTOPS and _NET_CURRENT_DESKTOP.  If the count
 * did not change, returns immediately.  Otherwise:
 *  - If desktops were removed: calls desk_free() for each removed desk.
 *  - If desktops were added: calls desk_new() for each new desk.
 * Then clears the entire task hash table (task_remove_all), and re-reads
 * the current desktop and client list to repopulate.
 *
 * Desktop count is clamped to [1, MAX_DESK_NUM].
 */
static void
pager_rebuild_all(FbEv *ev, pager_priv *pg)
{
    int desknum, dif, i;
    int curdesk G_GNUC_UNUSED;

    desknum = pg->desknum;
    curdesk = pg->curdesk;

    pg->desknum = get_net_number_of_desktops();
    if (pg->desknum < 1)
        pg->desknum = 1;
    else if (pg->desknum > MAX_DESK_NUM) {
        pg->desknum = MAX_DESK_NUM;
        ERR("pager: max number of supported desks is %d\n", MAX_DESK_NUM);
    }
    pg->curdesk = get_net_current_desktop();
    if (pg->curdesk >= pg->desknum)
        pg->curdesk = 0;
    DBG("desknum=%d curdesk=%d\n", desknum, curdesk);
    DBG("pg->desknum=%d pg->curdesk=%d\n", pg->desknum, pg->curdesk);
    dif = pg->desknum - desknum;

    if (dif == 0)
        return;

    if (dif < 0) {
        /* if desktops were deleted then delete their maps also */
        for (i = pg->desknum; i < desknum; i++)
            desk_free(pg, i);
    } else {
        for (i = desknum; i < pg->desknum; i++)
            desk_new(pg, i);
    }
    g_hash_table_foreach_remove(pg->htable, (GHRFunc) task_remove_all, (gpointer)pg);
    do_net_current_desktop(NULL, pg);
    do_net_client_list_stacking(NULL, pg);

    return;
}

/** Border width (pixels) between the plugin's GtkBgbox and the desk box. */
#define BORDER 1

/**
 * pager_constructor - plugin constructor; initialises and shows the pager.
 * @plug: Plugin instance (also usable as pager_priv*). (transfer none)
 *
 * Initialisation sequence:
 *  1. Allocate task hash table (keyed by Window integer value).
 *  2. Create inner GtkBox (horizontal or vertical, spacing=1).
 *  3. Set GtkBgbox background to BG_STYLE; add inner box to plug->pwid.
 *  4. Compute desk aspect ratio from primary monitor geometry.
 *  5. Compute dah/daw (desk area height/width) from panel dimensions.
 *  6. Optionally acquire FbBg and connect "changed" signal for wallpaper.
 *  7. Load default XPM icon into pg->gen_pixbuf.
 *  8. Call pager_rebuild_all() to create desks and populate tasks.
 *  9. Connect FbEv signals for desktop/window changes.
 *
 * Returns: 1 on success.
 */
static int
pager_constructor(plugin_instance *plug)
{
    pager_priv *pg;

    pg = (pager_priv *) plug;

#ifdef EXTRA_DEBUG
    cp = pg;
    signal(SIGUSR2, sig_usr);
#endif

    pg->htable = g_hash_table_new (g_int_hash, g_int_equal);
    pg->box = plug->panel->my_box_new(TRUE, 1);
    gtk_container_set_border_width (GTK_CONTAINER (pg->box), 0);
    gtk_widget_show(pg->box);

    gtk_bgbox_set_background(plug->pwid, BG_STYLE, 0, 0);
    gtk_container_set_border_width (GTK_CONTAINER (plug->pwid), BORDER);
    gtk_container_add(GTK_CONTAINER(plug->pwid), pg->box);

    {
        GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);
        pg->ratio = (gfloat)geom.width / (gfloat)geom.height;
    }
    if (plug->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        pg->dah = plug->panel->ah - 2 * BORDER;
        pg->daw = (gfloat) pg->dah * pg->ratio;
    } else {
        pg->daw = plug->panel->aw - 2 * BORDER;
        pg->dah = (gfloat) pg->daw / pg->ratio;
    }
    pg->wallpaper = 1;
    XCG(plug->xc, "showwallpaper", &pg->wallpaper, enum, bool_enum);
    if (pg->wallpaper) {
        pg->fbbg = fb_bg_get_for_display();
        DBG("get fbbg %p\n", pg->fbbg);
        g_signal_connect(G_OBJECT(pg->fbbg), "changed",
            G_CALLBACK(pager_bg_changed), pg);
    }

    /* Default icon for windows without _NET_WM_ICON or WM_HINTS icon.
     * Note: gen_pixbuf is not freed in pager_destructor (BUG-020). */
    pg->gen_pixbuf = gdk_pixbuf_new_from_xpm_data((const char **)icon_xpm);

    pager_rebuild_all(fbev, pg);

    g_signal_connect (G_OBJECT (fbev), "current_desktop",
          G_CALLBACK (do_net_current_desktop), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "active_window",
          G_CALLBACK (do_net_active_window), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "number_of_desktops",
          G_CALLBACK (pager_rebuild_all), (gpointer) pg);
    g_signal_connect (G_OBJECT (fbev), "client_list_stacking",
          G_CALLBACK (do_net_client_list_stacking), (gpointer) pg);
    return 1;
}

/**
 * pager_destructor - plugin destructor; frees all pager resources.
 * @p: Plugin instance (also usable as pager_priv*). (transfer none)
 *
 * Teardown sequence:
 *  1. Disconnect all FbEv signal handlers (current_desktop, active_window,
 *     number_of_desktops, client_list_stacking).
 *  2. desk_free() for each desk (destroys cairo surfaces and GtkDrawingAreas).
 *  3. task_remove_all() for all hash table entries via foreach_remove.
 *  4. g_hash_table_destroy().
 *  5. gtk_widget_destroy(pg->box).
 *  6. If wallpaper: disconnect pager_bg_changed and g_object_unref(pg->fbbg).
 *  7. XFree(pg->wins) if set.
 *
 * Note: pg->gen_pixbuf is NOT freed here (BUG-020).
 */
static void
pager_destructor(plugin_instance *p)
{
    pager_priv *pg = (pager_priv *)p;

    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_current_desktop, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_active_window, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            pager_rebuild_all, pg);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            do_net_client_list_stacking, pg);
    while (pg->desknum--) {
        desk_free(pg, pg->desknum);
    }
    g_hash_table_foreach_remove(pg->htable, (GHRFunc) task_remove_all,
            (gpointer)pg);
    g_hash_table_destroy(pg->htable);
    gtk_widget_destroy(pg->box);
    if (pg->wallpaper) {
        g_signal_handlers_disconnect_by_func(G_OBJECT (pg->fbbg),
              pager_bg_changed, pg);
        DBG("put fbbg %p\n", pg->fbbg);
        g_object_unref(pg->fbbg);
    }
    if (pg->wins)
        XFree(pg->wins);
    return;
}


static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "pager",
    .name        = "Pager",
    .version     = "1.0",
    .description = "Pager shows thumbnails of your desktops",
    .priv_size   = sizeof(pager_priv),

    .constructor = pager_constructor,
    .destructor  = pager_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
