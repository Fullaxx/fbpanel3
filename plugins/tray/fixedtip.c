/**
 * @file fixedtip.c
 * @brief Fixed-position tooltip for systray balloon messages.
 *
 * Copyright (C) 2001 Havoc Pennington
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Displays a GTK_WINDOW_POPUP tooltip near a systray icon when the icon's
 * application sends a SYSTEM_TRAY_BEGIN_MESSAGE balloon.  Only one tooltip
 * is displayed at a time; a new fixed_tip_show() replaces any existing one.
 *
 * POSITIONING
 * -----------
 * The balloon is positioned relative to a "strut" (the panel edge) and the
 * icon's root-window coordinates:
 *
 *  - For horizontal panels (strut_is_vertical=FALSE):
 *      root_y = strut + PAD (panel is above, balloon below)
 *              OR strut - h - PAD (panel is below, balloon above)
 *      root_x -= w/2 (centre the balloon over the icon)
 *
 *  - For vertical panels (strut_is_vertical=TRUE):
 *      root_x = strut + PAD (panel is left, balloon to the right)
 *              OR strut - w - PAD (panel is right, balloon to the left)
 *      root_y -= h/2 (centre the balloon alongside the icon)
 *
 * The position is clamped so the balloon stays within the primary monitor.
 *
 * GLOBAL STATE
 * ------------
 * `tip` and `label` are module-level static pointers.  A "destroy" signal
 * on `tip` is connected to gtk_widget_destroyed(&tip), which sets tip=NULL
 * when the widget is destroyed.  This prevents dangling pointer use if the
 * window is destroyed externally (e.g., by a button press).
 *
 * THREAD SAFETY
 * -------------
 * Not thread-safe; called only from the GTK main loop.
 */

/* Metacity fixed tooltip routine */

/*
 * Copyright (C) 2001 Havoc Pennington
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include "fixedtip.h"

/** Module-level tooltip window; NULL when not displayed.
 *  Set to NULL by gtk_widget_destroyed callback on "destroy". */
static GtkWidget *tip = NULL;

/** Label widget inside @tip; NULL when tip is NULL. */
static GtkWidget *label = NULL;

/** Primary monitor width (right edge, pixels); cached at first show. */
static int screen_width = 0;

/** Primary monitor height (bottom edge, pixels); cached at first show. */
static int screen_height = 0;

/**
 * button_press_handler - "button_press_event" handler for the tooltip window.
 * @tip:   The tooltip GtkWindow. (transfer none)
 * @event: Button press event. (transfer none)
 * @data:  Unused.
 *
 * Hides (destroys) the tooltip when the user clicks on it.
 *
 * Returns FALSE (event not consumed; let GTK process further).
 */
static gboolean
button_press_handler (GtkWidget *tip,
                      GdkEvent  *event,
                      void      *data)
{
  fixed_tip_hide ();

  return FALSE;
}

/**
 * expose_handler - "draw" signal handler for the tooltip window.
 * @widget: The tooltip GtkWindow. (transfer none)
 * @cr:     Cairo context. (transfer none)
 * @data:   Unused.
 *
 * Renders the GTK theme background and frame around the tooltip window
 * using gtk_render_background() and gtk_render_frame().  This gives the
 * balloon the standard "gtk-tooltips" CSS styling.
 *
 * Returns FALSE (allow further drawing).
 */
static gboolean
expose_handler (GtkWidget *widget, cairo_t *cr, gpointer data)
{
    GtkStyleContext *context;
    GtkAllocation alloc;

    context = gtk_widget_get_style_context(widget);
    gtk_widget_get_allocation(widget, &alloc);
    gtk_render_background(context, cr, 0, 0, alloc.width, alloc.height);
    gtk_render_frame(context, cr, 0, 0, alloc.width, alloc.height);
    return FALSE;
}

/**
 * fixed_tip_show - display (or update) the fixed tooltip with balloon text.
 * @screen_number:    Screen number (currently ignored; single-monitor only).
 * @root_x:           Root-window X coordinate of the icon the balloon points to.
 * @root_y:           Root-window Y coordinate of the icon the balloon points to.
 * @strut_is_vertical: TRUE if the panel is vertical (strut is a vertical bar).
 * @strut:            Panel edge coordinate used to position the balloon.
 * @markup_text:      Pango markup text to display. (transfer none)
 *
 * Creates the tooltip window on the first call, lazily:
 *  - GtkWindow (GTK_WINDOW_POPUP), named "gtk-tooltips" for CSS.
 *  - Border width 4; non-resizable.
 *  - GtkLabel with line wrap and centred alignment.
 *  - "draw" signal connected to expose_handler for theme rendering.
 *  - "button_press_event" to dismiss on click.
 *  - "destroy" connected to gtk_widget_destroyed(&tip) to NULL-guard.
 *
 * On subsequent calls, reuses the existing window and updates the label.
 *
 * After computing position (see module docblock), clamps to the primary
 * monitor bounds and calls gtk_window_move() + gtk_widget_show().
 */
void
fixed_tip_show (int screen_number,
                int root_x, int root_y,
                gboolean strut_is_vertical,
                int strut,
                const char *markup_text)
{
  int w, h;

  if (tip == NULL)
    {
      tip = gtk_window_new (GTK_WINDOW_POPUP);
      {
        GdkDisplay *dpy = gdk_display_get_default();
        GdkMonitor *mon = gdk_display_get_primary_monitor(dpy);
        GdkRectangle geom;
        gdk_monitor_get_geometry(mon, &geom);
        screen_width  = geom.x + geom.width;
        screen_height = geom.y + geom.height;
        (void)screen_number;
      }

      gtk_widget_set_app_paintable (tip, TRUE);
      gtk_window_set_resizable(GTK_WINDOW (tip), FALSE);
      gtk_widget_set_name (tip, "gtk-tooltips");
      gtk_container_set_border_width (GTK_CONTAINER (tip), 4);

      g_signal_connect (G_OBJECT (tip),
            "draw",
            G_CALLBACK (expose_handler),
            NULL);

      gtk_widget_add_events (tip, GDK_BUTTON_PRESS_MASK);

      g_signal_connect (G_OBJECT (tip),
            "button_press_event",
            G_CALLBACK (button_press_handler),
            NULL);

      label = gtk_label_new (NULL);
      gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.5);
      gtk_label_set_yalign (GTK_LABEL (label), 0.5);
      gtk_widget_show (label);

      gtk_container_add (GTK_CONTAINER (tip), label);

      /* When the window is destroyed externally, set tip=NULL to prevent
       * dangling pointer use on the next fixed_tip_show() call. */
      g_signal_connect (G_OBJECT (tip),
            "destroy",
            G_CALLBACK (gtk_widget_destroyed),
            &tip);
    }

  gtk_label_set_markup (GTK_LABEL (label), markup_text);

  /* FIXME should also handle Xinerama here, just to be
   * really cool
   */
  gtk_window_get_size (GTK_WINDOW (tip), &w, &h);

  /* pad between panel and message window */
#define PAD 5

  if (strut_is_vertical)
    {
      if (strut > root_x)
        root_x = strut + PAD;
      else
        root_x = strut - w - PAD;

      root_y -= h / 2;
    }
  else
    {
      if (strut > root_y)
        root_y = strut + PAD;
      else
        root_y = strut - h - PAD;

      root_x -= w / 2;
    }

  /* Push onscreen */
  if ((root_x + w) > screen_width)
    root_x -= (root_x + w) - screen_width;

  if ((root_y + h) > screen_height)
    root_y -= (root_y + h) - screen_height;

  gtk_window_move (GTK_WINDOW (tip), root_x, root_y);

  gtk_widget_show (tip);
}

/**
 * fixed_tip_hide - destroy the fixed tooltip if it is visible.
 *
 * Calls gtk_widget_destroy(tip), which triggers the "destroy" signal
 * and sets tip=NULL via the gtk_widget_destroyed callback.
 * Safe to call when tip is already NULL.
 */
void
fixed_tip_hide (void)
{
  if (tip)
    {
      gtk_widget_destroy (tip);
      tip = NULL;
    }
}
