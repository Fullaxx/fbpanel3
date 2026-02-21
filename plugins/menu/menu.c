/**
 * @file menu.c
 * @brief Menu plugin — config expansion, GtkMenu construction, and button lifecycle.
 *
 * CONFIG TREE EXPANSION (menu_expand_xc)
 * ----------------------------------------
 * The raw p->xc tree from the panel config is deep-copied into m->xc with
 * two special directives expanded inline:
 *
 *   <systemmenu/>  ->  replaced by the output of xconf_new_from_systemmenu()
 *                      (all XDG .desktop files, categorized and sorted).
 *   <include path> ->  replaced by the xconf tree loaded from the named file
 *                      via xconf_new_from_file().
 *
 * The expanded tree is owned by m->xc.  All xconf string values accessed via
 * XCG(..., str) are (transfer none) raw pointers into this tree; they must NOT
 * be g_free'd individually.  xconf_del(m->xc, FALSE) frees the entire tree.
 *
 * MENU CONSTRUCTION (menu_create_menu / menu_create_item)
 * -------------------------------------------------------
 * menu_create_menu() recursively builds a GtkMenu from an xconf subtree.
 * Child nodes are dispatched by name:
 *   "separator" -> gtk_separator_menu_item_new()
 *   "item"      -> menu_create_item() with NULL submenu (leaf item)
 *   "menu"      -> menu_create_item() with a recursively-built submenu
 *
 * menu_create_item() reads these xconf keys (all transfer none from xconf):
 *   name:   GtkMenuItem label text.
 *   image:  Path to an image file (expand_tilda applied; result g_free'd —
 *           image loading is not currently implemented).
 *   action: Shell command to run on "activate" (expand_tilda applied to make
 *           a g_strdup'd copy; stored in menu item's object data "activate"
 *           with g_free destructor so it is freed when the item is destroyed).
 *   command: Recognized but not yet implemented (XXX).
 *
 * REBUILD LIFECYCLE
 * -----------------
 * - menu_constructor calls schedule_rebuild_menu immediately (2s delay).
 * - On icon theme change (icon_theme "changed" signal): schedule_rebuild_menu.
 * - If has_system_menu: check_system_menu fires every 30s (m->tout);
 *   schedules rebuild if any .desktop mtime > m->btime.
 * - rebuild_menu: if menu is mapped, returns TRUE (defer); else menu_create().
 * - menu_create: calls menu_destroy first (destroys old GtkMenu and cancels
 *   old timers), then builds a fresh m->xc and m->menu.
 *
 * BUTTON CREATION (make_button)
 * -----------------------------
 * If the config has an "image" (file path) or "icon" (theme name) key,
 * fb_button_new creates a panel button.  Otherwise no button is shown.
 * The button's "button-press-event" is connected to my_button_pressed.
 *
 * AUTOHIDE INTERACTION
 * --------------------
 * When a menu pops up on an autohide panel, ah_stop() is called to keep the
 * panel visible.  When the menu is dismissed (GtkMenu "unmap"), ah_start()
 * resumes autohide.
 */

#include <stdlib.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "bg.h"
#include "gtkbgbox.h"
#include "run.h"
#include "menu.h"

//#define DEBUGPRN
#include "dbg.h"

xconf *xconf_new_from_systemmenu();
gboolean systemmenu_changed(time_t btime);
static void menu_create(plugin_instance *p);
static void menu_destroy(menu_priv *m);
static gboolean check_system_menu(plugin_instance *p);

/**
 * menu_expand_xc - deep-copy an xconf tree, expanding systemmenu and include nodes.
 * @xc: Source xconf node to copy (may be NULL).
 * @m:  Menu instance; m->has_system_menu is set to TRUE if a systemmenu is found.
 *
 * Recursively copies @xc into a new tree.  When a child node named "systemmenu"
 * is encountered, its sons are replaced by the output of xconf_new_from_systemmenu()
 * (the XDG application menu).  When "include" is found, the named file is loaded
 * with xconf_new_from_file() and its sons are appended.  In both cases the source
 * temporary xconf is freed with xconf_del after appending.
 *
 * Returns: (transfer full) new xconf root; caller must xconf_del.
 *          Returns NULL if @xc is NULL.
 */
static xconf *
menu_expand_xc(xconf *xc, menu_priv *m)
{
    xconf *nxc, *cxc, *smenu_xc;
    GSList *w;

    if (!xc)
        return NULL;
    nxc = xconf_new(xc->name, xc->value);
    DBG("new node:%s\n", nxc->name);
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        cxc = w->data;
        if (!strcmp(cxc->name, "systemmenu"))
        {
            smenu_xc = xconf_new_from_systemmenu();
            xconf_append_sons(nxc, smenu_xc);
            xconf_del(smenu_xc, FALSE);
            m->has_system_menu = TRUE;
            continue;
        }
        if (!strcmp(cxc->name, "include"))
        {
            smenu_xc = xconf_new_from_file(cxc->value, "include");
            xconf_append_sons(nxc, smenu_xc);
            xconf_del(smenu_xc, FALSE);
            continue;
        }
        xconf_append(nxc, menu_expand_xc(cxc, m));
    }
    return nxc;
}


/**
 * menu_create_separator - create a GtkSeparatorMenuItem.
 *
 * Returns: (transfer none) new separator menu item; owned by the parent GtkMenu.
 */
static GtkWidget *
menu_create_separator()
{
    return gtk_separator_menu_item_new();
}

/**
 * menu_create_item - create a GtkMenuItem from an xconf node.
 * @xc:   xconf node for the item (transfer none; owned by m->xc).
 * @menu: If non-NULL, attach as a submenu to the returned item.
 *        If NULL, attach an "activate" signal to run the item's command.
 * @m:    Menu instance (unused here but may be used by future command dispatch).
 *
 * Reads from @xc (all values are transfer none — raw pointers into the xconf tree):
 *   name:   Label text for the menu item.
 *   image:  Image file path — expand_tilda() makes a copy which is g_free'd here
 *           (image loading not yet implemented).
 *   action: Shell command — expand_tilda() makes a (transfer full) copy stored
 *           in the menu item's "activate" object data with g_free as destructor.
 *           Connected to run_app via g_signal_connect_swapped.
 *   command: Recognized key but not yet implemented.
 *
 * Returns: (transfer none) GtkMenuItem; owned by the parent GtkMenuShell.
 */
/* Creates menu item. Text and image are read from xconf. Action
 * depends on @menu. If @menu is NULL, action is to execute external
 * command. Otherwise it is to pop up @menu menu */
static GtkWidget *
menu_create_item(xconf *xc, GtkWidget *menu, menu_priv *m)
{
    gchar *name, *fname, *iname, *action, *cmd;
    GtkWidget *mi;

    cmd = name = fname = action = iname = NULL;
    XCG(xc, "name", &name, str);
    mi = gtk_menu_item_new_with_label(name ? name : "");
    gtk_container_set_border_width(GTK_CONTAINER(mi), 0);
    XCG(xc, "image", &fname, str);
    fname = expand_tilda(fname);  /* expand_tilda always g_strdup's */
    g_free(fname);
    /* iname would be XCG(..., str) — a raw pointer into the xconf tree;
     * do NOT free it here.  xconf_del(m->xc) owns and frees it on menu
     * destruction.  Freeing it here caused a double-free crash. */

    if (menu)
    {
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
        goto done;
    }
    XCG(xc, "action", &action, str);
    if (action)
    {
        action = expand_tilda(action);

        g_signal_connect_swapped(G_OBJECT(mi), "activate",
                (GCallback)run_app, action);
        g_object_set_data_full(G_OBJECT(mi), "activate",
            action, g_free);
        goto done;
    }
    XCG(xc, "command", &cmd, str);
    if (cmd)
    {
        /* XXX: implement command API */
    }

done:
    return mi;
}

/**
 * menu_create_menu - recursively build a GtkMenu from an xconf subtree.
 * @xc:       xconf node whose sons are iterated to build menu items.
 * @ret_menu: If TRUE, return the GtkMenu itself.
 *            If FALSE, return a GtkMenuItem with @xc's label and @menu as submenu.
 * @m:        Menu instance passed to child item builders.
 *
 * For each child node of @xc:
 *   "separator" -> menu_create_separator()
 *   "item"      -> menu_create_item(nxc, NULL, m) [leaf with action]
 *   "menu"      -> menu_create_menu(nxc, FALSE, m) [submenu item]
 *   other       -> skipped
 *
 * Returns: (transfer none) GtkMenu (if ret_menu) or GtkMenuItem; owned by parent.
 *          Returns NULL if @xc is NULL.
 */
/* Creates menu and optionally button to pop it up.
 * If @ret_menu is TRUE, then a menu is returned. Otherwise,
 * button is created, linked to a menu and returned instead. */
static GtkWidget *
menu_create_menu(xconf *xc, gboolean ret_menu, menu_priv *m)
{
    GtkWidget *mi, *menu;
    GSList *w;
    xconf *nxc;

    if (!xc)
        return NULL;
    menu = gtk_menu_new ();
    gtk_container_set_border_width(GTK_CONTAINER(menu), 0);
    for (w = xc->sons; w ; w = g_slist_next(w))
    {
        nxc = w->data;
        if (!strcmp(nxc->name, "separator"))
            mi = menu_create_separator();
        else if (!strcmp(nxc->name, "item"))
            mi = menu_create_item(nxc, NULL, m);
        else if (!strcmp(nxc->name, "menu"))
            mi = menu_create_menu(nxc, FALSE, m);
        else
            continue;
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    return (ret_menu) ? menu : menu_create_item(xc, menu, m);
}

/**
 * menu_unmap - resume autohide after the menu is dismissed.
 * @menu: The GtkMenu that was unmapped (unused).
 * @p:    Plugin instance.
 *
 * Connected to the GtkMenu "unmap" signal.  Calls ah_start() if the panel
 * has autohide enabled, allowing the panel to hide again after the user
 * closes the menu.
 *
 * Returns: FALSE (allow default GTK unmap handling to continue).
 */
static gboolean
menu_unmap(GtkWidget *menu, plugin_instance *p)
{
    if (p->panel->autohide)
        ah_start(p->panel);
    return FALSE;
}

/**
 * menu_create - (re)build the expanded xconf tree and the GtkMenu.
 * @p: Plugin instance.
 *
 * Destroys any existing menu (menu_destroy), then:
 *   1. Builds a fresh m->xc via menu_expand_xc (deep copy with expansions).
 *   2. Builds m->menu via menu_create_menu (the top-level popup GtkMenu).
 *   3. Connects the "unmap" signal for autohide resume.
 *   4. Sets m->btime to the current time (for change detection).
 *   5. If has_system_menu, installs a 30-second check_system_menu timer (m->tout).
 */
static void
menu_create(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    if (m->menu)
        menu_destroy(m);
    m->xc = menu_expand_xc(p->xc, m);
    m->menu = menu_create_menu(m->xc, TRUE, m);
    g_signal_connect(G_OBJECT(m->menu), "unmap",
        G_CALLBACK(menu_unmap), p);
    m->btime = time(NULL);
    if (m->has_system_menu)
        m->tout = g_timeout_add(30000, (GSourceFunc) check_system_menu, p);
    return;
}

/**
 * menu_destroy - tear down the GtkMenu, timers, and expanded xconf tree.
 * @m: Menu instance.
 *
 * Destroys m->menu (gtk_widget_destroy), removes m->tout (check timer) and
 * m->rtout (rebuild timer) via g_source_remove, and frees m->xc via xconf_del.
 * All pointers are set to NULL / 0 after cleanup.
 * Safe to call when any or all of these are already NULL / 0.
 */
static void
menu_destroy(menu_priv *m)
{
    if (m->menu) {
        gtk_widget_destroy(m->menu);
        m->menu = NULL;
        m->has_system_menu = FALSE;
    }
    if (m->tout) {
        g_source_remove(m->tout);
        m->tout = 0;
    }
    if (m->rtout) {
        g_source_remove(m->rtout);
        m->rtout = 0;
    }
    if (m->xc) {
        xconf_del(m->xc, FALSE);
        m->xc = NULL;
    }
    return;
}

/**
 * my_button_pressed - handle button-press-event on the menu launch button.
 * @widget: The panel button widget (m->bg).
 * @event:  The button-press event.
 * @p:      Plugin instance.
 *
 * Ctrl+RMB is propagated to the panel (returns FALSE to pass through).
 * Any other button press within the button's allocation:
 *   - Lazily creates the menu if not yet built.
 *   - Stops autohide (ah_stop) if enabled.
 *   - Pops up the menu at the pointer position.
 *
 * Returns: TRUE if the event was consumed; FALSE to pass to the panel.
 */
static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    /* propagate Control-Button3 to the panel */
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
        && event->state & GDK_CONTROL_MASK)
    {
        return FALSE;
    }

    {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        if ((event->type == GDK_BUTTON_PRESS)
            && (event->x >=0 && event->x < alloc.width)
            && (event->y >=0 && event->y < alloc.height))
        {
            if (!m->menu)
                menu_create(p);
            if (p->panel->autohide)
                ah_stop(p->panel);
            gtk_menu_popup_at_pointer(GTK_MENU(m->menu), (GdkEvent *)event);
        }
    }
    return TRUE;
}


/**
 * make_button - create the panel button widget for the menu plugin.
 * @p:  Plugin instance.
 * @xc: Config xconf node (the plugin's xconf from p->xc).
 *
 * Reads "image" (file path) and "icon" (theme name) from @xc (both transfer none).
 * If either is present, creates a fb_button_new with the panel's icon size.
 * The button is packed into p->pwid and connected to my_button_pressed.
 * If transparent, sets BG_INHERIT background on the button.
 * expand_tilda is applied to fname; the resulting g_strdup'd string is g_free'd.
 */
static void
make_button(plugin_instance *p, xconf *xc)
{
    int w, h;
    menu_priv *m;
    gchar *fname, *iname;

    m = (menu_priv *) p;
    /* XXX: this code is duplicated in every plugin.
     * Lets run it once in a panel */
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
        w = -1;
        h = p->panel->max_elem_height;
    }
    else
    {
        w = p->panel->max_elem_height;
        h = -1;
    }
    fname = iname = NULL;
    XCG(xc, "image", &fname, str);
    fname = expand_tilda(fname);
    XCG(xc, "icon", &iname, str);
    if (fname || iname)
    {
        m->bg = fb_button_new(iname, fname, w, h, 0x702020);
        gtk_container_add(GTK_CONTAINER(p->pwid), m->bg);
        if (p->panel->transparent)
            gtk_bgbox_set_background(m->bg, BG_INHERIT, 0, 0);
        g_signal_connect (G_OBJECT (m->bg), "button-press-event",
            G_CALLBACK (my_button_pressed), p);
    }
    g_free(fname);
}

/**
 * rebuild_menu - deferred menu rebuild callback.
 * @p: Plugin instance.
 *
 * Called by the m->rtout g_timeout_add (2-second delay).
 * If the menu is currently visible (mapped), defers again by returning TRUE.
 * Otherwise calls menu_create to rebuild, clears m->rtout, and returns FALSE.
 *
 * Returns: TRUE to keep the timeout (menu still visible); FALSE to remove it.
 */
static gboolean
rebuild_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    if (m->menu && gtk_widget_get_mapped(m->menu))
        return TRUE;
    menu_create(p);
    m->rtout = 0;
    return FALSE;
}

/**
 * schedule_rebuild_menu - schedule a deferred menu rebuild (2-second delay).
 * @p: Plugin instance.
 *
 * Installs a g_timeout_add(2000, rebuild_menu) if one is not already pending.
 * The timeout ID is saved in m->rtout so it can be cancelled in menu_destroy.
 * Called on icon_theme change and from check_system_menu when .desktop files change.
 */
static void
schedule_rebuild_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    if (!m->rtout) {
        DBG("scheduling menu rebuild p=%p\n", p);
        m->rtout = g_timeout_add(2000, (GSourceFunc) rebuild_menu, p);
    }
    return;

}

/**
 * check_system_menu - periodic timer to detect .desktop file changes.
 * @p: Plugin instance.
 *
 * Called every 30 seconds when the menu contains a systemmenu expansion.
 * Calls systemmenu_changed(m->btime); if any .desktop file's mtime is newer
 * than m->btime, schedules a menu rebuild via schedule_rebuild_menu.
 *
 * Returns: TRUE (keep the timer running).
 */
static gboolean
check_system_menu(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    if (systemmenu_changed(m->btime))
        schedule_rebuild_menu(p);

    return TRUE;
}

/**
 * menu_constructor - initialize the menu plugin instance.
 * @p: Plugin instance (size = menu_priv.priv_size).
 *
 * 1. Reads "iconsize" from config (default MENU_DEFAULT_ICON_SIZE).
 * 2. Creates the panel button (make_button).
 * 3. Connects to icon_theme "changed" for automatic menu rebuild on theme change.
 * 4. Schedules the initial menu build (schedule_rebuild_menu; fires after 2s).
 *    This deferred build avoids stalling panel startup for large system menus.
 *
 * Returns: 1 on success (plugin framework convention).
 */
static int
menu_constructor(plugin_instance *p)
{
    menu_priv *m;

    m = (menu_priv *) p;
    m->icon_size = MENU_DEFAULT_ICON_SIZE;
    XCG(p->xc, "iconsize", &m->icon_size, int);
    DBG("icon_size=%d\n", m->icon_size);
    make_button(p, p->xc);
    g_signal_connect_swapped(G_OBJECT(icon_theme),
        "changed", (GCallback) schedule_rebuild_menu, p);
    schedule_rebuild_menu(p);
    return 1;
}


/**
 * menu_destructor - tear down the menu plugin instance.
 * @p: Plugin instance.
 *
 * 1. Disconnects the icon_theme "changed" signal.
 * 2. Destroys the menu and cancels all timers (menu_destroy).
 * 3. Destroys the button widget (m->bg); the plugin framework destroys p->pwid.
 */
static void
menu_destructor(plugin_instance *p)
{
    menu_priv *m = (menu_priv *) p;

    g_signal_handlers_disconnect_by_func(G_OBJECT(icon_theme),
        schedule_rebuild_menu, p);
    menu_destroy(m);
    gtk_widget_destroy(m->bg);
    return;
}


static menu_class class = {
    .plugin = {
        .count       = 0,
        .type        = "menu",
        .name        = "Menu",
        .version     = "1.0",
        .description = "Menu",
        .priv_size   = sizeof(menu_priv),

        .constructor = menu_constructor,
        .destructor  = menu_destructor,
    }
};

static plugin_class *class_ptr = (plugin_class *) &class;
