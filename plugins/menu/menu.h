/**
 * @file menu.h
 * @brief Menu plugin — shared types for menu.c and system_menu.c.
 *
 * The menu plugin displays a configurable popup menu from a button on the panel.
 * Menu items are driven by the plugin's xconf tree (user config) with optional
 * automatic expansion of <systemmenu> and <include> directives.
 *
 * XCONF TREE OWNERSHIP
 * --------------------
 * The plugin's p->xc (read by menu_constructor via the panel framework) is owned
 * by the framework.  menu_expand_xc() creates a deep copy (m->xc) with systemmenu
 * and include nodes expanded.  All strings in m->xc are xconf-node-owned; they
 * must NOT be g_free'd individually.  m->xc is freed by xconf_del in menu_destroy.
 *
 * MENU REBUILD
 * ------------
 * The GtkMenu (m->menu) is rebuilt lazily:
 *   - schedule_rebuild_menu() schedules a 2-second delayed rebuild (m->rtout).
 *   - rebuild_menu() fires: if the menu is currently visible, defers again;
 *     otherwise calls menu_create() to replace m->menu and m->xc.
 *   - check_system_menu() fires every 30 seconds (m->tout) when has_system_menu
 *     is TRUE; calls systemmenu_changed() and schedules a rebuild if .desktop
 *     files have changed.
 *   - The icon_theme "changed" signal also triggers schedule_rebuild_menu.
 */

#ifndef MENU_H
#define MENU_H


#include "plugin.h"
#include "panel.h"

/** Default icon size for menu item icons (pixels). */
#define MENU_DEFAULT_ICON_SIZE 22

/**
 * menu_priv - menu plugin private state.
 *
 * Embedded as the first field of the plugin allocation (plugin_instance plugin
 * is first so casting between plugin_instance* and menu_priv* is safe).
 */
typedef struct {
    plugin_instance plugin;     /**< Must be first; plugin framework accesses this. */
    GtkWidget *menu;            /**< The popup GtkMenu; NULL if not yet built.
                                 *   gtk_widget_destroy'd in menu_destroy. */
    GtkWidget *bg;              /**< The panel button widget (fb_button_new); owned by p->pwid.
                                 *   gtk_widget_destroy'd in menu_destructor. */
    int iconsize;               /**< Unused; kept for potential future use. */
    int paneliconsize;          /**< Unused; kept for potential future use. */
    xconf *xc;                  /**< Expanded xconf tree for the current menu.
                                 *   Created by menu_expand_xc; freed by xconf_del in menu_destroy.
                                 *   All xconf string values are (transfer none); do NOT g_free. */
    guint tout;                 /**< 30-second g_timeout_add source ID for check_system_menu;
                                 *   0 when no system menu is present or when destroyed. */
    guint rtout;                /**< 2-second g_timeout_add source ID for rebuild_menu;
                                 *   0 when no rebuild is pending. */
    gboolean has_system_menu;   /**< TRUE if the expanded xconf contains a systemmenu node;
                                 *   controls whether tout is installed. */
    time_t btime;               /**< Build time of the current menu (used to detect .desktop changes). */
    gint icon_size;             /**< Icon size for menu items (config: iconsize; default 22). */
} menu_priv;

/**
 * menu_class - menu plugin class.
 *
 * Wraps plugin_class; the PLUGIN macro generates the constructor/destructor
 * attributes for class registration.
 */
typedef struct {
    plugin_class plugin;
} menu_class;


#endif /* MENU_H */
