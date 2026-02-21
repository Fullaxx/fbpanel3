/**
 * @file taskbar.c
 * @brief Taskbar plugin — constructor, destructor, and plugin class registration.
 *
 * CONSTRUCTOR SEQUENCE
 * --------------------
 * taskbar_constructor():
 *   1. Cast plugin_instance* p to taskbar_priv* tb (safe; plugin_instance is
 *      the first field of taskbar_priv).
 *   2. Probe button overhead (get_button_spacing).
 *   3. Detect _NET_ACTIVE_WINDOW support (net_active_detect).
 *   4. Set default config values, then override from xconf with XCG macros.
 *   5. Compute iconsize from panel thickness (ah for horizontal, aw for vertical)
 *      minus the button overhead height.  Clamp to >= 1.
 *   6. Build the GtkBar and plugin widget tree (taskbar_build_gui).
 *   7. Populate task list from the current _NET_CLIENT_LIST (tb_net_client_list).
 *   8. Refresh display (tb_display).
 *   9. Update focused window (tb_net_active_window).
 *
 * DESTRUCTOR SEQUENCE
 * -------------------
 * taskbar_destructor():
 *   1. Cancel any pending dim idle (pending_dim_id).
 *   2. Disconnect all FbEv signal handlers by func pointer.
 *   3. Remove all tasks from the hash table via task_remove_every (which calls
 *      del_task with hdel=0 for each task — per-window filters removed there).
 *   4. Destroy the hash table.
 *   5. XFree(tb->wins) if non-NULL.
 *   6. gtk_widget_destroy(tb->menu) — the bar itself is destroyed by the parent
 *      p->pwid destruction.
 *
 * NOTE ON MULTI-TU CLASS REGISTRATION
 * ------------------------------------
 * The PLUGIN macro is suppressed in taskbar_priv.h.  This file provides
 * a manual __attribute__((constructor)) ctor() and __attribute__((destructor))
 * dtor() for class_register / class_unregister, equivalent to what PLUGIN
 * would have generated for a single-file plugin.
 */

#include "taskbar_priv.h"


/**
 * taskbar_constructor - initialize the taskbar plugin instance.
 * @p: Plugin instance allocated by the framework (size = taskbar_priv.priv_size).
 *
 * See file-level docblock for the full constructor sequence.
 *
 * Returns: 1 on success (plugin framework convention).
 */
int
taskbar_constructor(plugin_instance *p)
{
    taskbar_priv *tb;
    GtkRequisition req;
    xconf *xc = p->xc;

    tb = (taskbar_priv *) p;
    get_button_spacing(&req, GTK_CONTAINER(p->pwid), "");
    net_active_detect();

    tb->topxwin           = p->panel->topxwin;
    tb->tooltips          = 1;
    tb->icons_only        = 0;
    tb->accept_skip_pager = 1;
    tb->show_iconified    = 1;
    tb->show_mapped       = 1;
    tb->show_all_desks    = 0;
    tb->task_width_max    = TASK_WIDTH_MAX;
    tb->task_height_max   = p->panel->max_elem_height;
    tb->task_list         = g_hash_table_new(g_int_hash, g_int_equal);
    tb->focused_state     = GTK_STATE_FLAG_ACTIVE;
    tb->normal_state      = GTK_STATE_FLAG_NORMAL;
    tb->spacing           = 0;
    tb->use_mouse_wheel   = 1;
    tb->use_urgency_hint  = 1;

    XCG(xc, "tooltips", &tb->tooltips, enum, bool_enum);
    XCG(xc, "iconsonly", &tb->icons_only, enum, bool_enum);
    XCG(xc, "acceptskippager", &tb->accept_skip_pager, enum, bool_enum);
    XCG(xc, "showiconified", &tb->show_iconified, enum, bool_enum);
    XCG(xc, "showalldesks", &tb->show_all_desks, enum, bool_enum);
    XCG(xc, "showmapped", &tb->show_mapped, enum, bool_enum);
    XCG(xc, "usemousewheel", &tb->use_mouse_wheel, enum, bool_enum);
    XCG(xc, "useurgencyhint", &tb->use_urgency_hint, enum, bool_enum);
    XCG(xc, "maxtaskwidth", &tb->task_width_max, int);

    /* FIXME: until per-plugin elem height limit is ready, lets
     * use hardcoded TASK_HEIGHT_MAX pixels */
    if (tb->task_height_max > TASK_HEIGHT_MAX)
        tb->task_height_max = TASK_HEIGHT_MAX;
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        tb->iconsize = MIN(p->panel->ah, tb->task_height_max) - req.height;
        if (tb->iconsize < 1) tb->iconsize = 1;
        if (tb->icons_only)
            tb->task_width_max = tb->iconsize + req.width;
    } else {
        if (p->panel->aw <= 30)
            tb->icons_only = 1;
        tb->iconsize = MIN(p->panel->aw, tb->task_height_max) - req.height;
        if (tb->iconsize < 1) tb->iconsize = 1;
        if (tb->icons_only)
            tb->task_width_max = tb->iconsize + req.height;
    }
    taskbar_build_gui(p);
    tb_net_client_list(NULL, tb);
    tb_display(tb);
    tb_net_active_window(NULL, tb);
    return 1;
}


/**
 * taskbar_destructor - tear down the taskbar plugin instance.
 * @p: Plugin instance.
 *
 * See file-level docblock for the full destructor sequence.
 * The GtkBar (tb->bar) is not explicitly destroyed here — it is a child of
 * p->pwid and will be destroyed when the plugin framework destroys p->pwid.
 */
static void
taskbar_destructor(plugin_instance *p)
{
    taskbar_priv *tb = (taskbar_priv *) p;

    if (tb->pending_dim_id) {
        g_source_remove(tb->pending_dim_id);
        tb->pending_dim_id = 0;
    }
    /* Per-window filters are removed in del_task via task_remove_every below. */
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_current_desktop, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_active_window, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_number_of_desktops, tb);
    g_signal_handlers_disconnect_by_func(G_OBJECT (fbev),
            tb_net_client_list, tb);

    g_hash_table_foreach_remove(tb->task_list, (GHRFunc) task_remove_every,
            NULL);
    g_hash_table_destroy(tb->task_list);
    if (tb->wins)
        XFree(tb->wins);
    //gtk_widget_destroy(tb->bar); // destroy of p->pwid does it all
    gtk_widget_destroy(tb->menu);
    DBG("alloc_no=%d\n", tb->alloc_no);
    return;
}

static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "taskbar",
    .name        = "Taskbar",
    .version     = "1.0",
    .description = "Shows opened windows",
    .priv_size   = sizeof(taskbar_priv),

    .constructor = taskbar_constructor,
    .destructor  = taskbar_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;

/* Manual class registration — the PLUGIN macro ctor/dtor are suppressed in
 * taskbar_priv.h because this plugin spans 4 TUs (see comment there). */
static void ctor(void) __attribute__ ((constructor));
static void ctor(void) { class_register(class_ptr); }
static void dtor(void) __attribute__ ((destructor));
static void dtor(void) { class_unregister(class_ptr); }
