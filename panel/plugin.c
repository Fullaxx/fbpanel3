/**
 * @file plugin.c
 * @brief Plugin class registry and instance lifecycle — implementation.
 *
 * REGISTRY
 * --------
 * class_ht is a GHashTable<type_string → plugin_class*> that tracks all
 * registered plugin classes.  It is created on first registration and
 * destroyed when the last class is unregistered.
 *
 * Built-in classes: registered before the_panel is set (dynamic = 0).
 *   class_put() never calls g_module_close() for built-in classes.
 *
 * Dynamic classes: registered after dlopen (dynamic = 1).
 *   class_put() calls g_module_close() twice when count → 0:
 *   once to undo the initial open in class_get(), and once more to
 *   trigger the destructor (__attribute__((destructor))) that calls
 *   class_unregister().
 *
 * INSTANCE LIFECYCLE
 * ------------------
 *   plugin_load()  → allocates priv_size bytes; sets instance->class
 *   plugin_start() → creates pwid; calls constructor()
 *   plugin_stop()  → calls destructor(); destroys pwid
 *   plugin_put()   → g_free(instance); class_put()
 *
 * pwid OWNERSHIP
 * --------------
 * plugin_start() creates pwid and adds it to panel->box (container takes
 * the floating ref).  plugin_stop() calls gtk_widget_destroy(pwid), which
 * triggers GTK's recursive child destruction.  Plugins must NOT destroy
 * pwid in their destructor.
 */

#include "plugin.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdlib.h>



#include "misc.h"
#include "bg.h"
#include "gtkbgbox.h"


//#define DEBUGPRN
#include "dbg.h"

/** Back-reference to the running panel; set before gtk_main(). */
extern panel *the_panel;


/**************************************************************/

/**
 * class_ht - global hash table mapping type strings to plugin_class pointers.
 *
 * Keys:   plugin_class::type (const char*; owned by the plugin_class struct).
 * Values: plugin_class* (not owned by the table; owned by the .so or static).
 *
 * Created on first class_register() call; destroyed when the last class
 * is unregistered via class_unregister().
 */
static GHashTable *class_ht;


/**
 * class_register - add @p to the plugin class registry.
 * @p: Plugin class descriptor.  p->type must be unique.
 *
 * Sets p->dynamic based on whether the_panel is already initialised
 * (post-startup dynamic load) or not (built-in, registered at startup).
 *
 * Calls exit(1) on duplicate type name — plugin type strings must be
 * globally unique across all loaded .so files.
 *
 * Called automatically by the PLUGIN macro's __attribute__((constructor))
 * on dlopen, and also manually for built-in classes during startup.
 */
void
class_register(plugin_class *p)
{
    if (!class_ht) {
        class_ht = g_hash_table_new(g_str_hash, g_str_equal);
        DBG("creating class hash table\n");
    }
    DBG("registering %s\n", p->type);
    if (g_hash_table_lookup(class_ht, p->type)) {
        ERR("Can't register plugin %s. Such name already exists.\n", p->type);
        exit(1);
    }
    p->dynamic = (the_panel != NULL); /* dynamic modules register after main */
    g_hash_table_insert(class_ht, p->type, p);
    return;
}

/**
 * class_unregister - remove @p from the plugin class registry.
 * @p: Plugin class to unregister.
 *
 * Destroys the hash table when it becomes empty (no registered classes).
 * Called automatically by the PLUGIN macro's __attribute__((destructor))
 * on dlclose.
 */
void
class_unregister(plugin_class *p)
{
    DBG("unregistering %s\n", p->type);
    if (!g_hash_table_remove(class_ht, p->type)) {
        ERR("Can't unregister plugin %s. No such name\n", p->type);
    }
    if (!g_hash_table_size(class_ht)) {
        DBG("dwstroying class hash table\n");
        g_hash_table_destroy(class_ht);
        class_ht = NULL;
    }
    return;
}

/**
 * class_put - decrement the reference count for class @name.
 * @name: Plugin type string (e.g. "taskbar").
 *
 * If count > 0 after decrement, or if the class is not dynamic, returns
 * immediately.  Otherwise opens the .so path a second time so there are
 * two open handles, then closes both.  The second close triggers the
 * __attribute__((destructor)) which calls class_unregister().
 *
 * Note: the double-open/double-close pattern is intentional — the first
 * open in class_get() incremented GModule's internal refcount; the extra
 * open here provides a second handle so both can be closed.
 */
void
class_put(char *name)
{
    GModule *m;
    gchar *s;
    plugin_class *tmp;

    DBG("%s\n", name);
    if (!(class_ht && (tmp = g_hash_table_lookup(class_ht, name))))
        return;
    tmp->count--;
    if (tmp->count || !tmp->dynamic)
        return;

    s = g_strdup_printf(LIBDIR "/lib%s.so", name);
    DBG("loading module %s\n", s);
    m = g_module_open(s, G_MODULE_BIND_LAZY);
    g_free(s);
    if (m) {
        /* Close it twice to undo initial open in class_get */
        g_module_close(m);
        g_module_close(m);
    }
    return;
}

/**
 * class_get - look up or dynamically load the plugin class named @name.
 * @name: Plugin type string (e.g. "taskbar").
 *
 * Algorithm:
 *   1. If class_ht already contains @name, increments count and returns it.
 *   2. Otherwise constructs the .so path (LIBDIR/lib<name>.so) and calls
 *      g_module_open().  The module's __attribute__((constructor)) fires,
 *      calling class_register() which inserts the class into class_ht.
 *   3. Looks up the newly registered class, increments count, and returns it.
 *
 * Returns: (transfer none) pointer into class_ht, or NULL if the .so cannot
 *   be opened or does not register the expected class name.
 *   Logs g_module_error() on failure.
 */
gpointer
class_get(char *name)
{
    GModule *m;
    plugin_class *tmp;
    gchar *s;

    DBG("%s\n", name);
    if (class_ht && (tmp = g_hash_table_lookup(class_ht, name))) {
        DBG("found\n");
        tmp->count++;
        return tmp;
    }
    s = g_strdup_printf(LIBDIR "/lib%s.so", name);
    DBG("loading module %s\n", s);
    m = g_module_open(s, G_MODULE_BIND_LAZY);
    g_free(s);
    if (m) {
        if (class_ht && (tmp = g_hash_table_lookup(class_ht, name))) {
            DBG("found\n");
            tmp->count++;
            return tmp;
        }
    }
    ERR("%s\n", g_module_error());
    return NULL;
}


/**************************************************************/

/**
 * plugin_load - allocate a plugin instance for class @type.
 * @type: Plugin type string (e.g. "taskbar").
 *
 * Calls class_get() to obtain (and refcount) the class descriptor, then
 * allocates priv_size bytes of zeroed memory.  The first
 * sizeof(plugin_instance) bytes are the public plugin_instance fields;
 * the remainder are plugin-private.
 *
 * Sets instance->class.  The caller (panel_start_gui) must set panel,
 * xc, expand, padding, and border before calling plugin_start().
 *
 * Returns: (transfer full) new plugin_instance, or NULL if class_get()
 *   fails.  Caller must eventually call plugin_stop() then plugin_put().
 */
plugin_instance *
plugin_load(char *type)
{
    plugin_class *pc = NULL;
    plugin_instance  *pp = NULL;

    /* nothing was found */
    if (!(pc = class_get(type)))
        return NULL;

    DBG("%s priv_size=%d\n", pc->type, pc->priv_size);
    pp = g_malloc0(pc->priv_size);
    g_return_val_if_fail (pp != NULL, NULL);
    pp->class = pc;
    return pp;
}


/**
 * plugin_put - free a plugin instance and release its class reference.
 * @this: Plugin instance previously returned by plugin_load().
 *
 * Calls g_free(this) first, then class_put(type) which may trigger
 * g_module_close() if the class was dynamically loaded and count → 0.
 *
 * Must only be called after plugin_stop() has already destroyed pwid.
 * Accessing @this after this call is undefined behaviour.
 */
void
plugin_put(plugin_instance *this)
{
    gchar *type;

    type = this->class->type;
    g_free(this);
    class_put(type);
    return;
}

/** Forward declaration for the panel right-click handler in panel.c. */
gboolean panel_button_press_event(GtkWidget *widget, GdkEventButton *event,
        panel *p);

/**
 * plugin_start - create the plugin container widget and call the constructor.
 * @this: Plugin instance with panel, xc, expand, padding, and border set.
 *
 * For visible plugins (class->invisible == 0):
 *   - Creates a GtkBgbox named after class->type and packs it into panel->box.
 *   - Sets BG_INHERIT background if the panel is transparent.
 *   - Connects the panel right-click button-press handler.
 *   - Shows the widget.
 *
 * For invisible plugins (class->invisible != 0):
 *   - Creates a hidden GtkBox placeholder to maintain index alignment in
 *     panel->box (required so Preferences child reordering stays consistent).
 *
 * Calls this->class->constructor(this).  If the constructor returns 0
 * (failure), destroys pwid and returns 0.
 *
 * Returns: 1 on success, 0 if the constructor fails.
 */
int
plugin_start(plugin_instance *this)
{

    DBG("%s\n", this->class->type);
    if (!this->class->invisible) {
        this->pwid = gtk_bgbox_new();
        gtk_widget_set_name(this->pwid, this->class->type);
        gtk_box_pack_start(GTK_BOX(this->panel->box), this->pwid, this->expand,
                TRUE, this->padding);
        DBG("%s expand %d\n", this->class->type, this->expand);
        gtk_container_set_border_width(GTK_CONTAINER(this->pwid), this->border);
        DBG("here this->panel->transparent = %d\n", this->panel->transparent);
        if (this->panel->transparent) {
            DBG("here g\n");
            gtk_bgbox_set_background(this->pwid, BG_INHERIT,
                    this->panel->tintcolor, this->panel->alpha);
        }
        DBG("here\n");
        g_signal_connect (G_OBJECT (this->pwid), "button-press-event",
              (GCallback) panel_button_press_event, this->panel);
        gtk_widget_show(this->pwid);
        DBG("here\n");
    } else {
        /* create a no-window widget and do not show it; it's useful to have
         * an unmapped widget for invisible plugins so their indexes in the
         * plugin list stay aligned with panel->box children (required for
         * child reordering in the Preferences dialog). */
        this->pwid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(this->panel->box), this->pwid, FALSE,
                TRUE,0);
        gtk_widget_hide(this->pwid);
    }
    DBG("here\n");
    if (!this->class->constructor(this)) {
        DBG("here\n");
        gtk_widget_destroy(this->pwid);
        return 0;
    }
    return 1;
}


/**
 * plugin_stop - call the plugin destructor and destroy the container widget.
 * @this: Plugin instance to stop.
 *
 * Calls this->class->destructor(this) first, giving the plugin a chance
 * to disconnect signal handlers and free private resources.  Then
 * decrements panel->plug_num and calls gtk_widget_destroy(this->pwid),
 * which recursively destroys all child widgets the plugin created inside pwid.
 *
 * After plugin_stop(), call plugin_put() to free the instance memory.
 * Do not access @this->pwid after this call.
 */
void
plugin_stop(plugin_instance *this)
{
    DBG("%s\n", this->class->type);
    this->class->destructor(this);
    this->panel->plug_num--;
    gtk_widget_destroy(this->pwid);
    return;
}


/**
 * default_plugin_edit_config - fallback Preferences widget for plugins
 *   without a custom edit_config callback.
 * @pl: Plugin instance whose class->name is shown in the message.
 *
 * Returns: (transfer full) GtkWidget (GtkBox) containing a GtkLabel with
 *   a human-readable message directing the user to edit the config file
 *   manually.
 *
 * NOTE: this function is defined here as default_plugin_edit_config but
 * declared in plugin.h as default_plugin_instance_edit_config.  The names
 * do not match, so calls to the header-declared name are unresolved.
 * See BUG-007 in docs/BUGS_AND_ISSUES.md.
 */
GtkWidget *
default_plugin_edit_config(plugin_instance *pl)
{
    GtkWidget *vbox, *label;
    gchar *msg;

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* XXX: harcoded default profile name */
    msg = g_strdup_printf("Graphical '%s' plugin configuration\n is not "
          "implemented yet.\n"
          "Please edit manually\n\t~/.config/fbpanel/default\n\n"
          "You can use as example files in \n\t%s/share/fbpanel/\n"
          "or visit\n"
          "\thttp://fbpanel.sourceforge.net/docs.html", pl->class->name,
          PREFIX);
    label = gtk_label_new(msg);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_box_pack_end(GTK_BOX(vbox), label, TRUE, TRUE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    g_free(msg);

    return vbox;
}
