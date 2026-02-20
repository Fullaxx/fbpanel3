/**
 * @file ev.h
 * @brief FbEv — EWMH event bus singleton.
 *
 * FbEv is a GObject that caches EWMH (Extended Window Manager Hints) state
 * read from the X11 root window and broadcasts it to plugins via GObject
 * signals.
 *
 * SIGNAL FLOW
 * -----------
 * 1. panel.c installs a GDK root-window filter that receives raw XEvents.
 * 2. On a PropertyNotify for a known _NET_* atom, it calls fb_ev_trigger()
 *    with the corresponding EV_* signal constant.
 * 3. fb_ev_trigger() calls g_signal_emit() on the global fbev singleton.
 * 4. The default class handler (ev_current_desktop, ev_active_window, etc.)
 *    invalidates the cached value so the next accessor call re-fetches.
 * 5. Plugin handlers connected to the signal read the new value via the
 *    fb_ev_*() accessor functions, which lazily re-fetch from X11 if needed.
 *
 * SINGLETON
 * ---------
 * Created once in panel.c and stored in the global FbEv *fbev.
 * Plugins must not g_object_unref() fbev.
 *
 * ACCESSOR OWNERSHIP
 * ------------------
 * fb_ev_current_desktop()         → int (value type, no ownership)
 * fb_ev_number_of_desktops()      → int (value type, no ownership)
 * fb_ev_active_window()           → Window XID (value type, no ownership)
 * fb_ev_client_list()             → Window* (transfer none; do NOT XFree)
 * fb_ev_client_list_stacking()    → Window* (transfer none; do NOT XFree)
 *
 * The Window* accessors return pointers into the FbEv instance's own storage.
 * They are valid until the next EV_CLIENT_LIST / EV_CLIENT_LIST_STACKING
 * signal fires, which frees and NULLs the arrays.
 *
 * See also: docs/MEMORY_MODEL.md §7 (signal handler cleanup).
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

#ifndef __FB_EV_H__
#define __FB_EV_H__

/* FIXME: this needs to be made multiscreen aware
 *        panel_bg_get should take
 *        a GdkScreen argument.
 */

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>

/** GObject type macro for FbEv. */
#define FB_TYPE_EV         (fb_ev_get_type ())

/** Cast @o to FbEv*, checking the type. */
#define FB_EV(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o),      \
				       FB_TYPE_EV,        \
				       FbEv))
/** Cast @k to FbEvClass*, checking the type. */
#define FB_EV_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k),         \
				       FB_TYPE_EV,        \
				       FbEvClass))
/** Return TRUE if @o is an FbEv instance. */
#define FB_IS_EV(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o),      \
				       FB_TYPE_EV))
/** Return TRUE if @k is the FbEv class. */
#define FB_IS_EV_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k),         \
				       FB_TYPE_EV))
/** Return the FbEvClass for instance @o. */
#define FB_EV_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o),       \
				       FB_TYPE_EV,        \
				       FbEvClass))

typedef struct _FbEvClass FbEvClass;
typedef struct _FbEv      FbEv;

/**
 * EV_* — signal index constants for FbEv.
 *
 * Used as the @signal argument to fb_ev_trigger().  Each constant
 * corresponds to one EWMH root-window property whose change triggers
 * the matching GObject signal on the fbev singleton.
 */
enum {
    EV_CURRENT_DESKTOP,        /**< _NET_CURRENT_DESKTOP changed */
    EV_NUMBER_OF_DESKTOPS,     /**< _NET_NUMBER_OF_DESKTOPS changed */
    EV_DESKTOP_NAMES,          /**< _NET_DESKTOP_NAMES changed */
    EV_ACTIVE_WINDOW,          /**< _NET_ACTIVE_WINDOW changed */
    EV_CLIENT_LIST_STACKING,   /**< _NET_CLIENT_LIST_STACKING changed */
    EV_CLIENT_LIST,            /**< _NET_CLIENT_LIST changed */
    EV_LAST_SIGNAL             /**< sentinel; must be last */
};

/**
 * fb_ev_get_type - return the GType for FbEv, registering it on first call.
 */
GType fb_ev_get_type       (void);

/**
 * fb_ev_new - allocate a new FbEv instance.
 *
 * In normal use the panel creates exactly one FbEv and stores it in the
 * global fbev.  Plugins must not call this function.
 *
 * Returns: (transfer full) new FbEv; caller must g_object_unref() when done.
 */
FbEv *fb_ev_new(void);

/**
 * fb_ev_notify_changed_ev - unused placeholder; prefer fb_ev_trigger().
 * @ev: FbEv instance.
 *
 * Not called anywhere in the current codebase.  Use fb_ev_trigger() to
 * emit a specific signal by index.
 */
void fb_ev_notify_changed_ev(FbEv *ev);

/**
 * fb_ev_trigger - emit the GObject signal identified by @signal on @ev.
 * @ev:     FbEv singleton.
 * @signal: One of the EV_* constants; must satisfy 0 <= signal < EV_LAST_SIGNAL.
 *
 * The default class handler for the signal invalidates the corresponding
 * cached value.  Plugin handlers connected to the signal then read the
 * fresh value via the fb_ev_*() accessors.
 *
 * Called from panel.c's GDK root-window filter on each relevant PropertyNotify.
 */
void fb_ev_trigger(FbEv *ev, int signal);


/**
 * fb_ev_current_desktop - return the index of the current virtual desktop.
 * @ev: FbEv instance.
 *
 * Lazy: queries _NET_CURRENT_DESKTOP from X11 if the cached value was
 * invalidated (set to -1) by the EV_CURRENT_DESKTOP signal handler.
 *
 * Returns: zero-based desktop index, or 0 if the property is absent.
 */
int fb_ev_current_desktop(FbEv *ev);

/**
 * fb_ev_number_of_desktops - return the total number of virtual desktops.
 * @ev: FbEv instance.
 *
 * Lazy: queries _NET_NUMBER_OF_DESKTOPS from X11 if the cached value was
 * invalidated (set to -1) by the EV_NUMBER_OF_DESKTOPS signal handler.
 *
 * Returns: desktop count, or 0 if the property is absent.
 */
int fb_ev_number_of_desktops(FbEv *ev);

/**
 * fb_ev_active_window - return the XID of the currently active window.
 * @ev: FbEv instance.
 *
 * NOTE: currently declared but not implemented in ev.c — see BUG-003
 * in docs/BUGS_AND_ISSUES.md.
 *
 * Returns: Window XID of the active window, or None if unavailable.
 */
Window fb_ev_active_window(FbEv *ev);

/**
 * fb_ev_client_list - return the array of mapped client window XIDs.
 * @ev: FbEv instance.
 *
 * NOTE: currently declared but not implemented in ev.c — see BUG-003
 * in docs/BUGS_AND_ISSUES.md.
 *
 * Returns: (transfer none) pointer into ev's internal storage, or NULL.
 *   Valid until the next EV_CLIENT_LIST signal.  Do NOT XFree the pointer.
 */
Window *fb_ev_client_list(FbEv *ev);

/**
 * fb_ev_client_list_stacking - return the stacking-order client window array.
 * @ev: FbEv instance.
 *
 * NOTE: currently declared but not implemented in ev.c — see BUG-003
 * in docs/BUGS_AND_ISSUES.md.
 *
 * Returns: (transfer none) pointer into ev's internal storage, or NULL.
 *   Valid until the next EV_CLIENT_LIST_STACKING signal.  Do NOT XFree.
 */
Window *fb_ev_client_list_stacking(FbEv *ev);


#endif /* __FB_EV_H__ */
