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

static void
space_destructor(plugin_instance *p)
{
    return;
}

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
