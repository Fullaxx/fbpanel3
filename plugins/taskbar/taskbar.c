#include "taskbar_priv.h"

static const gchar *taskbar_css =
    "#taskbar button { padding: 0; margin: 0; outline-width: 0; }";

int
taskbar_constructor(plugin_instance *p)
{
    taskbar_priv *tb;
    GtkRequisition req;
    xconf *xc = p->xc;

    tb = (taskbar_priv *) p;
    gtk_widget_set_name(p->pwid, "taskbar");
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, taskbar_css, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
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
