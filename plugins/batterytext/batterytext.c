/* batterytext_priv.c -- Generic monitor plugin for fbpanel
 *
 * Copyright (C) 2017 Fred Stober <mail@fredstober.de>
 *
 * This plugin is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991.
 *
 * It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/**
 * @file batterytext.c
 * @brief Battery charge text label plugin for fbpanel.
 *
 * Reads battery energy values from the Linux power supply sysfs interface
 * (default: /sys/class/power_supply/BAT0) and displays the charge ratio as
 * a coloured markup label.  Charging is shown in green with a '+' suffix;
 * discharging is shown in red with a '-' suffix.  The tooltip shows the
 * estimated remaining time (HH:MM:SS) computed from power_now.
 *
 * Config keys (all transfer-none xconf strings):
 *   DesignCapacity (bool, default false) — use energy_full_design instead
 *                  of energy_full as the 100% reference.
 *   PollingTimeMs  (int, ms, default 500) — update interval.
 *   TextSize       (str, default "medium") — Pango size string.
 *   BatteryPath    (str, default "/sys/class/power_supply/BAT0") — sysfs dir.
 *
 * Main widget: GtkLabel (gm->main) packed into p->pwid.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUG

typedef struct {
    plugin_instance plugin;
    int design;      /**< Use energy_full_design if true; energy_full otherwise. */
    int time;        /**< Polling interval in milliseconds. */
    char *textsize;  /**< Pango size string (transfer-none, xconf-owned). */
    char *battery;   /**< Path to sysfs battery directory (transfer-none, xconf-owned). */
    int timer;       /**< GLib timeout source ID; 0 when not active. */
    GtkWidget *main; /**< GtkLabel displaying charge %; owned by pwid. */
} batterytext_priv;

/**
 * read_bat_value - read a float from a sysfs battery file.
 * @dn: sysfs battery directory path. (transfer none)
 * @fn: filename within the directory (e.g., "energy_now"). (transfer none)
 *
 * Returns: the parsed float value, or -1 on error (file not found or parse
 * failure).
 */
static float
read_bat_value(const char *dn, const char *fn)
{
    FILE *fp;
    float value = -1;
    char value_path[256];
    g_snprintf(value_path, sizeof(value_path), "%s/%s", dn, fn);
    fp = fopen(value_path, "r");
    if (fp != NULL) {
        if (fscanf(fp, "%f", &value) != 1)
            value = -1;
        fclose(fp);
    }
    return value;
}

/**
 * text_update - read battery state and refresh the label and tooltip.
 * @gm: batterytext_priv. (transfer none)
 *
 * Reads energy_full_design, energy_full, energy_now, power_now, and the
 * "status" file from gm->battery.  Computes the charge ratio and formats
 * a colour-coded markup string (red/green) for the label and an HH:MM:SS
 * estimate for the tooltip.  Both markup and tooltip strings are
 * transfer-full from g_markup_printf_escaped() and are g_free'd here.
 *
 * Called from the GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static int
text_update(batterytext_priv *gm)
{
    FILE *fp_status;
    char battery_status[256];
    char *markup;
    char *tooltip;
    char buffer[256];
    float energy_full_design = -1;
    float energy_full = -1;
    float energy_now = -1;
    float power_now = -1;
    int discharging = 0;
    float charge_ratio = 0;
    int charge_time = 0;

    energy_full_design = read_bat_value(gm->battery, "energy_full_design");
    energy_full = read_bat_value(gm->battery, "energy_full");
    energy_now = read_bat_value(gm->battery, "energy_now");
    power_now = read_bat_value(gm->battery, "power_now");

    snprintf(battery_status, sizeof(battery_status), "%s/status", gm->battery);
    fp_status = fopen(battery_status, "r");
    if (fp_status != NULL) {
        while ((fgets(buffer, sizeof(buffer), fp_status)) != NULL) {
            if (strstr(buffer, "Discharging") != NULL)
                discharging = 1;
        }
        fclose(fp_status);
    }

    if ((energy_full_design >= 0) && (energy_now >= 0)) {
        if (gm->design)
            charge_ratio = 100 * energy_now / energy_full_design;
        else
            charge_ratio = 100 * energy_now / energy_full;
        if (discharging)
        {
            markup = g_markup_printf_escaped("<span size='%s' foreground='red'><b>%.2f-</b></span>",
                gm->textsize, charge_ratio);
            charge_time = (int)(energy_now / power_now * 3600);
        }
        else
        {
            markup = g_markup_printf_escaped("<span size='%s' foreground='green'><b>%.2f+</b></span>",
                gm->textsize, charge_ratio);
            charge_time = (int)((energy_full - energy_now) / power_now * 3600);
        }
        tooltip = g_markup_printf_escaped("%02d:%02d:%02d",
            charge_time / 3600, (charge_time / 60) % 60, charge_time % 60);
        gtk_label_set_markup (GTK_LABEL(gm->main), markup);
        g_free(markup);
        gtk_widget_set_tooltip_markup (gm->main, tooltip);
        g_free(tooltip);
    }
    else
    {
        gtk_label_set_markup (GTK_LABEL(gm->main), "N/A");
        gtk_widget_set_tooltip_markup (gm->main, "N/A");
    }
    return TRUE;
}

/**
 * batterytext_destructor - stop the polling timer.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout.  The GtkLabel is owned by p->pwid.
 * Config strings (textsize, battery) are transfer-none xconf pointers
 * and must NOT be freed here.
 */
static void
batterytext_destructor(plugin_instance *p)
{
    batterytext_priv *gm = (batterytext_priv *) p;

    if (gm->timer) {
        g_source_remove(gm->timer);
    }
    return;
}

/**
 * batterytext_constructor - configure and start the battery text plugin.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads config keys (all transfer-none; raw xconf pointers stored directly
 * in batterytext_priv without copying).  Creates a GtkLabel, calls
 * text_update() once for the initial display, then installs a GLib timeout
 * with PollingTimeMs interval.
 *
 * Returns: 1 on success.
 */
static int
batterytext_constructor(plugin_instance *p)
{
    batterytext_priv *gm;

    gm = (batterytext_priv *) p;
    gm->design = False;
    gm->time = 500;
    gm->textsize = "medium";
    gm->battery = "/sys/class/power_supply/BAT0";

    XCG(p->xc, "DesignCapacity", &gm->design, enum, bool_enum);
    XCG(p->xc, "PollingTimeMs", &gm->time, int);
    XCG(p->xc, "TextSize", &gm->textsize, str);
    XCG(p->xc, "BatteryPath", &gm->battery, str);

    gm->main = gtk_label_new(NULL);
    text_update(gm);
    gtk_container_set_border_width (GTK_CONTAINER (p->pwid), 1);
    gtk_container_add(GTK_CONTAINER(p->pwid), gm->main);
    gtk_widget_show_all(p->pwid);
    gm->timer = g_timeout_add((guint) gm->time,
        (GSourceFunc) text_update, (gpointer) gm);

    return 1;
}


static plugin_class class = {
    .count       = 0,
    .type        = "batterytext",
    .name        = "Generic Monitor",
    .version     = "0.1",
    .description = "Display battery usage in text form",
    .priv_size   = sizeof(batterytext_priv),

    .constructor = batterytext_constructor,
    .destructor  = batterytext_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
