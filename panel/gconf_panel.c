/**
 * @file gconf_panel.c
 * @brief Panel preferences dialog — global settings tab and dialog lifecycle.
 *
 * Implements the top-level `configure(xconf *xc)` entry point and the
 * three tab pages of the fbpanel preferences dialog:
 *
 *   1. "Panel" tab  (mk_tab_global)  — Geometry, Properties, Visual Effects
 *   2. "Plugins" tab (mk_tab_plugins) — plugin list (implemented in gconf_plugins.c)
 *   3. "Profile" tab (mk_tab_profile) — read-only profile path display
 *
 * DIALOG LIFECYCLE
 * ----------------
 * The dialog is a GTK_DIALOG_MODAL window, but gtk_window_set_modal() is
 * immediately called with FALSE so it behaves as a non-blocking preferences
 * window.  Only one instance can be open at a time (guarded by the static
 * `dialog` pointer).
 *
 * XCONF SNAPSHOT PATTERN
 * ----------------------
 * mk_dialog() creates two deep copies of the panel's config tree (oxc):
 *   - A "baseline" snapshot stored as dialog object data under "oxc".
 *   - A "working copy" (xc) passed to the tab builders; edit widgets write
 *     into this copy immediately on user interaction.
 *
 * When the user clicks Apply or OK, dialog_response_event() calls xconf_cmp()
 * to compare xc against the baseline.  If they differ, the config is saved
 * (xconf_save_to_profile) and gtk_main_quit() is called to trigger a panel
 * restart (the main loop restarts the panel from the new config on the next
 * iteration).  The dialog is destroyed and both xconf copies are freed on
 * Close/OK/delete-event.
 *
 * STATIC BLOCK POINTERS
 * ---------------------
 * gl_block, geom_block, prop_block, effects_block, color_block, corner_block,
 * layer_block, and ah_block are file-scope statics.  They are valid for the
 * lifetime of the dialog and are freed in dialog_response_event() after
 * gtk_widget_destroy(dialog).  These must NOT be accessed after the dialog is
 * closed.
 *
 * CONDITIONAL SENSITIVITY
 * -----------------------
 * Sub-blocks are conditionally enabled/disabled via gtk_widget_set_sensitive:
 *   - color_block  — enabled only when "transparent" is checked
 *   - corner_block — enabled only when "roundcorners" is checked
 *   - ah_block     — enabled only when "autohide" is checked
 *   - layer_block  — enabled only when "setlayer" is checked
 * The sensitivity is updated immediately on dialog creation and on every change
 * via the effects_changed() and prop_changed() callbacks.
 */

#include "gconf.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"

/** Active preferences dialog; NULL when closed. Only one dialog at a time. */
static GtkWidget *dialog;

/* Geometry tab widget handles (used by geom_changed to adjust spin ranges). */
static GtkWidget *width_spin, *width_opt;
static GtkWidget *xmargin_spin, *ymargin_spin;
static GtkWidget *allign_opt;

/* gconf_block handles for all sub-sections; freed in dialog_response_event. */
static gconf_block *gl_block;
static gconf_block *geom_block;
static gconf_block *prop_block;
static gconf_block *effects_block;
static gconf_block *color_block;
static gconf_block *corner_block;
static gconf_block *layer_block;
static gconf_block *ah_block;

/** Indent in pixels for second-level sub-blocks (color, corner, ah, layer). */
#define INDENT_2 25


/** Forward declaration: Plugins tab builder (implemented in gconf_plugins.c). */
GtkWidget *mk_tab_plugins(xconf *xc);

/*********************************************************
 * panel effects
 *********************************************************/

/**
 * effects_changed - update sensitivity of effect sub-blocks on any effects change.
 * @b: The effects_block whose xconf data (panel xconf) is read.
 *
 * Called as a swapped GCallback whenever any widget in effects_block changes.
 * Reads the current values of "transparent", "roundcorners", and "autohide"
 * from the working xconf and shows/hides the dependent sub-blocks accordingly.
 */
static void
effects_changed(gconf_block *b)
{
    int i;

    XCG(b->data, "transparent", &i, enum, bool_enum);
    gtk_widget_set_sensitive(color_block->main, i);
    XCG(b->data, "roundcorners", &i, enum, bool_enum);
    gtk_widget_set_sensitive(corner_block->main, i);
    XCG(b->data, "autohide", &i, enum, bool_enum);
    gtk_widget_set_sensitive(ah_block->main, i);

    return;
}


/**
 * mk_effects_block - build the "Visual Effects" section into gl_block.
 * @xc: Working xconf node for the "global" config section.
 *
 * Creates effects_block and its four sub-blocks (color_block, corner_block,
 * ah_block) and packs them all into gl_block.
 *
 * Note: the "Max Element Height" spin is added to effects_block directly
 * (not via a sub-block) without a conditional sensitivity guard.
 */
static void
mk_effects_block(xconf *xc)
{
    GtkWidget *w;


    /* label */
    w = gtk_label_new(NULL);
    gtk_widget_set_halign(w, GTK_ALIGN_START); gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Visual Effects</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* effects */
    effects_block = gconf_block_new((GCallback)effects_changed, xc, 10);

    /* transparency */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "transparent"),
        _("Transparency"));
    gconf_block_add(effects_block, w, TRUE);

    color_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Color settings"));
    gconf_block_add(color_block, w, TRUE);
    w = gconf_edit_color(color_block, xconf_get(xc, "tintcolor"),
        xconf_get(xc, "alpha"));
    gconf_block_add(color_block, w, FALSE);

    gconf_block_add(effects_block, color_block->main, TRUE);

    /* round corners */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "roundcorners"),
        _("Round corners"));
    gconf_block_add(effects_block, w, TRUE);

    corner_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Radius is "));
    gconf_block_add(corner_block, w, TRUE);
    w = gconf_edit_int(geom_block, xconf_get(xc, "roundcornersradius"), 0, 30);
    gconf_block_add(corner_block, w, FALSE);
    w = gtk_label_new(_("pixels"));
    gconf_block_add(corner_block, w, FALSE);
    gconf_block_add(effects_block, corner_block->main, TRUE);

    /* auto hide */
    w = gconf_edit_boolean(effects_block, xconf_get(xc, "autohide"),
        _("Autohide"));
    gconf_block_add(effects_block, w, TRUE);

    ah_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Height when hidden is "));
    gconf_block_add(ah_block, w, TRUE);
    w = gconf_edit_int(ah_block, xconf_get(xc, "heightwhenhidden"), 0, 10);
    gconf_block_add(ah_block, w, FALSE);
    w = gtk_label_new(_("pixels"));
    gconf_block_add(ah_block, w, FALSE);
    gconf_block_add(effects_block, ah_block->main, TRUE);

    w = gconf_edit_int(effects_block, xconf_get(xc, "maxelemheight"), 0, 128);
    gconf_block_add(effects_block, gtk_label_new(_("Max Element Height")), TRUE);
    gconf_block_add(effects_block, w, FALSE);
    gconf_block_add(gl_block, effects_block->main, TRUE);

    /* empty row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/*********************************************************
 * panel properties
 *********************************************************/

/**
 * prop_changed - update sensitivity of layer_block when "setlayer" changes.
 * @b: The prop_block whose xconf data is read.
 *
 * Called as a swapped GCallback whenever any widget in prop_block changes.
 * layer_block is enabled only when "setlayer" is checked.
 */
static void
prop_changed(gconf_block *b)
{
    int i = 0;

    XCG(b->data, "setlayer", &i, enum, bool_enum);
    gtk_widget_set_sensitive(layer_block->main, i);
    return;
}

/**
 * mk_prop_block - build the "Properties" section into gl_block.
 * @xc: Working xconf node for the "global" config section.
 *
 * Creates prop_block with checkboxes for "Do not cover by maximized windows"
 * (setpartialstrut), "Set 'Dock' type" (setdocktype), "Set stacking layer"
 * (setlayer), and the conditional layer_block with an enum combo.
 */
static void
mk_prop_block(xconf *xc)
{
    GtkWidget *w;


    /* label */
    w = gtk_label_new(NULL);
    gtk_widget_set_halign(w, GTK_ALIGN_START); gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Properties</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* properties */
    prop_block = gconf_block_new((GCallback)prop_changed, xc, 10);

    /* strut */
    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setpartialstrut"),
        _("Do not cover by maximized windows"));
    gconf_block_add(prop_block, w, TRUE);

    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setdocktype"),
        _("Set 'Dock' type"));
    gconf_block_add(prop_block, w, TRUE);

    /* set layer */
    w = gconf_edit_boolean(prop_block, xconf_get(xc, "setlayer"),
        _("Set stacking layer"));
    gconf_block_add(prop_block, w, TRUE);

    layer_block = gconf_block_new(NULL, NULL, INDENT_2);
    w = gtk_label_new(_("Panel is "));
    gconf_block_add(layer_block, w, TRUE);
    w = gconf_edit_enum(layer_block, xconf_get(xc, "layer"),
        layer_enum);
    gconf_block_add(layer_block, w, FALSE);
    w = gtk_label_new(_("all windows"));
    gconf_block_add(layer_block, w, FALSE);
    gconf_block_add(prop_block, layer_block->main, TRUE);


    gconf_block_add(gl_block, prop_block->main, TRUE);

    /* empty row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/*********************************************************
 * panel geometry
 *********************************************************/

/**
 * geom_changed - update width spin range and xmargin sensitivity on geometry change.
 * @b: The geom_block whose data (panel xconf) is read.
 *
 * - xmargin_spin is enabled only when allign != ALLIGN_CENTER.
 * - width_spin is enabled only when widthtype != WIDTH_REQUEST.
 * - If widthtype == WIDTH_PERCENT, the width range is clamped to [0, 100].
 * - If widthtype == WIDTH_PIXEL, the range is set to the monitor dimension
 *   (width for top/bottom panels, height for left/right panels).
 */
static void
geom_changed(gconf_block *b)
{
    int i, j;

    i = gtk_combo_box_get_active(GTK_COMBO_BOX(allign_opt));
    gtk_widget_set_sensitive(xmargin_spin, (i != ALLIGN_CENTER));
    i = gtk_combo_box_get_active(GTK_COMBO_BOX(width_opt));
    gtk_widget_set_sensitive(width_spin, (i != WIDTH_REQUEST));
    if (i == WIDTH_PERCENT)
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(width_spin), 0, 100);
    else if (i == WIDTH_PIXEL) {
        GdkRectangle  geometry;
        GdkDisplay   *display = gtk_widget_get_display(b->main);
        GdkWindow    *window  = gtk_widget_get_window(b->main);
        GdkMonitor   *monitor = gdk_display_get_monitor_at_window(display, window);
        gdk_monitor_get_geometry(monitor, &geometry);
        XCG(b->data, "edge", &j, enum, edge_enum);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(width_spin), 0,
            (j == EDGE_RIGHT || j == EDGE_LEFT)
            ? geometry.height : geometry.width);
    }
    return;
}

/**
 * mk_geom_block - build the "Geometry" section into gl_block.
 * @xc: Working xconf node for the "global" config section.
 *
 * Creates geom_block with spin+combo rows for Width (value + widthtype),
 * Height, Edge, Alignment, X Margin, and Y Margin.
 * The file-static width_spin, width_opt, allign_opt, xmargin_spin, ymargin_spin
 * pointers are saved for use by geom_changed.
 */
static void
mk_geom_block(xconf *xc)
{
    GtkWidget *w;


    /* label */
    w = gtk_label_new(NULL);
    gtk_widget_set_halign(w, GTK_ALIGN_START); gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_label_set_markup(GTK_LABEL(w), _("<b>Geometry</b>"));
    gconf_block_add(gl_block, w, TRUE);

    /* geometry */
    geom_block = gconf_block_new((GCallback)geom_changed, xc, 10);

    w = gconf_edit_int(geom_block, xconf_get(xc, "width"), 0, 2300);
    gconf_block_add(geom_block, gtk_label_new(_("Width")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    width_spin = w;

    w = gconf_edit_enum(geom_block, xconf_get(xc, "widthtype"),
        widthtype_enum);
    gconf_block_add(geom_block, w, FALSE);
    width_opt = w;

    w = gconf_edit_int(geom_block, xconf_get(xc, "height"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("Height")), TRUE);
    gconf_block_add(geom_block, w, FALSE);

    w = gconf_edit_enum(geom_block, xconf_get(xc, "edge"),
        edge_enum);
    gconf_block_add(geom_block, gtk_label_new(_("Edge")), TRUE);
    gconf_block_add(geom_block, w, FALSE);

    w = gconf_edit_enum(geom_block, xconf_get(xc, "allign"),
        allign_enum);
    gconf_block_add(geom_block, gtk_label_new(_("Alignment")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    allign_opt = w;

    w = gconf_edit_int(geom_block, xconf_get(xc, "xmargin"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("X Margin")), TRUE);
    gconf_block_add(geom_block, w, FALSE);
    xmargin_spin = w;

    w = gconf_edit_int(geom_block, xconf_get(xc, "ymargin"), 0, 300);
    gconf_block_add(geom_block, gtk_label_new(_("Y Margin")), FALSE);
    gconf_block_add(geom_block, w, FALSE);
    ymargin_spin = w;

    gconf_block_add(gl_block, geom_block->main, TRUE);

    /* empty row */
    gconf_block_add(gl_block, gtk_label_new(" "), TRUE);
}

/**
 * mk_tab_global - build and return the "Panel" notebook tab page.
 * @xc: Working xconf tree root.
 *
 * Creates gl_block and calls mk_geom_block, mk_prop_block, mk_effects_block
 * in order.  Then calls the three changed-callbacks to set initial sensitivity
 * of all conditional sub-blocks.
 *
 * Returns: (transfer none) GtkBox page widget; owned by the dialog notebook.
 */
static GtkWidget *
mk_tab_global(xconf *xc)
{
    GtkWidget *page;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gl_block = gconf_block_new(NULL, NULL, 0);
    gtk_box_pack_start(GTK_BOX(page), gl_block->main, FALSE, TRUE, 0);

    mk_geom_block(xc);
    mk_prop_block(xc);
    mk_effects_block(xc);

    gtk_widget_show_all(page);

    geom_changed(geom_block);
    effects_changed(effects_block);
    prop_changed(prop_block);

    return page;
}

/**
 * mk_tab_profile - build and return the "Profile" notebook tab page.
 * @xc: Working xconf tree (unused; profile info comes from panel_get_profile()).
 *
 * Displays a read-only label showing the active profile name and its file path.
 *
 * Returns: (transfer none) GtkBox page widget; owned by the dialog notebook.
 */
static GtkWidget *
mk_tab_profile(xconf *xc)
{
    GtkWidget *page, *label;
    gchar *s1;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    s1 = g_strdup_printf(_("You're using '<b>%s</b>' profile, stored at\n"
            "<tt>%s</tt>"), panel_get_profile(), panel_get_profile_file());
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), s1);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, TRUE, 0);
    g_free(s1);

    gtk_widget_show_all(page);
    return page;
}

/**
 * dialog_response_event - handle Apply, OK, Close, and delete-event responses.
 * @_dialog: The dialog (unused parameter; use the static `dialog` pointer).
 * @rid:     GTK_RESPONSE_APPLY, GTK_RESPONSE_OK, GTK_RESPONSE_CLOSE, or
 *           GTK_RESPONSE_DELETE_EVENT.
 * @xc:      The working xconf copy (connected via g_signal_connect).
 *
 * On Apply or OK:
 *   - Compares working xc against the baseline snapshot (retrieved from dialog
 *     object data "oxc") using xconf_cmp.
 *   - If different, saves the new config and calls gtk_main_quit() to trigger
 *     a panel restart.  The baseline snapshot is replaced with the new state.
 *
 * On Close, OK, or delete-event:
 *   - Destroys the dialog.
 *   - Frees all eight gconf_blocks (AFTER gtk_widget_destroy so widgets are gone).
 *   - Frees both the working xc and the baseline oxc trees via xconf_del.
 *   - Sets the static `dialog` pointer to NULL.
 */
static void
dialog_response_event(GtkDialog *_dialog, gint rid, xconf *xc)
{
    xconf *oxc = g_object_get_data(G_OBJECT(dialog), "oxc");

    if (rid == GTK_RESPONSE_APPLY ||
        rid == GTK_RESPONSE_OK)
    {
        DBG("apply changes\n");
        if (xconf_cmp(xc, oxc))
        {
            xconf_del(oxc, FALSE);
            oxc = xconf_dup(xc);
            g_object_set_data(G_OBJECT(dialog), "oxc", oxc);
            xconf_save_to_profile(xc);
            gtk_main_quit();
        }
    }
    if (rid == GTK_RESPONSE_DELETE_EVENT ||
        rid == GTK_RESPONSE_CLOSE ||
        rid == GTK_RESPONSE_OK)
    {
        gtk_widget_destroy(dialog);
        dialog = NULL;
        gconf_block_free(geom_block);
        gconf_block_free(gl_block);
        gconf_block_free(effects_block);
        gconf_block_free(color_block);
        gconf_block_free(corner_block);
        gconf_block_free(layer_block);
        gconf_block_free(prop_block);
        gconf_block_free(ah_block);
        xconf_del(xc, FALSE);
        xconf_del(oxc, FALSE);
    }
    return;
}

/**
 * dialog_cancel - handle the dialog's "delete-event" (window-manager close).
 * @_dialog: The dialog.
 * @event:   The GdkEvent (unused).
 * @xc:      The working xconf copy.
 *
 * Delegates to dialog_response_event with GTK_RESPONSE_CLOSE.
 * Returns FALSE so GTK proceeds with the default delete-event handling.
 */
static gboolean
dialog_cancel(GtkDialog *_dialog, GdkEvent *event, xconf *xc)
{
    dialog_response_event(_dialog, GTK_RESPONSE_CLOSE, xc);
    return FALSE;
}

/**
 * mk_dialog - create the preferences GtkDialog with all three tab pages.
 * @oxc: The original (live) panel xconf tree; two deep copies are taken here.
 *       @oxc itself is NOT modified by the dialog.
 *
 * Creates two xconf_dup copies of @oxc:
 *   1. Stored as dialog object data "oxc" — the baseline snapshot for change
 *      detection.  Freed in dialog_response_event on close.
 *   2. The working copy (xc) passed to tab builders and to the response
 *      signal.  Freed in dialog_response_event on close.
 *
 * The dialog is 400x500 px, non-modal (gtk_window_set_modal FALSE), and uses
 * IMGPREFIX "/logo.png" as its window icon.
 *
 * Returns: (transfer none) the newly-created GtkDialog (also stored in `dialog`).
 */
static GtkWidget *
mk_dialog(xconf *oxc)
{
    GtkWidget *sw, *nb, *label;
    gchar *name;
    xconf *xc;

    DBG("creating dialog\n");
    //name = g_strdup_printf("fbpanel settings: <%s> profile", cprofile);
    name = g_strdup_printf("fbpanel settings: <%s> profile",
        panel_get_profile());
    dialog = gtk_dialog_new_with_buttons(name,
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        _("_Apply"),  GTK_RESPONSE_APPLY,
        _("_OK"),     GTK_RESPONSE_OK,
        _("_Close"),  GTK_RESPONSE_CLOSE,
        NULL);
    g_free(name);
    DBG("connecting sugnal to %p\n",  dialog);

    xc = xconf_dup(oxc);
    g_object_set_data(G_OBJECT(dialog), "oxc", xc);
    xc = xconf_dup(oxc);

    g_signal_connect (G_OBJECT(dialog), "response",
        (GCallback) dialog_response_event, xc);
    // g_signal_connect (G_OBJECT(dialog), "destroy",
    //  (GCallback) dialog_cancel, xc);
    g_signal_connect (G_OBJECT(dialog), "delete_event",
        (GCallback) dialog_cancel, xc);

    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 500);
    gtk_window_set_icon_from_file(GTK_WINDOW(dialog),
        IMGPREFIX "/logo.png", NULL);

    nb = gtk_notebook_new();
    gtk_notebook_set_show_border (GTK_NOTEBOOK(nb), FALSE);
    gtk_container_add (GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), nb);

    sw = mk_tab_global(xconf_get(xc, "global"));
    label = gtk_label_new(_("Panel"));
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_widget_set_margin_top(label, 1);
    gtk_widget_set_margin_bottom(label, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    sw = mk_tab_plugins(xc);
    label = gtk_label_new(_("Plugins"));
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_widget_set_margin_top(label, 1);
    gtk_widget_set_margin_bottom(label, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    sw = mk_tab_profile(xc);
    label = gtk_label_new(_("Profile"));
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_widget_set_margin_top(label, 1);
    gtk_widget_set_margin_bottom(label, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, label);

    gtk_widget_show_all(dialog);
    return dialog;
}

/**
 * configure - open (or raise) the panel preferences dialog.
 * @xc: The live panel xconf tree; passed to mk_dialog to take a working copy.
 *
 * If no dialog is open, creates one via mk_dialog.  If a dialog is already
 * open, raises it via gtk_widget_show.
 *
 * Note: misc.h declares configure() with no parameters (void configure()).
 * This definition takes an xconf* parameter — see BUG-015 in
 * docs/BUGS_AND_ISSUES.md for the signature mismatch.
 */
void
configure(xconf *xc)
{
    DBG("dialog %p\n",  dialog);
    if (!dialog)
        dialog = mk_dialog(xc);
    gtk_widget_show(dialog);
    return;
}
