/**
 * @file ev.c
 * @brief FbEv — EWMH event bus singleton (implementation).
 *
 * FbEv caches the most recently known values of several EWMH root-window
 * properties and provides them to plugins via GObject signals and lazy
 * accessor functions.
 *
 * INVALIDATION PATTERN
 * --------------------
 * Each signal has a default class handler (ev_current_desktop, etc.) that
 * resets the cached value to a sentinel (-1 for integers, None for Window,
 * NULL for arrays).  The next call to the corresponding accessor then
 * re-fetches from X11 via get_xaproperty().
 *
 * This lazy re-fetch avoids reading properties that no plugin cares about
 * on every PropertyNotify — only the first accessor call after a signal
 * pays the X11 round-trip cost.
 *
 * ARRAY OWNERSHIP
 * ---------------
 * ev->client_list and ev->client_list_stacking are Window* arrays allocated
 * by XGetWindowProperty (X11 heap).  They are freed with XFree() in the
 * signal handlers (ev_client_list, ev_client_list_stacking) when the
 * corresponding signal fires.
 *
 * ev->desktop_names is a char** (g_strv) freed with g_strfreev() in
 * ev_desktop_names().  However it is NOT freed in fb_ev_finalize() —
 * see BUG-001 in docs/BUGS_AND_ISSUES.md.
 *
 * UNIMPLEMENTED ACCESSORS
 * -----------------------
 * fb_ev_active_window(), fb_ev_client_list(), fb_ev_client_list_stacking()
 * are declared but have no implementation body in this file — see BUG-003.
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

#include "ev.h"
#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"


/**
 * _FbEvClass - GObject class struct for FbEv.
 * @parent_class:           GObjectClass; must be first.
 * @dummy:                  Padding to align the signal slots with the
 *                          g_signal_new G_STRUCT_OFFSET layout.
 * @current_desktop:        Default handler for EV_CURRENT_DESKTOP signal.
 * @active_window:          Default handler for EV_ACTIVE_WINDOW signal.
 * @number_of_desktops:     Default handler for EV_NUMBER_OF_DESKTOPS signal.
 * @desktop_names:          Default handler for EV_DESKTOP_NAMES signal.
 * @client_list:            Default handler for EV_CLIENT_LIST signal.
 * @client_list_stacking:   Default handler for EV_CLIENT_LIST_STACKING signal.
 *
 * Each handler invalidates the cached value so the next accessor call
 * re-fetches from X11.
 */
struct _FbEvClass {
    GObjectClass   parent_class;
    void *dummy;
    void (*current_desktop)(FbEv *ev, gpointer p);
    void (*active_window)(FbEv *ev, gpointer p);
    void (*number_of_desktops)(FbEv *ev, gpointer p);
    void (*desktop_names)(FbEv *ev, gpointer p);
    void (*client_list)(FbEv *ev, gpointer p);
    void (*client_list_stacking)(FbEv *ev, gpointer p);
};

/**
 * _FbEv - GObject instance struct for the EWMH event bus.
 * @parent_instance:        GObject base; must be first.
 * @current_desktop:        Cached _NET_CURRENT_DESKTOP index; -1 = invalid.
 * @number_of_desktops:     Cached _NET_NUMBER_OF_DESKTOPS count; -1 = invalid.
 * @desktop_names:          Cached _NET_DESKTOP_NAMES as a NULL-terminated
 *                          g_strv; NULL = invalid.  Freed with g_strfreev().
 *                          NOTE: not freed in finalize — see BUG-001.
 * @active_window:          Cached _NET_ACTIVE_WINDOW XID; None = invalid.
 * @client_list:            Cached _NET_CLIENT_LIST array (X11 heap).
 *                          Freed with XFree() in ev_client_list().
 *                          NULL = invalid or not yet fetched.
 * @client_list_stacking:   Cached _NET_CLIENT_LIST_STACKING array (X11 heap).
 *                          Freed with XFree() in ev_client_list_stacking().
 *                          NULL = invalid or not yet fetched.
 * @xroot, @id, @gc, @dpy, @pixmap: Legacy fields copied from FbBg init;
 *                          currently unused in FbEv — historical artefact.
 */
struct _FbEv {
    GObject    parent_instance;

    int current_desktop;
    int number_of_desktops;
    char **desktop_names;
    Window active_window;
    Window *client_list;
    Window *client_list_stacking;

    Window   xroot;
    Atom     id;
    GC       gc;
    Display *dpy;
    Pixmap   pixmap;
};

static void fb_ev_class_init (FbEvClass *klass);
static void fb_ev_init (FbEv *monitor);
static void fb_ev_finalize (GObject *object);

static void ev_current_desktop(FbEv *ev, gpointer p);
static void ev_active_window(FbEv *ev, gpointer p);
static void ev_number_of_desktops(FbEv *ev, gpointer p);
static void ev_desktop_names(FbEv *ev, gpointer p);
static void ev_client_list(FbEv *ev, gpointer p);
static void ev_client_list_stacking(FbEv *ev, gpointer p);

/** GObject signal IDs, indexed by EV_* constants. */
static guint signals [EV_LAST_SIGNAL] = { 0 };


/**
 * fb_ev_get_type - return the GType for FbEv, registering it on first call.
 */
GType
fb_ev_get_type (void)
{
    static GType object_type = 0;

    if (!object_type) {
        static const GTypeInfo object_info = {
            sizeof (FbEvClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) fb_ev_class_init,
            NULL,           /* class_finalize */
            NULL,           /* class_data */
            sizeof (FbEv),
            0,              /* n_preallocs */
            (GInstanceInitFunc) fb_ev_init,
        };

        object_type = g_type_register_static (
            G_TYPE_OBJECT, "FbEv", &object_info, 0);
    }

    return object_type;
}


/**
 * fb_ev_class_init - initialise the FbEvClass.
 * @klass: Class struct to fill in.
 *
 * Registers the six EWMH GObject signals and installs the default class
 * handlers that invalidate cached values when a signal fires.
 * Installs fb_ev_finalize as the GObject finalize hook.
 */
static void
fb_ev_class_init (FbEvClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    signals [EV_CURRENT_DESKTOP] =
        g_signal_new ("current_desktop",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, current_desktop),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    signals [EV_NUMBER_OF_DESKTOPS] =
        g_signal_new ("number_of_desktops",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, number_of_desktops),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    signals [EV_DESKTOP_NAMES] =
        g_signal_new ("desktop_names",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, desktop_names),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    signals [EV_ACTIVE_WINDOW] =
        g_signal_new ("active_window",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, active_window),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    signals [EV_CLIENT_LIST_STACKING] =
        g_signal_new ("client_list_stacking",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, client_list_stacking),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    signals [EV_CLIENT_LIST] =
        g_signal_new ("client_list",
              G_OBJECT_CLASS_TYPE (object_class),
              G_SIGNAL_RUN_FIRST,
              G_STRUCT_OFFSET (FbEvClass, client_list),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);
    object_class->finalize = fb_ev_finalize;

    klass->current_desktop = ev_current_desktop;
    klass->active_window = ev_active_window;
    klass->number_of_desktops = ev_number_of_desktops;
    klass->desktop_names = ev_desktop_names;
    klass->client_list = ev_client_list;
    klass->client_list_stacking = ev_client_list_stacking;
}

/**
 * fb_ev_init - instance initialiser for FbEv.
 * @ev: Newly allocated FbEv instance.
 *
 * Sets all cached values to their "invalid" sentinels so that the first
 * accessor call after construction will fetch from X11.
 */
static void
fb_ev_init (FbEv *ev)
{
    ev->number_of_desktops = -1;
    ev->current_desktop = -1;
    ev->active_window = None;
    ev->client_list_stacking = NULL;
    ev->client_list = NULL;
}


/**
 * fb_ev_new - allocate a new FbEv instance.
 *
 * Returns: (transfer full) new FbEv; caller must g_object_unref() when done.
 */
FbEv *
fb_ev_new()
{
    return  g_object_new (FB_TYPE_EV, NULL);
}

/**
 * fb_ev_finalize - GObject finalize handler for FbEv.
 * @object: GObject being finalized (an FbEv instance).
 *
 * Frees ev->desktop_names (a g_strv) if the EV_DESKTOP_NAMES signal
 * never fired (which sets it to NULL via ev_desktop_names()).
 * The commented-out XFreeGC call is a historical artefact from FbBg.
 */
static void
fb_ev_finalize (GObject *object)
{
    FbEv *ev = FB_EV(object);

    if (ev->desktop_names) {
        g_strfreev(ev->desktop_names);
        ev->desktop_names = NULL;
    }
    //XFreeGC(ev->dpy, ev->gc);
}

/**
 * fb_ev_trigger - emit the GObject signal identified by @signal.
 * @ev:     FbEv instance (the global fbev singleton).
 * @signal: EV_* constant; must satisfy 0 <= signal < EV_LAST_SIGNAL.
 *
 * The matching default class handler fires first (G_SIGNAL_RUN_FIRST),
 * invalidating the cached value.  Then all plugin-connected handlers fire.
 *
 * Called exclusively from the GDK root-window PropertyNotify filter in
 * panel.c.  Must be called from the GTK main loop (single-threaded).
 */
void
fb_ev_trigger(FbEv *ev, int signal)
{
    DBG("signal=%d\n", signal);
    g_assert(signal >=0 && signal < EV_LAST_SIGNAL);
    DBG("\n");
    g_signal_emit(ev, signals [signal], 0);
}

/**
 * ev_current_desktop - default handler for EV_CURRENT_DESKTOP.
 * @ev: FbEv instance.
 * @p:  Unused (GObject signal user_data; always NULL for VOID__VOID).
 *
 * Invalidates the cached desktop index by setting it to -1.
 * The next call to fb_ev_current_desktop() will re-fetch from X11.
 */
static void
ev_current_desktop(FbEv *ev, gpointer p)
{
    ev->current_desktop = -1;
    return;
}

/**
 * ev_active_window - default handler for EV_ACTIVE_WINDOW.
 * @ev: FbEv instance.
 * @p:  Unused.
 *
 * Invalidates the cached active window by setting it to None.
 */
static void
ev_active_window(FbEv *ev, gpointer p)
{
    ev->active_window = None;
    return;
}

/**
 * ev_number_of_desktops - default handler for EV_NUMBER_OF_DESKTOPS.
 * @ev: FbEv instance.
 * @p:  Unused.
 *
 * Invalidates the cached desktop count by setting it to -1.
 */
static void
ev_number_of_desktops(FbEv *ev, gpointer p)
{
    ev->number_of_desktops = -1;
    return;
}

/**
 * ev_desktop_names - default handler for EV_DESKTOP_NAMES.
 * @ev: FbEv instance.
 * @p:  Unused.
 *
 * Frees the current desktop_names g_strv (if any) with g_strfreev() and
 * sets the pointer to NULL, invalidating the cache.
 */
static void
ev_desktop_names(FbEv *ev, gpointer p)
{
    if (ev->desktop_names) {
        g_strfreev (ev->desktop_names);
        ev->desktop_names = NULL;
    }
    return;
}

/**
 * ev_client_list - default handler for EV_CLIENT_LIST.
 * @ev: FbEv instance.
 * @p:  Unused.
 *
 * Frees the current client_list array (X11 heap) with XFree() and sets
 * the pointer to NULL, invalidating the cache.
 *
 * Note: XFree is correct here because the array was allocated by
 * XGetWindowProperty() inside get_xaproperty().
 */
static void
ev_client_list(FbEv *ev, gpointer p)
{
    if (ev->client_list) {
        XFree(ev->client_list);
        ev->client_list = NULL;
    }
    return;
}

/**
 * ev_client_list_stacking - default handler for EV_CLIENT_LIST_STACKING.
 * @ev: FbEv instance.
 * @p:  Unused.
 *
 * Frees the current client_list_stacking array (X11 heap) with XFree()
 * and sets the pointer to NULL.
 */
static void
ev_client_list_stacking(FbEv *ev, gpointer p)
{
    if (ev->client_list_stacking) {
        XFree(ev->client_list_stacking);
        ev->client_list_stacking = NULL;
    }
    return;
}

/**
 * fb_ev_current_desktop - return the current virtual desktop index.
 * @ev: FbEv instance.
 *
 * Lazy: if ev->current_desktop == -1 (invalidated by signal or first call),
 * queries _NET_CURRENT_DESKTOP from the X11 root window via get_xaproperty().
 * The result is cached until the next EV_CURRENT_DESKTOP signal.
 *
 * Returns: zero-based desktop index, or 0 if the property is absent.
 */
int
fb_ev_current_desktop(FbEv *ev)
{
    if (ev->current_desktop == -1) {
        guint *data;

        data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, XA_CARDINAL, 0);
        if (data) {
            ev->current_desktop = *data;
            XFree (data);   /* X11-heap; must use XFree */
        } else
            ev->current_desktop = 0;
    }
    return ev->current_desktop;
}

/**
 * fb_ev_number_of_desktops - return the total virtual desktop count.
 * @ev: FbEv instance.
 *
 * Lazy: if ev->number_of_desktops == -1, queries _NET_NUMBER_OF_DESKTOPS
 * from the X11 root window.  The result is cached until the next
 * EV_NUMBER_OF_DESKTOPS signal.
 *
 * Returns: desktop count, or 0 if the property is absent.
 */
int
fb_ev_number_of_desktops(FbEv *ev)
{
     if (ev->number_of_desktops == -1) {
        guint *data;

        data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 0);
        if (data) {
            ev->number_of_desktops = *data;
            XFree (data);   /* X11-heap; must use XFree */
        } else
            ev->number_of_desktops = 0;
    }
    return ev->number_of_desktops;

}

/*
 * NOTE — BUG-003: the following three functions are declared here as bare
 * prototypes without a function body.  They appear to be unfinished
 * implementations.  If any plugin calls them, the linker will fail to
 * resolve them (or resolve to an unrelated symbol via --export-dynamic).
 *
 * See docs/BUGS_AND_ISSUES.md §BUG-003 for the suspected fix.
 */
Window fb_ev_active_window(FbEv *ev);
Window *fb_ev_client_list(FbEv *ev);
Window *fb_ev_client_list_stacking(FbEv *ev);
