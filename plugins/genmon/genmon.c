/* genmon_priv.c -- Generic monitor plugin for fbpanel
 *
 * Copyright (C) 2007 Davide Truffa <davide@catoblepa.org>
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
 * @file genmon.c
 * @brief Generic monitor plugin: runs a shell command and displays its output.
 *
 * Runs an arbitrary shell command via popen() at a configurable interval,
 * reads the first line of stdout, and displays it as a Pango markup label
 * in the panel.  The label text is formatted with a configurable font size
 * and colour.
 *
 * Config keys (all transfer-none; stored as raw xconf pointers):
 *   Command       (str, default "date +%R") — shell command to run.
 *   TextSize      (str, default "medium")   — Pango size string.
 *   TextColor     (str, default "darkblue") — CSS/Pango colour name or hex.
 *   PollingTime   (int, seconds, default 1) — update interval.
 *   MaxTextLength (int, chars, default 30)  — gtk_label_set_max_width_chars.
 *
 * Main widget: GtkLabel (gm->main), packed into p->pwid.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUG

#define FMT "<span size='%s' foreground='%s'>%s</span>"

typedef struct {
    plugin_instance plugin;
    int time;        /**< Polling interval in seconds (from PollingTime). */
    int timer;       /**< GLib timeout source ID; 0 when not active. */
    int max_text_len;/**< Maximum label width in characters. */
    char *command;   /**< Shell command to run (transfer none, xconf-owned). */
    char *textsize;  /**< Pango size string (transfer none, xconf-owned). */
    char *textcolor; /**< Colour string (transfer none, xconf-owned). */
    GtkWidget *main; /**< GtkLabel displaying command output; owned by pwid. */
} genmon_priv;

/**
 * text_update - run the command and update the label markup.
 * @gm: genmon_priv instance. (transfer none)
 *
 * Opens gm->command via popen(), reads one line from stdout, strips the
 * trailing newline, formats it with g_markup_printf_escaped() using the
 * configured size and colour, and sets the label markup.  The markup string
 * is transfer-full from g_markup_printf_escaped() and is g_free'd here.
 *
 * Called from the GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static int
text_update(genmon_priv *gm)
{
    FILE *fp;
    char text[256];
    char *markup;
    int len;

    fp = popen(gm->command, "r");
    (void)fgets(text, sizeof(text), fp);
    pclose(fp);
    len = strlen(text) - 1;
    if (len >= 0) {
        if (text[len] == '\n')
            text[len] = 0;

        markup = g_markup_printf_escaped(FMT, gm->textsize, gm->textcolor,
            text);
        gtk_label_set_markup (GTK_LABEL(gm->main), markup);
        g_free(markup);
    }
    return TRUE;
}

/**
 * genmon_destructor - stop the polling timer.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout if it is active.  The GtkLabel (gm->main) is
 * owned by p->pwid and is destroyed when the parent widget is destroyed.
 * Config strings (command, textsize, textcolor) are transfer-none xconf
 * pointers and must NOT be freed here.
 */
static void
genmon_destructor(plugin_instance *p)
{
    genmon_priv *gm = (genmon_priv *) p;

    if (gm->timer) {
        g_source_remove(gm->timer);
    }
    return;
}

/**
 * genmon_constructor - configure and start the generic monitor plugin.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads all config keys (all transfer-none; raw xconf pointers stored in
 * genmon_priv fields without copying).  Creates a GtkLabel, performs an
 * initial text_update(), then installs a GLib timeout for subsequent polls.
 *
 * Returns: 1 on success.
 */
static int
genmon_constructor(plugin_instance *p)
{
    genmon_priv *gm;

    gm = (genmon_priv *) p;
    gm->command = "date +%R";
    gm->time = 1;
    gm->textsize = "medium";
    gm->textcolor = "darkblue";
    gm->max_text_len = 30;

    XCG(p->xc, "Command", &gm->command, str);
    XCG(p->xc, "TextSize", &gm->textsize, str);
    XCG(p->xc, "TextColor", &gm->textcolor, str);
    XCG(p->xc, "PollingTime", &gm->time, int);
    XCG(p->xc, "MaxTextLength", &gm->max_text_len, int);

    gm->main = gtk_label_new(NULL);
    gtk_label_set_max_width_chars(GTK_LABEL(gm->main), gm->max_text_len);
    text_update(gm);
    gtk_container_set_border_width (GTK_CONTAINER (p->pwid), 1);
    gtk_container_add(GTK_CONTAINER(p->pwid), gm->main);
    gtk_widget_show_all(p->pwid);
    gm->timer = g_timeout_add((guint) gm->time * 1000,
        (GSourceFunc) text_update, (gpointer) gm);

    return 1;
}


static plugin_class class = {
    .count       = 0,
    .type        = "genmon",
    .name        = "Generic Monitor",
    .version     = "0.3",
    .description = "Display the output of a program/script into the panel",
    .priv_size   = sizeof(genmon_priv),

    .constructor = genmon_constructor,
    .destructor  = genmon_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
