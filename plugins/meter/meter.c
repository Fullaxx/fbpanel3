/**
 * @file meter.c
 * @brief Base meter plugin: icon-based level indicator for fbpanel.
 *
 * Provides the meter_class base used by the battery and volume plugins.
 * Not intended to be loaded directly; battery/volume call class_get("meter")
 * and delegate to its constructor/destructor.
 *
 * Displays a single GtkImage (m->meter) inside p->pwid.  The icon displayed
 * is chosen from an array of icon theme names (set via meter_set_icons()) by
 * mapping the current level percentage [0..100] to an index into the array.
 * When the GtkIconTheme emits "changed" (e.g., after a theme switch), the
 * icon is reloaded via update_view().
 *
 * meter_class extends plugin_class with two virtual methods:
 *   set_level(m, level) — update the icon for the given percentage level.
 *   set_icons(m, icons) — set the NULL-terminated icon name array.
 *
 * Config keys: none (icon names and levels are driven by the subplugin).
 *
 * Main widget: GtkImage (m->meter) packed into p->pwid.
 */

#include "plugin.h"
#include "panel.h"
#include "meter.h"


//#define DEBUGPRN
#include "dbg.h"
float roundf(float x);

/**
 * meter_set_level - update the displayed icon to reflect the given level.
 * @m:     meter_priv. (transfer none)
 * @level: percentage [0..100].
 *
 * Maps level to an icon array index using round((level/100) * (num-1)).
 * Loads the icon from the current GtkIconTheme at m->size pixels with
 * GTK_ICON_LOOKUP_FORCE_SIZE.  Skips the reload if the computed index has
 * not changed.  The loaded GdkPixbuf is transfer-full and unreffed here
 * after being set on the GtkImage.
 */
/* level - per cent level from 0 to 100 */
static void
meter_set_level(meter_priv *m, int level)
{
    int i;
    GdkPixbuf *pb;

    if (m->level == level)
        return;
    if (!m->num)
        return;
    if (level < 0 || level > 100) {
        ERR("meter: illegal level %d\n", level);
        return;
    }
    i = roundf((gfloat) level / 100 * (m->num - 1));
    DBG("level=%f icon=%d\n", level, i);
    if (i != m->cur_icon) {
        m->cur_icon = i;
        pb = gtk_icon_theme_load_icon(icon_theme, m->icons[i],
            m->size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        DBG("loading icon '%s' %s\n", m->icons[i], pb ? "ok" : "failed");
        gtk_image_set_from_pixbuf(GTK_IMAGE(m->meter), pb);
        if (pb)
            g_object_unref(G_OBJECT(pb));
    }
    m->level = level;
    return;
}

/**
 * meter_set_icons - select the icon name array and reset the displayed icon.
 * @m:     meter_priv. (transfer none)
 * @icons: NULL-terminated array of icon theme names. (transfer none; caller owns)
 *
 * Sets m->icons, m->num, and resets m->cur_icon and m->level to -1 so that
 * the next meter_set_level() call forces an icon reload regardless of the
 * previous level.
 */
static void
meter_set_icons(meter_priv *m, gchar **icons)
{
    gchar **s;

    if (m->icons == icons)
        return;
    for (s = icons; *s; s++)
        DBG("icon %s\n", *s);
    m->num = (s - icons);
    DBG("total %d icons\n", m->num);
    m->icons = icons;
    m->cur_icon = -1;
    m->level = -1;
    return;
}

/**
 * update_view - force icon reload after a GtkIconTheme change.
 * @m: meter_priv. (transfer none)
 *
 * Resets cur_icon to -1 and calls meter_set_level() with the cached level
 * so the icon is reloaded from the new theme.  Connected to the "changed"
 * signal of icon_theme via g_signal_connect_swapped().
 */
static void
update_view(meter_priv *m)
{
    m->cur_icon = -1;
    meter_set_level(m, m->level);
    return;
}

/**
 * meter_constructor - create the meter GtkImage and connect the theme signal.
 * @p: plugin_instance. (transfer none)
 *
 * Creates a centred GtkImage, packs it into p->pwid, and initialises cur_icon
 * to -1.  Icon size defaults to panel->max_elem_height.  Connects
 * "changed" on icon_theme (swapped) to update_view so icons refresh on theme
 * changes.  Stores the signal ID in m->itc_id for disconnection in the
 * destructor.
 *
 * Returns: 1 on success.
 */
static int
meter_constructor(plugin_instance *p)
{
    meter_priv *m;

    m = (meter_priv *) p;
    m->meter = gtk_image_new();
    gtk_widget_set_halign(m->meter, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(m->meter, GTK_ALIGN_CENTER);
    gtk_widget_show(m->meter);
    gtk_container_add(GTK_CONTAINER(p->pwid), m->meter);
    m->cur_icon = -1;
    m->size = p->panel->max_elem_height;
    m->itc_id = g_signal_connect_swapped(G_OBJECT(icon_theme),
        "changed", (GCallback) update_view, m);
    return 1;
}

/**
 * meter_destructor - disconnect the icon theme signal handler.
 * @p: plugin_instance. (transfer none)
 *
 * Disconnects the "changed" signal from icon_theme using the stored m->itc_id.
 * The GtkImage is owned by p->pwid and destroyed by the parent.
 */
static void
meter_destructor(plugin_instance *p)
{
    meter_priv *m = (meter_priv *) p;

    g_signal_handler_disconnect(G_OBJECT(icon_theme), m->itc_id);
    return;
}

static meter_class class = {
    .plugin = {
        .type        = "meter",
        .name        = "Meter",
        .description = "Basic meter plugin",
        .version     = "1.0",
        .priv_size   = sizeof(meter_priv),

        .constructor = meter_constructor,
        .destructor  = meter_destructor,
    },
    .set_level = meter_set_level,
    .set_icons = meter_set_icons,
};


static plugin_class *class_ptr = (plugin_class *) &class;
