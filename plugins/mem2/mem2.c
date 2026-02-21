/**
 * @file mem2.c
 * @brief Memory and swap usage scrolling chart plugin for fbpanel.
 *
 * Displays a scrolling chart of RAM and optionally swap usage using the
 * chart_class base plugin.  Reads /proc/meminfo on Linux using the same
 * mt.h-generated lookup table as the mem plugin.  Updates every 2 seconds.
 *
 * Config keys (transfer-none xconf strings):
 *   MemColor  (str, default "red")  — chart bar colour for RAM usage.
 *   SwapColor (str, optional)       — chart bar colour for swap; omitting
 *             this key disables the swap row (single-row chart).
 *
 * Delegates construction and destruction to chart_class (obtained via
 * class_get("chart")/class_put("chart")).
 *
 * Licence: GPLv2
 *
 * bercik-rrp@users.sf.net
 */

#include "../chart/chart.h"
#include <stdlib.h>
#include <string.h>

//#define DEBUGPRN
#include "dbg.h"

#define CHECK_PERIOD   2 /* second */

typedef struct {
    chart_priv chart;    /**< Embedded chart_priv; must be first member. */
    int timer;           /**< GLib timeout source ID. */
    gulong max;          /**< Unused; retained for struct layout. */
    gchar *colors[2];    /**< Two-element colour array: [0]=mem, [1]=swap. */
} mem2_priv;

static chart_class *k;

static void mem2_destructor(plugin_instance *p);

typedef struct {
    char *name;
    gulong val;
    int valid;
} mem_type_t;


#if defined __linux__
#undef MT_ADD
#define MT_ADD(x) MT_ ## x,
enum {
#include "../mem/mt.h"
    MT_NUM
};

#undef MT_ADD
#define MT_ADD(x) { #x, 0, 0 },
mem_type_t mt[] =
{
#include "../mem/mt.h"
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
 * mem_usage - parse /proc/meminfo and push tick values to the chart.
 * @c: mem2_priv. (transfer none)
 *
 * Reads /proc/meminfo using the mt[] lookup table (generated from
 * mem/mt.h).  Computes used RAM = Total - (Free + Buffers + Cached + Slab)
 * and used swap = Total - Free.  Converts to fractions [0..1] for the
 * chart tick and formats a tooltip showing MB values.
 *
 * Called from the 2-second GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static int
mem_usage(mem2_priv *c)
{
    FILE *fp;
    char buf[160];
    long unsigned int total[2];
    float total_r[2];
    int i;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        RET(FALSE);;
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

    total[0] = (float)(mt[MT_MemTotal].val -(mt[MT_MemFree].val +
        mt[MT_Buffers].val + mt[MT_Cached].val + mt[MT_Slab].val));
    total[1] = (float)(mt[MT_SwapTotal].val - mt[MT_SwapFree].val);
    total_r[0] = (float)total[0] / mt[MT_MemTotal].val;
    total_r[1] = (float)total[1] / mt[MT_SwapTotal].val;

    g_snprintf(buf, sizeof(buf),
        "<b>Mem:</b> %d%%, %lu MB of %lu MB\n"
        "<b>Swap:</b> %d%%, %lu MB of %lu MB",
        (int)(total_r[0] * 100), total[0] >> 10, mt[MT_MemTotal].val >> 10,
        (int)(total_r[1] * 100), total[1] >> 10, mt[MT_SwapTotal].val >> 10);

    k->add_tick(&c->chart, total_r);
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    return TRUE;

}
#else
static int
mem_usage()
{

}
#endif

/**
 * mem2_constructor - initialise the memory chart plugin on top of chart_class.
 * @p: plugin_instance. (transfer none)
 *
 * Obtains chart_class via class_get("chart"), calls its constructor, then
 * reads MemColor and SwapColor config keys (both transfer-none).  Configures
 * 1 or 2 chart rows depending on whether SwapColor is set.  Performs an
 * initial mem_usage() call and installs a 2-second GLib timeout.
 *
 * Returns: 1 on success, 0 if chart class is unavailable.
 */
static int
mem2_constructor(plugin_instance *p)
{
    mem2_priv *c;

    if (!(k = class_get("chart")))
        return 0;
    if (!PLUGIN_CLASS(k)->constructor(p))
        return 0;
    c = (mem2_priv *) p;

    c->colors[0] = "red";
    c->colors[1] = NULL;
    XCG(p->xc, "MemColor", &c->colors[0], str);
    XCG(p->xc, "SwapColor", &c->colors[1], str);

    if (c->colors[1] == NULL) {
        k->set_rows(&c->chart, 1, c->colors);
    } else {
        k->set_rows(&c->chart, 2, c->colors);
    }
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid,
        "<b>Memory</b>");
    mem_usage(c);
    c->timer = g_timeout_add(CHECK_PERIOD * 1000,
        (GSourceFunc) mem_usage, (gpointer) c);
    return 1;
}


/**
 * mem2_destructor - stop the timer and release chart_class.
 * @p: plugin_instance. (transfer none)
 */
static void
mem2_destructor(plugin_instance *p)
{
    mem2_priv *c = (mem2_priv *) p;

    if (c->timer)
        g_source_remove(c->timer);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("chart");
    return;
}


static plugin_class class = {
    .fname       = NULL,
    .count       = 0,
    .type        = "mem2",
    .name        = "Chart Memory Monitor",
    .version     = "1.0",
    .description = "Show memory usage as chart",
    .priv_size   = sizeof(mem2_priv),

    .constructor = mem2_constructor,
    .destructor  = mem2_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
