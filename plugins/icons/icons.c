/**
 * @file icons.c
 * @brief Icons plugin — invisible plugin to inject _NET_WM_ICON on managed windows.
 *
 * An INVISIBLE plugin (plugin_class::invisible = 1) that adds no widget to
 * the panel.  Instead it monitors the X client list and ensures every
 * managed window has a _NET_WM_ICON property, either from a user-configured
 * mapping or from a configured default icon.
 *
 * ICON INJECTION LOGIC (set_icon_maybe)
 * ---------------------------------------
 *  1. Search the wmpix linked list for a user-configured icon matching
 *     the window's WM_CLASS (res_name and res_class from XGetClassHint).
 *     Either field in the wmpix entry may be NULL, acting as a wildcard.
 *  2. If no user icon is found AND the window already has a native icon
 *     (_NET_WM_ICON ARGB or WM_HINTS IconPixmapHint): do nothing.
 *  3. If no user icon AND no native icon: use ics->dicon (the default icon)
 *     if set.
 *  4. Set _NET_WM_ICON via XChangeProperty using the ARGB gulong array.
 *
 * CONFIGURATION (in the plugin's xconf tree)
 * ------------------------------------------
 *   defaulticon  <path>   — filesystem path to a default icon (used when a window
 *                           has no native icon and no specific mapping matches)
 *
 *   application            — one node per per-app icon mapping:
 *     image    <path>      — absolute path to image file
 *     icon     <name>      — GTK icon theme name
 *     appname  <name>      — WM_CLASS res_name to match (NULL = wildcard)
 *     classname <class>    — WM_CLASS res_class to match (NULL = wildcard)
 *
 * ICON FORMAT (pixbuf2argb)
 * -------------------------
 * _NET_WM_ICON uses an array of gulong in the form:
 *   [width, height, ARGB_0, ARGB_1, ..., ARGB_{W*H-1}]
 * where each ARGB value is: (alpha << 24) | (red << 16) | (green << 8) | blue.
 *
 * TASK LIFECYCLE
 * --------------
 * do_net_client_list() runs on every EV_CLIENT_LIST FbEv signal.
 * Uses the same two-pass stale-removal as taskbar/pager:
 *  - Known tasks: refcount++.
 *  - New tasks: allocate, install GDK filter, read WM_CLASS, call set_icon_maybe.
 *  - task_remove_stale() via foreach_remove deletes tasks absent from the list.
 *
 * THEME RELOAD
 * ------------
 * When GtkIconTheme emits "changed", theme_changed() drops all config and task
 * state, re-parses config, and re-reads the client list.  This handles icon
 * theme switches.
 *
 * MEMORY OWNERSHIP SUMMARY
 * -------------------------
 * - wmpix_t::data:    (transfer full, g_free in drop_config)
 * - wmpix_t::ch:      res_name/res_class are g_strdup'd; g_free'd in drop_config
 * - task::ch:         res_name/res_class from XGetClassHint; XFree'd in free_task
 *                     (NOT g_free — they are X11 heap allocations)
 * - ics->wins:        (transfer full, XFree in do_net_client_list and drop_config)
 *
 * KNOWN ISSUES
 * ------------
 * BUG-024: task_remove_stale() and task_remove_every() are passed as GHRFunc
 *   callbacks but are declared with only two parameters instead of the required
 *   three (key, value, user_data).  The cast to (GHRFunc) suppresses the type
 *   mismatch.  This is undefined behaviour but harmless on all relevant ABIs.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>



#include "panel.h"
#include "misc.h"
#include "plugin.h"


//#define DEBUGPRN
#include "dbg.h"

/**
 * wmpix_t - user-configured icon mapping for one WM_CLASS pattern.
 *
 * Stored as a singly-linked list in icons_priv::wmpix.
 * All allocations are freed in drop_config().
 */
typedef struct wmpix_t {
    struct wmpix_t *next;   /**< Next mapping in the list; NULL for the last. */
    gulong *data;           /**< _NET_WM_ICON ARGB array (transfer full, g_free).
                             *   Format: [width, height, ARGB_0, ..., ARGB_{W*H-1}]. */
    int size;               /**< Number of gulong entries in data (= 2 + width*height). */
    XClassHint ch;          /**< WM_CLASS match pattern.  res_name and res_class are
                             *   g_strdup'd strings; g_free'd in drop_config.
                             *   NULL field acts as a wildcard. */
} wmpix_t;

struct _icons;

/**
 * task - per-managed-window state for the icons plugin.
 *
 * Created in do_net_client_list() for each new window.
 * Freed by free_task() (called from task_remove_stale/task_remove_every).
 */
typedef struct _task{
    struct _icons *ics;     /**< Back-pointer to owning icons_priv instance. */
    Window win;             /**< X11 window ID; hash table key via &win. */
    GdkWindow *gdkwin;      /**< GDK wrapper for win; holds ics_event_filter.
                             *   g_object_unref'd in free_task. */
    int refcount;           /**< Stale-removal counter (two-pass pattern). */
    XClassHint ch;          /**< WM_CLASS (res_name, res_class) from XGetClassHint.
                             *   res_name/res_class are X11 heap allocations;
                             *   XFree'd in free_task (NOT g_free). */
} task;

/**
 * icons_priv - icons plugin private state.
 *
 * Embedded as first field of the plugin allocation.
 */
typedef struct _icons{
    plugin_instance plugin;     /**< Must be first; plugin framework accesses this. */
    Window *wins;               /**< (transfer full, XFree) _NET_CLIENT_LIST array.
                                 *   Updated on every EV_CLIENT_LIST event. */
    int win_num;                /**< Number of entries in wins[]. */
    GHashTable  *task_list;     /**< Window -> task* map (g_int_hash, g_int_equal).
                                 *   Key = &task::win (stable within task lifetime). */
    int num_tasks;              /**< Count of tasks; maintained by free_task/task_remove_every. */
    wmpix_t *wmpix;             /**< Singly-linked list of per-app icon mappings.
                                 *   Freed by drop_config. */
    wmpix_t *dicon;             /**< Default icon (single wmpix_t; ch fields are NULL).
                                 *   Freed by drop_config. */
} icons_priv;

static void ics_propertynotify(icons_priv *ics, XEvent *ev);
static GdkFilterReturn ics_event_filter( XEvent *, GdkEvent *, icons_priv *);
static void icons_destructor(plugin_instance *p);

/******************************************/
/* Resource Release Code                  */
/******************************************/

/**
 * free_task - release all resources held by one task.
 * @ics:  Icons instance. (transfer none)
 * @tk:   Task to free. (transfer none)
 * @hdel: If non-zero, also remove the task from ics->task_list hash table.
 *
 * Removes the GDK window filter and unrefs gdkwin (if set).
 * XFree's tk->ch.res_name and tk->ch.res_class (X11 heap, not GLib heap).
 * Decrements ics->num_tasks.
 * g_free's the task struct.
 */
static void
free_task(icons_priv *ics, task *tk, int hdel)
{
    ics->num_tasks--;
    if (hdel)
        g_hash_table_remove(ics->task_list, &tk->win);
    if (tk->gdkwin) {
        gdk_window_remove_filter(tk->gdkwin,
                (GdkFilterFunc)ics_event_filter, ics);
        g_object_unref(tk->gdkwin);
    }
    if (tk->ch.res_class)
        XFree(tk->ch.res_class);
    if (tk->ch.res_name)
        XFree(tk->ch.res_name);
    g_free(tk);
    return;
}

/**
 * task_remove_every - GHRFunc: unconditionally remove and free a task.
 * @win: Hash table key (Window*; unused). (transfer none)
 * @tk:  Task to free. (transfer none)
 *
 * Called via g_hash_table_foreach_remove() in drop_config().
 * Passes hdel=0 since the hash table is being cleared entirely.
 *
 * Note: BUG-024 — missing third `gpointer user_data` parameter; cast suppresses mismatch.
 *
 * Returns: TRUE always (remove all entries).
 */
static gboolean
task_remove_every(Window *win, task *tk)
{
    free_task(tk->ics, tk, 0);
    return TRUE;
}


/**
 * drop_config - release all icon mappings, default icon, tasks, and wins array.
 * @ics: Icons instance. (transfer none)
 *
 * Frees the entire wmpix linked list (data, ch.res_name, ch.res_class, node).
 * Frees ics->dicon (data and node; ch fields are NULL for the default icon).
 * Removes all tasks from the hash table via task_remove_every.
 * XFree's ics->wins if set.
 *
 * Called by theme_changed() (before re-reading config) and icons_destructor.
 */
static void
drop_config(icons_priv *ics)
{
    wmpix_t *wp;

    /* free application icons */
    while (ics->wmpix)
    {
        wp = ics->wmpix;
        ics->wmpix = ics->wmpix->next;
        g_free(wp->ch.res_name);
        g_free(wp->ch.res_class);
        g_free(wp->data);
        g_free(wp);
    }

    /* free default icon */
    if (ics->dicon)
    {
        g_free(ics->dicon->data);
        g_free(ics->dicon);
        ics->dicon = NULL;
    }

    /* free task list */
    g_hash_table_foreach_remove(ics->task_list,
        (GHRFunc) task_remove_every, (gpointer)ics);

    if (ics->wins)
    {
        DBG("free ics->wins\n");
        XFree(ics->wins);
        ics->wins = NULL;
    }
    return;
}

/**
 * get_wmclass - refresh a task's WM_CLASS from the X server.
 * @tk: Task to update. (transfer none)
 *
 * XFree's any existing res_name/res_class, then calls XGetClassHint.
 * On failure, sets both fields to NULL.
 *
 * Note: res_name/res_class from XGetClassHint are X11 heap allocations
 * and must be XFree'd (not g_free'd).
 */
static void
get_wmclass(task *tk)
{
    if (tk->ch.res_name)
        XFree(tk->ch.res_name);
    if (tk->ch.res_class)
        XFree(tk->ch.res_class);
    if (!XGetClassHint (GDK_DPY, tk->win, &tk->ch))
        tk->ch.res_class = tk->ch.res_name = NULL;
    DBG("name=%s class=%s\n", tk->ch.res_name, tk->ch.res_class);
    return;
}




/**
 * find_task - look up a task by X window ID.
 * @ics: Icons instance. (transfer none)
 * @win: X11 window ID to look up.
 *
 * Returns: (transfer none) task pointer, or NULL if not found.
 */
static inline task *
find_task (icons_priv * ics, Window win)
{
    return g_hash_table_lookup(ics->task_list, &win);
}


/**
 * task_has_icon - test whether a window has a native icon.
 * @tk: Task to test. (transfer none)
 *
 * Checks for _NET_WM_ICON (ARGB; any non-NULL result) and WM_HINTS
 * IconPixmapHint / IconMaskHint flags.  Frees all X11 allocations before
 * returning.
 *
 * Returns: 1 if the window already has an icon; 0 if not.
 */
static int task_has_icon(task *tk)
{
    XWMHints *hints;
    gulong *data;
    int n;

    data = get_xaproperty(tk->win, a_NET_WM_ICON, XA_CARDINAL, &n);
    if (data)
    {
        XFree(data);
        return 1;
    }

    hints = XGetWMHints(GDK_DPY, tk->win);
    if (hints)
    {
        if ((hints->flags & IconPixmapHint) || (hints->flags & IconMaskHint))
        {
            XFree (hints);
            return 1;
        }
        XFree (hints);
    }
    return 0;
}

/**
 * get_user_icon - find the user-configured icon mapping for a task.
 * @ics: Icons instance. (transfer none)
 * @tk:  Task to match WM_CLASS against. (transfer none)
 *
 * Iterates ics->wmpix.  A wmpix_t matches if both its res_class and
 * res_name match the task's corresponding fields.  A NULL field in the
 * wmpix_t acts as a wildcard (matches any value).
 *
 * Returns: (transfer none) matching wmpix_t, or NULL if no match.
 */
static wmpix_t *
get_user_icon(icons_priv *ics, task *tk)
{
    wmpix_t *tmp;
    int mc, mn;

    if (!(tk->ch.res_class || tk->ch.res_name))
        return NULL;
    DBG("\nch.res_class=[%s] ch.res_name=[%s]\n", tk->ch.res_class,
        tk->ch.res_name);

    for (tmp = ics->wmpix; tmp; tmp = tmp->next)
    {
        DBG("tmp.res_class=[%s] tmp.res_name=[%s]\n", tmp->ch.res_class,
            tmp->ch.res_name);
        mc = !tmp->ch.res_class || !strcmp(tmp->ch.res_class, tk->ch.res_class);
        mn = !tmp->ch.res_name  || !strcmp(tmp->ch.res_name, tk->ch.res_name);
        DBG("mc=%d mn=%d\n", mc, mn);
        if (mc && mn)
        {
            DBG("match !!!!\n");
            return tmp;
        }
    }
    return NULL;
}



/**
 * pixbuf2argb - convert a GdkPixbuf to a _NET_WM_ICON ARGB gulong array.
 * @pixbuf: Source GdkPixbuf (RGB or RGBA). (transfer none)
 * @size:   Output: number of gulong entries in the returned array.
 *
 * The returned array has the format expected by _NET_WM_ICON:
 *   [width, height, ARGB_0, ..., ARGB_{W*H-1}]
 * where each ARGB value = (alpha << 24) | (red << 16) | (green << 8) | blue.
 * For RGB pixbufs (no alpha channel), alpha is set to 255 (fully opaque).
 *
 * Returns: (transfer full, g_free) newly allocated gulong array of @size entries;
 *          sets *size = 2 + width*height.
 */
gulong *
pixbuf2argb (GdkPixbuf *pixbuf, int *size)
{
    gulong *data;
    guchar *pixels;
    gulong *p;
    gint width, height, stride;
    gint x, y;
    gint n_channels;

    *size = 0;
    width = gdk_pixbuf_get_width (pixbuf);
    height = gdk_pixbuf_get_height (pixbuf);
    stride = gdk_pixbuf_get_rowstride (pixbuf);
    n_channels = gdk_pixbuf_get_n_channels (pixbuf);

    *size += 2 + width * height;
    p = data = g_malloc (*size * sizeof (gulong));
    *p++ = width;
    *p++ = height;

    pixels = gdk_pixbuf_get_pixels (pixbuf);

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            guchar r, g, b, a;

            r = pixels[y*stride + x*n_channels + 0];
            g = pixels[y*stride + x*n_channels + 1];
            b = pixels[y*stride + x*n_channels + 2];
            if (n_channels >= 4)
                a = pixels[y*stride + x*n_channels + 3];
            else
                a = 255;

            *p++ = a << 24 | r << 16 | g << 8 | b ;
        }
    }
    return data;
}



/**
 * set_icon_maybe - inject _NET_WM_ICON into a window if it needs one.
 * @ics: Icons instance. (transfer none)
 * @tk:  Task to update. (transfer none)
 *
 * Decision logic:
 *  1. Check for a user-configured match via get_user_icon().
 *  2. If no user match AND the window already has a native icon: return (no change).
 *  3. Otherwise: use ics->dicon (default icon) if set, otherwise return.
 *  4. Write the chosen icon's ARGB data to _NET_WM_ICON via XChangeProperty.
 *
 * Does nothing if both get_user_icon() and ics->dicon return NULL.
 */
static void
set_icon_maybe (icons_priv *ics, task *tk)
{
    wmpix_t *pix;

    g_assert ((ics != NULL) && (tk != NULL));
    g_return_if_fail(tk != NULL);


    pix = get_user_icon(ics, tk);
    if (!pix)
    {
        if (task_has_icon(tk))
            return;
        pix = ics->dicon;
    }
    if (!pix)
        return;

    DBG("%s size=%d\n", pix->ch.res_name, pix->size);
    XChangeProperty (GDK_DPY, tk->win,
          a_NET_WM_ICON, XA_CARDINAL, 32, PropModeReplace, (guchar*) pix->data, pix->size);

    return;
}



/**
 * task_remove_stale - GHRFunc: remove tasks absent from the last client list scan.
 * @win: Hash table key (Window*; unused). (transfer none)
 * @tk:  Task to evaluate. (transfer none)
 *
 * Decrements refcount; if it was 0 before decrement, the task was absent
 * from _NET_CLIENT_LIST and is freed with hdel=0 (table being cleaned by
 * foreach_remove).
 *
 * Note: BUG-024 — missing third `gpointer user_data` parameter.
 *
 * Returns: TRUE to remove; FALSE to keep.
 */
static gboolean
task_remove_stale(Window *win, task *tk)
{
    if (tk->refcount-- == 0)
    {
        free_task(tk->ics, tk, 0);
        return TRUE;
    }
    return FALSE;
}

/*****************************************************
 * handlers for NET actions                          *
 *****************************************************/

/**
 * ics_event_filter - per-window GDK event filter.
 * @xev:  Raw X11 event. (transfer none)
 * @event: GDK event wrapper (unused). (transfer none)
 * @ics:  Icons instance. (transfer none)
 *
 * Installed on each task's GdkWindow via gdk_window_add_filter().
 * Dispatches PropertyNotify events to ics_propertynotify().
 * Always returns GDK_FILTER_CONTINUE.
 */
static GdkFilterReturn
ics_event_filter( XEvent *xev, GdkEvent *event, icons_priv *ics)
{

    g_assert(ics != NULL);
    if (xev->type == PropertyNotify)
	ics_propertynotify(ics, xev);
    return GDK_FILTER_CONTINUE;
}


/**
 * do_net_client_list - FbEv "client_list" signal handler; refreshes the task set.
 * @ics: Icons instance. (transfer none)
 *
 * Reads _NET_CLIENT_LIST from the root window (not stacking order).
 * Uses the two-pass stale-removal pattern:
 *  - Known tasks: refcount++.
 *  - New tasks: allocate, install GDK filter, call get_wmclass(), call set_icon_maybe().
 *  - task_remove_stale() cleans up tasks absent from the new list.
 *
 * ics->wins is XFree'd and replaced on each call.
 */
static void
do_net_client_list(icons_priv *ics)
{
    int i;
    task *tk;

    if (ics->wins)
    {
        DBG("free ics->wins\n");
        XFree(ics->wins);
        ics->wins = NULL;
    }
    ics->wins = get_xaproperty (GDK_ROOT_WINDOW(),
        a_NET_CLIENT_LIST, XA_WINDOW, &ics->win_num);
    if (!ics->wins)
	return;
    DBG("alloc ics->wins\n");
    for (i = 0; i < ics->win_num; i++)
    {
        if ((tk = g_hash_table_lookup(ics->task_list, &ics->wins[i])))
        {
            tk->refcount++;
        }
        else
        {
            tk = g_new0(task, 1);
            tk->refcount++;
            ics->num_tasks++;
            tk->win = ics->wins[i];
            tk->ics = ics;

            if (!FBPANEL_WIN(tk->win))
            {
                XSelectInput(GDK_DPY, tk->win,
                    PropertyChangeMask | StructureNotifyMask);
                tk->gdkwin = gdk_x11_window_foreign_new_for_display(
                        gdk_display_get_default(), tk->win);
                if (tk->gdkwin)
                    gdk_window_add_filter(tk->gdkwin,
                            (GdkFilterFunc)ics_event_filter, ics);
            }
            get_wmclass(tk);
            set_icon_maybe(ics, tk);
            g_hash_table_insert(ics->task_list, &tk->win, tk);
        }
    }

    /* remove windows that arn't in the NET_CLIENT_LIST anymore */
    g_hash_table_foreach_remove(ics->task_list,
        (GHRFunc) task_remove_stale, NULL);
    return;
}

/**
 * ics_propertynotify - handle PropertyNotify from a managed window.
 * @ics: Icons instance. (transfer none)
 * @ev:  XPropertyEvent from the per-window GDK filter. (transfer none)
 *
 * Ignores root-window properties (handled via FbEv).
 * On XA_WM_CLASS change: re-reads WM_CLASS and re-runs set_icon_maybe.
 * On XA_WM_HINTS change: re-runs set_icon_maybe (icon hints may have changed).
 */
static void
ics_propertynotify(icons_priv *ics, XEvent *ev)
{
    Atom at;
    Window win;


    win = ev->xproperty.window;
    at = ev->xproperty.atom;
    DBG("win=%lx at=%ld\n", win, at);
    if (win != GDK_ROOT_WINDOW())
    {
	task *tk = find_task(ics, win);

        if (!tk)
            return;
        if (at == XA_WM_CLASS)
        {
            get_wmclass(tk);
            set_icon_maybe(ics, tk);
        }
        else if (at == XA_WM_HINTS)
        {
            set_icon_maybe(ics, tk);
        }
    }
    return;
}


/**
 * read_application - parse one "application" xconf node and build a wmpix_t.
 * @ics: Icons instance. (transfer none)
 * @xc:  The "application" xconf node to read. (transfer none)
 *
 * Reads config keys (all XCG str = transfer none, except fname after expand_tilda):
 *   "image"     -> fname (expand_tilda, freed at end)
 *   "icon"      -> iname (raw xconf pointer; NOT freed; passed to fb_pixbuf_new)
 *   "appname"   -> appname (raw xconf pointer; NOT freed; g_strdup'd into wp->ch)
 *   "classname" -> classname (raw xconf pointer; NOT freed; g_strdup'd into wp->ch)
 *
 * If neither fname nor iname is set, returns 0.  Otherwise loads a 48x48 pixbuf,
 * converts to ARGB via pixbuf2argb(), and prepends a new wmpix_t to ics->wmpix.
 *
 * The loaded GdkPixbuf is g_object_unref'd after conversion.
 *
 * Returns: 1 on success; 0 if neither image nor icon was configured.
 */
static int
read_application(icons_priv *ics, xconf *xc)
{
    GdkPixbuf *gp = NULL;

    gchar *fname, *iname, *appname, *classname;
    wmpix_t *wp = NULL;
    gulong *data;
    int size;

    iname = fname = appname = classname = NULL;
    XCG(xc, "image", &fname, str);       /* transfer none */
    XCG(xc, "icon", &iname, str);        /* transfer none */
    XCG(xc, "appname", &appname, str);   /* transfer none */
    XCG(xc, "classname", &classname, str); /* transfer none */
    fname = expand_tilda(fname);          /* now transfer full */

    DBG("appname=%s classname=%s\n", appname, classname);
    if (!(fname || iname))
        goto error;
    gp = fb_pixbuf_new(iname, fname, 48, 48, FALSE);
    if (gp)
    {
        if ((data = pixbuf2argb(gp, &size)))
        {
            wp = g_new0 (wmpix_t, 1);
            wp->next = ics->wmpix;
            wp->data = data;
            wp->size = size;
            wp->ch.res_name  = g_strdup(appname);   /* g_strdup'd; g_free'd in drop_config */
            wp->ch.res_class = g_strdup(classname); /* g_strdup'd; g_free'd in drop_config */
            DBG("read name=[%s] class=[%s]\n",
                wp->ch.res_name, wp->ch.res_class);
            ics->wmpix = wp;
        }
        g_object_unref(gp);
    }
    g_free(fname);
    return 1;

error:
    g_free(fname);
    return 0;
}

/**
 * read_dicon - load the default icon from a filesystem path.
 * @ics:  Icons instance. (transfer none)
 * @name: Path to the image file (may contain '~'; expanded via expand_tilda). (transfer none)
 *
 * Loads via gdk_pixbuf_new_from_file(), converts to ARGB via pixbuf2argb(),
 * and stores in ics->dicon.  The GdkPixbuf is unref'd after conversion.
 *
 * Returns: 1 on success (even if pixbuf load fails; dicon may be NULL after return);
 *          0 if expand_tilda returns NULL.
 */
static int
read_dicon(icons_priv *ics, gchar *name)
{
    gchar *fname;
    GdkPixbuf *gp;
    int size;
    gulong *data;

    fname = expand_tilda(name);
    if (!fname)
        return 0;
    gp = gdk_pixbuf_new_from_file(fname, NULL);
    if (gp)
    {
        if ((data = pixbuf2argb(gp, &size)))
        {
            ics->dicon = g_new0 (wmpix_t, 1);
            ics->dicon->data = data;
            ics->dicon->size = size;
            /* dicon->ch.res_name and res_class are implicitly NULL (g_new0);
             * not used for matching. */
        }
        g_object_unref(gp);
    }
    g_free(fname);
    return 1;
}


/**
 * ics_parse_config - parse plugin config and populate wmpix list and dicon.
 * @ics: Icons instance. (transfer none)
 *
 * Reads "defaulticon" key; calls read_dicon() if set.
 * Iterates "application" child nodes; calls read_application() for each.
 *
 * Returns: 1 on success; 0 on error (after partial parse).
 */
static int
ics_parse_config(icons_priv *ics)
{
    gchar *def_icon;
    plugin_instance *p = (plugin_instance *) ics;
    int i;
    xconf *pxc;

    def_icon = NULL;
    XCG(p->xc, "defaulticon", &def_icon, str);
    if (def_icon && !read_dicon(ics, def_icon))
        goto error;

    for (i = 0; (pxc = xconf_find(p->xc, "application", i)); i++)
        if (!read_application(ics, pxc))
            goto error;
    return 1;

error:
    return 0;
}

/**
 * theme_changed - GtkIconTheme "changed" signal handler; reloads all icons.
 * @ics: Icons instance (swapped via g_signal_connect_swapped). (transfer none)
 *
 * Calls drop_config() to release all mappings and task state, then
 * re-parses config and re-reads the client list to re-apply icons with
 * the new theme.
 */
static void
theme_changed(icons_priv *ics)
{
    drop_config(ics);
    ics_parse_config(ics);
    do_net_client_list(ics);
    return;
}

/**
 * icons_constructor - plugin constructor; sets up the icon watcher.
 * @p: Plugin instance (also usable as icons_priv*). (transfer none)
 *
 * Initialisation sequence:
 *  1. Allocate task hash table (g_int_hash, g_int_equal).
 *  2. Call theme_changed() to parse config and populate from _NET_CLIENT_LIST.
 *  3. Connect GtkIconTheme "changed" signal (swapped) to theme_changed.
 *  4. Connect FbEv "client_list" signal to do_net_client_list.
 *
 * This plugin is invisible (plugin_class::invisible = 1); no widget is
 * created and no pwid is shown on the panel.
 *
 * Returns: 1 on success.
 */
static int
icons_constructor(plugin_instance *p)
{
    icons_priv *ics;

    ics = (icons_priv *) p;
    ics->task_list = g_hash_table_new(g_int_hash, g_int_equal);
    theme_changed(ics);
    g_signal_connect_swapped(G_OBJECT(gtk_icon_theme_get_default()),
        "changed", (GCallback) theme_changed, ics);
    g_signal_connect_swapped(G_OBJECT (fbev), "client_list",
        G_CALLBACK (do_net_client_list), (gpointer) ics);

    return 1;
}


/**
 * icons_destructor - plugin destructor; releases all icon watcher resources.
 * @p: Plugin instance (also usable as icons_priv*). (transfer none)
 *
 * Teardown sequence:
 *  1. Disconnect FbEv "client_list" handler.
 *  2. Disconnect GtkIconTheme "changed" handler.
 *  3. drop_config() — frees wmpix list, dicon, all tasks, and wins array.
 *  4. g_hash_table_destroy() — table is already empty after drop_config.
 */
static void
icons_destructor(plugin_instance *p)
{
    icons_priv *ics = (icons_priv *) p;

    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev), do_net_client_list,
        ics);
    g_signal_handlers_disconnect_by_func(G_OBJECT(gtk_icon_theme_get_default()),
        theme_changed, ics);
    drop_config(ics);
    g_hash_table_destroy(ics->task_list);
    return;
}

static plugin_class class = {
    .count     = 0,
    .invisible = 1,    /**< Invisible plugin: no panel widget; runs in the background. */

    .type        = "icons",
    .name        = "Icons",
    .version     = "1.0",
    .description = "Invisible plugin to change window icons",
    .priv_size   = sizeof(icons_priv),


    .constructor = icons_constructor,
    .destructor  = icons_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
