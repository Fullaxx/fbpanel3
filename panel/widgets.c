
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

#define MAX_SIZE 192

/* Creates a pixbuf. Several sources are tried in these order:
 *   icon named @iname
 *   file from @fname
 *   icon named "missing-image" as a fallabck, if @use_fallback is TRUE.
 * Returns pixbuf or NULL on failure
 *
 * Result pixbuf is always smaller then MAX_SIZE
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

/* Creates hilighted version of front image to reflect mouse enter
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

#define PRESS_GAP 2
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

#define PIXBBUF_NUM 3
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

/* Creates an image widget from fb_pixbuf and updates it on every icon theme
 * change. To keep its internal state, image allocates some data and frees it
 * in "destroy" callback
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


/* Frees image's resources
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

/* Reloads image's pixbuf upon icon theme change
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
    conf->pix[1] = fb_pixbuf_make_back_image(conf->pix[0], conf->hicolor);
    conf->pix[2] = fb_pixbuf_make_press_image(conf->pix[1]);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), conf->pix[0]);
    return;
}


/**********************************************************************
 * FB Button                                                          *
 **********************************************************************/

static gboolean fb_button_cross(GtkImage *widget, GdkEventCrossing *event);
static gboolean fb_button_pressed(GtkWidget *widget, GdkEventButton *event);

/* Creates fb_button - bgbox with fb_image. bgbox provides pseudo transparent
 * background and event capture. fb_image follows icon theme change.
 * Additionaly, fb_button highlightes an image on mouse enter and runs simple
 * animation when clicked.
 * FIXME: @label parameter is currently ignored
 */
GtkWidget *
fb_button_new(gchar *iname, gchar *fname, int width, int height,
      gulong hicolor, gchar *label)
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


/* Flips front and back images upon mouse cross event - GDK_ENTER_NOTIFY
 * or GDK_LEAVE_NOTIFY
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
