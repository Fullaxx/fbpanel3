
/*
 * plugin.h — fbpanel plugin API
 *
 * LIFECYCLE
 * ---------
 * When the panel starts (or a plugin is dynamically loaded), the panel:
 *   1. Opens the plugin's shared library via GModule.
 *   2. Locates the global `class_ptr` symbol (a pointer to the plugin's
 *      `plugin_class`).  The shared-library constructor `ctor()` defined
 *      below calls `class_register()` automatically on dlopen.
 *   3. Allocates `priv_size` bytes for the plugin's private state.  The
 *      first `sizeof(plugin_instance)` bytes are the public `plugin_instance`
 *      fields; the remaining bytes are the plugin's private struct members.
 *      Plugins cast the `plugin_instance *` pointer to their own struct:
 *          my_priv *priv = (my_priv *) plugin_instance_ptr;
 *   4. Creates a GtkEventBox (`pwid`) and adds it to the panel bar.
 *   5. Calls `constructor(plugin_instance *)`.  At this point `pwid` is
 *      already realized and added to the bar.  The plugin must populate
 *      `pwid` with its own child widgets.
 *   6. On panel exit or plugin removal, calls `destructor(plugin_instance *)`.
 *      The plugin must free its own resources but must NOT destroy `pwid`
 *      itself — the panel owns that.
 *
 * PLUGIN_INSTANCE FIELDS PROVIDED BY THE PANEL
 * ---------------------------------------------
 *   panel   *panel   — the panel this plugin lives in (orientation, size, …)
 *   xconf   *xc      — the parsed config subtree for this plugin
 *   GtkWidget *pwid  — the container widget (GtkEventBox) the panel created
 *   expand           — whether to expand to fill remaining space
 *   padding, border  — extra spacing (pixels) around the plugin widget
 *
 * MINIMAL PLUGIN EXAMPLE
 * ----------------------
 * A clock-like plugin that shows a label:
 *
 *   #include "plugin.h"
 *
 *   typedef struct {
 *       plugin_instance plugin;   // must be first
 *       GtkWidget *label;
 *   } my_priv;
 *
 *   static int my_constructor(plugin_instance *p) {
 *       my_priv *priv = (my_priv *) p;
 *       priv->label = gtk_label_new("Hello");
 *       gtk_widget_show(priv->label);
 *       gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
 *       return 1;   // 0 = failure, non-zero = success
 *   }
 *
 *   static void my_destructor(plugin_instance *p) {
 *       // GtkWidgets are destroyed by GTK when pwid is destroyed.
 *       // Free any other private resources here.
 *   }
 *
 *   static plugin_class class = {
 *       .type        = "myplugin",
 *       .name        = "My Plugin",
 *       .version     = "1.0",
 *       .description = "Example plugin",
 *       .priv_size   = sizeof(my_priv),
 *       .constructor = my_constructor,
 *       .destructor  = my_destructor,
 *   };
 *   static plugin_class *class_ptr = (plugin_class *) &class;
 */

#ifndef PLUGIN_H
#define PLUGIN_H
#include <gmodule.h>


#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <stdio.h>
#include "panel.h"

struct _plugin_instance;

typedef struct {
    /* Panel-managed fields — do not set these from the plugin */
    char *fname;        /* path to the plugin .so file, or NULL if built-in */
    int count;          /* reference count (managed by panel) */
    GModule *gmodule;   /* GModule handle for the loaded .so, or NULL */

    int dynamic : 1;    /* 1 if loaded from a .so at runtime */
    int invisible : 1;  /* 1 if the plugin has no visible widget */

    /* Plugin identity — set these in the static plugin_class definition */
    char *type;         /* short identifier used in config files, e.g. "taskbar" */
    char *name;         /* human-readable name shown in preferences */
    char *version;      /* version string, e.g. "1.0" */
    char *description;  /* one-line description */
    int priv_size;      /* sizeof(your_priv_struct); the panel allocates this */

    /* Callbacks — set constructor and destructor; the rest are optional */
    int (*constructor)(struct _plugin_instance *this);
    void (*destructor)(struct _plugin_instance *this);
    void (*save_config)(struct _plugin_instance *this, FILE *fp);
    GtkWidget *(*edit_config)(struct _plugin_instance *this);
} plugin_class;

#define PLUGIN_CLASS(class) ((plugin_class *) class)

typedef struct _plugin_instance{
    plugin_class *class;    /* back-pointer to the class descriptor */
    panel        *panel;    /* the panel this plugin lives in */
    xconf        *xc;       /* parsed config subtree for this plugin instance */
    GtkWidget    *pwid;     /* GtkEventBox container widget (panel-owned) */
    int           expand;   /* expand to fill remaining bar space */
    int           padding;  /* extra space (px) on each side of pwid */
    int           border;   /* border width (px) inside pwid */
} plugin_instance;

void class_put(char *name);
gpointer class_get(char *name);
/* if plugin_instance is external it will load its dll */
plugin_instance * plugin_load(char *type);
void plugin_put(plugin_instance *this);
int plugin_start(plugin_instance *this);
void plugin_stop(plugin_instance *this);
GtkWidget *default_plugin_instance_edit_config(plugin_instance *pl);

extern void class_register(plugin_class *p);
extern void class_unregister(plugin_class *p);

/*
 * PLUGIN macro — compiled into every plugin .so.
 * Defines a module constructor/destructor that automatically calls
 * class_register / class_unregister when the .so is dlopen'd / dlclose'd.
 * The plugin must define a file-scope `static plugin_class *class_ptr`
 * pointing at its plugin_class struct for this to work.
 */
#ifdef PLUGIN
static plugin_class *class_ptr;
static void ctor(void) __attribute__ ((constructor));
static void ctor(void) { class_register(class_ptr); }
static void dtor(void) __attribute__ ((destructor));
static void dtor(void) { class_unregister(class_ptr); }
#endif

#endif
