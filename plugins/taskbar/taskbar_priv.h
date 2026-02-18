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
 * its own ctor() with its own NULL class_ptr → class_register(NULL) →
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

struct _task {
    struct _taskbar *tb;
    Window win;
    char *name, *iname;
    GtkWidget *button, *label, *eb;
    GtkWidget *image;
    GdkPixbuf *pixbuf;

    int refcount;
    XClassHint ch;
    int pos_x;
    int width;
    guint desktop;
    net_wm_state nws;
    net_wm_window_type nwwt;
    guint flash_timeout;
    unsigned int focused:1;
    unsigned int iconified:1;
    unsigned int urgency:1;
    unsigned int using_netwm_icon:1;
    unsigned int flash:1;
    unsigned int flash_state:1;
};

struct _taskbar {
    plugin_instance plugin;
    Window *wins;
    Window topxwin;
    int win_num;
    GHashTable  *task_list;
    GtkWidget *hbox, *bar, *space, *menu;
    GdkPixbuf *gen_pixbuf;
    GtkStateFlags normal_state;
    GtkStateFlags focused_state;
    int num_tasks;
    int task_width;
    int vis_task_num;
    int req_width;
    int hbox_width;
    int spacing;
    guint cur_desk;
    task *focused;
    task *ptk;
    task *menutask;
    char **desk_names;
    int desk_namesno;
    int desk_num;
    guint dnd_activate;
    int alloc_no;

    int iconsize;
    int task_width_max;
    int task_height_max;
    int accept_skip_pager;
    int show_iconified;
    int show_mapped;
    int show_all_desks;
    int tooltips;
    int icons_only;
    int use_mouse_wheel;
    int use_urgency_hint;
    int discard_release_event;
};

/* Constants */
#define DRAG_ACTIVE_DELAY    1000
#define TASK_WIDTH_MAX       200
#define TASK_HEIGHT_MAX      28
#define TASK_PADDING         4
#define ALL_WORKSPACES       0xFFFFFFFF

/* Shared globals */
extern gboolean use_net_active;

/* For older Xlib headers */
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
