/**
 * @file user.c
 * @brief User photo and menu plugin for fbpanel.
 *
 * Extends the menu_class plugin to display a user avatar and a configurable
 * pop-up menu.  Optionally fetches a Gravatar image from gravatar.com using
 * wget (run as a child process) and rebuilds the menu once the download
 * completes.
 *
 * Config keys (all transfer-none xconf strings unless noted):
 *   image         (str, optional) — path to a local avatar image file.
 *   icon          (str, optional) — icon theme name; default "avatar-default"
 *                 is injected via XCS if neither image nor icon is configured.
 *   gravataremail (str, optional) — email address whose MD5 hash is used to
 *                 construct the Gravatar URL; triggers async wget download.
 *
 * The Gravatar download uses run_app_argv() to spawn wget (transfer-full GPid
 * stored in c->pid) and g_child_watch_add() to install a SIGCHLD callback
 * (c->sid).  On success, fetch_gravatar_done() re-runs the menu_class
 * constructor with the downloaded image file path set in the xconf tree.
 *
 * Ownership:
 *   c->pid: GPid closed via g_spawn_close_pid() in fetch_gravatar_done().
 *   c->sid: GLib source ID; removed via g_source_remove() in the destructor
 *           if wget is still running at destruction time.
 *   image/icon strings in fetch_gravatar_done(): obtained via XCG strdup
 *   (transfer-full), freed via g_free() after being written back to xconf.
 */

#include "misc.h"
#include "run.h"
#include "../menu/menu.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>


//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    menu_priv chart; /**< Embedded menu_priv; must be first member. */
    gint dummy;      /**< Unused padding. */
    guint sid;       /**< g_child_watch source ID for the wget process. */
    GPid pid;        /**< GPid of the running wget subprocess; 0 when idle. */
} user_priv;

static menu_class *k;


static void user_destructor(plugin_instance *p);

#define GRAVATAR_LEN  300

/**
 * fetch_gravatar_done - child-watch callback when wget completes.
 * @pid:    the wget process PID (already reaped by GLib). (transfer none)
 * @status: exit status from g_child_watch_add.
 * @data:   plugin_instance pointer (same as user_priv). (transfer none)
 *
 * Closes the child PID and clears c->pid and c->sid.  If wget succeeded
 * (status == 0), reads the current image and icon values from xconf
 * (transfer-full via XCG strdup), sets the downloaded file as the image
 * in the xconf tree, reconstructs the menu_class plugin by calling
 * destructor then constructor, and restores the image/icon values.
 */
static void
fetch_gravatar_done(GPid pid, gint status, gpointer data)
{
    user_priv *c G_GNUC_UNUSED = data;
    plugin_instance *p G_GNUC_UNUSED = data;
    gchar *image = NULL, *icon = NULL;

    DBG("status %d\n", status);
    g_spawn_close_pid(c->pid);
    c->pid = 0;
    c->sid = 0;

    if (status)
        return;
    DBG("rebuild menu\n");
    XCG(p->xc, "icon", &icon, strdup);   /* transfer-full; g_free'd below */
    XCG(p->xc, "image", &image, strdup); /* transfer-full; g_free'd below */
    XCS(p->xc, "image", image, value);
    xconf_del(xconf_find(p->xc, "icon", 0), FALSE);
    PLUGIN_CLASS(k)->destructor(p);
    PLUGIN_CLASS(k)->constructor(p);
    if (image) {
        XCS(p->xc, "image", image, value);
        g_free(image);
    }
    if (icon) {
        XCS(p->xc, "icon", icon, value);
        g_free(icon);
    }
    return;
}


/**
 * fetch_gravatar - one-shot GLib timeout callback to spawn the wget download.
 * @data: plugin_instance pointer (same as user_priv). (transfer none)
 *
 * Computes the MD5 hash of the configured "gravataremail" (via GChecksum)
 * and constructs the Gravatar URL.  Spawns wget -q to download to
 * /tmp/gravatar.  Stores the GPid in c->pid and installs a child-watch
 * (c->sid) to call fetch_gravatar_done() when wget exits.
 *
 * Called once via g_timeout_add(300ms) from the constructor.
 *
 * Returns: FALSE (one-shot timeout).
 */
static gboolean
fetch_gravatar(gpointer data)
{
    user_priv *c G_GNUC_UNUSED = data;
    plugin_instance *p G_GNUC_UNUSED = data;
    GChecksum *cs;
    gchar *gravatar = NULL;
    gchar buf[GRAVATAR_LEN];
    /* FIXME: select more secure path */
    gchar *image = "/tmp/gravatar";
    gchar *argv[] = { "wget", "-q", "-O", image, buf, NULL };

    cs = g_checksum_new(G_CHECKSUM_MD5);
    XCG(p->xc, "gravataremail", &gravatar, str); /* transfer-none */
    g_checksum_update(cs, (guchar *) gravatar, -1);
    snprintf(buf, sizeof(buf), "http://www.gravatar.com/avatar/%s",
        g_checksum_get_string(cs));
    g_checksum_free(cs);
    DBG("gravatar '%s'\n", buf);
    c->pid = run_app_argv(argv);
    c->sid = g_child_watch_add(c->pid, fetch_gravatar_done, data);
    return FALSE;
}


/**
 * user_constructor - set up the user plugin on top of menu_class.
 * @p: plugin_instance allocated by the plugin framework. (transfer none)
 *
 * Obtains menu_class via class_get("menu").  If neither "image" nor "icon"
 * is configured, injects "icon = avatar-default" into the xconf tree via XCS.
 * Calls menu_class constructor.  If "gravataremail" is configured, installs
 * a 300ms one-shot timeout to initiate the Gravatar download asynchronously.
 *
 * Returns: 1 on success, 0 if menu class is unavailable or its constructor
 * fails.
 */
static int
user_constructor(plugin_instance *p)
{
    user_priv *c G_GNUC_UNUSED = (user_priv *) p;
    gchar *image = NULL;
    gchar *icon = NULL;
    gchar *gravatar = NULL;

    if (!(k = class_get("menu")))
        return 0;
    XCG(p->xc, "image", &image, str);
    XCG(p->xc, "icon", &icon, str);
    if (!(image || icon))
        XCS(p->xc, "icon", "avatar-default", value);
    if (!PLUGIN_CLASS(k)->constructor(p))
        return 0;
    XCG(p->xc, "gravataremail", &gravatar, str);
    DBG("gravatar email '%s'\n", gravatar);
    if (gravatar)
        g_timeout_add(300, fetch_gravatar, p);
    gtk_widget_set_tooltip_markup(p->pwid, "<b>User</b>");
    return 1;
}


/**
 * user_destructor - tear down the user plugin.
 * @p: plugin_instance. (transfer none)
 *
 * Calls the menu_class destructor first.  If the wget subprocess is still
 * running (c->pid != 0), kills it with SIGKILL.  Removes the child-watch
 * source (c->sid) if active.  Releases the menu_class reference via
 * class_put("menu").
 */
static void
user_destructor(plugin_instance *p)
{
    user_priv *c G_GNUC_UNUSED = (user_priv *) p;

    PLUGIN_CLASS(k)->destructor(p);
    if (c->pid)
        kill(c->pid, SIGKILL);
    if (c->sid)
        g_source_remove(c->sid);
    class_put("menu");
    return;
}


static plugin_class class = {
    .count       = 0,
    .type        = "user",
    .name        = "User menu",
    .version     = "1.0",
    .description = "User photo and menu of user actions",
    .priv_size   = sizeof(user_priv),

    .constructor = user_constructor,
    .destructor  = user_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
