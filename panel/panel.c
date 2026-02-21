/**
 * @file panel.c
 * @brief fbpanel core -- startup, event dispatch, autohide, main loop.
 *
 * STARTUP SEQUENCE
 * ----------------
 * main()
 *   -> gtk_init(), fb_init() (atoms + icon_theme)
 *   -> do_argv() (parse command-line flags)
 *   -> ensure_profile() (create ~/.config/fbpanel/<profile> if absent)
 *   -> loop {
 *        p = g_new0(panel, 1)
 *        p->xc = xconf_new_from_file(profile_file, profile)
 *        panel_start(p->xc) {
 *          fbev = fb_ev_new()
 *          panel_parse_global() -> panel_start_gui()  [GTK hierarchy created]
 *          panel_parse_plugin() for each "plugin" xconf block
 *          g_timeout_add(200, panel_show_anyway)      [deferred show]
 *        }
 *        [optional: configure() if --configure flag set]
 *        gtk_main()          <- event loop
 *        panel_stop(p)       <- teardown
 *        xconf_del(p->xc, FALSE)
 *        g_free(p)
 *      } while (force_quit == 0)
 *   -> fb_free(); exit(0)
 *
 * RESTART (SIGUSR1)
 * -----------------
 * sig_usr1 calls gtk_main_quit() with force_quit still 0.
 * The loop restarts: panel_stop tears everything down and panel_start
 * re-reads the config and rebuilds the panel (hot-reload).
 *
 * SHUTDOWN (SIGUSR2 / destroy event)
 * -----------------------------------
 * sig_usr2 / panel_destroy_event set force_quit = 1 before calling
 * gtk_main_quit().  The loop exits and the process terminates cleanly.
 *
 * EWMH EVENT DISPATCH
 * -------------------
 * panel_event_filter() is installed as a GdkFilterFunc on the root window.
 * It receives all PropertyNotify events on the root window and:
 *   - translates EWMH property changes to FbEv signals (fb_ev_trigger)
 *   - updates p->curdesk / p->desknum caches
 *   - calls fb_bg_notify_changed_bg() when _XROOTPMAP_ID changes
 *   - calls gtk_main_quit() when _NET_DESKTOP_GEOMETRY changes (triggers restart)
 *
 * AUTOHIDE STATE MACHINE
 * ----------------------
 * Three states driven by mouse position (queried every PERIOD ms):
 *   ah_state_visible  -> mouse far -> ah_state_waiting
 *   ah_state_waiting  -> timer expires -> ah_state_hidden
 *                     -> mouse close -> ah_state_visible
 *   ah_state_hidden   -> mouse close -> ah_state_visible
 *
 * The active state is stored as a function pointer in p->ah_state.
 *
 * See also: docs/ARCHITECTURE.md, docs/MEMORY_MODEL.md sec.2.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "plugin.h"
#include "panel.h"
#include "misc.h"
#include "bg.h"
#include "gtkbgbox.h"


/** Project version string (substituted by CMake from PROJECT_VERSION). */
static gchar version[] = PROJECT_VERSION;

/** Active profile name; defaults to "default"; may be overridden by --profile. */
static gchar *profile = "default";

/** Full path to the profile config file; built in main() from profile name. */
static gchar *profile_file;

/* -------------------------------------------------------------------------
 * GtkBox / GtkSeparator factory wrappers
 * The panel stores function pointers (my_box_new, my_separator_new) so that
 * the same code creates horizontal or vertical layouts depending on edge.
 * ------------------------------------------------------------------------- */

/**
 * hbox_new - create a horizontal GtkBox (panel->my_box_new for top/bottom panels).
 * @o:       Ignored (orientation always forced to HORIZONTAL).
 * @spacing: Pixel gap between children.
 */
static GtkWidget *hbox_new(GtkOrientation o, gint spacing) {
    (void)o;
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}

/**
 * vbox_new - create a vertical GtkBox (panel->my_box_new for left/right panels).
 * @o:       Ignored (orientation always forced to VERTICAL).
 * @spacing: Pixel gap between children.
 */
static GtkWidget *vbox_new(GtkOrientation o, gint spacing) {
    (void)o;
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

/**
 * vseparator_new - create a vertical GtkSeparator (used between plugins in hbox).
 */
static GtkWidget *vseparator_new(void) {
    return gtk_separator_new(GTK_ORIENTATION_VERTICAL);
}

/**
 * hseparator_new - create a horizontal GtkSeparator (used between plugins in vbox).
 */
static GtkWidget *hseparator_new(void) {
    return gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
}

/** Mouse-watcher timer ID (g_timeout_add every PERIOD ms); removed by ah_stop(). */
guint mwid;
/** Hide-delay timer ID (g_timeout_add 2*PERIOD for WAITING->HIDDEN transition). */
guint hpid;


/** FbEv EWMH event singleton; created in panel_start(), unref'd in panel_stop(). */
FbEv *fbev;
/** Non-zero when the process should exit after panel_stop (set by SIGUSR2 or destroy event). */
gint force_quit = 0;
/** Non-zero when --configure was passed; causes configure() to run before gtk_main(). */
int config;
/** Xinerama head from --xineramaHead flag; stored in p->xineramaHead at start of each loop. */
int xineramaHead = FBPANEL_INVALID_XINERAMA_HEAD;

//#define DEBUGPRN
#include "dbg.h"

/** Log verbosity level; 0 = silent, higher = more verbose. Controlled by --log. */
int log_level = LOG_WARN;

/** The active panel instance; alias for 'p' accessible to external modules. */
static panel *p;
panel *the_panel;

/**
 * panel_set_wm_strut - set _NET_WM_STRUT_PARTIAL and _NET_WM_STRUT on topgwin.
 * @p: Panel instance.
 *
 * Computes a 12-element strut array for _NET_WM_STRUT_PARTIAL (and the
 * first 4 elements for the legacy _NET_WM_STRUT).  The strut reserves screen
 * space along p->edge so maximised windows do not overlap the panel.
 *
 * No-ops if:
 *   - The panel window is not yet mapped (WMs typically ignore struts for
 *     unmapped windows, which is how autohide hides itself)
 *   - p->autohide is set (autohiding panels retract to height_when_hidden,
 *     so a full strut would be wrong)
 *
 * The strut extent is p->aw + p->ymargin (left/right) or p->ah + p->ymargin
 * (top/bottom), covering the panel's full width/height in the perpendicular
 * direction (ax to ax+aw or ay to ay+ah).
 */
void
panel_set_wm_strut(panel *p)
{
    gulong data[12] = { 0 };
    int i = 4;

    if (!gtk_widget_get_mapped(p->topgwin))
        return;
    /* most wm's tend to ignore struts of unmapped windows, and that's how
     * fbpanel hides itself. so no reason to set it. */
    if (p->autohide)
        return;
    switch (p->edge) {
    case EDGE_LEFT:
        i = 0;
        data[i] = p->aw + p->ymargin;
        data[4 + i*2] = p->ay;
        data[5 + i*2] = p->ay + p->ah;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_RIGHT:
        i = 1;
        data[i] = p->aw + p->ymargin;
        data[4 + i*2] = p->ay;
        data[5 + i*2] = p->ay + p->ah;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_TOP:
        i = 2;
        data[i] = p->ah + p->ymargin;
        data[4 + i*2] = p->ax;
        data[5 + i*2] = p->ax + p->aw;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    case EDGE_BOTTOM:
        i = 3;
        data[i] = p->ah + p->ymargin;
        data[4 + i*2] = p->ax;
        data[5 + i*2] = p->ax + p->aw;
        if (p->autohide) data[i] = p->height_when_hidden;
        break;
    default:
        ERR("wrong edge %d. strut won't be set\n", p->edge);
        return;
    }
    DBG("type %d. width %ld. from %ld to %ld\n", i, data[i], data[4 + i*2],
          data[5 + i*2]);

    /* if wm supports STRUT_PARTIAL it will ignore STRUT */
    XChangeProperty(GDK_DPY, p->topxwin, a_NET_WM_STRUT_PARTIAL,
        XA_CARDINAL, 32, PropModeReplace,  (unsigned char *) data, 12);
    /* old spec, for wms that do not support STRUT_PARTIAL */
    XChangeProperty(GDK_DPY, p->topxwin, a_NET_WM_STRUT,
        XA_CARDINAL, 32, PropModeReplace,  (unsigned char *) data, 4);

    return;
}

/**
 * panel_event_filter - GdkFilterFunc for PropertyNotify events on the root window.
 * @xevent: Raw XEvent from GDK.
 * @event:  Unused GdkEvent wrapper.
 * @p:      Panel instance (passed as user_data).
 *
 * Installed via gdk_window_add_filter() in panel_start_gui().
 * Removed via gdk_window_remove_filter() in panel_stop().
 *
 * Handles PropertyNotify on the root window:
 *   _NET_CLIENT_LIST            -> fb_ev_trigger(EV_CLIENT_LIST)
 *   _NET_CURRENT_DESKTOP        -> update p->curdesk, trigger EV_CURRENT_DESKTOP
 *   _NET_NUMBER_OF_DESKTOPS     -> update p->desknum, trigger EV_NUMBER_OF_DESKTOPS
 *   _NET_DESKTOP_NAMES          -> trigger EV_DESKTOP_NAMES
 *   _NET_ACTIVE_WINDOW          -> trigger EV_ACTIVE_WINDOW
 *   _NET_CLIENT_LIST_STACKING   -> trigger EV_CLIENT_LIST_STACKING
 *   _XROOTPMAP_ID               -> notify FbBg of wallpaper change (if transparent)
 *   _NET_DESKTOP_GEOMETRY       -> call gtk_main_quit() to restart panel (screen resize)
 *
 * Non-root PropertyNotify events and all other event types return GDK_FILTER_CONTINUE.
 * Handled root events return GDK_FILTER_REMOVE.
 *
 * Returns: GDK_FILTER_REMOVE for handled events, GDK_FILTER_CONTINUE otherwise.
 */
static GdkFilterReturn
panel_event_filter(GdkXEvent *xevent, GdkEvent *event, panel *p)
{
    Atom at;
    Window win;
    XEvent *ev = (XEvent *) xevent;

    DBG("win = 0x%lx\n", ev->xproperty.window);
    if (ev->type != PropertyNotify )
        return GDK_FILTER_CONTINUE;

    at = ev->xproperty.atom;
    win = ev->xproperty.window;
    DBG("win=%lx at=%ld\n", win, at);
    if (win == GDK_ROOT_WINDOW()) {
        if (at == a_NET_CLIENT_LIST) {
            DBG("A_NET_CLIENT_LIST\n");
            fb_ev_trigger(fbev, EV_CLIENT_LIST);
        } else if (at == a_NET_CURRENT_DESKTOP) {
            DBG("A_NET_CURRENT_DESKTOP\n");
            p->curdesk = get_net_current_desktop();
            fb_ev_trigger(fbev, EV_CURRENT_DESKTOP);
        } else if (at == a_NET_NUMBER_OF_DESKTOPS) {
            DBG("A_NET_NUMBER_OF_DESKTOPS\n");
            p->desknum = get_net_number_of_desktops();
            fb_ev_trigger(fbev, EV_NUMBER_OF_DESKTOPS);
        } else if (at == a_NET_DESKTOP_NAMES) {
            DBG("A_NET_DESKTOP_NAMES\n");
            fb_ev_trigger(fbev, EV_DESKTOP_NAMES);
        } else if (at == a_NET_ACTIVE_WINDOW) {
            DBG("A_NET_ACTIVE_WINDOW\n");
            fb_ev_trigger(fbev, EV_ACTIVE_WINDOW);
        }else if (at == a_NET_CLIENT_LIST_STACKING) {
            DBG("A_NET_CLIENT_LIST_STACKING\n");
            fb_ev_trigger(fbev, EV_CLIENT_LIST_STACKING);
        } else if (at == a_NET_WORKAREA) {
            DBG("A_NET_WORKAREA\n");
            //p->workarea = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_WORKAREA,
            //      XA_CARDINAL, &p->wa_len);
            //print_wmdata(p);
        } else if (at == a_XROOTPMAP_ID) {
            if (p->transparent)
                fb_bg_notify_changed_bg(p->bg);
        } else if (at == a_NET_DESKTOP_GEOMETRY) {
            DBG("a_NET_DESKTOP_GEOMETRY\n");
            gtk_main_quit();
        } else
            return GDK_FILTER_CONTINUE;
        return GDK_FILTER_REMOVE;
    }
    DBG("non root %lx\n", win);
    return GDK_FILTER_CONTINUE;
}

/****************************************************
 *         panel's handlers for GTK events          *
 ****************************************************/

/**
 * panel_destroy_event - GtkWidget::destroy-event handler for topgwin.
 *
 * Called when the panel window is destroyed by the WM or the system.
 * Sets force_quit=1 so the main loop exits cleanly after panel_stop().
 *
 * Returns: FALSE (allow normal GTK destroy processing to continue).
 */
static gint
panel_destroy_event(GtkWidget * widget, GdkEvent * event, gpointer data)
{
    gtk_main_quit();
    force_quit = 1;
    return FALSE;
}

/**
 * panel_size_alloc - GtkWidget::size-allocate handler for topgwin.
 * @widget: topgwin (unused).
 * @a:      New allocation from GTK layout engine.
 * @p:      Panel instance.
 *
 * When widthtype or heighttype is WIDTH_REQUEST / HEIGHT_REQUEST, the panel
 * tracks its content size.  This callback updates p->width/height from the
 * actual allocation and re-runs calculate_position() to reposition the window.
 *
 * Note: this is the GTK3 replacement for the removed 'size-request' signal.
 */
static void
panel_size_alloc(GtkWidget *widget, GtkAllocation *a, panel *p)
{
    DBG("alloc %d %d\n", a->width, a->height);
    /* GTK3 replacement for the removed 'size-request' signal:
     * when widthtype/heighttype == REQUEST the panel tracks its content.
     * Update p->width/height from the actual allocation then reposition. */
    if (p->widthtype == WIDTH_REQUEST)
        p->width = (p->orientation == GTK_ORIENTATION_HORIZONTAL) ? a->width : a->height;
    if (p->heighttype == HEIGHT_REQUEST)
        p->height = (p->orientation == GTK_ORIENTATION_HORIZONTAL) ? a->height : a->width;
    calculate_position(p);
    gtk_window_move(GTK_WINDOW(p->topgwin), p->ax, p->ay);
}


/**
 * make_round_corners - apply a rounded-rectangle shape mask to topgwin.
 * @p: Panel instance; reads p->aw, p->ah, p->round_corners_radius.
 *
 * Creates a CAIRO_FORMAT_A1 surface, paints a filled rounded rectangle with
 * radius r (clamped to MIN(w,h)/2 if too large; skipped if r < 4), then
 * converts it to a cairo_region_t and passes it to gtk_widget_shape_combine_region.
 *
 * All cairo resources (surface, context, region) are created and destroyed
 * locally -- no ownership is transferred to the caller.
 */
static void
make_round_corners(panel *p)
{
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_region_t *region;
    int w, h, r, br;

    w = p->aw;
    h = p->ah;
    r = p->round_corners_radius;
    if (2*r > MIN(w, h)) {
        r = MIN(w, h) / 2;
        DBG("chaning radius to %d\n", r);
    }
    if (r < 4) {
        DBG("radius too small\n");
        return;
    }
    br = 2 * r;

    surface = cairo_image_surface_create(CAIRO_FORMAT_A1, w, h);
    cr = cairo_create(surface);

    /* Fill black (transparent) */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    /* Draw white (opaque) rounded rect */
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, r, 0);
    cairo_line_to(cr, w - r, 0);
    cairo_arc(cr, w - r, r, r, -G_PI/2, 0);
    cairo_line_to(cr, w, h - r);
    cairo_arc(cr, w - r, h - r, r, 0, G_PI/2);
    cairo_line_to(cr, r, h);
    cairo_arc(cr, r, h - r, r, G_PI/2, G_PI);
    cairo_line_to(cr, 0, r);
    cairo_arc(cr, r, r, r, G_PI, 3*G_PI/2);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_destroy(cr);
    region = gdk_cairo_region_create_from_surface(surface);
    gtk_widget_shape_combine_region(p->topgwin, region);
    cairo_region_destroy(region);
    cairo_surface_destroy(surface);

    return;
}

/**
 * panel_screen_changed - GdkScreen::monitors-changed handler.
 * @screen: The GdkScreen that changed (unused; geometry read via GDK monitor API).
 * @p:      Panel instance.
 *
 * Called when the screen layout changes (xrandr, monitor hot-plug/unplug).
 * Recalculates panel geometry, refreshes the minimum-height constraint so
 * GTK's own monitors-changed handler (which fires before ours) cannot shrink
 * the window below p->ah, then immediately moves and resizes topgwin.
 *
 * Connected in panel_start_gui() as:
 *   g_signal_connect(gtk_widget_get_screen(p->topgwin), "monitors-changed", ...)
 * Disconnected in panel_stop() using p->monitors_sid.
 *
 * This is the fast-path screen-resize handler for bare-X environments (no WM
 * or WMs that do not send _NET_DESKTOP_GEOMETRY).  WM environments additionally
 * trigger a full panel_stop/panel_start cycle via the PropertyNotify handler
 * when _NET_DESKTOP_GEOMETRY changes.  See docs/ARCHITECTURE.md sec.6.
 */
static void
panel_screen_changed(GdkScreen *screen, panel *p)
{
    (void)screen;
    calculate_position(p);
    /* Refresh the minimum-height constraint before queuing any layout so
     * GTK's own monitors-changed handler (which fires before ours and calls
     * gtk_widget_queue_resize) cannot shrink the window below p->ah. */
    gtk_widget_set_size_request(p->topgwin, -1, p->ah);
    gtk_window_move(GTK_WINDOW(p->topgwin), p->ax, p->ay);
    gtk_window_resize(GTK_WINDOW(p->topgwin), p->aw, p->ah);
    if (p->setstrut)
        panel_set_wm_strut(p);
}

/**
 * panel_configure_event - GtkWidget::configure-event handler for topgwin.
 * @widget: topgwin.
 * @e:      Configure event with actual geometry.
 * @p:      Panel instance.
 *
 * Tracks the actual window geometry in p->cx/cy/cw/ch.  Because WMs may
 * adjust the window position (gravity, borders), this handler re-issues
 * gtk_window_move() if the position does not match p->ax/ay.
 *
 * Once the window is at the right place and size:
 *   - Notifies FbBg of the new position (if transparent)
 *   - Sets the WM strut (if setstrut)
 *   - Applies the rounded-corner shape mask (if round_corners)
 *   - Shows the window (gtk_widget_show)
 *
 * Returns: FALSE (do not suppress further event processing).
 */
static gboolean
panel_configure_event(GtkWidget *widget, GdkEventConfigure *e, panel *p)
{
    DBG("cur geom: %dx%d+%d+%d\n", e->width, e->height, e->x, e->y);
    DBG("req geom: %dx%d+%d+%d\n", p->aw, p->ah, p->ax, p->ay);
    if (e->width == p->cw && e->height == p->ch && e->x == p->cx && e->y ==
            p->cy) {
        DBG("dup. exiting\n");
        return FALSE;
    }
    /* save current geometry */
    p->cw = e->width;
    p->ch = e->height;
    p->cx = e->x;
    p->cy = e->y;

    /* if panel size is not what we have requested, just wait, it will */
    if (e->width != p->aw || e->height != p->ah) {
        DBG("size_req not yet ready. exiting\n");
        return FALSE;
    }

    /* if panel wasn't at requested position, then send another request */
    if (e->x != p->ax || e->y != p->ay) {
        DBG("move %d,%d\n", p->ax, p->ay);
        gtk_window_move(GTK_WINDOW(widget), p->ax, p->ay);
        return FALSE;
    }

    /* panel is at right place, lets go on */
    DBG("panel is at right place, lets go on\n");
    if (p->transparent) {
        DBG("remake bg image\n");
        fb_bg_notify_changed_bg(p->bg);
    }
    if (p->setstrut) {
        DBG("set_wm_strut\n");
        panel_set_wm_strut(p);
    }
    if (p->round_corners) {
        DBG("make_round_corners\n");
        make_round_corners(p);

    }
    gtk_widget_show(p->topgwin);
    if (p->setstrut) {
        DBG("set_wm_strut\n");
        panel_set_wm_strut(p);
    }
    return FALSE;

}

/****************************************************
 *         autohide                                 *
 ****************************************************/

/* Autohide is behaviour when panel hides itself when mouse is "far enough"
 * and pops up again when mouse comes "close enough".
 * Formally, it's a state machine with 3 states that driven by mouse
 * coordinates and timer:
 * 1. VISIBLE - ensures that panel is visible. When/if mouse goes "far enough"
 *      switches to WAITING state
 * 2. WAITING - starts timer. If mouse comes "close enough", stops timer and
 *      switches to VISIBLE.  If timer expires, switches to HIDDEN
 * 3. HIDDEN - hides panel. When mouse comes "close enough" switches to VISIBLE
 *
 * Note 1
 * Mouse coordinates are queried every PERIOD milisec
 *
 * Note 2
 * If mouse is less then GAP pixels to panel it's considered to be close,
 * otherwise it's far
 */

/** Autohide proximity threshold in pixels. Mouse within GAP px = "close". */
#define GAP 2
/** Autohide mouse-polling interval in milliseconds. */
#define PERIOD 300

static gboolean ah_state_visible(panel *p);
static gboolean ah_state_waiting(panel *p);
static gboolean ah_state_hidden(panel *p);

/**
 * panel_mapped - GtkWidget::map-event handler for topgwin.
 *
 * Called each time topgwin becomes mapped (visible on screen).  If autohide
 * is configured, restarts the autohide machinery (ah_stop then ah_start) so
 * the mouse watcher timer is always active while the panel is mapped.
 *
 * Returns: FALSE (do not suppress the map event).
 */
static gboolean
panel_mapped(GtkWidget *widget, GdkEvent *event, panel *p)
{
    if (p->autohide) {
        ah_stop(p);
        ah_start(p);
    }
    return FALSE;
}

/**
 * mouse_watch - g_timeout_add callback; polls mouse position and drives autohide.
 * @p: Panel instance.
 *
 * Called every PERIOD ms.  Reads the absolute pointer position via GDK seat API.
 * Sets p->ah_far to non-zero if the pointer is outside the panel's sensitive area:
 *   - In VISIBLE/WAITING states: sensitive area is the full panel extent (p->aw x p->ah).
 *   - In HIDDEN state: sensitive area shrinks to a GAP-pixel strip at the screen edge
 *     so the panel does not interfere with adjacent applications.
 *
 * Then calls p->ah_state(p) to let the state machine transition if needed.
 *
 * Returns: TRUE (keep the timer active).
 */
static gboolean
mouse_watch(panel *p)
{
    gint x, y;

    {
        GdkDisplay *_dpy = gdk_display_get_default();
        GdkDevice  *_ptr = gdk_seat_get_pointer(gdk_display_get_default_seat(_dpy));
        gdk_device_get_position(_ptr, NULL, &x, &y);
    }

/*  Reduce sensitivity area
    p->ah_far = ((x < p->cx - GAP) || (x > p->cx + p->cw + GAP)
        || (y < p->cy - GAP) || (y > p->cy + p->ch + GAP));
*/

    gint cx, cy, cw, ch;

    cx = p->cx;
    cy = p->cy;
    cw = p->aw;
    ch = p->ah;

    /* reduce area which will raise panel so it does not interfere with apps */
    if (p->ah_state == ah_state_hidden) {
        switch (p->edge) {
        case EDGE_LEFT:
            cw = GAP;
            break;
        case EDGE_RIGHT:
            cx = cx + cw - GAP;
            cw = GAP;
            break;
        case EDGE_TOP:
            ch = GAP;
            break;
        case EDGE_BOTTOM:
            cy = cy + ch - GAP;
            ch = GAP;
            break;
       }
    }
    p->ah_far = ((x < cx) || (x > cx + cw) || (y < cy) || (y > cy + ch));

    p->ah_state(p);
    return TRUE;
}

/**
 * ah_state_visible - autohide VISIBLE state handler.
 * @p: Panel instance.
 *
 * On first entry (p->ah_state != ah_state_visible): shows topgwin and sticks
 * it to all desktops.
 * While in this state: if p->ah_far, transitions to WAITING (calls
 * ah_state_waiting directly).
 *
 * Returns: FALSE (unused; function pointer type requires gboolean).
 */
static gboolean
ah_state_visible(panel *p)
{
    if (p->ah_state != ah_state_visible) {
        p->ah_state = ah_state_visible;
        gtk_widget_show(p->topgwin);
        gtk_window_stick(GTK_WINDOW(p->topgwin));
    } else if (p->ah_far) {
        ah_state_waiting(p);
    }
    return FALSE;
}

/**
 * ah_state_waiting - autohide WAITING state handler.
 * @p: Panel instance.
 *
 * On first entry: starts a 2*PERIOD timer.  If the timer fires, ah_state_hidden
 * is called directly by g_timeout_add.
 * While waiting: if mouse returns close (not p->ah_far), cancels the timer and
 * transitions back to VISIBLE.
 *
 * Returns: FALSE (unused; function pointer type requires gboolean).
 */
static gboolean
ah_state_waiting(panel *p)
{
    if (p->ah_state != ah_state_waiting) {
        p->ah_state = ah_state_waiting;
        hpid = g_timeout_add(2 * PERIOD, (GSourceFunc) ah_state_hidden, p);
    } else if (!p->ah_far) {
        g_source_remove(hpid);
        hpid = 0;
        ah_state_visible(p);
    }
    return FALSE;
}

/**
 * ah_state_hidden - autohide HIDDEN state handler; also used as timer callback.
 * @p: Panel instance.
 *
 * On first entry (p->ah_state != ah_state_hidden): hides topgwin.
 * While hidden: if mouse comes close (not p->ah_far), transitions to VISIBLE.
 *
 * Also used directly as a g_timeout_add callback from ah_state_waiting;
 * in that context it is called once when the 2*PERIOD timer expires.
 *
 * Returns: FALSE (removes the timer when used as GSourceFunc).
 */
static gboolean
ah_state_hidden(panel *p)
{
    if (p->ah_state != ah_state_hidden) {
        p->ah_state = ah_state_hidden;
        gtk_widget_hide(p->topgwin);
    } else if (!p->ah_far) {
        ah_state_visible(p);
    }
    return FALSE;
}

/**
 * ah_start - begin autohide behaviour.
 * @p: Panel instance with autohide=1.
 *
 * Starts the mouse-watcher timer (mwid) and immediately enters VISIBLE state.
 */
void
ah_start(panel *p)
{
    mwid = g_timeout_add(PERIOD, (GSourceFunc) mouse_watch, p);
    ah_state_visible(p);
    return;
}

/**
 * ah_stop - cancel all autohide timers.
 * @p: Panel instance.
 *
 * Removes mwid (mouse-watch timer) and hpid (hide-delay timer) if active.
 * Sets both to 0 after removal.  Safe to call when autohide is not running.
 */
void
ah_stop(panel *p)
{
    if (mwid) {
        g_source_remove(mwid);
        mwid = 0;
    }
    if (hpid) {
        g_source_remove(hpid);
        hpid = 0;
    }
    return;
}

/****************************************************
 *         panel creation                           *
 ****************************************************/

/**
 * about - show the GTK About dialog for fbpanel.
 *
 * Called from the panel context menu "About" item.  Uses gtk_show_about_dialog
 * which creates a transient, non-blocking dialog.
 */
void
about()
{
    gchar *authors[] = { "Anatoly Asviyan <aanatoly@users.sf.net>", NULL };

    gtk_show_about_dialog(NULL,
        "authors", authors,
        "comments", "Lightweight GTK+ desktop panel",
        "license", "GPLv2",
        "program-name", PROJECT_NAME,
        "version", PROJECT_VERSION,
        "website", "http://fbpanel.sf.net",
        "logo-icon-name", "logo",
        "translator-credits", _("translator-credits"),
        NULL);
    return;
}

/**
 * panel_make_menu - create the panel right-click context menu.
 * @p: Panel instance.
 *
 * Creates a GtkMenu with three items:
 *   "Preferences" -> configure(p->xc)   [using g_signal_connect_swapped]
 *   separator
 *   "About"       -> about(p)
 *
 * Returns: (transfer full) GtkMenu*; stored in p->menu and destroyed in panel_stop().
 */
static GtkWidget *
panel_make_menu(panel *p)
{
    GtkWidget *mi, *menu;

    menu = gtk_menu_new();

    /* panel's preferences */
    mi = gtk_menu_item_new_with_label(_("Preferences"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect_swapped(G_OBJECT(mi), "activate",
        (GCallback)configure, p->xc);
    gtk_widget_show (mi);

    /* separator */
    mi = gtk_separator_menu_item_new();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    gtk_widget_show (mi);

    /* about */
    mi = gtk_menu_item_new_with_label(_("About"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate",
        (GCallback)about, p);
    gtk_widget_show (mi);

    return menu;
}

/**
 * panel_button_press_event - GtkWidget::button-press-event handler for topgwin.
 * @widget: topgwin.
 * @event:  Button event.
 * @p:      Panel instance.
 *
 * Opens the context menu on Ctrl+right-click.  The menu is popped up at the
 * pointer position using gtk_menu_popup_at_pointer.
 *
 * Returns: TRUE (event consumed) on Ctrl+right-click, FALSE otherwise.
 */
gboolean
panel_button_press_event(GtkWidget *widget, GdkEventButton *event, panel *p)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3
          && event->state & GDK_CONTROL_MASK) {
        DBG("ctrl-btn3\n");
        gtk_menu_popup_at_pointer(GTK_MENU(p->menu), (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

/**
 * panel_scroll_event - GtkWidget::scroll-event handler for topgwin.
 * @widget: topgwin (unused).
 * @event:  Scroll event.
 * @p:      Panel instance.
 *
 * Switches the active desktop on mouse wheel scroll:
 *   UP or LEFT  -> previous desktop (wrapping at 0)
 *   DOWN or RIGHT -> next desktop (wrapping at desknum-1)
 *
 * Sends a _NET_CURRENT_DESKTOP ClientMessage to the root window.
 *
 * Returns: TRUE (event consumed).
 */
static gboolean
panel_scroll_event(GtkWidget *widget, GdkEventScroll *event, panel *p)
{
    int i;

    DBG("scroll direction = %d\n", event->direction);
    i = p->curdesk;
    if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT) {
        i--;
        if (i < 0)
            i = p->desknum - 1;
    } else {
        i++;
        if (i >= p->desknum)
            i = 0;
    }
    Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, i, 0, 0, 0, 0);
    return TRUE;
}


/**
 * panel_start_gui - create the full GTK widget hierarchy for the panel.
 * @p: Panel instance with all config fields already set by panel_parse_global().
 *
 * Construction sequence:
 *   1. Create topgwin (GtkWindow, DOCK hint, no decorations, no focus).
 *   2. Realize topgwin to get p->topxwin; connect GdkScreen::monitors-changed.
 *   3. Move window to a temporary position (20,20) to force a configure event.
 *   4. Calculate final position and set gtk_widget_set_size_request(-1, p->ah)
 *      as a hard minimum-height floor (v8.3.24 fix).
 *   5. Create bbox (GtkBgbox "panel-bg"); add to topgwin.
 *      If transparent: add CSS fallback provider, acquire FbBg, set BG_ROOT.
 *   6. Create lbox (alignment GtkBox); add to bbox.
 *   7. Create box (plugin container GtkBox); pack into lbox.
 *   8. gtk_widget_show_all then hide topgwin (hidden until configure-event confirms position).
 *   9. Create context menu (p->menu).
 *  10. Set WM strut if p->setstrut.
 *  11. XSelectInput(PropertyChangeMask) on root; install panel_event_filter.
 *
 * Note: the window is initially hidden (gtk_widget_hide) after show_all.  The
 * configure-event handler re-shows it once the WM confirms the correct position.
 * panel_show_anyway() (scheduled with g_timeout_add 200 ms) ensures the window
 * shows even if configure-event never arrives (e.g. when no WM is running).
 */
static void
panel_start_gui(panel *p)
{

    // main toplevel window
    p->topgwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width(GTK_CONTAINER(p->topgwin), 0);
    g_signal_connect(G_OBJECT(p->topgwin), "destroy-event",
        (GCallback) panel_destroy_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "size-allocate",
        (GCallback) panel_size_alloc, p);
    g_signal_connect(G_OBJECT(p->topgwin), "map-event",
        (GCallback) panel_mapped, p);
    g_signal_connect(G_OBJECT(p->topgwin), "configure-event",
        (GCallback) panel_configure_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "button-press-event",
        (GCallback) panel_button_press_event, p);
    g_signal_connect(G_OBJECT(p->topgwin), "scroll-event",
        (GCallback) panel_scroll_event, p);

    gtk_window_set_resizable(GTK_WINDOW(p->topgwin), FALSE);
    /* gtk_window_set_wmclass deprecated in GTK3; WM_CLASS set by GDK from binary name */
    gtk_window_set_title(GTK_WINDOW(p->topgwin), "panel");
    gtk_window_set_position(GTK_WINDOW(p->topgwin), GTK_WIN_POS_NONE);
    gtk_window_set_decorated(GTK_WINDOW(p->topgwin), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(p->topgwin), FALSE);
    if (p->setdocktype)
        gtk_window_set_type_hint(GTK_WINDOW(p->topgwin),
            GDK_WINDOW_TYPE_HINT_DOCK);

    if (p->layer == LAYER_ABOVE)
        gtk_window_set_keep_above(GTK_WINDOW(p->topgwin), TRUE);
    else if (p->layer == LAYER_BELOW)
        gtk_window_set_keep_below(GTK_WINDOW(p->topgwin), TRUE);
    gtk_window_stick(GTK_WINDOW(p->topgwin));

    gtk_widget_realize(p->topgwin);
    p->topxwin = GDK_WINDOW_XID(gtk_widget_get_window(p->topgwin));
    p->monitors_sid = g_signal_connect(
        gtk_widget_get_screen(p->topgwin), "monitors-changed",
        G_CALLBACK(panel_screen_changed), p);
    DBG("topxwin = %lx\n", p->topxwin);
    /* ensure configure event */
    XMoveWindow(GDK_DPY, p->topxwin, 20, 20);
    XSync(GDK_DPY, False);

    gtk_widget_set_app_paintable(p->topgwin, TRUE);
    calculate_position(p);
    /* Set a hard minimum height so GTK never shrinks the window below p->ah
     * during transient queue_resize passes (e.g. triggered by the internal
     * GtkWindow::monitors-changed handler before our own handler fires). */
    gtk_widget_set_size_request(p->topgwin, -1, p->ah);
    gtk_window_move(GTK_WINDOW(p->topgwin), p->ax, p->ay);
    gtk_window_resize(GTK_WINDOW(p->topgwin), p->aw, p->ah);
    DBG("move-resize x %d y %d w %d h %d\n", p->ax, p->ay, p->aw, p->ah);
    //XSync(GDK_DISPLAY(), False);
    //gdk_display_flush(gdk_display_get_default());

    // background box all over toplevel
    p->bbox = gtk_bgbox_new();
    gtk_widget_set_name(p->bbox, "panel-bg");
    gtk_container_add(GTK_CONTAINER(p->topgwin), p->bbox);
    gtk_container_set_border_width(GTK_CONTAINER(p->bbox), 0);
    if (p->transparent) {
        /* Apply fallback CSS rules at PRIORITY_FALLBACK so desktop themes can
         * override them.  When a wallpaper is set, BG_ROOT mode paints the
         * root-pixmap slice directly and these rules are never invoked.  When
         * no wallpaper exists (bare X / VNC), gtk_bgbox_draw falls back to
         * gtk_render_background(), which picks up these rules.
         *
         * Rules applied screen-wide so all GtkBgbox widgets (bbox AND every
         * plugin pwid) get a consistent dark background:
         *   #panel-bg      — main panel strip: dark-to-medium gradient
         *   .panel-plugin  — per-plugin containers: flat dark fill
         *                    (the "panel-plugin" class is added to each pwid
         *                    in plugin_start() when transparent == true)
         */
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#panel-bg { background-color: #2a2a2a;"
            "             background-image: linear-gradient(to bottom, #555555, #2a2a2a); }\n"
            ".panel-plugin { background-color: #2a2a2a; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);
        g_object_unref(css);
        p->bg = fb_bg_get_for_display();
        gtk_bgbox_set_background(p->bbox, BG_ROOT, p->tintcolor, p->alpha);
    }

    // main layout manager as a single child of background widget box
    p->lbox = p->my_box_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(p->lbox), 0);
    gtk_container_add(GTK_CONTAINER(p->bbox), p->lbox);

    p->box = p->my_box_new(FALSE, p->spacing);
    gtk_container_set_border_width(GTK_CONTAINER(p->box), 0);
    gtk_box_pack_start(GTK_BOX(p->lbox), p->box, TRUE, TRUE,
        (p->round_corners) ? p->round_corners_radius : 0);
    if (p->round_corners) {
        DBG("make_round_corners\n");
        make_round_corners(p);
    }
    /* start window creation process */
    gtk_widget_show_all(p->topgwin);
    /* .. and hide it from user until everything is done */
    gtk_widget_hide(p->topgwin);

    p->menu = panel_make_menu(p);

    if (p->setstrut)
        panel_set_wm_strut(p);

    XSelectInput(GDK_DPY, GDK_ROOT_WINDOW(), PropertyChangeMask);
    gdk_window_add_filter(gdk_get_default_root_window(),
          (GdkFilterFunc)panel_event_filter, p);
    //XSync(GDK_DISPLAY(), False);
    gdk_display_flush(gdk_display_get_default());
    return;
}

/**
 * panel_parse_global - parse the "global" xconf block and build the GUI.
 * @xc: xconf node for the "global" section (child of root).
 *
 * Sets all panel config fields to defaults, then overrides them with values
 * read from xc via XCG macros.  After validation and sanity-clamping, calls
 * panel_start_gui() to create the widget tree.
 *
 * Config keys read: edge, allign, widthtype, heighttype, width, height,
 * xmargin, ymargin, setdocktype, setpartialstrut, autohide, heightwhenhidden,
 * setlayer, layer, roundcorners, roundcornersradius, transparent, alpha,
 * tintcolor, maxelemheight.
 *
 * Returns: 1 always (return value is unused by the caller).
 */
static int
panel_parse_global(xconf *xc)
{
    /* Set default values */
    p->allign = ALLIGN_CENTER;
    p->edge = EDGE_BOTTOM;
    p->widthtype = WIDTH_PERCENT;
    p->width = 100;
    p->heighttype = HEIGHT_PIXEL;
    p->height = PANEL_HEIGHT_DEFAULT;
    p->max_elem_height = PANEL_HEIGHT_MAX;
    p->setdocktype = 1;
    p->setstrut = 1;
    p->round_corners = 1;
    p->round_corners_radius = 7;
    p->autohide = 0;
    p->height_when_hidden = 2;
    p->transparent = 0;
    p->alpha = 127;
    p->tintcolor_name = "white";
    p->spacing = 0;
    p->setlayer = FALSE;
    p->layer = LAYER_ABOVE;

    /* Read config */
    /* geometry */
    XCG(xc, "edge", &p->edge, enum, edge_enum);
    XCG(xc, "allign", &p->allign, enum, allign_enum);
    XCG(xc, "widthtype", &p->widthtype, enum, widthtype_enum);
    XCG(xc, "heighttype", &p->heighttype, enum, heighttype_enum);
    XCG(xc, "width", &p->width, int);
    XCG(xc, "height", &p->height, int);
    XCG(xc, "xmargin", &p->xmargin, int);
    XCG(xc, "ymargin", &p->ymargin, int);

    /* properties */
    XCG(xc, "setdocktype", &p->setdocktype, enum, bool_enum);
    XCG(xc, "setpartialstrut", &p->setstrut, enum, bool_enum);
    XCG(xc, "autohide", &p->autohide, enum, bool_enum);
    XCG(xc, "heightwhenhidden", &p->height_when_hidden, int);
    XCG(xc, "setlayer", &p->setlayer, enum, bool_enum);
    XCG(xc, "layer", &p->layer, enum, layer_enum);

    /* effects */
    XCG(xc, "roundcorners", &p->round_corners, enum, bool_enum);
    XCG(xc, "roundcornersradius", &p->round_corners_radius, int);
    XCG(xc, "transparent", &p->transparent, enum, bool_enum);
    XCG(xc, "alpha", &p->alpha, int);
    XCG(xc, "tintcolor", &p->tintcolor_name, str);
    XCG(xc, "maxelemheight", &p->max_elem_height, int);

    /* Sanity checks */
    if (!gdk_rgba_parse(&p->gtintcolor, p->tintcolor_name))
        gdk_rgba_parse(&p->gtintcolor, "white");
    p->tintcolor = gcolor2rgb24(&p->gtintcolor);
    DBG("tintcolor=%x\n", p->tintcolor);
    if (p->alpha > 255)
        p->alpha = 255;
    p->orientation = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM)
        ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
    if (p->orientation == GTK_ORIENTATION_HORIZONTAL) {
        p->my_box_new = hbox_new;
        p->my_separator_new = vseparator_new;
    } else {
        p->my_box_new = vbox_new;
        p->my_separator_new = hseparator_new;
    }
    if (p->width < 0)
        p->width = 100;
    if (p->widthtype == WIDTH_PERCENT && p->width > 100)
        p->width = 100;
    if (p->heighttype == HEIGHT_PIXEL) {
        if (p->height < PANEL_HEIGHT_MIN)
            p->height = PANEL_HEIGHT_MIN;
        else if (p->height > PANEL_HEIGHT_MAX)
            p->height = PANEL_HEIGHT_MAX;
    }
    if (p->max_elem_height > p->height ||
            p->max_elem_height < PANEL_HEIGHT_MIN)
        p->max_elem_height = p->height;
    p->curdesk = get_net_current_desktop();
    p->desknum = get_net_number_of_desktops();
    //p->workarea = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_WORKAREA,
    //    XA_CARDINAL, &p->wa_len);
    //print_wmdata(p);
    panel_start_gui(p);
    return 1;
}

/**
 * panel_parse_plugin - parse one "plugin" xconf block and load the plugin.
 * @xc: xconf node for one "plugin" section.
 *
 * Reads the "type" key to determine the plugin name, loads it via plugin_load(),
 * sets plug->panel, expand/padding/border/xc from config, then calls
 * plugin_start().  On success, appends the plugin_instance to p->plugins.
 * On failure (load or start), calls plugin_put() and returns without adding.
 *
 * Note: "type" is read with xconf_get_str (transfer none) -- the pointer is
 * into the xconf tree and must not be g_free()'d.
 */
static void
panel_parse_plugin(xconf *xc)
{
    plugin_instance *plug = NULL;
    gchar *type = NULL;

    xconf_get_str(xconf_find(xc, "type", 0), &type);
    if (!type || !(plug = plugin_load(type))) {
        ERR( "fbpanel: can't load %s plugin\n", type);
        return;
    }
    plug->panel = p;
    XCG(xc, "expand", &plug->expand, enum, bool_enum);
    XCG(xc, "padding", &plug->padding, int);
    XCG(xc, "border", &plug->border, int);
    plug->xc = xconf_find(xc, "config", 0);

    if (!plugin_start(plug)) {
        g_message("fbpanel: plugin '%s' failed to start -- skipping", type);
        plugin_put(plug);
        return;
    }
    p->plugins = g_list_append(p->plugins, plug);
}

/**
 * panel_show_anyway - g_timeout_add callback; ensures the panel is shown.
 * @data: Unused.
 *
 * Scheduled with g_timeout_add(200, ...) at the end of panel_start().
 * This fallback ensures the panel becomes visible even if the configure-event
 * handler never fires (e.g. when no WM is running and no configure event arrives
 * to trigger gtk_widget_show via panel_configure_event).
 *
 * Returns: FALSE (one-shot; removes itself after firing).
 */
static gboolean
panel_show_anyway(gpointer data)
{
    gtk_widget_show_all(p->topgwin);
    return FALSE;
}


/**
 * panel_start - initialise fbev, parse global config and all plugins.
 * @xc: Root xconf node from xconf_new_from_file().
 *
 * Creates the FbEv singleton, then calls panel_parse_global (for the "global"
 * sub-node) and panel_parse_plugin for each "plugin" sub-node.
 * Schedules panel_show_anyway as a 200 ms fallback.
 */
static void
panel_start(xconf *xc)
{
    int i;
    xconf *pxc;

    fbev = fb_ev_new();

    //xconf_prn(stdout, xc, 0, FALSE);
    panel_parse_global(xconf_find(xc, "global", 0));
    for (i = 0; (pxc = xconf_find(xc, "plugin", i)); i++)
        panel_parse_plugin(pxc);
    g_timeout_add(200, panel_show_anyway, NULL);
    return;
}

/**
 * delete_plugin - g_list_foreach callback; stop and unload one plugin.
 * @data:  plugin_instance* to stop and free.
 * @udata: Unused.
 *
 * Calls plugin_stop() then plugin_put() on the instance.  Used by panel_stop()
 * to tear down all plugins before destroying the widget tree.
 */
static void
delete_plugin(gpointer data, gpointer udata)
{
    plugin_stop((plugin_instance *)data);
    plugin_put((plugin_instance *)data);
    return;
}

/**
 * panel_stop - tear down the panel: plugins, widgets, X11 filters, signals.
 * @p: Panel instance.
 *
 * Teardown sequence:
 *   1. Stop autohide timers (if autohide).
 *   2. Stop and unload all plugins (delete_plugin on each element).
 *   3. Free the plugin list.
 *   4. Deselect PropertyChangeMask on root; remove panel_event_filter.
 *   5. Disconnect the monitors-changed signal (p->monitors_sid).
 *   6. Destroy topgwin (recursively destroys bbox, lbox, box, all plugin pwids).
 *   7. Destroy p->menu explicitly (independent GtkWidget not in tree).
 *   8. Unref fbev.
 *   9. Flush and sync the X display.
 *
 * After panel_stop(), the caller calls xconf_del(p->xc, FALSE) then g_free(p).
 */
static void
panel_stop(panel *p)
{

    if (p->autohide)
        ah_stop(p);
    g_list_foreach(p->plugins, delete_plugin, NULL);
    g_list_free(p->plugins);
    p->plugins = NULL;

    XSelectInput(GDK_DPY, GDK_ROOT_WINDOW(), NoEventMask);
    gdk_window_remove_filter(gdk_get_default_root_window(),
          (GdkFilterFunc)panel_event_filter, p);
    if (p->monitors_sid) {
        g_signal_handler_disconnect(
            gtk_widget_get_screen(p->topgwin), p->monitors_sid);
        p->monitors_sid = 0;
    }
    gtk_widget_destroy(p->topgwin);
    gtk_widget_destroy(p->menu);
    g_object_unref(fbev);
    //g_free(p->workarea);
    gdk_display_flush(gdk_display_get_default());
    XFlush(GDK_DPY);
    XSync(GDK_DPY, True);
    return;
}

/**
 * usage - print command-line usage information to stdout and return.
 */
void
usage()
{
    printf("fbpanel %s - lightweight GTK3 panel for UNIX desktops\n", version);
    printf("Command line options:\n");
    printf(" --help      -- print this help and exit\n");
    printf(" --version   -- print version and exit\n");
    printf(" --log <number> -- set log level 0-5. 0 - none 5 - chatty\n");
    printf(" --configure -- launch configuration utility\n");
    printf(" --profile name -- use specified profile\n");
    printf("\n");
    printf(" -h  -- same as --help\n");
    printf(" -p  -- same as --profile\n");
    printf(" -v  -- same as --version\n");
    printf(" -C  -- same as --configure\n");
    printf("\nVisit http://fbpanel.sourceforge.net/ for detailed documentation,\n\n");
}

/**
 * handle_error - Xlib error handler; logs the error and continues.
 * @d:  X Display (unused; GDK_DPY used directly for XGetErrorText).
 * @ev: XErrorEvent with error_code to describe.
 *
 * Installed via XSetErrorHandler() in main().  By default Xlib would abort
 * the process on X errors; this handler logs the error and returns so the
 * program continues running.
 */
void
handle_error(Display * d, XErrorEvent * ev)
{
    char buf[256];

    XGetErrorText(GDK_DPY, ev->error_code, buf, 256);
    DBG("fbpanel : X error: %s\n", buf);

    return;
}

/**
 * sig_usr1 - SIGUSR1 handler; triggers a hot-reload of the panel.
 * @signum: Must be SIGUSR1 (guard check included).
 *
 * Calls gtk_main_quit() without setting force_quit, so the main loop
 * restarts with panel_stop / xconf_del / g_free / panel_start.
 */
static void
sig_usr1(int signum)
{
    if (signum != SIGUSR1)
        return;
    gtk_main_quit();
}

/**
 * sig_usr2 - SIGUSR2 handler; triggers clean shutdown.
 * @signum: Must be SIGUSR2 (guard check included).
 *
 * Sets force_quit=1 then calls gtk_main_quit().  The main loop condition
 * (force_quit == 0) will be false, so the process exits after panel_stop.
 */
static void
sig_usr2(int signum)
{
    if (signum != SIGUSR2)
        return;
    gtk_main_quit();
    force_quit = 1;
}

/**
 * do_argv - parse command-line arguments into global flags.
 * @argc: Argument count.
 * @argv: Argument vector.
 *
 * Recognised options:
 *   -h / --help            -> usage(); exit(0)
 *   -v / --version         -> print version; exit(0)
 *   --log N                -> log_level = N
 *   --configure / -C       -> config = 1 (show prefs dialog on start)
 *   --profile / -p NAME    -> profile = g_strdup(NAME)
 *   --xineramaHead / -x N  -> xineramaHead = N
 *
 * Unknown options print usage and exit(1).
 * Missing arguments to --log, --profile, --xineramaHead print an error and exit(1).
 */
static void
do_argv(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("fbpanel %s\n", version);
            exit(0);
        } else if (!strcmp(argv[i], "--log")) {
            i++;
            if (i == argc) {
                ERR( "fbpanel: missing log level\n");
                usage();
                exit(1);
            } else {
                log_level = atoi(argv[i]);
            }
        } else if (!strcmp(argv[i], "--configure") || !strcmp(argv[i], "-C")) {
            config = 1;
        } else if (!strcmp(argv[i], "--profile") || !strcmp(argv[i], "-p")) {
            i++;
            if (i == argc) {
                ERR( "fbpanel: missing profile name\n");
                usage();
                exit(1);
            } else {
                profile = g_strdup(argv[i]);
            }
        } else if (!strcmp(argv[i], "--xineramaHead") ||
                   !strcmp(argv[i], "-x")) {
          i++;
          if(i == argc) {
            ERR("fbpanel: xinerama head not specified\n");
            usage();
            exit(1);
          }
          xineramaHead = atoi(argv[i]);
        } else {
            printf("fbpanel: unknown option - %s\n", argv[i]);
            usage();
            exit(1);
        }
    }
}

/**
 * panel_get_profile - return the active profile name.
 *
 * Returns: (transfer none) static or g_strdup'd string; do NOT g_free().
 */
gchar *panel_get_profile()
{
    return profile;
}

/**
 * panel_get_profile_file - return the full path to the active profile file.
 *
 * Returns: (transfer none) string built by g_build_filename() in main();
 *          freed at process exit via g_free(profile_file).  Do NOT g_free().
 */
gchar *panel_get_profile_file()
{
    return profile_file;
}

/**
 * ensure_profile - create the profile config file from a template if missing.
 * @(implicit) profile_file: Full path to the profile file.
 * @(implicit) profile:      Profile name used as argument to make_profile.
 *
 * If the profile file does not exist, runs LIBEXECDIR/fbpanel/make_profile
 * via g_spawn_command_line_sync() to create a default config.
 * If the file still does not exist after the script, logs an error and calls
 * exit(1).
 */
static void
ensure_profile()
{
    gchar *cmd;

    if (g_file_test(profile_file,
            G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
        return;
    }
    cmd = g_strdup_printf("%s %s", LIBEXECDIR "/fbpanel/make_profile",
        profile);
    g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
    g_free(cmd);
    if (g_file_test(profile_file,
            G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
        return;
    }
    ERR("Can't open profile %s - %s\n", profile, profile_file);
    exit(1);
}

/**
 * main - fbpanel entry point.
 * @argc: Argument count.
 * @argv: Argument vector.
 *
 * Initialisation:
 *   1. setlocale / bindtextdomain / textdomain (i18n)
 *   2. gtk_init() (GTK + GDK + GLib)
 *   3. XSetLocaleModifiers, XSetErrorHandler
 *   4. fb_init() (X11 atoms + icon_theme)
 *   5. do_argv() (parse flags)
 *   6. Build profile_file path; ensure_profile()
 *   7. Append IMGPREFIX to icon theme search path
 *   8. Install SIGUSR1 / SIGUSR2 handlers
 *
 * Main loop (restart on SIGUSR1, exit on SIGUSR2 or destroy-event):
 *   Allocate panel struct (g_new0), parse config, run panel_start(),
 *   optionally open configure dialog, enter gtk_main(), teardown with
 *   panel_stop().  Loop while force_quit == 0.
 *
 * Shutdown: g_free(profile_file), fb_free(), exit(0).
 */
int
main(int argc, char *argv[])
{
    setlocale(LC_CTYPE, "");
    bindtextdomain(PROJECT_NAME, LOCALEDIR);
    textdomain(PROJECT_NAME);

    gtk_init(&argc, &argv);
    XSetLocaleModifiers("");
    XSetErrorHandler((XErrorHandler) handle_error);
    fb_init();
    do_argv(argc, argv);
    profile_file = g_build_filename(g_get_user_config_dir(),
        "fbpanel", profile, NULL);
    ensure_profile();
    gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), IMGPREFIX);
    signal(SIGUSR1, sig_usr1);
    signal(SIGUSR2, sig_usr2);

    do {
        the_panel = p = g_new0(panel, 1);
        p->xineramaHead = xineramaHead;
        p->xc = xconf_new_from_file(profile_file, profile);
        if (!p->xc)
            exit(1);

        panel_start(p->xc);
        if (config)
            configure(p->xc);
        gtk_main();
        panel_stop(p);
        //xconf_save_to_profile(cprofile, xc);
        xconf_del(p->xc, FALSE);
        g_free(p);
        DBG("force_quit=%d\n", force_quit);
    } while (force_quit == 0);
    g_free(profile_file);
    fb_free();
    exit(0);
}
