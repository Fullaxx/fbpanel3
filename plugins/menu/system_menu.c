/**
 * @file system_menu.c
 * @brief XDG application menu builder for the menu plugin.
 *
 * Builds a `systemmenu` xconf tree by scanning all XDG application directories
 * (system $XDG_DATA_DIRS and user $XDG_DATA_HOME) for `.desktop` files, grouping
 * applications into the ten recognised XDG categories, stripping empty categories,
 * and sorting alphabetically.
 *
 * PUBLIC API (called from menu.c)
 * --------------------------------
 * - xconf_new_from_systemmenu() — build the full menu tree (transfer full).
 * - systemmenu_changed()        — detect whether any .desktop file changed
 *                                 since a given timestamp.
 *
 * XCONF TREE SHAPE
 * ----------------
 * The returned tree has the form:
 *
 *   systemmenu
 *     menu                         (one per non-empty XDG category)
 *       name    "Audio & Video"
 *       icon    "applications-multimedia"
 *       item                       (one per matching .desktop file)
 *         icon  "icon-name"        (XDG theme name; OR)
 *         image "/absolute/path"   (only for absolute icon paths)
 *         name  "Application Name"
 *         action "exec-string"
 *       item
 *       ...
 *     menu
 *     ...
 *
 * All xconf nodes are freshly allocated by xconf_new(); the caller (menu_expand_xc)
 * must free the tree with xconf_del().
 *
 * DIRECTORY TRAVERSAL
 * -------------------
 * The scanner uses g_chdir() for relative-path traversal.  fbpanel is
 * single-threaded (GTK main loop), so changing the process-wide working directory
 * is safe.  Every function saves and restores cwd via g_get_current_dir().
 *
 * DEDUPLICATION
 * -------------
 * do_app_dir() uses the category hash table as a visited-set: it inserts the
 * top-level XDG data directory pointer itself as a sentinel value (dir -> ht),
 * so subsequent calls for the same directory are skipped without a separate set.
 *
 * KNOWN ISSUES
 * ------------
 * BUG-017: do_app_file() strips '%' format codes from Exec with a while loop
 * that re-calls strchr after replacing characters.  When Exec ends with a lone
 * '%' (dot[1] == '\0'), the replacement is skipped but strchr finds the same
 * '%' again on the next iteration, causing an infinite loop.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#include "panel.h"
#include "xconf.h"

//#define DEBUGPRN
#include "dbg.h"

/** Group name in .desktop files (XDG Desktop Entry Specification). */
static const char desktop_ent[] = "Desktop Entry";

/** Subdirectory name within each XDG data directory that holds .desktop files. */
static const gchar app_dir_name[] = "applications";

/**
 * cat_info - static descriptor for one XDG application category.
 *
 * @name:       XDG category string (e.g. "AudioVideo"); used as hash table key.
 * @icon:       Icon theme name for the category submenu.
 * @local_name: Translatable display name (wrapped in c_() at definition).
 */
typedef struct {
    gchar *name;
    gchar *icon;
    gchar *local_name;
} cat_info;

/**
 * main_cats - the ten XDG application categories recognised by the system menu.
 *
 * Categories not in this list are silently ignored.  The c_() macro marks
 * strings for extraction by xgettext without translating at compile time;
 * _(local_name) is called at runtime in xconf_new_from_systemmenu() so the
 * user's locale is active.
 */
static cat_info main_cats[] = {
    { "AudioVideo", "applications-multimedia", c_("Audio & Video") },
    { "Education",  "applications-other", c_("Education") },
    { "Game",       "applications-games", c_("Game") },
    { "Graphics",   "applications-graphics", c_("Graphics") },
    { "Network",    "applications-internet", c_("Network") },
    { "Office",     "applications-office", c_("Office") },
    { "Settings",   "preferences-system", c_("Settings") },
    { "System",     "applications-system", c_("System") },
    { "Utility",    "applications-utilities", c_("Utilities") },
    { "Development","applications-development", c_("Development") },
};

/**
 * do_app_file - parse one .desktop file and insert it into the category tree.
 * @ht:   Hash table mapping XDG category name (const char*) to the category's
 *        xconf "menu" node (xconf*).  (transfer none)
 * @file: Path to the .desktop file; may be relative to the process cwd.
 *        (transfer none)
 *
 * Reads the [Desktop Entry] group and filters out entries that:
 *  - fail to load
 *  - set NoDisplay=true
 *  - set OnlyShowIn (environment-specific)
 *  - have no Exec key
 *  - have no Categories key
 *  - have no Name key
 *  - belong only to categories not in main_cats[]
 *
 * For accepted entries, a new "item" xconf subtree is appended to the
 * matching category node:
 *   item
 *     icon  <theme-name>   (if icon is a non-absolute theme name)
 *     image <abs-path>     (if icon is an absolute filesystem path)
 *     name  <display-name>
 *     action <exec-string>
 *
 * The icon extension (.png, .svg) is stripped from non-absolute paths to
 * allow the GTK icon theme engine to find the icon across resolutions.
 *
 * Note: Exec format codes (%f, %u, etc.) are replaced with spaces.
 * BUG-017: if Exec ends with a lone '%' the replacement loop does not
 * advance past it and loops forever; see BUGS_AND_ISSUES.md.
 */
static void
do_app_file(GHashTable *ht, const gchar *file)
{
    GKeyFile *f;
    gchar *name, *icon, *action,*dot;
    gchar **cats, **tmp;
    xconf *ixc, *vxc, *mxc;

    DBG("desktop: %s\n", file);
    /* get values */
    name = icon = action = dot = NULL;
    cats = tmp = NULL;
    f = g_key_file_new();
    if (!g_key_file_load_from_file(f, file, 0, NULL))
        goto out;
    if (g_key_file_get_boolean(f, desktop_ent, "NoDisplay", NULL))
    {
        DBG("\tNoDisplay\n");
        goto out;
    }
    if (g_key_file_has_key(f, desktop_ent, "OnlyShowIn", NULL))
    {
        DBG("\tOnlyShowIn\n");
        goto out;
    }
    if (!(action = g_key_file_get_string(f, desktop_ent, "Exec", NULL)))
    {
        DBG("\tNo Exec\n");
        goto out;
    }
    if (!(cats = g_key_file_get_string_list(f,
                desktop_ent, "Categories", NULL, NULL)))
    {
        DBG("\tNo Categories\n");
        goto out;
    }
    if (!(name = g_key_file_get_locale_string(f,
                desktop_ent, "Name", NULL, NULL)))
    {
        DBG("\tNo Name\n");
        goto out;
    }
    icon = g_key_file_get_string(f, desktop_ent, "Icon", NULL);
    if (!icon)
        DBG("\tNo Icon\n");

    /* Strip Exec format codes (%f, %u, %U, etc.) by overwriting with spaces.
     * BUG-017: loop does not advance when dot[1]=='\0'; lone trailing '%' loops forever. */
    while ((dot = strchr(action, '%'))) {
        if (dot[1] != '\0')
            dot[0] = dot[1] = ' ';
    }
    DBG("action: %s\n", action);
    /* if icon is NOT an absolute path but has an extention,
     * e.g. firefox.png, then drop an extenstion to allow to load it
     * as themable icon */
    if (icon && icon[0] != '/' && (dot = strrchr(icon, '.' )) &&
        !(strcasecmp(dot + 1, "png") && strcasecmp(dot + 1, "svg")))
    {
        *dot = '\0';
    }
    DBG("icon: %s\n", icon);

    /* Find the first listed category that we recognise. */
    for (mxc = NULL, tmp = cats; *tmp; tmp++)
        if ((mxc = g_hash_table_lookup(ht, *tmp)))
            break;
    if (!mxc)
    {
        DBG("\tUnknown categories\n");
        goto out;
    }

    /* Build the "item" subtree and append to the category menu node. */
    ixc = xconf_new("item", NULL);
    xconf_append(mxc, ixc);
    if (icon)
    {
        /* "image" for absolute paths; "icon" for theme names. */
        vxc = xconf_new((icon[0] == '/') ? "image" : "icon", icon);
        xconf_append(ixc, vxc);
    }
    vxc = xconf_new("name", name);
    xconf_append(ixc, vxc);
    vxc = xconf_new("action", action);
    xconf_append(ixc, vxc);

out:
    g_free(icon);
    g_free(name);
    g_free(action);
    g_strfreev(cats);
    g_key_file_free(f);
}

/**
 * do_app_dir_real - recursively scan a directory for .desktop files.
 * @ht:  Category hash table passed through to do_app_file(). (transfer none)
 * @dir: Directory path to scan; may be relative to the current process cwd.
 *       (transfer none)
 *
 * Changes to @dir, opens it, and iterates entries:
 *  - subdirectories: recurse via do_app_dir_real()
 *  - files with suffix ".desktop": call do_app_file()
 *  - other files: skip
 *
 * The process cwd is saved and restored around the chdir so the caller's
 * context is preserved.
 */
static void
do_app_dir_real(GHashTable *ht, const gchar *dir)
{
    GDir *d = NULL;
    gchar *cwd;
    const gchar *name;

    DBG("%s\n", dir);
    cwd = g_get_current_dir();
    if (g_chdir(dir))
    {
        DBG("can't chdir to %s\n", dir);
        goto out;
    }
    if (!(d = g_dir_open(".", 0, NULL)))
    {
        ERR("can't open dir %s\n", dir);
        goto out;
    }

    while ((name = g_dir_read_name(d)))
    {
        if (g_file_test(name, G_FILE_TEST_IS_DIR))
        {
            do_app_dir_real(ht, name);
            continue;
        }
        if (!g_str_has_suffix(name, ".desktop"))
            continue;
        do_app_file(ht, name);
    }

out:
    if (d)
        g_dir_close(d);
    g_chdir(cwd);
    g_free(cwd);
    return;
}

/**
 * do_app_dir - deduplication wrapper around do_app_dir_real().
 * @ht:  Category/visited hash table.  (transfer none)
 *       The pointer @dir is used as a deduplication sentinel: if @dir is
 *       already a key in @ht the directory was already scanned and we skip it.
 *       On first visit, (@dir -> @ht) is inserted as the sentinel entry.
 * @dir: Top-level XDG data directory to scan.  The actual scan descends into
 *       the "applications" subdirectory.  (transfer none; pointer used as key)
 *
 * This prevents the same data directory being scanned twice when it appears
 * in both system and user XDG data dir lists.
 */
static void
do_app_dir(GHashTable *ht, const gchar *dir)
{
    gchar *cwd;

    cwd = g_get_current_dir();
    DBG("%s\n", dir);
    if (g_hash_table_lookup(ht, dir))
    {
        DBG("already visited\n");
        goto out;
    }
    /* Mark as visited; value is the hash table itself (arbitrary non-NULL sentinel). */
    g_hash_table_insert(ht, (gpointer) dir, ht);
    if (g_chdir(dir))
    {
        ERR("can't chdir to %s\n", dir);
        goto out;
    }
    do_app_dir_real(ht, app_dir_name);

out:
    g_chdir(cwd);
    g_free(cwd);
    return;
}

/**
 * xconf_cmp_names - GCompareFunc for alphabetical sort of xconf "menu" or "item" nodes.
 * @a: First xconf node. (transfer none)
 * @b: Second xconf node. (transfer none)
 *
 * Reads the "name" child of each node via XCG(str) (transfer none).
 * Returns g_strcmp0(name_a, name_b).
 *
 * Used by g_slist_sort() to sort category menus and items alphabetically.
 */
static int
xconf_cmp_names(gpointer a, gpointer b)
{
    xconf *aa = a, *bb = b;
    gchar *s1 = NULL, *s2 = NULL;
    int ret;

    XCG(aa, "name", &s1, str);
    XCG(bb, "name", &s2, str);
    ret = g_strcmp0(s1, s2);
    DBG("cmp %s %s - %d\n", s1, s2, ret);
    return ret;
}

/**
 * dir_changed - recursively check if any .desktop file in @dir is newer than @btime.
 * @dir:   Directory to check; may be relative to the current process cwd. (transfer none)
 * @btime: Build time of the current menu (seconds since epoch).
 *
 * Returns TRUE if:
 *  - @dir's own mtime > @btime, OR
 *  - any .desktop file in @dir (recursively) has mtime > @btime.
 *
 * Returns FALSE if @dir cannot be stat'd (treated as unchanged).
 *
 * The process cwd is saved and restored so callers are unaffected.
 */
static gboolean
dir_changed(const gchar *dir, time_t btime)
{
    GDir *d = NULL;
    gchar *cwd;
    const gchar *name;
    gboolean ret = FALSE;
    struct stat buf;

    DBG("%s\n", dir);
    if (g_stat(dir, &buf))
        return FALSE;
    DBG("dir=%s ct=%lu mt=%lu\n", dir, buf.st_ctime, buf.st_mtime);
    if ((ret = buf.st_mtime > btime))
        return TRUE;

    cwd = g_get_current_dir();
    if (g_chdir(dir))
    {
        DBG("can't chdir to %s\n", dir);
        goto out;
    }
    if (!(d = g_dir_open(".", 0, NULL)))
    {
        ERR("can't open dir %s\n", dir);
        goto out;
    }

    while (!ret && (name = g_dir_read_name(d)))
    {
        if (g_file_test(name, G_FILE_TEST_IS_DIR))
            ret = dir_changed(name, btime);
        else if (!g_str_has_suffix(name, ".desktop"))
            continue;
        else if (g_stat(name, &buf))
            continue;
        DBG("name=%s ct=%lu mt=%lu\n", name, buf.st_ctime, buf.st_mtime);
        ret = buf.st_mtime > btime;
    }
out:
    if (d)
        g_dir_close(d);
    g_chdir(cwd);
    g_free(cwd);
    return ret;
}

/**
 * systemmenu_changed - check if any .desktop file changed since the menu was built.
 * @btime: Timestamp (seconds since epoch) when the current menu was last built
 *         (stored in menu_priv::btime).
 *
 * Iterates all g_get_system_data_dirs() and g_get_user_data_dir(), checking the
 * "applications" subdirectory of each for .desktop file mtime changes via
 * dir_changed().  Stops early on first detected change.
 *
 * Called from check_system_menu() every 30 seconds when has_system_menu is TRUE.
 *
 * Returns: TRUE if any .desktop file or directory is newer than @btime;
 *          FALSE if the menu is up to date.
 */
gboolean
systemmenu_changed(time_t btime)
{
    const gchar * const * dirs;
    gboolean ret = FALSE;
    gchar *cwd = g_get_current_dir();

    for (dirs = g_get_system_data_dirs(); *dirs && !ret; dirs++)
    {
        g_chdir(*dirs);
        ret = dir_changed(app_dir_name, btime);
    }

    DBG("btime=%lu\n", btime);
    if (!ret)
    {
        g_chdir(g_get_user_data_dir());
        ret = dir_changed(app_dir_name, btime);
    }
    g_chdir(cwd);
    g_free(cwd);
    return ret;
}

/**
 * xconf_new_from_systemmenu - build the full XDG application menu as an xconf tree.
 *
 * Creates a "systemmenu" xconf tree by:
 *
 *  1. Allocating one "menu" node per entry in main_cats[] and inserting it into
 *     a GHashTable keyed by the XDG category name string.
 *
 *  2. Scanning all g_get_system_data_dirs() and g_get_user_data_dir() for
 *     .desktop files via do_app_dir(), populating category nodes.
 *
 *  3. Deleting empty category nodes (those with no "item" children).
 *     Uses a goto-retry loop because xconf_del() modifies the parent's
 *     sons GSList while iterating.
 *
 *  4. Sorting category menus and their items alphabetically by "name" child
 *     using xconf_cmp_names().
 *
 * The hash table is destroyed after scanning; its keys are static strings from
 * main_cats[] (no free needed) and its values are xconf* nodes owned by the
 * returned tree (freed by xconf_del on the root).
 *
 * Returns: (transfer full) root "systemmenu" xconf node; caller must call
 *          xconf_del() to free the entire tree.  Never returns NULL.
 */
xconf *
xconf_new_from_systemmenu()
{
    xconf *xc, *mxc, *tmp;
    GSList *w;
    GHashTable *ht;
    int i;
    const gchar * const * dirs;

    /* Create category menus and populate the lookup hash table. */
    ht = g_hash_table_new(g_str_hash, g_str_equal);
    xc = xconf_new("systemmenu", NULL);
    for (i = 0; i < G_N_ELEMENTS(main_cats); i++)
    {
        mxc = xconf_new("menu", NULL);
        xconf_append(xc, mxc);

        tmp = xconf_new("name", _(main_cats[i].local_name));
        xconf_append(mxc, tmp);

        tmp = xconf_new("icon", main_cats[i].icon);
        xconf_append(mxc, tmp);

        /* key: static string (no free); value: xconf* owned by xc tree */
        g_hash_table_insert(ht, main_cats[i].name, mxc);
    }

    /* Scan all XDG data directories for .desktop files. */
    for (dirs = g_get_system_data_dirs(); *dirs; dirs++)
        do_app_dir(ht, *dirs);
    do_app_dir(ht, g_get_user_data_dir());

    /* Delete empty categories.  Uses goto-retry because xconf_del modifies xc->sons
     * in place; restarting the walk is simpler than iterator invalidation handling. */
retry:
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        tmp = w->data;
        if (!xconf_find(tmp, "item", 0))
        {
            xconf_del(tmp, FALSE);
            goto retry;
        }
    }

    /* Sort categories, then sort items within each category. */
    xc->sons = g_slist_sort(xc->sons, (GCompareFunc) xconf_cmp_names);
    for (w = xc->sons; w; w = g_slist_next(w))
    {
        tmp = w->data;
        tmp->sons = g_slist_sort(tmp->sons, (GCompareFunc) xconf_cmp_names);
    }

    g_hash_table_destroy(ht);

    return xc;
}
