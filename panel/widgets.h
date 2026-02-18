#ifndef WIDGETS_H
#define WIDGETS_H

/*
 * widgets.h — fbpanel image/button/calendar widget helpers
 *
 * These three widget factories (fb_pixbuf_new, fb_image_new, fb_button_new,
 * fb_create_calendar) are the only public API defined in widgets.c.
 *
 * fb_pixbuf_new   — creates a GdkPixbuf from an icon name or file path.
 * fb_image_new    — creates a GtkImage that automatically reloads on icon
 *                   theme changes.
 * fb_button_new   — creates a clickable GtkBgbox wrapping an fb_image with
 *                   hover-highlight and press animation.
 * fb_create_calendar — creates a floating top-level calendar window.
 */

#include <gtk/gtk.h>

GdkPixbuf *fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback);

GtkWidget *fb_image_new(gchar *iname, gchar *fname, int width, int height);

GtkWidget *fb_button_new(gchar *iname, gchar *fname, int width, int height,
        gulong hicolor, gchar *name);

GtkWidget *fb_create_calendar(void);

#endif /* WIDGETS_H */
