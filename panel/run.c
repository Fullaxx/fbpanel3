/**
 * @file run.c
 * @brief Application launcher helpers — implementation.
 *
 * Both functions use GLib's g_spawn_xxx APIs and display a GtkMessageDialog
 * on error.  Neither modifies global panel state.
 */

#include "run.h"

/**
 * run_app - launch an application from a shell command string.
 *
 * Calls g_spawn_command_line_async(@cmd).  On failure, creates a transient
 * GTK_MESSAGE_ERROR dialog, runs it modally, destroys it, and frees the error.
 * If @cmd is NULL the function returns immediately without spawning anything.
 */
void
run_app(gchar *cmd)
{
    GError *error = NULL;

    if (!cmd)
        return;

    if (!g_spawn_command_line_async(cmd, &error))
    {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_error_free(error);
    }
    return;
}


/**
 * run_app_argv - launch an application from an argv array, returning its PID.
 *
 * Spawns @argv[0] via g_spawn_async with:
 *   - working directory: NULL (inherits the panel's cwd)
 *   - envp: NULL (inherits the panel's environment)
 *   - flags: G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH |
 *            G_SPAWN_STDOUT_TO_DEV_NULL
 *   - child_setup: NULL
 *   - user_data: NULL
 *
 * On failure, shows a modal error dialog and returns 0.
 * On success, returns the child GPid; the caller must reap it (the
 * DO_NOT_REAP_CHILD flag prevents GLib from doing so automatically).
 */
GPid
run_app_argv(gchar **argv)
{
    GError *error = NULL;
    GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
    GPid pid;

    flags |= G_SPAWN_STDOUT_TO_DEV_NULL;
    if (!g_spawn_async(NULL, argv, NULL, flags, NULL, NULL, &pid, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_error_free(error);
    }

    return pid;
}
