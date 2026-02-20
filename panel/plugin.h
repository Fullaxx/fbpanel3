/**
 * @file plugin.h
 * @brief fbpanel plugin API — class descriptor, instance struct, and loader.
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
 *   4. Creates a GtkBgbox (`pwid`) and adds it to the panel bar.
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
 *   GtkWidget *pwid  — the container widget (GtkBgbox) the panel created
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

/**
 * plugin_class - descriptor for a plugin type, shared across all instances.
 *
 * Plugins define one static instance of this struct and point class_ptr at it.
 * The panel reads it after dlopen (via class_register) to learn the plugin's
 * identity, memory requirements, and callbacks.
 *
 * Fields set by the panel (do not set from plugin code):
 * @fname:    Path to the plugin .so file, or NULL for built-in plugins.
 *            Owned by the panel; do not g_free.
 * @count:    Reference count managed by class_get / class_put.
 *            Tracks how many instances of this class are active.
 * @gmodule:  GModule handle from g_module_open(); NULL for built-in plugins.
 *            Closed by class_put() when count drops to zero.
 * @dynamic:  Set to 1 if the class was registered after panel startup
 *            (i.e., the .so was opened at runtime).  Used by class_put()
 *            to decide whether to call g_module_close().
 * @invisible: Set to 1 if the plugin has no visible widget.  plugin_start()
 *            creates a hidden GtkBox placeholder instead of a GtkBgbox.
 *
 * Fields set by the plugin in its static plugin_class definition:
 * @type:        Short identifier used in config files (e.g. "taskbar").
 *               Must be unique; class_register() calls exit(1) on collision.
 * @name:        Human-readable plugin name shown in the Preferences UI.
 * @version:     Version string (e.g. "1.0").
 * @description: One-line description shown in the Preferences UI.
 * @priv_size:   sizeof(your_private_struct).  The panel allocates this many
 *               bytes for each instance; the first sizeof(plugin_instance)
 *               bytes are the public fields, the rest are plugin-private.
 * @constructor: Called after pwid is created and packed into the bar.
 *               Returns non-zero on success, 0 on failure (plugin skipped).
 * @destructor:  Called before pwid is destroyed.  Must free private resources;
 *               must NOT destroy pwid (the panel destroys it).
 * @save_config: Optional; called when the user saves the preferences dialog.
 *               May be NULL.
 * @edit_config: Optional; returns a GtkWidget for the plugin's preferences
 *               page.  May be NULL (panel uses default_plugin_edit_config).
 */
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

/** Cast any pointer to plugin_class*; used for built-in class tables. */
#define PLUGIN_CLASS(class) ((plugin_class *) class)

/**
 * plugin_instance - per-instance state visible to both the panel and the plugin.
 *
 * Each loaded plugin receives a block of priv_size bytes allocated by the panel.
 * The first sizeof(plugin_instance) bytes are this public struct; the remaining
 * bytes are the plugin's private data.  Plugins access private fields by casting:
 *     my_priv *priv = (my_priv *) plugin_instance_ptr;
 *
 * All fields below are set by the panel before constructor() is called.
 *
 * @class:   Back-pointer to the plugin_class descriptor.  (transfer none)
 * @panel:   The panel this plugin lives in; valid for the plugin's lifetime.
 *           (transfer none)
 * @xc:      The parsed xconf subtree for this plugin's Config {} block.
 *           (transfer none) owned by the panel's config tree; do not xconf_del.
 *           Use XCG() to read values; see docs/XCONF_REFERENCE.md.
 * @pwid:    The GtkBgbox (or hidden GtkBox for invisible plugins) created by
 *           plugin_start() and packed into panel->box.  Owned by the panel.
 *           Plugins must NOT destroy pwid; the panel destroys it in plugin_stop().
 *           Child widgets added to pwid are destroyed automatically with it.
 * @expand:  If non-zero, gtk_box_pack_start expands pwid to fill remaining space.
 * @padding: Extra empty pixels added on each side of pwid within the bar.
 * @border:  Border width (pixels) set on the GtkContainer (pwid).
 */
typedef struct _plugin_instance{
    plugin_class *class;    /* back-pointer to the class descriptor */
    panel        *panel;    /* the panel this plugin lives in */
    xconf        *xc;       /* parsed config subtree for this plugin instance */
    GtkWidget    *pwid;     /* GtkBgbox container widget (panel-owned) */
    int           expand;   /* expand to fill remaining bar space */
    int           padding;  /* extra space (px) on each side of pwid */
    int           border;   /* border width (px) inside pwid */
} plugin_instance;

/**
 * class_put - decrement the reference count for plugin class @name.
 * @name: The plugin type string (e.g. "taskbar").
 *
 * If the count drops to zero and the class is dynamic (loaded from a .so),
 * closes the GModule.  The module's __attribute__((destructor)) then fires,
 * calling class_unregister() automatically.
 */
void class_put(char *name);

/**
 * class_get - look up or load the plugin class named @name.
 * @name: The plugin type string (e.g. "taskbar").
 *
 * First checks the class hash table.  If not found, attempts to open
 * LIBDIR/lib<name>.so via g_module_open().  The module's
 * __attribute__((constructor)) then fires, calling class_register().
 * Increments the class reference count on success.
 *
 * Returns: (transfer none) pointer to the plugin_class, or NULL on failure.
 */
gpointer class_get(char *name);

/**
 * plugin_load - allocate a plugin instance for class @type.
 * @type: The plugin type string (e.g. "taskbar").
 *
 * Calls class_get() to load or look up the class, then allocates
 * priv_size bytes zeroed memory for the instance.  Sets instance->class.
 * Does NOT set panel, xc, pwid, expand, padding, or border — the caller
 * (panel_start_gui) sets those before calling plugin_start().
 *
 * Returns: (transfer full) new plugin_instance, or NULL if the class
 *   cannot be found.  Caller must eventually call plugin_put().
 */
plugin_instance * plugin_load(char *type);

/**
 * plugin_put - free a plugin instance and release the class reference.
 * @this: Plugin instance allocated by plugin_load().
 *
 * Calls g_free(this) then class_put(type).  Must only be called after
 * plugin_stop() has already destroyed pwid and called the destructor.
 */
void plugin_put(plugin_instance *this);

/**
 * plugin_start - create pwid, pack it into the bar, and call constructor.
 * @this: Plugin instance with panel, xc, expand, padding, border set.
 *
 * For visible plugins (invisible == 0):
 *   - Creates a GtkBgbox, names it after the plugin type.
 *   - Packs it into panel->box with gtk_box_pack_start.
 *   - Sets background mode if the panel is transparent.
 *   - Connects the panel right-click context menu handler.
 *   - Shows the widget.
 *
 * For invisible plugins (invisible == 1):
 *   - Creates a hidden GtkBox placeholder to maintain index consistency
 *     in panel->box (required for child reordering in the Preferences UI).
 *
 * Then calls this->class->constructor(this).  If constructor returns 0,
 * destroys pwid and returns 0 (failure).
 *
 * Returns: 1 on success, 0 on constructor failure.
 */
int plugin_start(plugin_instance *this);

/**
 * plugin_stop - call destructor and destroy pwid.
 * @this: Plugin instance to stop.
 *
 * Calls this->class->destructor(this), decrements panel->plug_num,
 * then calls gtk_widget_destroy(this->pwid).  The destructor must
 * NOT destroy pwid itself.
 *
 * After plugin_stop(), call plugin_put() to free the instance memory.
 */
void plugin_stop(plugin_instance *this);

/**
 * default_plugin_edit_config - fallback Preferences widget for plugins
 *   that do not implement edit_config.
 * @pl: Plugin instance.
 *
 * Returns: (transfer full) GtkWidget showing a "not implemented" message
 *   with instructions for manual config editing.
 *
 * NOTE: declared as default_plugin_instance_edit_config in this header
 * but defined as default_plugin_edit_config in plugin.c — name mismatch.
 * See BUG-007 in docs/BUGS_AND_ISSUES.md.
 */
GtkWidget *default_plugin_instance_edit_config(plugin_instance *pl);

/**
 * class_register - add @p to the plugin class registry.
 * @p: Pointer to the plugin's static plugin_class struct.
 *
 * Called automatically by the PLUGIN macro's __attribute__((constructor))
 * when a plugin .so is dlopen'd.  Also called for built-in plugins during
 * startup before the_panel is set.
 *
 * Sets p->dynamic = (the_panel != NULL) to distinguish runtime-loaded
 * plugins from those registered before the panel window is created.
 * Calls exit(1) if a class with the same type name is already registered.
 */
extern void class_register(plugin_class *p);

/**
 * class_unregister - remove @p from the plugin class registry.
 * @p: Pointer to the plugin_class to unregister.
 *
 * Called automatically by the PLUGIN macro's __attribute__((destructor))
 * when a plugin .so is dlclose'd.  Destroys the hash table when empty.
 */
extern void class_unregister(plugin_class *p);

/**
 * PLUGIN macro - compiled into every plugin .so.
 *
 * Defines module constructor/destructor functions that automatically call
 * class_register / class_unregister when the .so is dlopen'd / dlclose'd.
 *
 * The plugin must define a file-scope:
 *     static plugin_class *class_ptr = &my_class;
 *
 * The constructor fires during g_module_open() (before any explicit code
 * runs), and the destructor fires during g_module_close().
 *
 * Usage: place `#define PLUGIN` before `#include "plugin.h"` in the
 * plugin's .c file, or pass -DPLUGIN on the compiler command line.
 */
#ifdef PLUGIN
static plugin_class *class_ptr;
static void ctor(void) __attribute__ ((constructor));
static void ctor(void) { class_register(class_ptr); }
static void dtor(void) __attribute__ ((destructor));
static void dtor(void) { class_unregister(class_ptr); }
#endif

#endif
