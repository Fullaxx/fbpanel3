/**
 * @file gconf.c
 * @brief gconf_block implementation — row-layout helpers for the preferences dialog.
 *
 * Implements the four config-edit widget factories (int, enum, boolean, color)
 * and the three gconf_block management functions (new, free, add).
 *
 * DESIGN
 * ------
 * gconf_block is a layout helper, not a GObject.  It wraps a GtkBox hierarchy
 * (main -> optional-indent-spacer + area -> rows-of-hboxes) and a horizontal
 * GtkSizeGroup that keeps the first widget in each new row the same width.
 *
 * Each edit factory:
 *   1. Reads the current value from the xconf node (with a sensible default).
 *   2. Creates a GTK control pre-populated with that value.
 *   3. Connects a "value-changed" / "changed" / "toggled" / "color-set" handler
 *      that writes the new value back to xconf immediately.
 *   4. If a block (@b) with a callback is provided, also connects a swapped
 *      signal to b->cb(b) so the dialog can react to the change (e.g., show/hide
 *      dependent sub-blocks via gtk_widget_set_sensitive).
 *
 * INDENT_SIZE
 * -----------
 * INDENT_SIZE (20 px) is the pixel width of the indent spacer used by sub-blocks
 * (color_block, corner_block, ah_block, layer_block) inside gconf_panel.c.
 */

#include "gconf.h"
#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"

/** Pixel width of the indent spacer for nested sub-blocks. */
#define INDENT_SIZE 20


/**
 * gconf_block_new - allocate and initialize a gconf_block.
 *
 * Creates the outer horizontal box (b->main), an optional fixed-width indent
 * spacer (if indent > 0), and the inner vertical area box (b->area).
 * Also creates the horizontal GtkSizeGroup (b->sgr) that aligns first-column
 * labels across rows.
 *
 * Returns: (transfer full) newly-allocated gconf_block; never NULL.
 */
gconf_block *
gconf_block_new(GCallback cb, gpointer data, int indent)
{
    GtkWidget *w;
    gconf_block *b;

    b = g_new0(gconf_block, 1);
    b->cb = cb;
    b->data = data;
    b->main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    /* indent */
    if (indent > 0)
    {
        w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(w, indent, -1);
        gtk_box_pack_start(GTK_BOX(b->main), w, FALSE, FALSE, 0);
    }

    /* area */
    w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(b->main), w, FALSE, FALSE, 0);
    b->area = w;

    b->sgr = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    return b;
}

/**
 * gconf_block_free - release a gconf_block's non-widget resources.
 *
 * Unref's the GtkSizeGroup (which releases its reference on each widget it
 * tracks), frees the rows GSList (not the hbox widgets, which are already
 * destroyed by the parent widget tree), and g_free's the struct.
 *
 * Must be called AFTER gtk_widget_destroy on the dialog that owns b->main,
 * so all widget references have already been released by GTK.
 */
void
gconf_block_free(gconf_block *b)
{
    g_object_unref(b->sgr);
    g_slist_free(b->rows);
    g_free(b);
}

/**
 * gconf_block_add - add a widget to the current row or start a new row.
 *
 * If new_row is TRUE (or the rows list is empty), creates a new horizontal
 * GtkBox row and prepends it to b->rows.  Adds a trailing expand-filler GtkBox
 * so content left-aligns.  If @w is a GtkLabel when starting a new row, its
 * x-alignment is set to 0.0 and it is added to b->sgr for column alignment.
 *
 * Packs @w into the current (possibly just-created) last row.
 */
void
gconf_block_add(gconf_block *b, GtkWidget *w, gboolean new_row)
{
    GtkWidget *hbox;

    if (!b->rows || new_row)
    {
        GtkWidget *s;

        new_row = TRUE;
        hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        b->rows = g_slist_prepend(b->rows, hbox);
        gtk_box_pack_start(GTK_BOX(b->area), hbox, FALSE, FALSE, 0);
        /* space */
        s = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_end(GTK_BOX(hbox), s, TRUE, TRUE, 0);

        /* allign first elem */
        if (GTK_IS_WIDGET(w))
        {
            DBG("misc \n");
            gtk_label_set_xalign(GTK_LABEL(w), 0.0);
            gtk_size_group_add_widget(b->sgr, w);
        }
    }
    else
        hbox = b->rows->data;
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
}

/*********************************************************
 * Edit int
 *********************************************************/

/**
 * gconf_edit_int_cb - write the spin button value back to the xconf node.
 * @w:  Spin button whose value changed.
 * @xc: xconf integer node to update (transfer none — owned by the config tree).
 */
static void
gconf_edit_int_cb(GtkSpinButton *w, xconf *xc)
{
    int i;

    i = (int) gtk_spin_button_get_value(w);
    xconf_set_int(xc, i);
}

/**
 * gconf_edit_int - create a GtkSpinButton bound to an integer xconf node.
 *
 * Reads the current integer value from @xc, seeds the spin button with it,
 * and writes it back to @xc on every "value-changed" emission.
 * If @b has a callback, also fires it (swapped) on value changes.
 *
 * Returns: (transfer none) GtkSpinButton; owned by @b's widget tree after
 *          gconf_block_add.
 */
GtkWidget *
gconf_edit_int(gconf_block *b, xconf *xc, int min, int max)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_int(xc, &i);
    xconf_set_int(xc, i);
    w = gtk_spin_button_new_with_range((gdouble) min, (gdouble) max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), (gdouble) i);
    g_signal_connect(G_OBJECT(w), "value-changed",
        G_CALLBACK(gconf_edit_int_cb), xc);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "value-changed",
            G_CALLBACK(b->cb), b);
    }
    return w;
}

/*********************************************************
 * Edit enum
 *********************************************************/

/**
 * gconf_edit_enum_cb - write the selected combo index back to the xconf node.
 * @w:  Combo box whose selection changed.
 * @xc: xconf enum node to update.
 *
 * The xconf_enum table pointer is retrieved from the widget's "enum" object
 * data (set during gconf_edit_enum) so xconf_set_enum can translate the index
 * back to the string representation.
 */
static void
gconf_edit_enum_cb(GtkComboBox *w, xconf *xc)
{
    int i;

    i = gtk_combo_box_get_active(w);
    DBG("%s=%d\n", xc->name, i);
    xconf_set_enum(xc, i, g_object_get_data(G_OBJECT(w), "enum"));
}

/**
 * gconf_edit_enum - create a GtkComboBoxText bound to an enum xconf node.
 *
 * Reads the current enum value from @xc, populates the combo with all
 * options from @e (using desc if available, otherwise str), sets the active
 * index, and writes back on "changed".
 *
 * The @e pointer is stored in the widget's "enum" object data for use by the
 * callback; the table must remain valid for the lifetime of the widget.
 *
 * Returns: (transfer none) GtkComboBoxText.
 */
GtkWidget *
gconf_edit_enum(gconf_block *b, xconf *xc, xconf_enum *e)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_enum(xc, &i, e);
    xconf_set_enum(xc, i, e);
    w = gtk_combo_box_text_new();
    g_object_set_data(G_OBJECT(w), "enum", e);
    while (e && e->str)
    {
        gtk_combo_box_text_insert((GtkComboBoxText*)(w), e->num, NULL,
            e->desc ? _(e->desc) : _(e->str));
        e++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w), i);
    g_signal_connect(G_OBJECT(w), "changed",
        G_CALLBACK(gconf_edit_enum_cb), xc);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "changed",
            G_CALLBACK(b->cb), b);
    }

    return w;
}

/*********************************************************
 * Edit boolean
 *********************************************************/

/**
 * gconf_edit_bool_cb - write the check button state back to the xconf node.
 * @w:  Toggle button whose state changed.
 * @xc: xconf bool node to update (uses bool_enum for str conversion).
 */
static void
gconf_edit_bool_cb(GtkToggleButton *w, xconf *xc)
{
    int i;

    i = gtk_toggle_button_get_active(w);
    DBG("%s=%d\n", xc->name, i);
    xconf_set_enum(xc, i, bool_enum);
}

/**
 * gconf_edit_boolean - create a GtkCheckButton bound to a boolean xconf node.
 *
 * Reads the current boolean value from @xc via bool_enum, initialises the
 * check button's state, and writes it back on "toggled".
 *
 * Returns: (transfer none) GtkCheckButton.
 */
GtkWidget *
gconf_edit_boolean(gconf_block *b, xconf *xc, gchar *text)
{
    gint i = 0;
    GtkWidget *w;

    xconf_get_enum(xc, &i, bool_enum);
    xconf_set_enum(xc, i, bool_enum);
    w = gtk_check_button_new_with_label(text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), i);

    g_signal_connect(G_OBJECT(w), "toggled",
        G_CALLBACK(gconf_edit_bool_cb), xc);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "toggled",
            G_CALLBACK(b->cb), b);
    }

    return w;
}


/*********************************************************
 * Edit color
 *********************************************************/

/**
 * gconf_edit_color_cb - write the chosen colour (and alpha) back to xconf nodes.
 * @w:        GtkColorButton whose colour changed.
 * @xc:       xconf node for the colour string (set as "#RRGGBB" via
 *            gdk_color_to_RRGGBB + xconf_set_value).
 *
 * If an alpha xconf node was stored in the widget's "alpha" object data,
 * extracts the alpha channel (0.0–1.0 scaled to 0–255) and stores it as an
 * integer via xconf_set_int.
 */
static void
gconf_edit_color_cb(GtkColorButton *w, xconf *xc)
{
    GdkRGBA c;
    xconf *xc_alpha;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(w), &c);
    xconf_set_value(xc, gdk_color_to_RRGGBB(&c));
    if ((xc_alpha = g_object_get_data(G_OBJECT(w), "alpha")))
    {
        guint16 a = (guint16)(c.alpha * 255.0 + 0.5);
        xconf_set_int(xc_alpha, (int) a);
    }
}

/**
 * gconf_edit_color - create a GtkColorButton bound to colour and alpha xconf nodes.
 *
 * Initialises the button with the colour from @xc_color (parsed as "#RRGGBB").
 * If @xc_alpha is provided, alpha editing is enabled and the xc_alpha pointer
 * is stored in the widget's "alpha" object data for the callback.
 *
 * If @xc_alpha is provided the stored 0-255 alpha value is read and applied
 * to the colour button's initial display via c.alpha before the first
 * gtk_color_chooser_set_rgba() call.
 *
 * Returns: (transfer none) GtkColorButton.
 */
GtkWidget *
gconf_edit_color(gconf_block *b, xconf *xc_color, xconf *xc_alpha)
{

    GtkWidget *w;
    GdkRGBA c;

    gdk_rgba_parse(&c, xconf_get_value(xc_color));

    w = gtk_color_button_new();
    if (xc_alpha)
    {
        gint a = 0;

        xconf_get_int(xc_alpha, &a);
        c.alpha = CLAMP(a, 0, 255) / 255.0;
        g_object_set_data(G_OBJECT(w), "alpha", xc_alpha);
    }
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w), &c);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(w),
        xc_alpha != NULL);

    g_signal_connect(G_OBJECT(w), "color-set",
        G_CALLBACK(gconf_edit_color_cb), xc_color);
    if (b && b->cb)
    {
        g_signal_connect_swapped(G_OBJECT(w), "color-set",
            G_CALLBACK(b->cb), b);
    }

    return w;
}
