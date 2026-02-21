/**
 * @file space.c
 * @brief Fixed-size blank space plugin for fbpanel.
 *
 * Reserves a configurable amount of blank space in the panel by calling
 * gtk_widget_set_size_request() on p->pwid.  Useful for padding between
 * other plugins.
 *
 * Config keys:
 *   size  (int, pixels, default 1) — width (horizontal panel) or
 *          height (vertical panel) of the blank region.
 *
 * Main widget: p->pwid itself (GtkBgbox); no child widget is created.
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"



typedef struct {
    plugin_instance plugin;
    int size;
    GtkWidget *mainw;

} space_priv;

/**
 * space_destructor - no-op; all cleanup is handled by GTK widget destruction.
 * @p: plugin_instance. (transfer none)
 */
static void
space_destructor(plugin_instance *p)
{
    return;
}

/**
 * space_constructor - set the size request on pwid to create blank space.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads the "size" config key (default 1 pixel).  For a horizontal panel,
 * sets width=size, height=2.  For a vertical panel, sets width=2, height=size.
 * The minimum dimension of 2 keeps the widget visible to GTK.
 *
 * Returns: 1 on success.
 */
static int
space_constructor(plugin_instance *p)
{
    int w, h, size;

    size = 1;
    XCG(p->xc, "size", &size, int);

    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        h = 2;
        w = size;
    } else {
        w = 2;
        h = size;
    }
    gtk_widget_set_size_request(p->pwid, w, h);
    return 1;
}

static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "space",
    .name        = "Space",
    .version     = "1.0",
    .description = "Ocupy space in a panel",
    .priv_size   = sizeof(space_priv),

    .constructor = space_constructor,
    .destructor  = space_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
