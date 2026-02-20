/**
 * @file run.h
 * @brief Application launcher helpers.
 *
 * Provides two thin wrappers around GLib's g_spawn_xxx APIs for launching
 * external applications from fbpanel plugins (launchbar, menu, wincmd, etc.).
 *
 * Both functions display a modal GtkMessageDialog on launch failure.
 * Neither function modifies the panel state or any global variables.
 */

#ifndef _RUN_H_
#define _RUN_H_

#include <gtk/gtk.h>

/**
 * run_app - launch an application from a shell command string.
 * @cmd: Shell command line to execute (passed to g_spawn_command_line_async).
 *       May be NULL; if so the function returns immediately without error.
 *
 * Spawns @cmd asynchronously via g_spawn_command_line_async.  On failure,
 * shows a modal GtkMessageDialog with the GError message.
 *
 * The child process is not tracked; no SIGCHLD handler is installed.  Use
 * run_app_argv() when you need the child PID for tracking.
 */
void run_app(gchar *cmd);

/**
 * run_app_argv - launch an application from an argv array, returning its PID.
 * @argv: NULL-terminated argv array (argv[0] is the program, searched via PATH).
 *        Passed to g_spawn_async with G_SPAWN_DO_NOT_REAP_CHILD so the caller
 *        is responsible for watching and reaping the child via g_child_watch_add
 *        or waitpid when it is done.
 *
 * Spawns @argv[0] asynchronously with stdout redirected to /dev/null.
 * On failure, shows a modal GtkMessageDialog with the GError message and
 * returns 0.
 *
 * Returns: GPid of the spawned child (valid, un-reaped); 0 on failure.
 *          Caller must eventually reap the child (the DO_NOT_REAP_CHILD flag
 *          prevents automatic reaping by GLib).
 */
GPid run_app_argv(gchar **argv);

#endif /* _RUN_H_ */
