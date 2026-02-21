/**
 * @file mem.c
 * @brief Memory and swap usage progress-bar plugin for fbpanel.
 *
 * Displays one or two GtkProgressBar widgets showing RAM and (optionally)
 * swap usage.  Reads /proc/meminfo on Linux using a compile-time-generated
 * lookup table defined in mt.h.  Updates every 3 seconds.
 *
 * Config keys:
 *   ShowSwap (bool, default false) — show a second progress bar for swap.
 *
 * Main widget: GtkBox (mem->box) containing GtkProgressBar widgets,
 * packed into p->pwid.  Progress bars are oriented opposite to the panel
 * (vertical bars in a horizontal panel, horizontal in a vertical panel)
 * to act as level indicators.
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"


typedef struct
{
    plugin_instance plugin;
    GtkWidget *mem_pb;   /**< GtkProgressBar for RAM usage; owned by mem->box. */
    GtkWidget *swap_pb;  /**< GtkProgressBar for swap (NULL if ShowSwap=false). */
    GtkWidget *box;      /**< GtkBox containing the progress bar(s); owned by pwid. */
    int timer;           /**< GLib timeout source ID; 0 when not active. */
    int show_swap;       /**< Boolean: show the swap progress bar. */
} mem_priv;

typedef struct
{
    char *name;
    gulong val;
    int valid;
} mem_type_t;

typedef struct
{
    struct
    {
        gulong total;
        gulong used;
    } mem;
    struct
    {
        gulong total;
        gulong used;
    } swap;
} stats_t;

static stats_t stats;

#if defined __linux__
#undef MT_ADD
#define MT_ADD(x) MT_ ## x,
enum {
#include "mt.h"
    MT_NUM
};

#undef MT_ADD
#define MT_ADD(x) { #x, 0, 0 },
mem_type_t mt[] =
{
#include "mt.h"
};

static gboolean
mt_match(char *buf, mem_type_t *m)
{
    gulong val;
    int len;

    len = strlen(m->name);
    if (strncmp(buf, m->name, len))
        return FALSE;
    if (sscanf(buf + len + 1, "%lu", &val) != 1)
        return FALSE;
    m->val = val;
    m->valid = 1;
    DBG("%s: %lu\n", m->name, val);
    return TRUE;
}

/**
 * mem_usage - parse /proc/meminfo and populate the static stats struct.
 *
 * Reads the mt[] lookup table (generated from mt.h) to parse the fields of
 * /proc/meminfo.  Computes used RAM as Total - (Free + Buffers + Cached + Slab)
 * and swap used as Total - Free.  Results are stored in the module-level
 * stats variable.
 */
static void
mem_usage()
{
    FILE *fp;
    char buf[160];
    int i;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return;
    for (i = 0; i < MT_NUM; i++)
    {
        mt[i].valid = 0;
        mt[i].val = 0;
    }

    while ((fgets(buf, sizeof(buf), fp)) != NULL)
    {
        for (i = 0; i < MT_NUM; i++)
        {
            if (!mt[i].valid && mt_match(buf, mt + i))
                break;
        }
    }
    fclose(fp);

    stats.mem.total = mt[MT_MemTotal].val;
    stats.mem.used = mt[MT_MemTotal].val -(mt[MT_MemFree].val +
        mt[MT_Buffers].val + mt[MT_Cached].val + mt[MT_Slab].val);
    stats.swap.total = mt[MT_SwapTotal].val;
    stats.swap.used = mt[MT_SwapTotal].val - mt[MT_SwapFree].val;
}
#else
static void
mem_usage()
{

}
#endif

/**
 * mem_update - refresh progress bars and tooltip from current memory stats.
 * @mem: mem_priv instance. (transfer none)
 *
 * Calls mem_usage(), computes fractional usage [0..1], updates both progress
 * bars and the tooltip markup.  Called from the GLib timeout and once from
 * the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static gboolean
mem_update(mem_priv *mem)
{
    gdouble mu, su;
    char str[90];

    mu = su = 0;
    bzero(&stats, sizeof(stats));
    mem_usage();
    if (stats.mem.total)
        mu = (gdouble) stats.mem.used / (gdouble) stats.mem.total;
    if (stats.swap.total)
        su = (gdouble) stats.swap.used / (gdouble) stats.swap.total;
    g_snprintf(str, sizeof(str),
        "<b>Mem:</b> %d%%, %lu MB of %lu MB\n"
        "<b>Swap:</b> %d%%, %lu MB of %lu MB",
        (int)(mu * 100), stats.mem.used >> 10, stats.mem.total >> 10,
        (int)(su * 100), stats.swap.used >> 10, stats.swap.total >> 10);
    DBG("%s\n", str);
    gtk_widget_set_tooltip_markup(mem->plugin.pwid, str);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR(mem->mem_pb), mu);
    if (mem->show_swap)
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR(mem->swap_pb), su);
    return TRUE;
}


/**
 * mem_destructor - stop the update timer and destroy the box widget.
 * @p: plugin_instance. (transfer none)
 *
 * Removes the GLib timeout.  Explicitly destroys mem->box (and its children)
 * rather than relying solely on parent widget destruction.
 */
static void
mem_destructor(plugin_instance *p)
{
    mem_priv *mem = (mem_priv *)p;

    if (mem->timer)
        g_source_remove(mem->timer);
    gtk_widget_destroy(mem->box);
    return;
}

/**
 * mem_constructor - create the memory monitor plugin.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Reads ShowSwap config key.  Creates a GtkBox containing one (or two)
 * GtkProgressBar widgets oriented perpendicular to the panel direction.
 * Each progress bar has a fixed minor-dimension of 9 pixels.  Performs an
 * initial mem_update() call and installs a 3-second GLib timeout.
 *
 * Returns: 1 on success.
 */
static int
mem_constructor(plugin_instance *p)
{
    mem_priv *mem;
    gint w, h;

    mem = (mem_priv *) p;
    XCG(p->xc, "ShowSwap", &mem->show_swap, enum, bool_enum);
    mem->box = p->panel->my_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width (GTK_CONTAINER (mem->box), 0);

    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
        w = 9;
        h = 0;
    }
    else
    {
        w = 0;
        h = 9;
    }
    mem->mem_pb = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(mem->box), mem->mem_pb, FALSE, FALSE, 0);
    if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL) {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(mem->mem_pb), GTK_ORIENTATION_VERTICAL);
    } else {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(mem->mem_pb), GTK_ORIENTATION_HORIZONTAL);
    }
    gtk_widget_set_size_request(mem->mem_pb, w, h);

    if (mem->show_swap)
    {
        mem->swap_pb = gtk_progress_bar_new();
        gtk_box_pack_start(GTK_BOX(mem->box), mem->swap_pb, FALSE, FALSE, 0);
        if (p->panel->orientation == GTK_ORIENTATION_HORIZONTAL)
            gtk_orientable_set_orientation(GTK_ORIENTABLE(mem->swap_pb), GTK_ORIENTATION_VERTICAL);
        else
            gtk_orientable_set_orientation(GTK_ORIENTABLE(mem->swap_pb), GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_size_request(mem->swap_pb, w, h);
    }

    gtk_widget_show_all(mem->box);
    gtk_container_add(GTK_CONTAINER(p->pwid), mem->box);
    gtk_widget_set_tooltip_markup(mem->plugin.pwid, "XXX");
    mem_update(mem);
    mem->timer = g_timeout_add(3000, (GSourceFunc) mem_update, (gpointer)mem);
    return 1;
}

static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "mem",
    .name        = "Memory Monitor",
    .version     = "1.0",
    .description = "Show memory usage",
    .priv_size   = sizeof(mem_priv),

    .constructor = mem_constructor,
    .destructor  = mem_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
