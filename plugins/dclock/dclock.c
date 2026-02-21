/* dclock is an adaptation of blueclock by Jochen Baier <email@Jochen-Baier.de> */

/**
 * @file dclock.c
 * @brief Graphical (pixbuf) digital clock plugin for fbpanel.
 *
 * Renders the current time by compositing digit and colon glyphs from the
 * image file dclock_glyphs.png into a GdkPixbuf backing buffer (dc->clock),
 * which is displayed via a GtkImage.  Supports horizontal and vertical
 * orientations; in vertical mode the colon is rendered rotated.
 *
 * Config keys:
 *   TooltipFmt  (str, default "%A %x") — strftime format for the tooltip.
 *   ShowSeconds (bool, default false)  — include HH:MM:SS instead of HH:MM.
 *   HoursView   (enum: 12/24, default 24) — 12-hour or 24-hour clock.
 *   Action      (str, optional)        — shell command to run on click
 *                                        (overrides calendar popup).
 *   Color       (str, optional)        — CSS/RGB colour to tint the glyphs.
 *   ClockFmt    (str, deprecated)      — overrides ShowSeconds/HoursView;
 *               a warning is emitted and the key is removed from xconf.
 *
 * Note: all XCG str pointers are transfer-none (xconf-owned); Color is
 * obtained via a separate local variable and not stored.
 *
 * Main widgets:
 *   dc->glyphs  (GdkPixbuf of all glyphs; owned by dclock_priv)
 *   dc->clock   (GdkPixbuf backing buffer for the rendered time; owned here)
 *   dc->main    (GtkImage displaying dc->clock; added to p->pwid)
 *   dc->calendar_window (GtkWindow pop-up; NULL when not shown)
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


#define TOOLTIP_FMT    "%A %x"
#define CLOCK_24H_FMT  "%R"
#define CLOCK_12H_FMT  "%I:%M"

#define CLOCK_24H_FMT     "%R"
#define CLOCK_24H_SEC_FMT "%T"
#define CLOCK_12H_FMT     "%I:%M"
#define CLOCK_12H_SEC_FMT "%I:%M:%S"

#define COLON_WIDTH   7
#define COLON_HEIGHT  5
#define VCOLON_WIDTH   10
#define VCOLON_HEIGHT  6
#define DIGIT_WIDTH   11
#define DIGIT_HEIGHT  15
#define SHADOW 2

#define STR_SIZE  64

enum { DC_24H, DC_12H };

xconf_enum hours_view_enum[] = {
    { .num = DC_24H, .str = "24" },
    { .num = DC_12H, .str = "12" },
    { .num = 0, .str = NULL },
};

typedef struct
{
    plugin_instance plugin;
    GtkWidget *main;             /**< GtkImage displaying dc->clock pixbuf. */
    GtkWidget *calendar_window;  /**< Pop-up calendar; NULL when hidden. */
    gchar *tfmt, tstr[STR_SIZE]; /**< Tooltip format (xconf-owned) and last rendered value. */
    gchar *cfmt, cstr[STR_SIZE]; /**< Clock format (static string) and last rendered value. */
    char *action;    /**< Optional click command (transfer-none, xconf-owned). */
    int timer;       /**< GLib timeout source ID. */
    GdkPixbuf *glyphs; /**< Source glyph sheet: vertical row of '0'-'9' and ':' (20px wide each). */
    GdkPixbuf *clock;  /**< Backing pixbuf for the rendered clock display; owned. */
    guint32 color;     /**< Glyph tint colour in 0xRRGGBB format (default 0xff000000). */
    gboolean show_seconds;  /**< Include seconds in the display. */
    gboolean hours_view;    /**< DC_24H or DC_12H. */
    GtkOrientation orientation; /**< Panel orientation. */
} dclock_priv;


/**
 * clicked - "button_press_event" handler for the clock widget.
 * @widget: the GtkBgbox p->pwid. (transfer none)
 * @event:  button event. (transfer none)
 * @dc:     dclock_priv. (transfer none)
 *
 * If Ctrl+RMB, passes through (returns FALSE) to allow panel right-click menu.
 * If dc->action is set, runs it with g_spawn_command_line_async().
 * Otherwise, toggles the pop-up GtkCalendar window.
 *
 * Returns: TRUE to consume the event.
 */
static gboolean
clicked(GtkWidget *widget, GdkEventButton *event, dclock_priv *dc)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
            && event->state & GDK_CONTROL_MASK)
    {
        return FALSE;
    }
    if (dc->action != NULL)
        g_spawn_command_line_async(dc->action, NULL);
    else
    {
        if (dc->calendar_window == NULL)
        {
            dc->calendar_window = fb_create_calendar();
            gtk_widget_show_all(dc->calendar_window);
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, NULL);
        }
        else
        {
            gtk_widget_destroy(dc->calendar_window);
            dc->calendar_window = NULL;
        }
    }
    return TRUE;
}

/**
 * clock_update - redraw the clock pixbuf for the current time.
 * @dc: dclock_priv. (transfer none)
 *
 * Formats the current local time using dc->cfmt and, if it differs from
 * dc->cstr (the last rendered string), iterates the characters to composite
 * digit and colon glyphs from dc->glyphs into dc->clock via
 * gdk_pixbuf_copy_area().  In vertical mode the colon is rotated 270 degrees.
 * Also updates the tooltip from dc->tfmt once per day.
 *
 * Called from the 1-second GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static gint
clock_update(dclock_priv *dc)
{
    char output[STR_SIZE], *tmp, *utf8;
    time_t now;
    struct tm * detail;
    int i, x, y;

    time(&now);
    detail = localtime(&now);

    if (!strftime(output, sizeof(output), dc->cfmt, detail))
        strcpy(output, "  :  ");
    if (strcmp(dc->cstr, output))
    {
        strncpy(dc->cstr, output, sizeof(dc->cstr));
        x = y = SHADOW;
        for (tmp = output; *tmp; tmp++)
        {
            DBGE("%c", *tmp);
            if (isdigit(*tmp))
            {
                i = *tmp - '0';
                gdk_pixbuf_copy_area(dc->glyphs, i * 20, 0,
                    DIGIT_WIDTH, DIGIT_HEIGHT,
                    dc->clock, x, y);
                x += DIGIT_WIDTH;
            }
            else if (*tmp == ':')
            {
                if (dc->orientation == GTK_ORIENTATION_HORIZONTAL) {
                    gdk_pixbuf_copy_area(dc->glyphs, 10 * 20, 0,
                        COLON_WIDTH, DIGIT_HEIGHT - 2,
                        dc->clock, x, y + 2);
                    x += COLON_WIDTH;
                } else {
                    x = SHADOW;
                    y += DIGIT_HEIGHT;
                    gdk_pixbuf_copy_area(dc->glyphs, 10 * 20, 0,
                        VCOLON_WIDTH, VCOLON_HEIGHT,
                        dc->clock, x + DIGIT_WIDTH / 2, y);
                    y += VCOLON_HEIGHT;
                }
            }
            else
            {
                ERR("dclock: got %c while expecting for digit or ':'\n", *tmp);
            }
        }
        DBG("\n");
        gtk_widget_queue_draw(dc->main);
    }

    if (dc->calendar_window || !strftime(output, sizeof(output),
            dc->tfmt, detail))
        output[0] = 0;
    if (strcmp(dc->tstr, output))
    {
        strcpy(dc->tstr, output);
        if (dc->tstr[0] && (utf8 = g_locale_to_utf8(output, -1,
                    NULL, NULL, NULL)))
        {
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, utf8);
            g_free(utf8);
        }
        else
            gtk_widget_set_tooltip_markup(dc->plugin.pwid, NULL);
    }
    return TRUE;
}

/**
 * dclock_set_color - recolour all non-transparent, non-black pixels in glyphs.
 * @glyphs: the glyph GdkPixbuf to modify in place. (transfer none)
 * @color:  target colour in 0xAARRGGBB format (alpha byte ignored).
 *
 * Iterates every RGBA pixel in the glyph sheet and replaces the RGB
 * components with those from @color, leaving fully transparent or fully
 * black pixels unchanged.
 */
static void
dclock_set_color(GdkPixbuf *glyphs, guint32 color)
{
    guchar *p1, *p2;
    int w, h;
    guint r, g, b;

    p1 = gdk_pixbuf_get_pixels(glyphs);
    h = gdk_pixbuf_get_height(glyphs);
    r = (color & 0x00ff0000) >> 16;
    g = (color & 0x0000ff00) >> 8;
    b = (color & 0x000000ff);
    DBG("%dx%d: %02x %02x %02x\n",
        gdk_pixbuf_get_width(glyphs), gdk_pixbuf_get_height(glyphs), r, g, b);
    while (h--)
    {
        for (p2 = p1, w = gdk_pixbuf_get_width(glyphs); w; w--, p2 += 4)
        {
            DBG("here %02x %02x %02x %02x\n", p2[0], p2[1], p2[2], p2[3]);
            if (p2[3] == 0 || !(p2[0] || p2[1] || p2[2]))
                continue;
            p2[0] = r;
            p2[1] = g;
            p2[2] = b;
        }
        p1 += gdk_pixbuf_get_rowstride(glyphs);
    }
    DBG("here\n");
    return;
}

/**
 * dclock_create_pixbufs - allocate dc->clock backing pixbuf for the current config.
 * @dc: dclock_priv with orientation, show_seconds, and panel->aw set. (transfer none)
 *
 * Computes the required pixel dimensions for the clock display (horizontal or
 * vertical layout, with or without seconds).  For vertical panels whose width
 * is too narrow for a horizontal layout, switches to the vertical colon format.
 * Creates dc->clock as a transparent RGBA GdkPixbuf of the computed size.
 */
static void
dclock_create_pixbufs(dclock_priv *dc)
{
    int width, height;
    GdkPixbuf *ch, *cv;

    width = height = SHADOW;
    width += COLON_WIDTH + 4 * DIGIT_WIDTH;
    height += DIGIT_HEIGHT;
    if (dc->show_seconds)
        width += COLON_WIDTH + 2 * DIGIT_WIDTH;
    if (dc->orientation == GTK_ORIENTATION_VERTICAL) {
        DBG("width=%d height=%d aw=%d\n", width, height, dc->plugin.panel->aw);
        if (width < dc->plugin.panel->aw) {
            dc->orientation = GTK_ORIENTATION_HORIZONTAL;
            goto done;
        }
        width = height = SHADOW;
        ch = gdk_pixbuf_new_subpixbuf(dc->glyphs, 200, 0, 8, 8);
        cv = gdk_pixbuf_rotate_simple(ch, 270);
        gdk_pixbuf_copy_area(cv, 0, 0, 8, 8, ch, 0, 0);
        g_object_unref(cv);
        g_object_unref(ch);
        height += DIGIT_HEIGHT * 2 + VCOLON_HEIGHT;
        width += DIGIT_WIDTH * 2;
        if (dc->show_seconds)
            height += VCOLON_HEIGHT + DIGIT_HEIGHT;
    }
done:
    dc->clock = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    DBG("width=%d height=%d\n", width, height);
    gdk_pixbuf_fill(dc->clock, 0);
    return;
}

/**
 * dclock_destructor - stop the timer and destroy the main GtkImage.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout and explicitly destroys dc->main.
 * dc->glyphs and dc->clock (GdkPixbuf) are not unreffed here — they leak.
 * dc->calendar_window, if open, will be closed by GTK's widget destruction
 * cascade from the parent window.
 */
static void
dclock_destructor(plugin_instance *p)
{
    dclock_priv *dc = (dclock_priv *)p;

    if (dc->timer)
        g_source_remove(dc->timer);
    gtk_widget_destroy(dc->main);
    return;
}

/**
 * dclock_constructor - load glyphs, create pixbufs, and start the clock.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Loads dclock_glyphs.png (from IMGPREFIX or SRCIMGPREFIX fallback).
 * Reads config keys (all XCG str/enum); the deprecated ClockFmt key is
 * removed from the xconf tree if present.  Allocates dc->clock via
 * dclock_create_pixbufs(), applies colour tinting if Color is set,
 * creates a GtkImage, connects "button_press_event", and installs a 1-second
 * GLib timeout.
 *
 * Returns: 1 on success, 0 if the glyph image cannot be loaded.
 */
static int
dclock_constructor(plugin_instance *p)
{
    gchar *color_str;
    dclock_priv *dc;

    DBG("dclock: use 'tclock' plugin for text version of a time and date\n");
    dc = (dclock_priv *) p;
    dc->glyphs = gdk_pixbuf_new_from_file(IMGPREFIX "/dclock_glyphs.png", NULL);
    if (!dc->glyphs)
        dc->glyphs = gdk_pixbuf_new_from_file(SRCIMGPREFIX "/dclock_glyphs.png", NULL);
    if (!dc->glyphs) {
        ERR("dclock: can't load " IMGPREFIX "/dclock_glyphs.png"
            " (use 'tclock' plugin for a text clock)\n");
        return 0;
    }

    dc->cfmt = NULL;
    dc->tfmt = TOOLTIP_FMT;
    dc->action = NULL;
    dc->color = 0xff000000;
    dc->show_seconds = FALSE;
    dc->hours_view = DC_24H;
    dc->orientation = p->panel->orientation;
    color_str = NULL;
    XCG(p->xc, "TooltipFmt", &dc->tfmt, str);
    XCG(p->xc, "ClockFmt", &dc->cfmt, str);
    XCG(p->xc, "ShowSeconds", &dc->show_seconds, enum, bool_enum);
    XCG(p->xc, "HoursView", &dc->hours_view, enum, hours_view_enum);
    XCG(p->xc, "Action", &dc->action, str);
    XCG(p->xc, "Color", &color_str, str);
    if (dc->cfmt)
    {
        ERR("dclock: ClockFmt option is deprecated. Please use\n"
            "following options instead\n"
            "  ShowSeconds = false | true\n"
            "  HoursView = 12 | 24\n");
        xconf_del(xconf_get(p->xc, "ClockFmt"), FALSE);
        dc->cfmt = NULL;
    }
    if (color_str)
    {
        GdkRGBA color;
        if (gdk_rgba_parse (&color, color_str))
            dc->color = gcolor2rgb24(&color);
    }
    if (dc->hours_view == DC_24H)
        dc->cfmt = (dc->show_seconds) ? CLOCK_24H_SEC_FMT : CLOCK_24H_FMT;
    else
        dc->cfmt = (dc->show_seconds) ? CLOCK_12H_SEC_FMT : CLOCK_12H_FMT;
    dclock_create_pixbufs(dc);
    if (dc->color != 0xff000000)
        dclock_set_color(dc->glyphs, dc->color);

    dc->main = gtk_image_new_from_pixbuf(dc->clock);
    gtk_widget_set_halign(dc->main, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(dc->main, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(dc->main, 1);
    gtk_widget_set_margin_end(dc->main, 1);
    gtk_widget_set_margin_top(dc->main, 1);
    gtk_widget_set_margin_bottom(dc->main, 1);
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    g_signal_connect (G_OBJECT (p->pwid), "button_press_event",
            G_CALLBACK (clicked), (gpointer) dc);
    gtk_widget_show_all(dc->main);
    dc->timer = g_timeout_add(1000, (GSourceFunc) clock_update, (gpointer)dc);
    clock_update(dc);

    return 1;
}


static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "dclock",
    .name        = "Digital Clock",
    .version     = "1.0",
    .description = "Digital clock with tooltip",
    .priv_size   = sizeof(dclock_priv),

    .constructor = dclock_constructor,
    .destructor  = dclock_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
