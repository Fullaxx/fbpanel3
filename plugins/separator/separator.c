/**
 * @file separator.c
 * @brief Separator line plugin for fbpanel.
 *
 * Adds a single GtkSeparator (vertical line on a horizontal panel,
 * horizontal line on a vertical panel) to the panel box.  The separator
 * is created via panel->my_separator_new() so it respects the panel
 * orientation automatically.
 *
 * Config keys: none.
 * Main widget: GtkSeparator (owned by the GtkBgbox parent pwid).
 */

#include "panel.h"
#include "misc.h"
#include "plugin.h"




/**
 * separator_constructor - create and pack the separator widget.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Calls panel->my_separator_new() to create an orientation-aware
 * GtkSeparator and adds it to p->pwid.  The separator is owned by pwid
 * and is destroyed with it.
 *
 * Returns: 1 on success.
 */
static int
separator_constructor(plugin_instance *p)
{
    GtkWidget *sep;

    sep = p->panel->my_separator_new();
    gtk_container_add(GTK_CONTAINER(p->pwid), sep);
    gtk_widget_show_all(p->pwid);
    return 1;
}

/**
 * separator_destructor - no-op; all cleanup is handled by GTK widget destruction.
 * @p: plugin_instance. (transfer none)
 */
static void
separator_destructor(plugin_instance *p)
{
    return;
}


static plugin_class class = {
    .count = 0,
    .type        = "separator",
    .name        = "Separator",
    .version     = "1.0",
    .description = "Separator line",
    .priv_size   = sizeof(plugin_instance),

    .constructor = separator_constructor,
    .destructor  = separator_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
