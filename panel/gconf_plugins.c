/**
 * @file gconf_plugins.c
 * @brief Plugins tab for the panel preferences dialog.
 *
 * Implements mk_tab_plugins(), which is called from gconf_panel.c to build
 * the "Plugins" notebook page in the preferences dialog.
 *
 * CURRENT STATE
 * -------------
 * The plugins tab is partially implemented — it displays a read-only list of
 * loaded plugins but the Add/Edit/Delete/Up/Down buttons are not wired to any
 * callbacks.  The tree selection only prints the plugin type to stdout and
 * enables the button row; no further action is taken.
 *
 * TREE MODEL
 * ----------
 * A GtkTreeStore with two string columns is used:
 *   TYPE_COL (0): plugin type string (e.g., "taskbar", "menu")
 *   NAME_COL (1): hardcoded "Martin Heidegger" placeholder — not a real name field.
 *
 * The model is populated from the xconf tree by iterating xconf_find(xc, "plugin", i)
 * until NULL.  The "type" string is read with XCG str (transfer none).
 * gtk_tree_store_set copies the string value into the model, so the model does
 * not hold a direct reference to the xconf data.
 *
 * BUTTON STATE
 * ------------
 * The Edit/Delete/Up/Down button group (bbox) is initially insensitive and is
 * enabled when a row is selected in tree_selection_changed_cb.  The Add button
 * is always sensitive.
 */

#include "gconf.h"
#include "panel.h"


/** Column indices for the GtkTreeStore. */
enum
{
    TYPE_COL,   /**< Plugin type string column (e.g., "taskbar"). */
    NAME_COL,   /**< Placeholder name column (currently hardcoded). */
    N_COLUMNS
};

/** GtkTreeStore backing the plugin list view. File-scope; lives with the tab page. */
GtkTreeStore *store;

/** GtkTreeView displaying the plugin list. */
GtkWidget *tree;

/** Button row containing Edit/Delete/Up/Down; enabled on row selection. */
GtkWidget *bbox;

/**
 * mk_model - build the GtkTreeStore from the xconf plugin list.
 * @xc: xconf tree root (the whole panel config); plugins are found via
 *      xconf_find(xc, "plugin", i).
 *
 * Creates a new GtkTreeStore and populates one row per "plugin" child xconf
 * node.  The "type" field is read with XCG str (transfer none from xconf).
 * gtk_tree_store_set makes its own copy of the string, so no ownership is
 * transferred to the model.
 */
static void
mk_model(xconf *xc)
{
    GtkTreeIter iter;
    xconf *pxc;
    int i;
    gchar *type;

    store = gtk_tree_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    for (i = 0; (pxc = xconf_find(xc, "plugin", i)); i++)
    {
        XCG(pxc, "type", &type, str);
        gtk_tree_store_append(store, &iter, NULL);
        gtk_tree_store_set (store, &iter,
            TYPE_COL, type,
            NAME_COL, "Martin Heidegger",
            -1);
    }
}

/**
 * tree_selection_changed_cb - handle plugin list selection changes.
 * @selection: The GtkTreeSelection that changed.
 * @data:      Unused.
 *
 * Prints the selected plugin type to stdout (debug) and enables or disables
 * the bbox button group based on whether a row is selected.
 * The retrieved `type` string is (transfer full) from gtk_tree_model_get and
 * must be g_free'd — this is done correctly here.
 */
static void
tree_selection_changed_cb(GtkTreeSelection *selection, gpointer data)
{
    GtkTreeIter iter;
    GtkTreeModel *model;
    gchar *type;
    gboolean sel;

    sel = gtk_tree_selection_get_selected(selection, &model, &iter);
    if (sel)
    {
        gtk_tree_model_get(model, &iter, TYPE_COL, &type, -1);
        g_print("%s\n", type);
        g_free(type);
    }
    gtk_widget_set_sensitive(bbox, sel);
}

/**
 * mk_view - create the GtkTreeView for the plugin list.
 *
 * Creates a single-selection GtkTreeView backed by the file-scope `store`.
 * Displays only the TYPE_COL column (the plugin type string).
 * Connects tree_selection_changed_cb to the selection's "changed" signal.
 *
 * Returns: (transfer none) GtkTreeView widget; owned by the tab page widget tree.
 */
static GtkWidget *
mk_view()
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection *select;

    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Type",
        renderer, "text", TYPE_COL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

    select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed",
        G_CALLBACK(tree_selection_changed_cb), NULL);
    return tree;
}

/**
 * mk_buttons - create the Add/Edit/Delete/Up/Down button row.
 *
 * The Add button is always sensitive.
 * Edit/Delete/Up/Down are grouped inside bbox and start insensitive;
 * they are enabled when a row is selected (see tree_selection_changed_cb).
 *
 * Note: none of the buttons are connected to action callbacks — the plugin
 * management actions (add, edit, delete, reorder) are not yet implemented.
 *
 * Returns: (transfer none) GtkBox containing all buttons.
 */
GtkWidget *
mk_buttons()
{
    GtkWidget *bm, *b, *w;

    bm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);

    w = gtk_button_new_with_label(_("Add"));
    gtk_box_pack_start(GTK_BOX(bm), w, FALSE, TRUE, 0);

    b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(bm), b, FALSE, TRUE, 0);
    bbox = b;
    gtk_widget_set_sensitive(bbox, FALSE);

    w = gtk_button_new_with_label(_("Edit"));
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    w = gtk_button_new_with_label(_("Delete"));
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    w = gtk_button_new_with_label(_("Down"));
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);
    w = gtk_button_new_with_label(_("Up"));
    gtk_box_pack_start(GTK_BOX(b), w, FALSE, TRUE, 0);

    return bm;
}

/**
 * mk_tab_plugins - build and return the "Plugins" notebook tab page.
 * @xc: xconf tree root passed to mk_model for plugin enumeration.
 *
 * Calls mk_model to populate the tree store, mk_view to create the list view,
 * and mk_buttons to create the action buttons.  The tree expands to fill
 * available space; the buttons are fixed-height at the bottom.
 *
 * Returns: (transfer none) GtkBox page widget; owned by the dialog notebook.
 */
GtkWidget *
mk_tab_plugins(xconf *xc)
{
    GtkWidget *page, *w;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    mk_model(xc);

    w = mk_view();
    gtk_box_pack_start(GTK_BOX(page), w, TRUE, TRUE, 0);
    w = mk_buttons();
    gtk_box_pack_start(GTK_BOX(page), w, FALSE, TRUE, 0);

    gtk_widget_show_all(page);
    return page;
}
