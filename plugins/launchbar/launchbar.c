/**
 * @file launchbar.c
 * @brief Launchbar plugin — row of application launcher buttons.
 *
 * Displays a configurable row (or column) of icon buttons on the panel.
 * Each button runs a command when clicked and supports drag-and-drop of
 * URIs and URLs from browsers and file managers.
 *
 * CONFIGURATION (in the plugin's xconf tree)
 * ------------------------------------------
 * Each "button" child node may contain:
 *   image   — absolute path to an image file (expanded through expand_tilda)
 *   icon    — GTK icon theme name (looked up at run time)
 *   action  — shell command to run on click (expanded through expand_tilda)
 *   tooltip — Pango markup tooltip text
 *
 * Up to MAXBUTTONS (40) buttons may be configured.
 *
 * BUTTON PRIVATE DATA (struct btn)
 * ---------------------------------
 * - lb:     back-pointer to the owning launchbar_priv
 * - action: (transfer full, g_free'd in launchbar_destructor)
 *           Result of expand_tilda() on the "action" xconf value.
 *
 * DRAG AND DROP
 * -------------
 * Each button accepts drops of text/uri-list and text/x-moz-url.
 *
 * text/uri-list: whitespace-separated list of URIs.  Each URI is converted
 *   to a filesystem path via g_filename_from_uri(); on failure the raw URI
 *   is used.  The action command is called once with all paths appended as
 *   single-quoted arguments.
 *
 * text/x-moz-url: Firefox/Mozilla URL format — UTF-16 string with URL and
 *   title separated by '\n'.  Only the URL (before the '\n') is passed to
 *   the action.
 *
 * CTRL+RMB HANDLING
 * -----------------
 * Ctrl+Right-click opens the panel's configuration dialog.  The press event
 * is passed through (returns FALSE) so the panel framework handles it.  The
 * subsequent release event is then discarded via the discard_release_event
 * bit field to prevent the action command from running on release.
 *
 * ICON SIZE AND LAYOUT
 * --------------------
 * Icon size is set to p->panel->max_elem_height at construction.
 * launchbar_size_alloc() is connected to plug->pwid "size-allocate" and
 * updates the GtkBar dimension (rows/columns) whenever the panel resizes.
 *
 * CSS
 * ---
 * Adds a screen-level CSS rule:
 *   #launchbar button { padding: 0; margin: 0; outline-width: 0; }
 * Applied at GTK_STYLE_PROVIDER_PRIORITY_APPLICATION.  The provider is
 * unreffed after adding (the GtkStyleContext holds its own reference).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>


#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "gtkbgbox.h"
#include "run.h"
#include "gtkbar.h"

//#define DEBUGPRN
#include "dbg.h"


/** Cursor type enum (currently unused; kept for potential future DnD cursor feedback). */
typedef enum {
    CURSOR_STANDARD,
    CURSOR_DND
} CursorType;

/**
 * DnD target info values.
 * TARGET_URILIST and TARGET_MOZ_URL are handled in drag_data_received_cb();
 * the others (UTF8_STRING, COMPOUND_TEXT, TEXT, STRING) are accepted but
 * their data is ignored.
 */
enum {
    TARGET_URILIST,
    TARGET_MOZ_URL,
    TARGET_UTF8_STRING,
    TARGET_STRING,
    TARGET_TEXT,
    TARGET_COMPOUND_TEXT
};

/** Accepted drag-and-drop targets for launchbar buttons. */
static const GtkTargetEntry target_table[] = {
    { "text/uri-list", 0, TARGET_URILIST},
    { "text/x-moz-url", 0, TARGET_MOZ_URL},
    { "UTF8_STRING", 0, TARGET_UTF8_STRING },
    { "COMPOUND_TEXT", 0, 0 },
    { "TEXT",          0, 0 },
    { "STRING",        0, 0 }
};

struct launchbarb;

/**
 * btn - per-button private data for one launchbar button.
 *
 * Stored in a fixed-size array (launchbar_priv::btns[]).
 */
typedef struct btn {
    struct launchbar_priv *lb;  /**< Back-pointer to owning launchbar instance. */
    gchar *action;              /**< Shell command to run on click.
                                 *   (transfer full) result of expand_tilda();
                                 *   g_free'd in launchbar_destructor. */
} btn;

/** Maximum number of buttons in a launchbar instance. */
#define MAXBUTTONS 40

/**
 * launchbar_priv - launchbar plugin private state.
 *
 * Embedded as the first field of the plugin allocation so that
 * plugin_instance* and launchbar_priv* are interchangeable.
 */
typedef struct launchbar_priv {
    plugin_instance plugin;              /**< Must be first; plugin framework accesses this. */
    GtkWidget *box;                      /**< GtkBar containing all buttons;
                                          *   owned by plug->pwid GTK container. */
    btn btns[MAXBUTTONS];                /**< Flat array of button state; [0..btn_num-1] valid. */
    int btn_num;                         /**< Number of buttons currently configured. */
    int iconsize;                        /**< Icon size in pixels; set to max_elem_height. */
    unsigned int discard_release_event:1;/**< TRUE: next button-release-event should be discarded.
                                          *   Set by Ctrl+RMB press; cleared on next release. */
} launchbar_priv;


/**
 * my_button_pressed - "button-press-event" and "button-release-event" handler.
 * @widget: The button GtkWidget. (transfer none)
 * @event:  The button event. (transfer none)
 * @b:      The btn state for this button. (transfer none)
 *
 * Ctrl+RMB press: sets discard_release_event and returns FALSE so the panel
 *   framework can open its configuration dialog.
 *
 * Release with discard_release_event set: clears the flag, returns TRUE
 *   (event consumed; prevents action execution after Ctrl+RMB).
 *
 * Normal button-release: if the pointer is within the button allocation,
 *   calls run_app(b->action) to launch the configured command.
 *   Returns TRUE (event consumed).
 *
 * Press (non-Ctrl+RMB): returns TRUE (consumed; waits for release).
 */
static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, btn *b )
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
        && event->state & GDK_CONTROL_MASK)
    {
        b->lb->discard_release_event = 1;
        return FALSE;
    }
    if (event->type == GDK_BUTTON_RELEASE && b->lb->discard_release_event)
    {
        b->lb->discard_release_event = 0;
        return TRUE;
    }
    g_assert(b != NULL);
    if (event->type == GDK_BUTTON_RELEASE)
    {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        if ((event->x >=0 && event->x < alloc.width)
            && (event->y >=0 && event->y < alloc.height))
        {
            run_app(b->action);
        }
    }
    return TRUE;
}

/**
 * launchbar_destructor - plugin destructor; frees all button resources.
 * @p: Plugin instance (also usable as launchbar_priv*). (transfer none)
 *
 * Destroys lb->box (which also destroys all button widgets it contains,
 * since GTK parent-owns-children), then g_free's each btn::action string.
 */
static void
launchbar_destructor(plugin_instance *p)
{
    launchbar_priv *lb = (launchbar_priv *) p;
    int i;

    gtk_widget_destroy(lb->box);
    for (i = 0; i < lb->btn_num; i++)
        g_free(lb->btns[i].action);

    return;
}


/**
 * drag_data_received_cb - GtkWidget "drag_data_received" handler.
 * @widget:  The button receiving the drop. (transfer none)
 * @context: Drag context. (transfer none)
 * @x:       Drop X position (relative to widget; unused).
 * @y:       Drop Y position (relative to widget; unused).
 * @sd:      Selection data containing the dropped content. (transfer none)
 * @info:    Target info value (TARGET_URILIST or TARGET_MOZ_URL).
 * @time:    Event timestamp.
 * @b:       The btn state for the receiving button. (transfer none)
 *
 * TARGET_URILIST: Tokenises the URI list on whitespace.  Each URI is
 *   converted to a filesystem path via g_filename_from_uri(); on failure,
 *   the raw URI string is used as-is.  Builds a command string by appending
 *   each path as a single-quoted argument to b->action, then launches it
 *   via g_spawn_command_line_async() (errors silently ignored).
 *
 * TARGET_MOZ_URL: Converts UTF-16 data to UTF-8.  The format is
 *   "URL\nTitle"; only the URL (before the '\n') is appended to b->action
 *   and launched.
 *
 * Other info values: silently ignored (handler returns early).
 *
 * Note: uses g_spawn_command_line_async (not run_app) to avoid showing an
 * error dialog on drag-and-drop failures.
 */
static void
drag_data_received_cb (GtkWidget *widget,
    GdkDragContext *context,
    gint x,
    gint y,
    GtkSelectionData *sd,
    guint info,
    guint time,
    btn *b)
{
    gchar *s, *str, *tmp, *tok, *tok2;

    if (gtk_selection_data_get_length(sd) <= 0)
        return;
    DBG("uri drag received: info=%d/%s len=%d data=%s\n",
         info, target_table[info].target, gtk_selection_data_get_length(sd),
         gtk_selection_data_get_data(sd));
    if (info == TARGET_URILIST)
    {
        /* white-space separated list of uri's */
        s = g_strdup((gchar *)gtk_selection_data_get_data(sd));
        str = g_strdup(b->action);
        for (tok = strtok(s, "\n \t\r"); tok; tok = strtok(NULL, "\n \t\r"))
        {
            tok2 = g_filename_from_uri(tok, NULL, NULL);
            /* if conversion to filename was ok, use it, otherwise
             * lets append original uri */
            tmp = g_strdup_printf("%s '%s'", str, tok2 ? tok2 : tok);
            g_free(str);
            g_free(tok2);
            str = tmp;
        }
        DBG("cmd=<%s>\n", str);
        g_spawn_command_line_async(str, NULL);
        g_free(str);
        g_free(s);
    }
    else if (info == TARGET_MOZ_URL)
    {
        gchar *utf8, *tmp;

	utf8 = g_utf16_to_utf8((gunichar2 *) gtk_selection_data_get_data(sd), (glong) gtk_selection_data_get_length(sd),
              NULL, NULL, NULL);
        tmp = utf8 ? strchr(utf8, '\n') : NULL;
	if (!tmp)
        {
            ERR("Invalid UTF16 from text/x-moz-url target");
            g_free(utf8);
            gtk_drag_finish(context, FALSE, FALSE, time);
            return;
	}
	*tmp = '\0';
        tmp = g_strdup_printf("%s %s", b->action, utf8);
        g_spawn_command_line_async(tmp, NULL);
        DBG("%s %s\n", b->action, utf8);
        g_free(utf8);
        g_free(tmp);
    }
    return;
}

/**
 * read_button - parse one "button" xconf node and create its GtkWidget.
 * @p:  Plugin instance (also usable as launchbar_priv*). (transfer none)
 * @xc: The "button" xconf node to read config from. (transfer none)
 *
 * Reads config keys from @xc (all transfer none via XCG str):
 *   "image"   -> fname (expanded via expand_tilda, freed after button creation)
 *   "icon"    -> iname (raw pointer, passed to fb_button_new; NOT freed)
 *   "action"  -> action (expanded via expand_tilda, stored as btn::action)
 *   "tooltip" -> tooltip (raw pointer, passed to gtk_widget_set_tooltip_markup; NOT freed)
 *
 * Creates a fb_button_new widget with the icon/image and icon size.
 * Connects "button-release-event" and "button-press-event" to my_button_pressed.
 * Sets up DnD destinations for all target_table entries.
 * Packs the button into lb->box; sets transparent background if transparent panel.
 *
 * Returns: 1 on success; 0 if MAXBUTTONS limit would be exceeded.
 *
 * Note: btn_num is incremented and action is stored in lb->btns[btn_num].
 */
static int
read_button(plugin_instance *p, xconf *xc)
{
    launchbar_priv *lb = (launchbar_priv *) p;
    gchar *iname, *fname, *tooltip, *action;
    GtkWidget *button;

    if (lb->btn_num >= MAXBUTTONS)
    {
        ERR("launchbar: max number of buttons (%d) was reached."
            "skipping the rest\n", lb->btn_num );
        return 0;
    }
    iname = tooltip = fname = action = NULL;
    XCG(xc, "image", &fname, str);     /* transfer none: raw pointer into xconf */
    XCG(xc, "icon",  &iname, str);     /* transfer none: raw pointer into xconf */
    XCG(xc, "action", &action, str);   /* transfer none: raw pointer into xconf */
    XCG(xc, "tooltip", &tooltip, str); /* transfer none: raw pointer into xconf */

    /* expand_tilda returns a newly g_strdup'd string (transfer full) */
    action = expand_tilda(action);
    fname  = expand_tilda(fname);

    button = fb_button_new(iname, fname, lb->iconsize,
        lb->iconsize, 0x202020);

    g_signal_connect (G_OBJECT (button), "button-release-event",
          G_CALLBACK (my_button_pressed), (gpointer) &lb->btns[lb->btn_num]);
    g_signal_connect (G_OBJECT (button), "button-press-event",
          G_CALLBACK (my_button_pressed), (gpointer) &lb->btns[lb->btn_num]);

    gtk_widget_set_can_focus(button, FALSE);
    /* DnD support: accept URI lists and Mozilla URLs */
    gtk_drag_dest_set (GTK_WIDGET(button),
        GTK_DEST_DEFAULT_ALL,
        target_table, G_N_ELEMENTS (target_table),
        GDK_ACTION_COPY);
    g_signal_connect (G_OBJECT(button), "drag_data_received",
        G_CALLBACK (drag_data_received_cb),
        (gpointer) &lb->btns[lb->btn_num]);

    gtk_box_pack_start(GTK_BOX(lb->box), button, FALSE, FALSE, 0);
    gtk_widget_show(button);

    if (p->panel->transparent)
        gtk_bgbox_set_background(button, BG_INHERIT,
            p->panel->tintcolor, p->panel->alpha);
    /* tooltip is transfer none (raw xconf pointer); gtk copies it */
    gtk_widget_set_tooltip_markup(button, tooltip);

    g_free(fname);   /* free the expand_tilda copy (iname is transfer none; not freed) */
    DBG("here\n");

    /* Store the expand_tilda'd action string (transfer full; freed in destructor) */
    lb->btns[lb->btn_num].action = action;
    lb->btns[lb->btn_num].lb     = lb;
    lb->btn_num++;

    return 1;
}

/**
 * launchbar_size_alloc - "size-allocate" signal handler for the plugin's pwid.
 * @widget: The plugin's pwid GtkBgbox. (transfer none)
 * @a:      New allocation. (transfer none)
 * @lb:     Launchbar plugin instance. (transfer none)
 *
 * Computes how many icon slots of size lb->iconsize fit in the current
 * allocation dimension (height for horizontal panels, width for vertical),
 * and updates the GtkBar dimension for correct wrapping into rows or columns.
 */
static void
launchbar_size_alloc(GtkWidget *widget, GtkAllocation *a,
    launchbar_priv *lb)
{
    int dim;

    if (lb->plugin.panel->orientation == GTK_ORIENTATION_HORIZONTAL)
        dim = a->height / lb->iconsize;
    else
        dim = a->width / lb->iconsize;
    DBG("width=%d height=%d iconsize=%d -> dim=%d\n",
        a->width, a->height, lb->iconsize, dim);
    gtk_bar_set_dimension(GTK_BAR(lb->box), dim);
    return;
}

/**
 * launchbar_constructor - plugin constructor; creates the launchbar widget.
 * @p: Plugin instance (also usable as launchbar_priv*). (transfer none)
 *
 * Initialisation sequence:
 *  1. Set lb->iconsize = panel->max_elem_height.
 *  2. Name the pwid "launchbar" for CSS targeting.
 *  3. Load per-screen CSS rule removing button padding/margin/outline.
 *  4. Connect "size-allocate" on plug->pwid to launchbar_size_alloc.
 *  5. Create a GtkBar (horizontal or vertical, spacing=0, row/col size =
 *     iconsize), centre-align it, add to plug->pwid.
 *  6. Iterate all "button" xconf child nodes in order; call read_button()
 *     for each.
 *
 * Returns: 1 on success.
 */
static int
launchbar_constructor(plugin_instance *p)
{
    launchbar_priv *lb;
    int i;
    xconf *pxc;
    static const gchar *launchbar_css =
        "#launchbar button { padding: 0; margin: 0; outline-width: 0; }";

    lb = (launchbar_priv *) p;
    lb->iconsize = p->panel->max_elem_height;
    DBG("iconsize=%d\n", lb->iconsize);

    gtk_widget_set_name(p->pwid, "launchbar");
    {
        /* Install CSS at application priority; unreffed after adding
         * (GtkStyleContext holds the reference). */
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, launchbar_css, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    g_signal_connect(G_OBJECT(p->pwid), "size-allocate",
        (GCallback) launchbar_size_alloc, lb);
    lb->box = gtk_bar_new(p->panel->orientation, 0,
        lb->iconsize, lb->iconsize);
    gtk_widget_set_halign(lb->box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(lb->box, GTK_ALIGN_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(lb->box), 0);
    gtk_container_add(GTK_CONTAINER(p->pwid), lb->box);
    gtk_widget_show_all(lb->box);

    /* Read all "button" child nodes and create a button for each */
    for (i = 0; (pxc = xconf_find(p->xc, "button", i)); i++)
        read_button(p, pxc);
    return 1;
}

static plugin_class class = {
    .count       = 0,
    .type        = "launchbar",
    .name        = "Launchbar",
    .version     = "1.0",
    .description = "Bar with application launchers",
    .priv_size   = sizeof(launchbar_priv),

    .constructor = launchbar_constructor,
    .destructor  = launchbar_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
