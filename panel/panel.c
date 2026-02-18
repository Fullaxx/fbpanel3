
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


static gchar version[] = PROJECT_VERSION;
static gchar *profile = "default";
static gchar *profile_file;

/* Wrapper functions matching my_box_new signature */
static GtkWidget *hbox_new(GtkOrientation o, gint spacing) {
    (void)o;
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}
static GtkWidget *vbox_new(GtkOrientation o, gint spacing) {
    (void)o;
    return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}
/* Wrapper functions matching my_separator_new signature */
static GtkWidget *vseparator_new(void) {
    return gtk_separator_new(GTK_ORIENTATION_VERTICAL);
}
static GtkWidget *hseparator_new(void) {
    return gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
}

guint mwid; // mouse watcher thread id
guint hpid; // hide panel thread id


FbEv *fbev;
gint force_quit = 0;
int config;
int xineramaHead = FBPANEL_INVALID_XINERAMA_HEAD;

//#define DEBUGPRN
#include "dbg.h"

/** verbosity level of dbg and log functions */
int log_level = LOG_WARN;

static panel *p;
panel *the_panel;

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

static gint
panel_destroy_event(GtkWidget * widget, GdkEvent * event, gpointer data)
{
    gtk_main_quit();
    force_quit = 1;
    return FALSE;
}

static void
panel_size_alloc(GtkWidget *widget, GdkRectangle *a, panel *p)
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

#define GAP 2
#define PERIOD 300

static gboolean ah_state_visible(panel *p);
static gboolean ah_state_waiting(panel *p);
static gboolean ah_state_hidden(panel *p);

static gboolean
panel_mapped(GtkWidget *widget, GdkEvent *event, panel *p)
{
    if (p->autohide) {
        ah_stop(p);
        ah_start(p);
    }
    return FALSE;
}

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

/* starts autohide behaviour */
void
ah_start(panel *p)
{
    mwid = g_timeout_add(PERIOD, (GSourceFunc) mouse_watch, p);
    ah_state_visible(p);
    return;
}

/* stops autohide */
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
    DBG("topxwin = %lx\n", p->topxwin);
    /* ensure configure event */
    XMoveWindow(GDK_DPY, p->topxwin, 20, 20);
    XSync(GDK_DPY, False);

    gtk_widget_set_app_paintable(p->topgwin, TRUE);
    calculate_position(p);
    gtk_window_move(GTK_WINDOW(p->topgwin), p->ax, p->ay);
    gtk_window_resize(GTK_WINDOW(p->topgwin), p->aw, p->ah);
    DBG("move-resize x %d y %d w %d h %d\n", p->ax, p->ay, p->aw, p->ah);
    //XSync(GDK_DISPLAY(), False);
    //gdk_display_flush(gdk_display_get_default());

    // background box all over toplevel
    p->bbox = gtk_bgbox_new();
    gtk_container_add(GTK_CONTAINER(p->topgwin), p->bbox);
    gtk_container_set_border_width(GTK_CONTAINER(p->bbox), 0);
    if (p->transparent) {
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
    p->heighttype = HEIGHT_PIXEL;
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
        ERR( "fbpanel: can't start plugin %s\n", type);
        exit(1);
    }
    p->plugins = g_list_append(p->plugins, plug);
}

static gboolean
panel_show_anyway(gpointer data)
{
    gtk_widget_show_all(p->topgwin);
    return FALSE;
}


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

static void
delete_plugin(gpointer data, gpointer udata)
{
    plugin_stop((plugin_instance *)data);
    plugin_put((plugin_instance *)data);
    return;
}

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
    gtk_widget_destroy(p->topgwin);
    gtk_widget_destroy(p->menu);
    g_object_unref(fbev);
    //g_free(p->workarea);
    gdk_display_flush(gdk_display_get_default());
    XFlush(GDK_DPY);
    XSync(GDK_DPY, True);
    return;
}

void
usage()
{
    printf("fbpanel %s - lightweight GTK2+ panel for UNIX desktops\n", version);
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

void
handle_error(Display * d, XErrorEvent * ev)
{
    char buf[256];

    XGetErrorText(GDK_DPY, ev->error_code, buf, 256);
    DBG("fbpanel : X error: %s\n", buf);

    return;
}

static void
sig_usr1(int signum)
{
    if (signum != SIGUSR1)
        return;
    gtk_main_quit();
}

static void
sig_usr2(int signum)
{
    if (signum != SIGUSR2)
        return;
    gtk_main_quit();
    force_quit = 1;
}

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

gchar *panel_get_profile()
{
    return profile;
}

gchar *panel_get_profile_file()
{
    return profile_file;
}

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

