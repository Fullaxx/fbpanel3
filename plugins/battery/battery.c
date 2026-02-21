/**
 * @file battery.c
 * @brief Battery charge indicator plugin for fbpanel.
 *
 * Displays battery charge level using the meter_class icon system.
 * Uses three icon sets:
 *   batt_working[]  — "battery_0" .. "battery_8"  (discharging)
 *   batt_charging[] — "battery_charging_0" .. "battery_charging_8"
 *   batt_na[]       — "battery_na" (no battery or AC-only)
 *
 * On Linux, the actual charge level is read from the ACPI sysfs interface
 * via the os_linux.c.inc include.  On other platforms, battery_update_os()
 * sets c->exist = FALSE and the "N/A" set is used.
 *
 * Config keys: none (icon sizes come from the meter base plugin).
 *
 * Delegates construction and destruction to meter_class (obtained via
 * class_get("meter")/class_put("meter")).  Installs a 2-second GLib timeout
 * calling battery_update().
 */

#include "misc.h"
#include "../meter/meter.h"

//#define DEBUGPRN
#include "dbg.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


static meter_class *k;

typedef struct {
    meter_priv meter;   /**< Embedded meter_priv; must be first member. */
    int timer;          /**< GLib timeout source ID. */
    gfloat level;       /**< Current charge level [0..100]. */
    gboolean charging;  /**< TRUE if the battery is currently charging. */
    gboolean exist;     /**< TRUE if a battery was detected. */
} battery_priv;

static gboolean battery_update_os(battery_priv *c);

static gchar *batt_working[] = {
    "battery_0",
    "battery_1",
    "battery_2",
    "battery_3",
    "battery_4",
    "battery_5",
    "battery_6",
    "battery_7",
    "battery_8",
    NULL
};

static gchar *batt_charging[] = {
    "battery_charging_0",
    "battery_charging_1",
    "battery_charging_2",
    "battery_charging_3",
    "battery_charging_4",
    "battery_charging_5",
    "battery_charging_6",
    "battery_charging_7",
    "battery_charging_8",
    NULL
};

static gchar *batt_na[] = {
    "battery_na",
    NULL
};

#if defined __linux__
#include "os_linux.c.inc"
#else

static void
battery_update_os(battery_priv *c)
{
    c->exist = FALSE;
}

#endif

/**
 * battery_update - poll battery state and update the meter icon.
 * @c: battery_priv. (transfer none)
 *
 * Calls battery_update_os() (platform-specific sysfs reader) to populate
 * c->level, c->charging, and c->exist.  Selects the appropriate icon set
 * and updates the tooltip markup.  Then delegates to meter_class->set_icons()
 * and set_level() to update the display.
 *
 * Called from the 2-second GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static gboolean
battery_update(battery_priv *c)
{
    gchar buf[50];
    gchar **i;

    battery_update_os(c);
    if (c->exist) {
        i = c->charging ? batt_charging : batt_working;
        g_snprintf(buf, sizeof(buf), "<b>Battery:</b> %d%%%s",
            (int) c->level, c->charging ? "\nCharging" : "");
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    } else {
        i = batt_na;
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid,
            "Runing on AC\nNo battery found");
    }
    k->set_icons(&c->meter, i);
    k->set_level(&c->meter, c->level);
    return TRUE;
}


/**
 * battery_constructor - initialise the battery plugin on top of meter_class.
 * @p: plugin_instance. (transfer none)
 *
 * Obtains the meter_class singleton via class_get("meter") and calls its
 * constructor to set up the GtkImage in p->pwid.  Installs a 2-second
 * GLib timeout for periodic updates and calls battery_update() once
 * immediately.
 *
 * Returns: 1 on success, 0 if meter class is unavailable.
 */
static int
battery_constructor(plugin_instance *p)
{
    battery_priv *c;

    if (!(k = class_get("meter")))
        return 0;
    if (!PLUGIN_CLASS(k)->constructor(p))
        return 0;
    c = (battery_priv *) p;
    c->timer = g_timeout_add(2000, (GSourceFunc) battery_update, c);
    battery_update(c);
    return 1;
}

/**
 * battery_destructor - stop the polling timer and release meter_class.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout, calls meter_class destructor to disconnect the
 * icon-theme signal, then releases the meter_class reference via class_put().
 */
static void
battery_destructor(plugin_instance *p)
{
    battery_priv *c = (battery_priv *) p;

    if (c->timer)
        g_source_remove(c->timer);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("meter");
    return;
}

static plugin_class class = {
    .count       = 0,
    .type        = "battery",
    .name        = "battery usage",
    .version     = "1.1",
    .description = "Display battery usage",
    .priv_size   = sizeof(battery_priv),
    .constructor = battery_constructor,
    .destructor  = battery_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
