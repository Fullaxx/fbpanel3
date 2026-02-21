/**
 * @file cpu.c
 * @brief CPU usage scrolling chart plugin for fbpanel.
 *
 * Displays a scrolling bar chart of total CPU utilisation (user+nice+sys
 * as a fraction of total CPU time) using the chart_class base plugin.
 * On Linux the data source is /proc/stat; on FreeBSD it uses kern.cp_time
 * via sysctl.  Updates every 1 second.
 *
 * Config keys (transfer-none xconf strings):
 *   Color (str, default "green") — CSS colour string for the chart bar.
 *
 * Delegates construction and destruction to chart_class (obtained via
 * class_get("chart")/class_put("chart")).
 *
 * Free BSD support
 * A little bug fixed by Mykola <mykola@2ka.mipt.ru>:)
 * FreeBSD support added by Andreas Wiese <aw@instandbesetzt.net>
 * and was extended by Eygene Ryabinkin <rea-fbsd@codelabs.ru>
 */


#include <string.h>
#include "misc.h"
#include "../chart/chart.h"

//#define DEBUGPRN
#include "dbg.h"
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

struct cpu_stat {
    gulong u, n, s, i, w; /* user, nice, system, idle, wait */
};

typedef struct {
    chart_priv chart;       /**< Embedded chart_priv; must be first member. */
    struct cpu_stat cpu_prev; /**< CPU counters from previous poll cycle. */
    int timer;              /**< GLib timeout source ID. */
    gchar *colors[1];       /**< Single-element colour array for chart_set_rows(). */
} cpu_priv;

static chart_class *k;

static void cpu_destructor(plugin_instance *p);


#if defined __linux__
/**
 * cpu_get_load_real - read raw CPU counters from /proc/stat.
 * @cpu: output struct to fill. (transfer none)
 *
 * Returns: 0 on success, -1 if /proc/stat cannot be opened.
 */
static int
cpu_get_load_real(struct cpu_stat *cpu)
{
    FILE *stat;

    memset(cpu, 0, sizeof(struct cpu_stat));
    stat = fopen("/proc/stat", "r");
    if(!stat)
        return -1;

    (void)fscanf(stat, "cpu %lu %lu %lu %lu %lu", &cpu->u, &cpu->n, &cpu->s,
            &cpu->i, &cpu->w);
    fclose(stat);

    return 0;
}
#elif defined __FreeBSD__
static int
cpu_get_load_real(struct cpu_stat *cpu)
{
    static int mib[2] = { -1, -1 }, init = 0;
    size_t j;
    long ct[CPUSTATES];

    memset(cpu, 0, sizeof(struct cpu_stat));
    if (init == 0) {
        j = 2;
        if (sysctlnametomib("kern.cp_time", mib, &j) != 0)
            return -1;

        init = 1;
    }

    j = sizeof(ct);
    if (sysctl(mib, 2, ct, &j, NULL, 0) != 0)
        return -1;
    cpu->u = ct[CP_USER];
    cpu->n = ct[CP_NICE];
    cpu->s = ct[CP_SYS];
    cpu->i = ct[CP_IDLE];
    cpu->w = 0;

    return 0;
}
#else
static int
cpu_get_load_real(struct cpu_stat *s)
{
    memset(cpu, 0, sizeof(struct cpu_stat));
    return 0;
}
#endif

/**
 * cpu_get_load - compute CPU load fraction and push a tick to the chart.
 * @c: cpu_priv. (transfer none)
 *
 * Reads current CPU counters, subtracts the previous sample to get deltas,
 * computes total[0] = active / (active + idle + wait) in [0.0..1.0], and
 * calls k->add_tick().  Also updates the tooltip with the current percentage.
 * Stores the current counters in c->cpu_prev for the next cycle.
 *
 * Called from the 1-second GLib timeout and once from the constructor.
 *
 * Returns: TRUE to keep the timeout active.
 */
static int
cpu_get_load(cpu_priv *c)
{
    gfloat a, b;
    struct cpu_stat cpu, cpu_diff;
    float total[1];
    gchar buf[40];

    memset(&cpu, 0, sizeof(cpu));
    memset(&cpu_diff, 0, sizeof(cpu_diff));
    memset(&total, 0, sizeof(total));

    if (cpu_get_load_real(&cpu))
        goto end;

    cpu_diff.u = cpu.u - c->cpu_prev.u;
    cpu_diff.n = cpu.n - c->cpu_prev.n;
    cpu_diff.s = cpu.s - c->cpu_prev.s;
    cpu_diff.i = cpu.i - c->cpu_prev.i;
    cpu_diff.w = cpu.w - c->cpu_prev.w;
    c->cpu_prev = cpu;

    a = cpu_diff.u + cpu_diff.n + cpu_diff.s;
    b = a + cpu_diff.i + cpu_diff.w;
    total[0] = b ? a / b : 1.0;

end:
    DBG("total=%f a=%f b=%f\n", total[0], a, b);
    g_snprintf(buf, sizeof(buf), "<b>Cpu:</b> %d%%", (int)(total[0] * 100));
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    k->add_tick(&c->chart, total);
    return TRUE;

}

/**
 * cpu_constructor - initialise the CPU chart plugin on top of chart_class.
 * @p: plugin_instance. (transfer none)
 *
 * Obtains chart_class via class_get("chart"), calls its constructor, then
 * reads the "Color" config key (transfer-none), configures one data row, and
 * installs a 1-second GLib timeout.  Calls cpu_get_load() once immediately.
 *
 * Returns: 1 on success, 0 if chart class is unavailable.
 */
static int
cpu_constructor(plugin_instance *p)
{
    cpu_priv *c;

    if (!(k = class_get("chart")))
        return 0;
    if (!PLUGIN_CLASS(k)->constructor(p))
        return 0;
    c = (cpu_priv *) p;
    c->colors[0] = "green";
    XCG(p->xc, "Color", &c->colors[0], str);

    k->set_rows(&c->chart, 1, c->colors);
    gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, "<b>Cpu</b>");
    cpu_get_load(c);
    c->timer = g_timeout_add(1000, (GSourceFunc) cpu_get_load, (gpointer) c);
    return 1;
}


/**
 * cpu_destructor - stop the timer and release chart_class.
 * @p: plugin_instance. (transfer none)
 */
static void
cpu_destructor(plugin_instance *p)
{
    cpu_priv *c = (cpu_priv *) p;

    g_source_remove(c->timer);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("chart");
    return;
}



static plugin_class class = {
    .count       = 0,
    .type        = "cpu",
    .name        = "Cpu usage",
    .version     = "1.0",
    .description = "Display cpu usage",
    .priv_size   = sizeof(cpu_priv),
    .constructor = cpu_constructor,
    .destructor  = cpu_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
