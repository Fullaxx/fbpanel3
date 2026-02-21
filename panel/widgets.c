/**
 * @file widgets.c
 * @brief fbpanel widget factories — pixbuf, image, button, calendar (implementation).
 *
 * Implements the four public factories declared in widgets.h.
 *
 * INTERNAL DESIGN
 * ---------------
 * State for fb_image and fb_button is stored in an fb_image_conf_t struct
 * allocated with g_new0() in fb_image_new() and attached to the GtkImage widget
 * via g_object_set_data(G_OBJECT(image), "conf", conf).  This avoids subclassing
 * GtkImage while still allowing per-instance data to follow the widget lifetime:
 * a "destroy" callback frees the struct when the widget is destroyed.
 *
 * PIXBUF TRIPLE LIFETIME
 * ----------------------
 * pix[0] — normal; created in fb_image_new() and refreshed on icon-theme change.
 * pix[1] — highlight; created lazily by fb_button_new(); NULL for plain fb_images.
 * pix[2] — press; created lazily by fb_button_new(); NULL for plain fb_images.
 *
 * On icon-theme change (fb_image_icon_theme_changed), pix[0] is always rebuilt.
 * pix[1] and pix[2] are only rebuilt when conf->hicolor != 0 (button mode);
 * plain images (hicolor==0) skip the highlight/press rebuild.
 *
 * BUTTON EVENT ROUTING
 * --------------------
 * Events are connected on the GtkBgbox parent, not the GtkImage child, using
 * g_signal_connect_swapped so that the GtkImage is passed as the first argument
 * to the callbacks.  button-press/release handlers return FALSE to allow event
 * propagation to the plugin's own "button-press-event" handler.
 * enter/leave-notify handlers return TRUE (event consumed).
 */

#include <gtk/gtk.h>

#include "panel.h"
#include "misc.h"
#include "widgets.h"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"

/**********************************************************************
 * FB Pixbuf                                                          *
 **********************************************************************/

/** Maximum pixbuf size in either dimension (clamped in fb_pixbuf_new). */
#define MAX_SIZE 192

/**
 * fb_pixbuf_new - load a GdkPixbuf from an icon name and/or a file path.
 * @iname:        Icon name for gtk_icon_theme_load_icon(); may be NULL.
 * @fname:        File path for gdk_pixbuf_new_from_file_at_size(); may be NULL.
 * @width:        Desired width; used as the size hint for icon lookup.
 * @height:       Desired height; used when loading from a file.
 * @use_fallback: If TRUE and both sources fail, load "gtk-missing-image".
 *
 * Tries sources in order: icon name → file path → fallback.
 * The size passed to icon_theme is clamped to MIN(192, MAX(width, height)) so
 * the theme never has to produce an excessively large icon.
 * GTK_ICON_LOOKUP_FORCE_SIZE ensures the theme scales to exactly that size.
 *
 * Returns: (transfer full) GdkPixbuf*, or NULL if all sources failed.
 *          Caller must g_object_unref() the result when done.
 */
GdkPixbuf *
fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback)
{
    GdkPixbuf *pb = NULL;
    int size;

    size = MIN(192, MAX(width, height));
    if (iname && !pb)
        pb = gtk_icon_theme_load_icon(icon_theme, iname, size,
            GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (fname && !pb)
        pb = gdk_pixbuf_new_from_file_at_size(fname, width, height, NULL);
    if (use_fallback && !pb)
        pb = gtk_icon_theme_load_icon(icon_theme, "gtk-missing-image", size,
            GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    return pb;
}

/**
 * fb_pixbuf_make_back_image - create a hover-highlight pixbuf from a base image.
 * @front:   Base pixbuf to highlight; may be NULL (returns NULL immediately).
 * @hicolor: Highlight colour as 0xRRGGBB; each channel is added to the pixel's
 *           R/G/B values and clamped to 255.  0x000000 produces a no-op copy.
 *
 * Creates a new RGBA pixbuf (via gdk_pixbuf_add_alpha) and additively blends
 * @hicolor into each non-transparent pixel.  The alpha channel is preserved;
 * fully transparent pixels (A == 0) are skipped.
 *
 * On allocation failure (gdk_pixbuf_add_alpha returns NULL), returns @front
 * with an extra g_object_ref() — the caller still receives a (transfer full)
 * reference that it must eventually g_object_unref().
 *
 * Returns: (transfer full) GdkPixbuf*, or NULL if @front was NULL.
 */
static GdkPixbuf *
fb_pixbuf_make_back_image(GdkPixbuf *front, gulong hicolor)
{
    GdkPixbuf *back;
    guchar *src, *up, extra[3];
    int i;

    if(!front)
    {
        return front;
    }
    back = gdk_pixbuf_add_alpha(front, FALSE, 0, 0, 0);
    if (!back) {
        g_object_ref(G_OBJECT(front));
        return front;
    }
    src = gdk_pixbuf_get_pixels (back);
    for (i = 2; i >= 0; i--, hicolor >>= 8)
        extra[i] = hicolor & 0xFF;
    for (up = src + gdk_pixbuf_get_height(back) * gdk_pixbuf_get_rowstride (back);
            src < up; src+=4) {
        if (src[3] == 0)
            continue;
        for (i = 0; i < 3; i++) {
            if (src[i] + extra[i] >= 255)
                src[i] = 255;
            else
                src[i] += extra[i];
        }
    }
    return back;
}

/**
 * PRESS_GAP - pixel inset used to simulate button-press shrink effect.
 * The press pixbuf is scaled to (W - 2*PRESS_GAP) × (H - 2*PRESS_GAP) and
 * then composited onto a transparent full-size canvas at offset (PRESS_GAP, PRESS_GAP).
 */
#define PRESS_GAP 2

/**
 * fb_pixbuf_make_press_image - create a pressed-state pixbuf from a highlight image.
 * @front: Source pixbuf (normally pix[1], the highlight version); may be NULL.
 *
 * Creates a pressed-state visual by scaling @front down by PRESS_GAP pixels on
 * each edge and centring it on a transparent canvas the same size as @front.
 * This gives the illusion that the icon has been pushed in when clicked.
 *
 * Allocation path:
 *   - gdk_pixbuf_copy(@front) → cleared canvas (transparent)
 *   - gdk_pixbuf_scale_simple → scaled-down version
 *   - gdk_pixbuf_copy_area → composited centred into canvas
 *
 * On any allocation failure, returns @front with an extra g_object_ref().
 * If @front is NULL, returns NULL immediately.
 *
 * Returns: (transfer full) GdkPixbuf*, or NULL if @front was NULL.
 */
static GdkPixbuf *
fb_pixbuf_make_press_image(GdkPixbuf *front)
{
    GdkPixbuf *press, *tmp;
    int w, h;

    if(!front)
    {
        return front;
    }
    w = gdk_pixbuf_get_width(front) - 2 * PRESS_GAP;
    h = gdk_pixbuf_get_height(front) - 2 * PRESS_GAP;
    press = gdk_pixbuf_copy(front);
    tmp = gdk_pixbuf_scale_simple(front, w, h, GDK_INTERP_HYPER);
    if (press && tmp) {
        gdk_pixbuf_fill(press, 0);
        gdk_pixbuf_copy_area(tmp,
                0, 0,  // src_x, src_y
                w, h,  // width, height
                press, // dest_pixbuf
                PRESS_GAP, PRESS_GAP);  // dst_x, dst_y
        g_object_unref(G_OBJECT(tmp));
        return press;
    }
    if (press)
        g_object_unref(G_OBJECT(press));
    if (tmp)
        g_object_unref(G_OBJECT(tmp));

    g_object_ref(G_OBJECT(front));
    return front;
}

/**********************************************************************
 * FB Image                                                           *
 **********************************************************************/

/** Number of pixbuf slots per image: normal (0), highlight (1), press (2). */
#define PIXBBUF_NUM 3

/**
 * fb_image_conf_t - per-instance state attached to a GtkImage via g_object_set_data.
 *
 * @iname:   g_strdup() copy of the icon name; g_free()'d in fb_image_free().
 * @fname:   g_strdup() copy of the file path; g_free()'d in fb_image_free().
 * @width:   Desired pixel width passed to fb_pixbuf_new().
 * @height:  Desired pixel height passed to fb_pixbuf_new().
 * @itc_id:  Signal handler ID for GtkIconTheme::changed; disconnected in
 *           fb_image_free() before the conf struct is freed.
 * @hicolor: Hover highlight colour as 0xRRGGBB; 0 disables highlighting.
 *           Set by fb_button_new() after fb_image_new() returns.
 * @i:       Index of the currently displayed pixbuf (0/1/2); used to avoid
 *           redundant gtk_image_set_from_pixbuf() calls.
 * @pix:     Array of PIXBBUF_NUM (3) GdkPixbuf* refs, each (transfer full).
 *           pix[0] = normal; pix[1] = highlight; pix[2] = press.
 *           pix[1] and pix[2] are NULL for plain fb_image_new() images until
 *           the first icon-theme-changed event (see BUG-008).
 */
typedef struct {
    gchar *iname, *fname;
    int width, height;
    gulong itc_id; /* icon theme change callback id */
    gulong hicolor;
    int i; /* pixbuf index */
    GdkPixbuf *pix[PIXBBUF_NUM];
} fb_image_conf_t;

static void fb_image_free(GObject *image);
static void fb_image_icon_theme_changed(GtkIconTheme *icon_theme,
        GtkWidget *image);

/**
 * fb_image_new - create a self-updating GtkImage widget.
 * @iname:  Icon name; passed to fb_pixbuf_new(); may be NULL.
 * @fname:  File path; passed to fb_pixbuf_new(); may be NULL.
 * @width:  Desired image width in pixels.
 * @height: Desired image height in pixels.
 *
 * Allocates an fb_image_conf_t (g_new0, exits on OOM) and attaches it to the
 * GtkImage via g_object_set_data(G_OBJECT(image), "conf", conf).
 *
 * Connects two signals:
 *   - GtkIconTheme::changed → fb_image_icon_theme_changed: rebuilds all pixbufs
 *     when the user switches icon themes.  Handler ID stored in conf->itc_id and
 *     disconnected in fb_image_free() to avoid dangling callbacks.
 *   - GtkWidget::destroy → fb_image_free: frees conf, disconnects the theme
 *     handler, and unrefs all pixbufs.
 *
 * @iname and @fname are g_strdup()'d; the caller need not keep them alive.
 * gtk_widget_show() is called before return so the image is immediately visible
 * when added to a container.
 *
 * Returns: (transfer full) GtkWidget* (GtkImage).
 */
GtkWidget *
fb_image_new(gchar *iname, gchar *fname, int width, int height)
{
    GtkWidget *image;
    fb_image_conf_t *conf;

    image = gtk_image_new();
    conf = g_new0(fb_image_conf_t, 1); /* exits if fails */
    g_object_set_data(G_OBJECT(image), "conf", conf);
    conf->itc_id = g_signal_connect_after (G_OBJECT(icon_theme),
            "changed", (GCallback) fb_image_icon_theme_changed, image);
    g_signal_connect (G_OBJECT(image),
            "destroy", (GCallback) fb_image_free, NULL);
    conf->iname = g_strdup(iname);
    conf->fname = g_strdup(fname);
    conf->width = width;
    conf->height = height;
    conf->pix[0] = fb_pixbuf_new(iname, fname, width, height, TRUE);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), conf->pix[0]);
    gtk_widget_show(image);
    return image;
}


/**
 * fb_image_free - GtkWidget::destroy handler; frees all fb_image_conf_t resources.
 * @image: GObject* (GtkImage) being destroyed.
 *
 * Disconnects the icon-theme "changed" signal handler (conf->itc_id) from the
 * global icon_theme singleton, frees conf->iname and conf->fname, unrefs all
 * non-NULL entries in conf->pix[], then g_free()s the conf struct itself.
 *
 * Called automatically when the GtkImage widget is destroyed (either explicitly
 * or when its parent container is destroyed).
 */
static void
fb_image_free(GObject *image)
{
    fb_image_conf_t *conf;
    int i;

    conf = g_object_get_data(image, "conf");
    g_signal_handler_disconnect(G_OBJECT(icon_theme), conf->itc_id);
    g_free(conf->iname);
    g_free(conf->fname);
    for (i = 0; i < PIXBBUF_NUM; i++)
        if (conf->pix[i])
            g_object_unref(G_OBJECT(conf->pix[i]));
    g_free(conf);
    return;
}

/**
 * fb_image_icon_theme_changed - GtkIconTheme::changed handler; rebuilds pixbufs.
 * @icon_theme: The global GtkIconTheme that changed (unused directly; global
 *              icon_theme is used via fb_pixbuf_new).
 * @image:      GtkWidget* (GtkImage) whose pixbufs need refreshing.
 *
 * Unrefs all three pixbuf slots (setting them to NULL), then rebuilds:
 *   pix[0] — fresh load from icon name / file path with use_fallback=TRUE
 *   pix[1] — highlight version via fb_pixbuf_make_back_image
 *   pix[2] — press version via fb_pixbuf_make_press_image
 *
 * pix[1] and pix[2] are only rebuilt when conf->hicolor is non-zero (i.e.
 * the image is used as a button with hover highlighting).  Plain images
 * (created via fb_image_new()) have hicolor==0 and skip the rebuild.
 *
 * Sets the image to display pix[0] (normal state) after rebuilding.
 */
static void
fb_image_icon_theme_changed(GtkIconTheme *icon_theme, GtkWidget *image)
{
    fb_image_conf_t *conf;
    int i;

    conf = g_object_get_data(G_OBJECT(image), "conf");
    DBG("%s / %s\n", conf->iname, conf->fname);
    for (i = 0; i < PIXBBUF_NUM; i++)
        if (conf->pix[i]) {
            g_object_unref(G_OBJECT(conf->pix[i]));
	    conf->pix[i] = NULL;
	}
    conf->pix[0] = fb_pixbuf_new(conf->iname, conf->fname,
            conf->width, conf->height, TRUE);
    if (conf->hicolor) {
        conf->pix[1] = fb_pixbuf_make_back_image(conf->pix[0], conf->hicolor);
        conf->pix[2] = fb_pixbuf_make_press_image(conf->pix[1]);
    }
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), conf->pix[0]);
    return;
}


/**********************************************************************
 * FB Button                                                          *
 **********************************************************************/

static gboolean fb_button_cross(GtkImage *widget, GdkEventCrossing *event);
static gboolean fb_button_pressed(GtkWidget *widget, GdkEventButton *event);

/**
 * fb_button_new - create a clickable icon button with hover and press animation.
 * @iname:   Icon name for the button image; may be NULL.
 * @fname:   File path for the button image; may be NULL.
 * @width:   Desired icon width in pixels.
 * @height:  Desired icon height in pixels.
 * @hicolor: Hover highlight colour as 0xRRGGBB; 0 = no highlight.
 *
 * Creates a GtkBgbox (has_window=TRUE; pseudo-transparent background) containing
 * an fb_image_new() child.  After creation, populates pix[1] and pix[2] on the
 * image's conf struct:
 *   pix[1] = fb_pixbuf_make_back_image(pix[0], hicolor)  — hover highlight
 *   pix[2] = fb_pixbuf_make_press_image(pix[1])          — press shrink
 *
 * Event connections on the GtkBgbox (using g_signal_connect_swapped so the
 * GtkImage pointer is passed to the callbacks):
 *   enter-notify-event  → fb_button_cross (displays pix[1])
 *   leave-notify-event  → fb_button_cross (displays pix[0])
 *   button-press-event  → fb_button_pressed (displays pix[2])
 *   button-release-event→ fb_button_pressed (displays pix[1] or pix[0])
 *
 * gtk_widget_show_all() is called before return.
 *
 * Returns: (transfer full) GtkWidget* (GtkBgbox containing a GtkImage).
 */
GtkWidget *
fb_button_new(gchar *iname, gchar *fname, int width, int height, gulong hicolor)
{
    GtkWidget *b, *image;
    fb_image_conf_t *conf;

    b = gtk_bgbox_new();
    gtk_container_set_border_width(GTK_CONTAINER(b), 0);
    gtk_widget_set_can_focus(b, FALSE);
    image = fb_image_new(iname, fname, width, height);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    conf = g_object_get_data(G_OBJECT(image), "conf");
    conf->hicolor = hicolor;
    conf->pix[1] = fb_pixbuf_make_back_image(conf->pix[0], conf->hicolor);
    conf->pix[2] = fb_pixbuf_make_press_image(conf->pix[1]);
    gtk_widget_add_events(b, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect_swapped (G_OBJECT (b), "enter-notify-event",
            G_CALLBACK (fb_button_cross), image);
    g_signal_connect_swapped (G_OBJECT (b), "leave-notify-event",
            G_CALLBACK (fb_button_cross), image);
    g_signal_connect_swapped (G_OBJECT (b), "button-release-event",
          G_CALLBACK (fb_button_pressed), image);
    g_signal_connect_swapped (G_OBJECT (b), "button-press-event",
          G_CALLBACK (fb_button_pressed), image);
    gtk_container_add(GTK_CONTAINER(b), image);
    gtk_widget_show_all(b);
    return b;
}


/**
 * fb_button_cross - enter/leave-notify handler; swaps the displayed pixbuf.
 * @widget: GtkImage* (swapped from GtkBgbox via g_signal_connect_swapped).
 * @event:  GDK crossing event from the GtkBgbox.
 *
 * On GDK_ENTER_NOTIFY: switches to pix[1] (highlight).
 * On GDK_LEAVE_NOTIFY: switches to pix[0] (normal).
 *
 * Uses conf->i to avoid redundant gtk_image_set_from_pixbuf() calls when the
 * image is already in the target state.
 *
 * Returns: TRUE — event is consumed and does not propagate further.
 */
static gboolean
fb_button_cross(GtkImage *widget, GdkEventCrossing *event)
{
    fb_image_conf_t *conf;
    int i;

    conf = g_object_get_data(G_OBJECT(widget), "conf");
    if (event->type == GDK_LEAVE_NOTIFY) {
        i = 0;
    } else {
        i = 1;
    }
    if (conf->i != i) {
        conf->i = i;
        gtk_image_set_from_pixbuf(GTK_IMAGE(widget), conf->pix[i]);
    }
    DBG("%s/%s - %s - pix[%d]=%p\n", conf->iname, conf->fname,
	(event->type == GDK_LEAVE_NOTIFY) ? "out" : "in",
	conf->i, conf->pix[conf->i]);
    return TRUE;
}

/**
 * fb_button_pressed - button-press/release handler; shows press or hover pixbuf.
 * @widget: GtkImage* (swapped from GtkBgbox via g_signal_connect_swapped).
 * @event:  GDK button event from the GtkBgbox.
 *
 * On GDK_BUTTON_PRESS: switches to pix[2] (press / shrunk icon).
 * On GDK_BUTTON_RELEASE: if the pointer is still within the widget bounds,
 *   switches to pix[1] (highlight); otherwise switches to pix[0] (normal).
 *   Bounds check uses the GtkImage widget allocation, not the GtkBgbox, because
 *   the event coordinates are relative to the GtkBgbox window.
 *
 * Uses conf->i to suppress redundant gtk_image_set_from_pixbuf() calls.
 *
 * Returns: FALSE — event propagates to the plugin's own button-press handler.
 */
static gboolean
fb_button_pressed(GtkWidget *widget, GdkEventButton *event)
{
    fb_image_conf_t *conf;
    int i;

    conf = g_object_get_data(G_OBJECT(widget), "conf");
    if (event->type == GDK_BUTTON_PRESS) {
        i = 2;
    } else {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        if ((event->x >=0 && event->x < alloc.width)
                && (event->y >=0 && event->y < alloc.height))
            i = 1;
        else
            i = 0;
    }
    if (conf->i != i) {
        conf->i = i;
        gtk_image_set_from_pixbuf(GTK_IMAGE(widget), conf->pix[i]);
    }
    return FALSE;
}


/**********************************************************************
 * FB Calendar                                                        *
 **********************************************************************/

/**
 * fb_create_calendar - create a floating calendar popup window.
 *
 * Creates a borderless, non-resizable GTK_WINDOW_TOPLEVEL with a GtkCalendar
 * child.  Window properties:
 *   - No decorations (gtk_window_set_decorated FALSE)
 *   - 180×180 default size; not resizable
 *   - 5 px border width around the calendar
 *   - Excluded from taskbar and pager hint lists
 *   - Positioned at the mouse cursor (GTK_WIN_POS_MOUSE)
 *   - Title "calendar" (used by some WMs for identification)
 *   - Sticky (gtk_window_stick — visible on all virtual desktops)
 *
 * Calendar display options: week numbers, day names, and heading are shown.
 *
 * The caller is responsible for:
 *   - Calling gtk_widget_show_all() to make the window visible
 *   - Calling gtk_widget_destroy() when the calendar is dismissed
 *
 * Returns: (transfer full) GtkWidget* (GtkWindow containing a GtkCalendar).
 */
GtkWidget *
fb_create_calendar(void)
{
    GtkWidget *calendar, *win;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 5);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    gtk_window_set_title(GTK_WINDOW(win), "calendar");
    gtk_window_stick(GTK_WINDOW(win));

    calendar = gtk_calendar_new();
    gtk_calendar_set_display_options(
        GTK_CALENDAR(calendar),
        GTK_CALENDAR_SHOW_WEEK_NUMBERS | GTK_CALENDAR_SHOW_DAY_NAMES
        | GTK_CALENDAR_SHOW_HEADING);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(calendar));

    return win;
}
