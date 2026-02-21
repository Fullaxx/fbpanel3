/**
 * @file image.c
 * @brief Static image plugin for fbpanel.
 *
 * Loads an image file (any format supported by GdkPixbuf), scales it to fit
 * the panel height (horizontal) or width (vertical) with a 2-pixel margin,
 * and displays it in a GtkImage wrapped in a GtkEventBox.  An optional
 * tooltip markup string can also be configured.
 *
 * Config keys:
 *   image   (str, required)   — path to the image file; "~" prefix is
 *            expanded via expand_tilda() to the home directory.
 *   tooltip (str, optional)   — Pango markup tooltip text.
 *
 * Note on ownership: "image" is read via XCG str (transfer-none, xconf-owned)
 * and immediately passed to expand_tilda() which returns a new g_strdup'd
 * string (transfer-full, freed below with g_free).  "tooltip" is also read
 * via XCG str (transfer-none) but is then incorrectly g_free'd — see BUG-025.
 *
 * Main widget: GtkEventBox (img->mainw) containing a GtkImage, packed into
 * p->pwid.  img->pix (cairo_surface_t) is allocated in the struct but is
 * always NULL in practice (never set in the constructor).
 */

#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"



typedef struct {
    plugin_instance plugin;
    cairo_surface_t *pix;  /**< Unused; always NULL. Struct dead field. */
    void *mask;            /**< Unused. */
    GtkWidget *mainw;      /**< GtkEventBox wrapping the image; owned by pwid. */
} image_priv;


/**
 * image_destructor - destroy the event box and free the unused cairo surface.
 * @p: plugin_instance. (transfer none)
 *
 * img->mainw is explicitly destroyed (it is a child of p->pwid and would
 * also be destroyed by the parent, but explicit destruction is safe).
 * img->pix is always NULL in practice (never set), so the cairo_surface_destroy
 * call is a no-op.
 */
static void
image_destructor(plugin_instance *p)
{
    image_priv *img = (image_priv *) p;

    gtk_widget_destroy(img->mainw);
    if (img->pix)
        cairo_surface_destroy(img->pix);
    return;
}

/**
 * image_constructor - load, scale, and display the configured image.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads "image" (transfer-none xconf str) and passes it through expand_tilda()
 * to get a transfer-full path string (freed at end of constructor).  Loads the
 * pixbuf, scales it to fill the panel dimension minus 2 pixels, creates a
 * GtkImage from the scaled pixbuf (which transfers ownership to GtkImage), and
 * packs it into a GtkEventBox added to p->pwid.
 *
 * If the "tooltip" key is configured, its value (transfer-none xconf str) is
 * set as markup on the event box then g_free'd.  NOTE: this g_free is incorrect
 * (BUG-025) — the tooltip pointer is transfer-none and is freed a second time
 * by xconf_del when the panel exits, causing a heap double-free.
 *
 * Returns: 1 on success (even if the image could not be loaded; a "?" label
 * is shown instead).
 */
static int
image_constructor(plugin_instance *p)
{
    gchar *tooltip, *fname;
    image_priv *img;
    GdkPixbuf *gp, *gps;
    GtkWidget *wid;
    GError *err = NULL;

    img = (image_priv *) p;
    tooltip = fname = 0;
    XCG(p->xc, "image", &fname, str);
    XCG(p->xc, "tooltip", &tooltip, str);
    fname = expand_tilda(fname); /* returns transfer-full g_strdup'd path */

    img->mainw = gtk_event_box_new();
    gtk_widget_show(img->mainw);
    gp = gdk_pixbuf_new_from_file(fname, &err);
    if (!gp) {
        g_warning("image: can't read image %s\n", fname);
        wid = gtk_label_new("?");
    } else {
        float ratio;

        ratio = (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) ?
            (float) (p->panel->ah - 2) / (float) gdk_pixbuf_get_height(gp)
            : (float) (p->panel->aw - 2) / (float) gdk_pixbuf_get_width(gp);
        gps =  gdk_pixbuf_scale_simple (gp,
              ratio * ((float) gdk_pixbuf_get_width(gp)),
              ratio * ((float) gdk_pixbuf_get_height(gp)),
              GDK_INTERP_HYPER);
        g_object_unref(gp);
        wid = gtk_image_new_from_pixbuf(gps);
        g_object_unref(gps);
    }
    gtk_widget_show(wid);
    gtk_container_add(GTK_CONTAINER(img->mainw), wid);
    gtk_container_set_border_width(GTK_CONTAINER(img->mainw), 0);
    g_free(fname); /* correct: expand_tilda returned transfer-full */
    gtk_container_add(GTK_CONTAINER(p->pwid), img->mainw);
    if (tooltip) {
        gtk_widget_set_tooltip_markup(img->mainw, tooltip);
        g_free(tooltip); /* BUG-025: tooltip is transfer-none (XCG str) */
    }
    return 1;
}


static plugin_class class = {
    .count       = 0,
    .type        = "image",
    .name        = "Show Image",
    .version     = "1.0",
    .description = "Dispaly Image and Tooltip",
    .priv_size   = sizeof(image_priv),

    .constructor = image_constructor,
    .destructor  = image_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
