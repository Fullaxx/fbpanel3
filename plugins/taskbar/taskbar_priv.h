/**
 * @file taskbar_priv.h
 * @brief Taskbar plugin — private types, structs, and cross-file declarations.
 *
 * This header is included by all four taskbar translation units:
 *   taskbar.c      — plugin_class registration, constructor, destructor
 *   taskbar_net.c  — EWMH event handlers, GDK event filter
 *   taskbar_task.c — task lifecycle, icon loading, flash/urgency
 *   taskbar_ui.c   — task button creation, cairo draw, click callbacks
 *
 * MULTI-TU PLUGIN DESIGN
 * ----------------------
 * The PLUGIN macro in plugin.h emits a __attribute__((constructor)) ctor() that
 * calls class_register(class_ptr) when the .so is dlopen'd.  For a single-file
 * plugin this is fine, but taskbar is split across 4 TUs that are all compiled
 * with -DPLUGIN.  Without the #undef below, each TU gets its own ctor() with its
 * own NULL class_ptr, causing class_register(NULL) -> crash at p->type (offset
 * 0x20).  The PLUGIN macro is suppressed here; taskbar.c registers the class
 * manually with its own constructor/destructor attributes.
 *
 * TASK LIFECYCLE
 * --------------
 * Tasks are created in tb_net_client_list() when a window appears in
 * _NET_CLIENT_LIST that passes the accept_net_wm_state / accept_net_wm_window_type
 * filters.  Each task is:
 *   1. g_new0(task, 1) — zeroed allocation
 *   2. tk_build_gui    — button + label + image + per-window GDK filter
 *   3. tk_get_names    — read _NET_WM_NAME or WM_NAME
 *   4. tk_set_names    — populate label text and tooltip
 *   5. g_hash_table_insert into tb->task_list (keyed by tk->win)
 *
 * Tasks are destroyed in del_task(), which:
 *   1. Removes the flash timer (g_source_remove tk->flash_timeout)
 *   2. Removes the per-window GDK filter and unref's the GdkWindow
 *   3. gtk_widget_destroy(tk->button) — removes from bar widget tree
 *   4. tk_free_names — g_free's name and iname
 *   5. g_free(tk)
 *
 * ICON LOADING PRIORITY
 * ---------------------
 * tk_update_icon() tries sources in this order:
 *   1. _NET_WM_ICON (ARGB pixel data) via get_netwm_icon()
 *   2. WM_HINTS icon_pixmap via get_wm_icon() (cairo-xlib path)
 *   3. tb->gen_pixbuf (built from default.xpm) as fallback
 *
 * Ownership: tk->pixbuf is a (transfer full) GdkPixbuf ref; replaced in
 * tk_update_icon by g_object_unref of the old value if it changes.
 *
 * GDK FILTER
 * ----------
 * Each task gets a per-window GdkWindow filter (tb_event_filter) via
 * gdk_window_add_filter(tk->gdkwin, ...).  This filter handles PropertyNotify
 * events on client windows (name changes, state changes, icon updates, desktop
 * changes).  Root-window events (client list, active window, etc.) arrive
 * through FbEv signals, not through this filter.
 * The filter is removed in del_task() via gdk_window_remove_filter before
 * g_object_unref(tk->gdkwin).
 *
 * DEFERRED RESIZE (pending_dim / pending_dim_id)
 * -----------------------------------------------
 * taskbar_size_alloc() must not call gtk_bar_set_dimension (which triggers
 * gtk_widget_queue_resize) from inside a size-allocate signal handler — this
 * causes stale allocations in GTK3.  Instead it stores the new dimension in
 * tb->pending_dim and schedules taskbar_apply_dim via g_idle_add, removing
 * any previously pending idle first via tb->pending_dim_id.
 */

#ifndef TASKBAR_PRIV_H
#define TASKBAR_PRIV_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <cairo/cairo-xlib.h>

/* The PLUGIN macro in plugin.h emits a __attribute__((constructor)) ctor()
 * that calls class_register(class_ptr) when the .so is dlopen'd.  For
 * single-file plugins this works fine, but taskbar is split across 4 TUs
 * that are all compiled with -DPLUGIN.  Without this undef, each TU gets
 * its own ctor() with its own NULL class_ptr -> class_register(NULL) ->
 * segfault at p->type (offset 0x20).  Suppress the macro here; taskbar.c
 * registers the class manually. */
#undef PLUGIN
#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "data/images/default.xpm"
#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"

/* Task and taskbar structs */
typedef struct _task task;
typedef struct _taskbar taskbar_priv;

/**
 * struct _task - per-window task entry.
 *
 * One task exists for each client window accepted by the taskbar.
 * Allocated with g_new0; freed in del_task().
 */
struct _task {
    struct _taskbar *tb;    /**< Back-pointer to the owning taskbar instance. */
    Window win;             /**< X11 window ID; used as hash table key. */
    GdkWindow *gdkwin;      /**< GDK wrapper for win; holds tb_event_filter.
                             *   Created with gdk_x11_window_foreign_new_for_display;
                             *   removed via gdk_window_remove_filter in del_task;
                             *   g_object_unref'd in del_task. */
    char *name;             /**< Window title with spaces: " Title " (g_strdup'd). */
    char *iname;            /**< Iconified title with brackets: "[Title]" (g_strdup'd).
                             *   Both name and iname are freed together in tk_free_names. */
    GtkWidget *button;      /**< GtkButton for this task; owned by tb->bar widget tree.
                             *   gtk_widget_destroy'd in del_task. */
    GtkWidget *label;       /**< GtkLabel inside button (NULL if icons_only). */
    GtkWidget *eb;          /**< Unused; kept for potential future use. */
    GtkWidget *image;       /**< GtkImage showing tk->pixbuf inside button. */
    GdkPixbuf *pixbuf;      /**< Current task icon; (transfer full) ref.
                             *   Replaced in tk_update_icon; old ref is g_object_unref'd. */

    int refcount;           /**< Reference count for tb_net_client_list stale removal.
                             *   Incremented when win still appears in _NET_CLIENT_LIST;
                             *   decremented in task_remove_stale; removed when it hits 0. */
    XClassHint ch;          /**< WM_CLASS hint (res_name + res_class); not currently used. */
    int pos_x;              /**< Unused; kept for potential future use. */
    int width;              /**< Unused; kept for potential future use. */
    guint desktop;          /**< Virtual desktop this window is on (_NET_WM_DESKTOP).
                             *   0xFFFFFFFF means "all desktops" (sticky). */
    net_wm_state nws;       /**< _NET_WM_STATE bitfield snapshot. */
    net_wm_window_type nwwt;/**< _NET_WM_WINDOW_TYPE bitfield snapshot. */
    guint flash_timeout;    /**< g_timeout_add source ID for urgency flash; 0 if not flashing. */
    unsigned int focused:1;         /**< Non-zero when this is the active (_NET_ACTIVE_WINDOW) task. */
    unsigned int iconified:1;       /**< Non-zero when the window is hidden/iconified. */
    unsigned int urgency:1;         /**< Non-zero when WM_HINTS has XUrgencyHint set. */
    unsigned int using_netwm_icon:1;/**< Non-zero if pixbuf came from _NET_WM_ICON. */
    unsigned int flash:1;           /**< Non-zero if urgency flash is active. */
    unsigned int flash_state:1;     /**< Current flash phase (toggles each interval). */
};

/**
 * struct _taskbar - taskbar plugin private state.
 *
 * Embedded at the start of plugin_instance (first field is plugin_instance plugin)
 * so that (taskbar_priv *)p == (plugin_instance *)p casts are safe.
 */
struct _taskbar {
    plugin_instance plugin;     /**< Must be first; plugin framework accesses this directly. */
    Window *wins;               /**< _NET_CLIENT_LIST window array; (transfer full) XFree.
                                 *   Refreshed on every tb_net_client_list call. */
    Window topxwin;             /**< X11 window ID of the panel window (for focus detection). */
    int win_num;                /**< Number of entries in wins[]. */
    GHashTable  *task_list;     /**< Hash table: Window -> task*.  Owns all task values.
                                 *   Key is &tk->win (pointer into the task struct).
                                 *   Destroyed in taskbar_destructor after all tasks removed. */
    GtkWidget *hbox;            /**< Unused; kept for potential future use. */
    GtkWidget *bar;             /**< GtkBar containing all task buttons; owned by p->pwid. */
    GtkWidget *space;           /**< Unused spacer; kept for potential future use. */
    GtkWidget *menu;            /**< Right-click context menu; gtk_widget_destroy'd in destructor. */
    GdkPixbuf *gen_pixbuf;      /**< Generic fallback icon from default.xpm; (transfer full) ref.
                                 *   g_object_ref'd when used as tk->pixbuf fallback. */
    GtkStateFlags normal_state; /**< GTK state flags for unfocused tasks (GTK_STATE_FLAG_NORMAL). */
    GtkStateFlags focused_state;/**< GTK state flags for focused task (GTK_STATE_FLAG_ACTIVE). */
    int num_tasks;              /**< Count of tasks in task_list (incremented/decremented with add/del). */
    int task_width;             /**< Unused; kept for potential future use. */
    int vis_task_num;           /**< Unused; kept for potential future use. */
    int req_width;              /**< Unused; kept for potential future use. */
    int hbox_width;             /**< Unused; kept for potential future use. */
    int spacing;                /**< Pixel spacing between task buttons in bar. */
    guint cur_desk;             /**< Current virtual desktop index (from _NET_CURRENT_DESKTOP). */
    task *focused;              /**< Currently focused task (active window); NULL if none. */
    task *ptk;                  /**< Previously focused task; used for iconify-on-refocus logic. */
    task *menutask;             /**< Task whose right-click context menu is currently open. */
    char **desk_names;          /**< Desktop name strings (g_strfreev'd in tb_update_desktops_names). */
    int desk_namesno;           /**< Number of desktop names in desk_names[]. */
    int desk_num;               /**< Total number of virtual desktops. */
    guint dnd_activate;         /**< g_timeout_add source ID for drag-over activation delay; 0 if none. */
    int alloc_no;               /**< Debug counter: number of currently allocated name pairs. */

    /* Config values (read from xconf in taskbar_constructor): */
    int iconsize;               /**< Computed icon pixel size (based on panel thickness and button overhead). */
    int task_width_max;         /**< Maximum task button width in pixels (config: maxtaskwidth). */
    int task_height_max;        /**< Maximum task button height in pixels (clamped to TASK_HEIGHT_MAX). */
    int accept_skip_pager;      /**< If non-zero, hide windows with _NET_WM_STATE_SKIP_PAGER. */
    int show_iconified;         /**< If non-zero, show iconified (minimised) windows. */
    int show_mapped;            /**< If non-zero, show mapped (visible) windows. */
    int show_all_desks;         /**< If non-zero, show windows from all virtual desktops. */
    int tooltips;               /**< If non-zero, show window title as button tooltip. */
    int icons_only;             /**< If non-zero, show only the icon (no text label). */
    int use_mouse_wheel;        /**< If non-zero, connect scroll-event for un/iconify. */
    int use_urgency_hint;       /**< If non-zero, flash buttons for windows with XUrgencyHint. */
    int discard_release_event;  /**< Set to 1 when Ctrl+RMB propagated to bar to suppress next release. */
    int     pending_dim;        /**< Dimension value waiting to be applied via idle callback. */
    guint   pending_dim_id;     /**< g_idle_add source ID for taskbar_apply_dim; 0 if none pending. */
};

/** Milliseconds before a drag-hover raises the target window. */
#define DRAG_ACTIVE_DELAY    1000

/** Default maximum task button width in pixels. */
#define TASK_WIDTH_MAX       200

/** Maximum task button height in pixels (hard cap; see taskbar_constructor). */
#define TASK_HEIGHT_MAX      28

/** Pixel padding inside task buttons (border + spacing). */
#define TASK_PADDING         4

/** Desktop value meaning "show on all desktops" (_NET_WM_DESKTOP == 0xFFFFFFFF). */
#define ALL_WORKSPACES       0xFFFFFFFF

/** TRUE if _NET_ACTIVE_WINDOW is in _NET_SUPPORTED; set by net_active_detect(). */
extern gboolean use_net_active;

/* For older Xlib headers that do not define XUrgencyHint. */
#ifndef XUrgencyHint
#define XUrgencyHint (1 << 8)
#endif

/* Cross-file function declarations */

/* taskbar_task.c */
int accept_net_wm_state(net_wm_state *nws, int accept_skip_pager);
int accept_net_wm_window_type(net_wm_window_type *nwwt);
int task_visible(taskbar_priv *tb, task *tk);
void tk_free_names(task *tk);
void tk_get_names(task *tk);
void tk_set_names(task *tk);
task *find_task(taskbar_priv *tb, Window win);
void del_task(taskbar_priv *tb, task *tk, int hdel);
gboolean task_remove_every(Window *win, task *tk);
gboolean task_remove_stale(Window *win, task *tk, gpointer data);
gboolean tk_has_urgency(task *tk);
void tk_update_icon(taskbar_priv *tb, task *tk, Atom a);
void tk_flash_window(task *tk);
void tk_unflash_window(task *tk);
void tk_raise_window(task *tk, guint32 time);

/* taskbar_net.c */
void net_active_detect(void);
void tb_net_client_list(GtkWidget *widget, taskbar_priv *tb);
void tb_net_current_desktop(GtkWidget *widget, taskbar_priv *tb);
void tb_net_number_of_desktops(GtkWidget *widget, taskbar_priv *tb);
void tb_net_active_window(GtkWidget *widget, taskbar_priv *tb);
GdkFilterReturn tb_event_filter(XEvent *, GdkEvent *, taskbar_priv *);

/* taskbar_ui.c */
void tk_display(taskbar_priv *tb, task *tk);
void tb_display(taskbar_priv *tb);
void tk_build_gui(taskbar_priv *tb, task *tk);
void tb_make_menu(GtkWidget *widget, taskbar_priv *tb);
void taskbar_build_gui(plugin_instance *p);

#endif /* TASKBAR_PRIV_H */
