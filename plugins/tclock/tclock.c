/**
 * @file tclock.c
 * @brief Text clock plugin for fbpanel.
 *
 * Displays the current time as a GTK markup label (default format:
 * "<b>%R</b>", i.e. bold HH:MM).  A configurable tooltip shows the full
 * date.  Clicking toggles a pop-up GtkCalendar window (or runs a custom
 * action command).  Updates every second via a GLib timeout.
 *
 * Config keys (all transfer-none xconf strings):
 *   ClockFmt     (str, default "<b>%R</b>") — strftime format for the label.
 *   TooltipFmt   (str, default "%A %x")     — strftime format for the tooltip.
 *   Action       (str, optional)            — command to run on click
 *                                             (overrides ShowCalendar).
 *   ShowCalendar (bool, default true)       — toggle calendar window on click.
 *   ShowTooltip  (bool, default true)       — show/update the date tooltip.
 *
 * Main widgets:
 *   dc->main   (GtkEventBox, invisible window, receives button-press events)
 *   dc->clockw (GtkLabel inside main)
 *   dc->calendar_window (GtkWindow, created on demand, destroyed on second click)
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"


/* 2010-04 Jared Minch  < jmminch@sourceforge.net >
 *     Calendar and transparency support
 *     See patch "2981313: Enhancements to 'tclock' plugin" on sf.net
 */

#define TOOLTIP_FMT    "%A %x"
#define CLOCK_24H_FMT  "<b>%R</b>"
#define CLOCK_12H_FMT  "%I:%M"

typedef struct {
    plugin_instance plugin;
    GtkWidget *main;             /**< GtkEventBox (invisible) receiving click events. */
    GtkWidget *clockw;           /**< GtkLabel displaying formatted time markup. */
    GtkWidget *calendar_window;  /**< Pop-up GtkCalendar; NULL when not shown. */
    char *tfmt;    /**< Tooltip strftime format (transfer-none, xconf-owned). */
    char *cfmt;    /**< Clock label strftime format (transfer-none, xconf-owned). */
    char *action;  /**< Optional click command (transfer-none, xconf-owned). */
    short lastDay; /**< Last tm_mday seen; used to avoid redundant tooltip updates. */
    int timer;     /**< GLib timeout source ID (1-second interval). */
    int show_calendar; /**< Boolean: show calendar on click. */
    int show_tooltip;  /**< Boolean: update the tooltip markup each day. */
} tclock_priv;


/**
 * clock_update - update the clock label and optionally the tooltip.
 * @data: pointer to tclock_priv. (transfer none)
 *
 * Formats the current local time using cfmt, sets the GtkLabel markup,
 * then updates the tooltip using tfmt when the day changes (or clears it
 * when the calendar window is open).  The tooltip string is converted from
 * locale encoding to UTF-8 before setting it.
 *
 * Called from the GLib timeout (every 1 second) and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static gint
clock_update(gpointer data)
{
    char output[256];
    time_t now;
    struct tm * detail;
    tclock_priv *dc;
    gchar *utf8;
    size_t rc;

    g_assert(data != NULL);
    dc = (tclock_priv *)data;

    time(&now);
    detail = localtime(&now);
    rc = strftime(output, sizeof(output), dc->cfmt, detail) ;
    if (rc) {
        gtk_label_set_markup (GTK_LABEL(dc->clockw), output) ;
    }

    if (dc->show_tooltip) {
        if (dc->calendar_window) {
            gtk_widget_set_tooltip_markup(dc->main, NULL);
            dc->lastDay = 0;
        } else {
            if (detail->tm_mday != dc->lastDay) {
                dc->lastDay = detail->tm_mday;

                rc = strftime(output, sizeof(output), dc->tfmt, detail) ;
                if (rc &&
                    (utf8 = g_locale_to_utf8(output, -1, NULL, NULL, NULL))) {
                    gtk_widget_set_tooltip_markup(dc->main, utf8);
                    g_free(utf8);
                }
            }
        }
    }

    return TRUE;
}

/**
 * clicked - handle button-press on the event box.
 * @widget:  the GtkEventBox. (transfer none)
 * @event:   button press event. (transfer none)
 * @dc:      tclock_priv. (transfer none)
 *
 * If dc->action is set, runs it with g_spawn_command_line_async().
 * Otherwise, if ShowCalendar is enabled, toggles the pop-up calendar
 * window and updates the clock display.
 *
 * Returns: TRUE (event consumed).
 */
static gboolean
clicked(GtkWidget *widget, GdkEventButton *event, tclock_priv *dc)
{
    if (dc->action) {
        g_spawn_command_line_async(dc->action, NULL);
    } else if (dc->show_calendar) {
        if (dc->calendar_window == NULL)
        {
            dc->calendar_window = fb_create_calendar();
            gtk_widget_show_all(dc->calendar_window);
        }
        else
        {
            gtk_widget_destroy(dc->calendar_window);
            dc->calendar_window = NULL;
        }

        clock_update(dc);
    }
    return TRUE;
}

/**
 * tclock_constructor - configure and start the text clock plugin.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads config keys (all transfer-none raw xconf pointers stored directly in
 * tclock_priv without copying).  Creates a GtkEventBox with an invisible
 * window, creates a GtkLabel inside it, calls clock_update() once to show
 * the initial time, then installs a 1-second GLib timeout.
 *
 * Returns: 1 on success.
 */
static int
tclock_constructor(plugin_instance *p)
{
    tclock_priv *dc;

    dc = (tclock_priv *) p;
    dc->cfmt = CLOCK_24H_FMT;
    dc->tfmt = TOOLTIP_FMT;
    dc->action = NULL;
    dc->show_calendar = TRUE;
    dc->show_tooltip = TRUE;
    XCG(p->xc, "TooltipFmt", &dc->tfmt, str);
    XCG(p->xc, "ClockFmt", &dc->cfmt, str);
    XCG(p->xc, "Action", &dc->action, str);
    XCG(p->xc, "ShowCalendar", &dc->show_calendar, enum, bool_enum);
    XCG(p->xc, "ShowTooltip", &dc->show_tooltip, enum, bool_enum);

    dc->main = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(dc->main), FALSE);
    if (dc->action || dc->show_calendar)
        g_signal_connect (G_OBJECT (dc->main), "button_press_event",
              G_CALLBACK (clicked), (gpointer) dc);

    dc->clockw = gtk_label_new(NULL);

    clock_update(dc);

    gtk_label_set_xalign(GTK_LABEL(dc->clockw), 0.5);
    gtk_label_set_yalign(GTK_LABEL(dc->clockw), 0.5);
    gtk_widget_set_margin_start(dc->clockw, 4);
    gtk_widget_set_margin_end(dc->clockw, 4);
    gtk_label_set_justify(GTK_LABEL(dc->clockw), GTK_JUSTIFY_CENTER);
    gtk_container_add(GTK_CONTAINER(dc->main), dc->clockw);
    gtk_widget_show_all(dc->main);
    dc->timer = g_timeout_add(1000, (GSourceFunc) clock_update, (gpointer)dc);
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    return 1;
}

/**
 * tclock_destructor - stop the timer and destroy the main event box.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout.  Destroys dc->main (which also destroys clockw
 * as a child).  Any open calendar_window has already been destroyed by GTK
 * when the parent panel window is cleaned up; dc->calendar_window is not
 * explicitly destroyed here.  Config strings are transfer-none and must NOT
 * be freed.
 */
static void
tclock_destructor( plugin_instance *p )
{
    tclock_priv *dc = (tclock_priv *) p;

    if (dc->timer)
        g_source_remove(dc->timer);
    gtk_widget_destroy(dc->main);
    return;
}

static plugin_class class = {
    .count       = 0,
    .type        = "tclock",
    .name        = "Text Clock",
    .version     = "2.0",
    .description = "Text clock/date with tooltip",
    .priv_size   = sizeof(tclock_priv),

    .constructor = tclock_constructor,
    .destructor = tclock_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
