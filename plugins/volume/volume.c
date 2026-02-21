/**
 * @file volume.c
 * @brief OSS/ALSA mixer volume control plugin for fbpanel.
 *
 * Displays a meter-class icon showing the current volume level and provides
 * interactive volume control:
 *   LMB: toggles a floating GtkScale slider window (created on demand near
 *        the mouse cursor; auto-dismissed 1.2 seconds after the pointer leaves).
 *   MMB: toggles mute (saves vol in muted_vol, sets vol to 0; restores on unmute).
 *   Scroll: adjusts volume by 2 steps up or down.
 *
 * Reads and writes the OSS mixer via /dev/mixer ioctl() calls (MIXER_READ /
 * MIXER_WRITE on SOUND_MIXER_VOLUME channel).  Works with ALSA when the
 * OSS compatibility layer is available.  Returns 0 from the constructor if
 * /dev/mixer cannot be opened (plugin gracefully disabled).
 *
 * Config keys: none.  All state is runtime-only.
 *
 * OSS volume plugin. Will works with ALSA since it usually
 * emulates OSS layer.
 */

#include "misc.h"
#include "../meter/meter.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if defined __linux__
#include <linux/soundcard.h>
#endif

//#define DEBUGPRN
#include "dbg.h"

static gchar *names[] = {
    "stock_volume-min",
    "stock_volume-med",
    "stock_volume-max",
    NULL
};

static gchar *s_names[] = {
    "stock_volume-mute",
    NULL
};

typedef struct {
    meter_priv meter;      /**< Embedded meter_priv; must be first member. */
    int fd;                /**< File descriptor for /dev/mixer. */
    int chan;              /**< OSS mixer channel (SOUND_MIXER_VOLUME). */
    guchar vol;            /**< Last known volume [0..100]. */
    guchar muted_vol;      /**< Volume saved when muted; restored on unmute. */
    int update_id;         /**< GLib timeout source ID for periodic vol polling. */
    int leave_id;          /**< GLib timeout source ID for slider auto-dismiss. */
    int has_pointer;       /**< Pointer enter/leave counter for slider window. */
    gboolean muted;        /**< TRUE while muted. */
    GtkWidget *slider_window; /**< Floating volume slider window; NULL when hidden. */
    GtkWidget *slider;        /**< GtkScale inside slider_window. */
} volume_priv;

static meter_class *k;

static void slider_changed(GtkRange *range, volume_priv *c);
static gboolean crossed(GtkWidget *widget, GdkEventCrossing *event,
    volume_priv *c);

/**
 * oss_get_volume - read the current OSS mixer volume.
 * @c: volume_priv with c->fd and c->chan set. (transfer none)
 *
 * Returns: volume in [0..100] (low byte of MIXER_READ result), or 0 on error.
 */
static int
oss_get_volume(volume_priv *c)
{
    int volume;

    if (ioctl(c->fd, MIXER_READ(c->chan), &volume)) {
        ERR("volume: can't get volume from /dev/mixer\n");
        return 0;
    }
    volume &= 0xFF;
    DBG("volume=%d\n", volume);
    return volume;
}

/**
 * oss_set_volume - write a new volume level to the OSS mixer.
 * @c:      volume_priv. (transfer none)
 * @volume: new volume in [0..100].
 *
 * Duplicates the value into both the left and right channel bytes
 * (value | (value << 8)) as expected by MIXER_WRITE.
 */
static void
oss_set_volume(volume_priv *c, int volume)
{
    DBG("volume=%d\n", volume);
    volume = (volume << 8) | volume;
    ioctl(c->fd, MIXER_WRITE(c->chan), &volume);
    return;
}

/**
 * volume_update_gui - refresh the meter icon and slider position from the mixer.
 * @c: volume_priv. (transfer none)
 *
 * Reads current volume via oss_get_volume().  If the muted/unmuted transition
 * changed, swaps the icon set (names vs s_names).  Updates the meter level,
 * updates the tooltip (when the slider is hidden), or synchronises the slider
 * position (when shown) without triggering the slider_changed callback.
 *
 * Called from the 1-second GLib timeout and from slider_changed/icon_clicked.
 *
 * Returns: TRUE to keep the timeout active.
 */
static gboolean
volume_update_gui(volume_priv *c)
{
    int volume;
    gchar buf[20];

    volume = oss_get_volume(c);
    if ((volume != 0) != (c->vol != 0)) {
        if (volume)
            k->set_icons(&c->meter, names);
        else
            k->set_icons(&c->meter, s_names);
        DBG("seting %s icons\n", volume ? "normal" : "muted");
    }
    c->vol = volume;
    k->set_level(&c->meter, volume);
    g_snprintf(buf, sizeof(buf), "<b>Volume:</b> %d%%", volume);
    if (!c->slider_window)
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    else {
        g_signal_handlers_block_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
        gtk_range_set_value(GTK_RANGE(c->slider), volume);
        g_signal_handlers_unblock_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
    }
    return TRUE;
}

/**
 * slider_changed - "value_changed" handler for the GtkScale slider.
 * @range: the GtkScale. (transfer none)
 * @c:     volume_priv. (transfer none)
 *
 * Writes the new slider value to the OSS mixer and updates the GUI.
 */
static void
slider_changed(GtkRange *range, volume_priv *c)
{
    int volume = (int) gtk_range_get_value(range);
    DBG("value=%d\n", volume);
    oss_set_volume(c, volume);
    volume_update_gui(c);
    return;
}

/**
 * volume_create_slider - create the floating slider window.
 * @c: volume_priv. (transfer none)
 *
 * Creates an undecorated, non-resizable GTK_WINDOW_TOPLEVEL positioned at
 * the current mouse cursor.  The GtkScale (vertical, inverted, range 0..100)
 * is wrapped in a GtkFrame inside the window.  "value_changed", "enter-notify",
 * and "leave-notify" signals on the slider are connected.
 *
 * Returns: (transfer full) the new GtkWindow.
 */
static GtkWidget *
volume_create_slider(volume_priv *c)
{
    GtkWidget *slider, *win;
    GtkWidget *frame;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 1);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    gtk_window_stick(GTK_WINDOW(win));

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_add(GTK_CONTAINER(win), frame);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 1);

    slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0.0, 100.0, 1.0);
    gtk_widget_set_size_request(slider, 25, 82);
    gtk_scale_set_draw_value(GTK_SCALE(slider), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(slider), GTK_POS_BOTTOM);
    gtk_scale_set_digits(GTK_SCALE(slider), 0);
    gtk_range_set_inverted(GTK_RANGE(slider), TRUE);
    gtk_range_set_value(GTK_RANGE(slider), ((meter_priv *) c)->level);
    DBG("meter->level %f\n", ((meter_priv *) c)->level);
    g_signal_connect(G_OBJECT(slider), "value_changed",
        G_CALLBACK(slider_changed), c);
    g_signal_connect(G_OBJECT(slider), "enter-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    g_signal_connect(G_OBJECT(slider), "leave-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    gtk_container_add(GTK_CONTAINER(frame), slider);

    c->slider = slider;
    return win;
}

/**
 * icon_clicked - "button_press_event" handler for the meter icon area.
 * @widget: the p->pwid GtkBgbox. (transfer none)
 * @event:  button event. (transfer none)
 * @c:      volume_priv. (transfer none)
 *
 * LMB (button 1): toggles the slider window.  Clears the tooltip when the
 *   slider is shown; restores the tooltip when hidden (via volume_update_gui).
 *   Also cancels the auto-dismiss timeout if one is pending.
 * MMB (button 2): toggles mute, saves/restores the volume level.
 *
 * Returns: FALSE (does not consume the event fully, allowing GTK to proceed).
 */
static gboolean
icon_clicked(GtkWidget *widget, GdkEventButton *event, volume_priv *c)
{
    int volume;

    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        if (c->slider_window == NULL) {
            c->slider_window = volume_create_slider(c);
            gtk_widget_show_all(c->slider_window);
            gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, NULL);
        } else {
            gtk_widget_destroy(c->slider_window);
            c->slider_window = NULL;
            if (c->leave_id) {
                g_source_remove(c->leave_id);
                c->leave_id = 0;
            }
        }
        return FALSE;
    }
    if (!(event->type == GDK_BUTTON_PRESS && event->button == 2))
        return FALSE;

    if (c->muted) {
        volume = c->muted_vol;
    } else {
        c->muted_vol = c->vol;
        volume = 0;
    }
    c->muted = !c->muted;
    oss_set_volume(c, volume);
    volume_update_gui(c);
    return FALSE;
}

/**
 * icon_scrolled - "scroll-event" handler: adjust volume by 2 steps.
 * @widget: the meter icon widget. (transfer none)
 * @event:  scroll event. (transfer none)
 * @c:      volume_priv. (transfer none)
 *
 * Scroll-up or scroll-left increases by 2; scroll-down or scroll-right
 * decreases by 2.  Clamped to [0..100].  If muted, adjusts muted_vol
 * without writing to the mixer.
 *
 * Returns: TRUE (event consumed).
 */
static gboolean
icon_scrolled(GtkWidget *widget, GdkEventScroll *event, volume_priv *c)
{
    int volume;

    volume = (c->muted) ? c->muted_vol : ((meter_priv *) c)->level;
    volume += 2 * ((event->direction == GDK_SCROLL_UP
            || event->direction == GDK_SCROLL_LEFT) ? 1 : -1);
    if (volume > 100)
        volume = 100;
    if (volume < 0)
        volume = 0;

    if (c->muted)
        c->muted_vol = volume;
    else {
        oss_set_volume(c, volume);
        volume_update_gui(c);
    }
    return TRUE;
}

/**
 * leave_cb - auto-dismiss timeout callback: destroy the slider window.
 * @c: volume_priv. (transfer none)
 *
 * Called 1.2 seconds after the pointer leaves both the icon and slider.
 * Destroys the slider window and resets leave_id and has_pointer.
 *
 * Returns: FALSE (one-shot timeout).
 */
static gboolean
leave_cb(volume_priv *c)
{
    c->leave_id = 0;
    c->has_pointer = 0;
    gtk_widget_destroy(c->slider_window);
    c->slider_window = NULL;
    return FALSE;
}

/**
 * crossed - "enter-notify-event" / "leave-notify-event" handler.
 * @widget: the widget generating the crossing event. (transfer none)
 * @event:  crossing event. (transfer none)
 * @c:      volume_priv. (transfer none)
 *
 * Maintains c->has_pointer as an enter/leave counter.  When positive,
 * cancels any pending auto-dismiss timeout.  When it reaches zero, installs
 * a 1.2-second auto-dismiss timeout if the slider window is open.
 *
 * Returns: FALSE.
 */
static gboolean
crossed(GtkWidget *widget, GdkEventCrossing *event, volume_priv *c)
{
    if (event->type == GDK_ENTER_NOTIFY)
        c->has_pointer++;
    else
        c->has_pointer--;
    if (c->has_pointer > 0) {
        if (c->leave_id) {
            g_source_remove(c->leave_id);
            c->leave_id = 0;
        }
    } else {
        if (!c->leave_id && c->slider_window) {
            c->leave_id = g_timeout_add(1200, (GSourceFunc) leave_cb, c);
        }
    }
    DBG("has_pointer=%d\n", c->has_pointer);
    return FALSE;
}

/**
 * volume_constructor - open /dev/mixer and set up the volume plugin.
 * @p: plugin_instance. (transfer none)
 *
 * Obtains meter_class via class_get("meter") and calls its constructor.
 * Opens /dev/mixer O_RDWR; returns 0 if unavailable (plugin disabled).
 * Sets the icon set to names[], installs a 1-second polling timeout,
 * and connects scroll, button_press, and enter/leave-notify signals on pwid.
 *
 * Returns: 1 on success, 0 if meter class unavailable or /dev/mixer fails.
 */
static int
volume_constructor(plugin_instance *p)
{
    volume_priv *c;

    if (!(k = class_get("meter")))
        return 0;
    if (!PLUGIN_CLASS(k)->constructor(p))
        return 0;
    c = (volume_priv *) p;
    if ((c->fd = open ("/dev/mixer", O_RDWR, 0)) < 0) {
        g_message("volume: /dev/mixer not available — plugin disabled");
        return 0;
    }
    k->set_icons(&c->meter, names);
    c->update_id = g_timeout_add(1000, (GSourceFunc) volume_update_gui, c);
    c->vol = 200;
    c->chan = SOUND_MIXER_VOLUME;
    volume_update_gui(c);
    g_signal_connect(G_OBJECT(p->pwid), "scroll-event",
        G_CALLBACK(icon_scrolled), (gpointer) c);
    g_signal_connect(G_OBJECT(p->pwid), "button_press_event",
        G_CALLBACK(icon_clicked), (gpointer)c);
    g_signal_connect(G_OBJECT(p->pwid), "enter-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    g_signal_connect(G_OBJECT(p->pwid), "leave-notify-event",
        G_CALLBACK(crossed), (gpointer)c);

    return 1;
}

/**
 * volume_destructor - stop polling, destroy slider, release meter_class.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the 1-second update timeout.  Destroys the slider window if open
 * (note: leave_id is not cancelled here — harmless since the window
 * destruction prevents the leave_cb from accessing freed memory for 1.2s,
 * but leave_id should ideally be removed too).  Calls meter_class destructor
 * and releases the class reference.
 */
static void
volume_destructor(plugin_instance *p)
{
    volume_priv *c = (volume_priv *) p;

    g_source_remove(c->update_id);
    if (c->slider_window)
        gtk_widget_destroy(c->slider_window);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("meter");
    return;
}



static plugin_class class = {
    .count       = 0,
    .type        = "volume",
    .name        = "Volume",
    .version     = "2.0",
    .description = "OSS volume control",
    .priv_size   = sizeof(volume_priv),
    .constructor = volume_constructor,
    .destructor  = volume_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
