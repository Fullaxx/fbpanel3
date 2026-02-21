/**
 * @file widgets.h
 * @brief fbpanel widget factories — pixbuf, image, button, calendar.
 *
 * Provides four public factory functions used by plugins to create
 * icon-themed images, clickable icon buttons, and a floating calendar window.
 *
 * FACTORY OVERVIEW
 * ----------------
 *   fb_pixbuf_new       — load a GdkPixbuf from an icon name or file path,
 *                         with an optional "gtk-missing-image" fallback.
 *   fb_image_new        — create a GtkImage that tracks icon-theme changes and
 *                         automatically reloads when the theme changes.
 *   fb_button_new       — create a clickable GtkBgbox wrapping an fb_image;
 *                         adds hover-highlight and press-shrink animation.
 *   fb_create_calendar  — create a floating, decorated-less GtkWindow
 *                         containing a GtkCalendar.
 *
 * OWNERSHIP
 * ---------
 * All four functions return (transfer full) — the caller is responsible for
 * the returned object.  For GtkWidget* returns the caller typically passes the
 * widget to a container (e.g. gtk_container_add), which then owns it.
 *
 * PIXBUF TRIPLE (fb_image_conf_t::pix[])
 * ----------------------------------------
 * Internally, fb_image keeps three pixbufs:
 *   pix[0] — normal (base icon)
 *   pix[1] — highlight (base + hicolor additive blend); used on mouse enter
 *   pix[2] — press (pix[1] scaled down by PRESS_GAP and centred); used on
 *             button-press
 *
 * pix[1] and pix[2] are populated by fb_button_new after creation.  For plain
 * fb_image_new images they remain NULL until the first icon-theme-changed event
 * (see BUG-008 in docs/BUGS_AND_ISSUES.md).
 *
 * See also: docs/ARCHITECTURE.md §5 (background rendering), docs/MEMORY_MODEL.md §6.
 */

#ifndef WIDGETS_H
#define WIDGETS_H

#include <gtk/gtk.h>

/**
 * fb_pixbuf_new - load a GdkPixbuf from an icon name and/or a file path.
 * @iname:        Icon name for gtk_icon_theme_load_icon(); may be NULL.
 * @fname:        File path for gdk_pixbuf_new_from_file_at_size(); may be NULL.
 * @width:        Desired width in pixels; also used as size hint for icon lookup.
 * @height:       Desired height in pixels.
 * @use_fallback: If TRUE and both @iname and @fname fail, load "gtk-missing-image".
 *
 * Tries sources in order: icon name → file path → fallback icon.
 * The resulting pixbuf is always <= MAX_SIZE (192) pixels in its larger dimension;
 * GTK_ICON_LOOKUP_FORCE_SIZE is passed so the icon theme scales to size exactly.
 *
 * Returns: (transfer full) GdkPixbuf*, or NULL if all sources failed and
 *          @use_fallback is FALSE (or the fallback itself is unavailable).
 *          Caller must g_object_unref() when done.
 */
GdkPixbuf *fb_pixbuf_new(gchar *iname, gchar *fname, int width, int height,
        gboolean use_fallback);

/**
 * fb_image_new - create a self-updating GtkImage widget.
 * @iname:  Icon name; passed to fb_pixbuf_new(); may be NULL.
 * @fname:  File path; passed to fb_pixbuf_new(); may be NULL.
 * @width:  Desired image width in pixels.
 * @height: Desired image height in pixels.
 *
 * Creates a GtkImage pre-loaded with fb_pixbuf_new(@iname, @fname, ..., TRUE).
 * Attaches an fb_image_conf_t to the widget via g_object_set_data("conf") that:
 *   - holds the icon name/file copies and three pixbuf slots (pix[0..2])
 *   - connects to GtkIconTheme::changed so pix[0] is refreshed automatically
 *   - frees all resources in the GtkWidget::destroy callback
 *
 * The caller must not free @iname or @fname after this call — they are
 * g_strdup()'d internally.
 *
 * Returns: (transfer full) GtkWidget* (GtkImage); gtk_widget_show() is called
 *          before return.  Pass to gtk_container_add() to transfer ownership.
 */
GtkWidget *fb_image_new(gchar *iname, gchar *fname, int width, int height);

/**
 * fb_button_new - create a clickable icon button with hover and press animation.
 * @iname:   Icon name for the button image; may be NULL.
 * @fname:   File path for the button image; may be NULL.
 * @width:   Desired icon width in pixels.
 * @height:  Desired icon height in pixels.
 * @hicolor: Hover highlight colour as 0xRRGGBB; 0 disables highlighting.
 *
 * Constructs a GtkBgbox (so it may have a pseudo-transparent background) that
 * contains an fb_image_new(@iname, @fname, @width, @height) child.  Three visual
 * states are maintained:
 *   pix[0] — normal: displayed at rest
 *   pix[1] — highlight: pix[0] with each pixel R/G/B incremented by @hicolor
 *             components (clamped at 255); displayed on GDK_ENTER_NOTIFY
 *   pix[2] — press: pix[1] scaled to (width - 2*PRESS_GAP) × (height - 2*PRESS_GAP)
 *             and centred; displayed on GDK_BUTTON_PRESS
 *
 * Event signals (enter-notify, leave-notify, button-press, button-release) are
 * connected on the GtkBgbox (not the GtkImage) and swap the displayed pixbuf.
 * button-press/release return FALSE so the event propagates to the plugin's
 * own handler; enter/leave return TRUE (consumed).
 *
 * gtk_widget_show_all() is called before return.
 *
 * Returns: (transfer full) GtkWidget* (GtkBgbox); caller passes to a container.
 */
GtkWidget *fb_button_new(gchar *iname, gchar *fname, int width, int height,
        gulong hicolor);

/**
 * fb_create_calendar - create a floating calendar window.
 *
 * Creates a borderless, non-resizable GtkWindow (GTK_WINDOW_TOPLEVEL) containing
 * a GtkCalendar with week numbers, day names, and heading displayed.  The window:
 *   - has no window manager decorations (gtk_window_set_decorated FALSE)
 *   - is excluded from taskbar and pager hint lists
 *   - is positioned at the mouse cursor on map
 *   - is sticky (visible on all virtual desktops)
 *
 * The caller is responsible for showing (gtk_widget_show_all) and eventually
 * destroying (gtk_widget_destroy) the returned window.  Typically plugins toggle
 * visibility in their click handler and hide/destroy on focus-out.
 *
 * Returns: (transfer full) GtkWidget* (GtkWindow containing GtkCalendar).
 *          gtk_widget_show_all() is NOT called — caller must show it.
 */
GtkWidget *fb_create_calendar(void);

#endif /* WIDGETS_H */
