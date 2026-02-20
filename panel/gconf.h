/**
 * @file gconf.h
 * @brief gconf_block — row-based layout helper for the panel preferences dialog.
 *
 * gconf_block is a lightweight container that arranges GTK widgets into labelled
 * rows inside the fbpanel preferences dialog.  It is NOT a GObject subclass; it
 * is a plain C struct that wraps a GtkBox hierarchy and a GtkSizeGroup.
 *
 * STRUCTURE
 * ---------
 * A gconf_block holds:
 *   main  — a horizontal GtkBox with an optional leading indent spacer, plus
 *            an inner area box.  This is the widget added to the dialog page.
 *   area  — a vertical GtkBox inside main; rows are pack_start'd here.
 *   rows  — a GSList of row GtkHBox widgets (most-recently-prepended first).
 *           The list itself is owned by the block; the hbox widgets are owned
 *           by the GTK widget tree.
 *   sgr   — a GtkSizeGroup (HORIZONTAL) that makes the first widget in every
 *           new row the same width, so labels align across rows.
 *
 * OWNERSHIP AND LIFECYCLE
 * -----------------------
 * - gconf_block_new() allocates the struct with g_new0() and creates the widget
 *   hierarchy.  The caller receives (transfer full) ownership of the struct.
 * - All GtkWidgets created inside the block are referenced by the GTK widget
 *   tree from the moment they are packed.  They are destroyed when their parent
 *   container is destroyed (usually via gtk_widget_destroy on the dialog).
 * - gconf_block_free() must be called AFTER the dialog is destroyed (so GTK has
 *   already removed the widget references).  It unref's sgr and frees the rows
 *   GSList and the struct itself.  It does NOT call gtk_widget_destroy on main.
 *
 * SIGNAL WIRING
 * -------------
 * The optional @cb / @data pair is stored in the block.  Each edit widget
 * (int, enum, boolean, color) that receives the block connects a swapped signal
 * back to @cb(@data) whenever the user changes the value, allowing the caller
 * to react (e.g., show/hide dependent sub-blocks).
 *
 * CHANGE DETECTION
 * ----------------
 * All edit widgets write their new value back into the provided xconf node
 * immediately on change (via xconf_set_int, xconf_set_enum, xconf_set_value).
 * The dialog compares the working xconf tree against a baseline snapshot using
 * xconf_cmp() on Apply/OK to decide whether to save and restart the panel.
 */

#ifndef _GCONF_H_
#define _GCONF_H_

#include <gtk/gtk.h>
#include "panel.h"

/**
 * gconf_block - row-layout helper for the preferences dialog.
 *
 * Wraps a GtkBox widget hierarchy and provides helper functions for building
 * aligned rows of labelled config controls.
 */
typedef struct
{
    GtkWidget  *main;  /**< Outer horizontal GtkBox (indent spacer + area); add to page. */
    GtkWidget  *area;  /**< Inner vertical GtkBox; rows are packed here. */
    GCallback   cb;    /**< Optional change-notification callback; called swapped with @data. */
    gpointer    data;  /**< Userdata for @cb (usually the parent xconf node). */
    GSList     *rows;  /**< Most-recent-first list of row GtkHBoxes; owned by this block. */
    GtkSizeGroup *sgr; /**< Horizontal size group; aligns first widget of each new row. */
} gconf_block;


/**
 * gconf_block_new - allocate and initialize a gconf_block.
 * @cb:     Optional change callback (may be NULL).  Called swapped with @data
 *          when any edit widget inside the block changes.
 * @data:   Userdata passed to @cb (may be NULL).
 * @indent: Width in pixels of the leading indent spacer (0 for no indent).
 *
 * Creates the internal GtkBox hierarchy.  The caller is responsible for
 * packing block->main into a parent container and for calling gconf_block_free()
 * when the dialog is closed.
 *
 * Returns: (transfer full) newly-allocated gconf_block; never NULL.
 */
gconf_block *gconf_block_new(GCallback cb, gpointer data, int indent);

/**
 * gconf_block_free - release a gconf_block's non-widget resources.
 * @b: Block to free; must not be NULL.
 *
 * Unref's the GtkSizeGroup, frees the rows GSList (not the hbox widgets
 * themselves — they are owned by the GTK widget tree), and g_free's the struct.
 *
 * Must be called AFTER the parent dialog has been destroyed so GTK has already
 * cleaned up the widget references.  Calling before gtk_widget_destroy will
 * leave dangling widget references in the size group.
 */
void gconf_block_free(gconf_block *b);

/**
 * gconf_block_add - add a widget to the block's current row (or a new row).
 * @b:       Block to add to; must not be NULL.
 * @w:       Widget to add.  If this is the first widget in a new row and it is
 *           a GtkLabel, its x-alignment is set to 0.0 and it is added to sgr.
 * @new_row: If TRUE (or if no rows exist yet), start a new horizontal row first,
 *           then pack @w into it.  If FALSE, pack @w into the current last row.
 *
 * New rows are prepended to b->rows (most-recent-first order).  A trailing
 * expand-filler GtkBox is added to each new row after the first element, so
 * content left-aligns.
 */
void gconf_block_add(gconf_block *b, GtkWidget *w, gboolean new_row);

/**
 * gconf_edit_int - create a GtkSpinButton bound to an integer xconf node.
 * @b:   Block to add the spin button to (may be NULL; then no block wiring).
 * @xc:  xconf node whose integer value is read and written.
 *       Reads the current value with xconf_get_int(); writes it back with
 *       xconf_set_int() on each "value-changed" signal.
 * @min: Minimum spin button value.
 * @max: Maximum spin button value.
 *
 * The current xconf value is set as the initial spin button value.  If the
 * xconf node has no value yet, xconf_get_int returns 0 and xconf_set_int
 * stores that as the default.
 *
 * Returns: (transfer none) GtkSpinButton widget; owned by @b's widget tree
 *          after gconf_block_add is called.  Caller must NOT g_object_unref.
 */
GtkWidget *gconf_edit_int(gconf_block *b, xconf *xc, int min, int max);

/**
 * gconf_edit_enum - create a GtkComboBoxText bound to an enum xconf node.
 * @b:  Block to add the combo box to (may be NULL).
 * @xc: xconf node whose enum value is read and written.
 *      Reads with xconf_get_enum(); writes with xconf_set_enum() on "changed".
 * @e:  NULL-sentinel xconf_enum table; each entry provides label text and
 *      its integer index.  The table pointer is stored in the widget's object
 *      data under the key "enum" for use in the callback.
 *
 * Returns: (transfer none) GtkComboBoxText widget.
 */
GtkWidget *gconf_edit_enum(gconf_block *b, xconf *xc, xconf_enum *e);

/**
 * gconf_edit_boolean - create a GtkCheckButton bound to a boolean xconf node.
 * @b:    Block to add the check button to (may be NULL).
 * @xc:   xconf node whose boolean value is read (via xconf_get_enum with
 *        bool_enum) and written (via xconf_set_enum) on "toggled".
 * @text: Label text displayed next to the check box.
 *
 * Returns: (transfer none) GtkCheckButton widget.
 */
GtkWidget *gconf_edit_boolean(gconf_block *b, xconf *xc, gchar *text);

/**
 * gconf_edit_color - create a GtkColorButton bound to color (and alpha) xconf nodes.
 * @b:        Block to add the color button to (may be NULL).
 * @xc_color: xconf node holding the color as an "#RRGGBB" string.
 *            Reads via xconf_get_value() / gdk_rgba_parse(); writes via
 *            xconf_set_value() / gdk_color_to_RRGGBB() on "color-set".
 * @xc_alpha: Optional xconf node holding the alpha as an integer 0-255.
 *            If non-NULL, alpha-channel editing is enabled on the button
 *            (gtk_color_chooser_set_use_alpha TRUE).  The alpha is stored
 *            in the widget's object data under "alpha" for the callback.
 *            If NULL, alpha editing is disabled.
 *
 * Note: The initial display of the alpha channel from @xc_alpha is incomplete
 * (see BUG-015 in docs/BUGS_AND_ISSUES.md — the alpha is read but not applied
 * to the displayed colour on initialisation).
 *
 * Returns: (transfer none) GtkColorButton widget.
 */
GtkWidget *gconf_edit_color(gconf_block *b, xconf *xc_color, xconf *xc_alpha);

#endif /* _GCONF_H_ */
