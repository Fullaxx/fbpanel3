

#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "misc.h"

//#define DEBUGPRN
#include "dbg.h"

GtkIconTheme *icon_theme;

/* X11 data types */
Atom a_UTF8_STRING;
Atom a_XROOTPMAP_ID;

/* old WM spec */
Atom a_WM_STATE;
Atom a_WM_CLASS;
Atom a_WM_DELETE_WINDOW;
Atom a_WM_PROTOCOLS;

/* new NET spec */
Atom a_NET_WORKAREA;
Atom a_NET_CLIENT_LIST;
Atom a_NET_CLIENT_LIST_STACKING;
Atom a_NET_NUMBER_OF_DESKTOPS;
Atom a_NET_CURRENT_DESKTOP;
Atom a_NET_DESKTOP_NAMES;
Atom a_NET_DESKTOP_GEOMETRY;
Atom a_NET_ACTIVE_WINDOW;
Atom a_NET_CLOSE_WINDOW;
Atom a_NET_SUPPORTED;
Atom a_NET_WM_DESKTOP;
Atom a_NET_WM_STATE;
Atom a_NET_WM_STATE_SKIP_TASKBAR;
Atom a_NET_WM_STATE_SKIP_PAGER;
Atom a_NET_WM_STATE_STICKY;
Atom a_NET_WM_STATE_HIDDEN;
Atom a_NET_WM_STATE_SHADED;
Atom a_NET_WM_STATE_ABOVE;
Atom a_NET_WM_STATE_BELOW;
Atom a_NET_WM_WINDOW_TYPE;
Atom a_NET_WM_WINDOW_TYPE_DESKTOP;
Atom a_NET_WM_WINDOW_TYPE_DOCK;
Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;
Atom a_NET_WM_WINDOW_TYPE_MENU;
Atom a_NET_WM_WINDOW_TYPE_UTILITY;
Atom a_NET_WM_WINDOW_TYPE_SPLASH;
Atom a_NET_WM_WINDOW_TYPE_DIALOG;
Atom a_NET_WM_WINDOW_TYPE_NORMAL;
Atom a_NET_WM_DESKTOP;
Atom a_NET_WM_NAME;
Atom a_NET_WM_VISIBLE_NAME;
Atom a_NET_WM_STRUT;
Atom a_NET_WM_STRUT_PARTIAL;
Atom a_NET_WM_ICON;
Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;

xconf_enum allign_enum[] = {
    { .num = ALLIGN_LEFT, .str = c_("left") },
    { .num = ALLIGN_RIGHT, .str = c_("right") },
    { .num = ALLIGN_CENTER, .str = c_("center")},
    { .num = 0, .str = NULL },
};
xconf_enum edge_enum[] = {
    { .num = EDGE_LEFT, .str = c_("left") },
    { .num = EDGE_RIGHT, .str = c_("right") },
    { .num = EDGE_TOP, .str = c_("top") },
    { .num = EDGE_BOTTOM, .str = c_("bottom") },
    { .num = 0, .str = NULL },
};
xconf_enum widthtype_enum[] = {
    { .num = WIDTH_REQUEST, .str = "request" , .desc = c_("dynamic") },
    { .num = WIDTH_PIXEL, .str = "pixel" , .desc = c_("pixels") },
    { .num = WIDTH_PERCENT, .str = "percent", .desc = c_("% of screen") },
    { .num = 0, .str = NULL },
};
xconf_enum heighttype_enum[] = {
    { .num = HEIGHT_PIXEL, .str = c_("pixel") },
    { .num = 0, .str = NULL },
};
xconf_enum bool_enum[] = {
    { .num = 0, .str = "false" },
    { .num = 1, .str = "true" },
    { .num = 0, .str = NULL },
};
xconf_enum pos_enum[] = {
    { .num = POS_NONE, .str = "none" },
    { .num = POS_START, .str = "start" },
    { .num = POS_END,  .str = "end" },
    { .num = 0, .str = NULL},
};
xconf_enum layer_enum[] = {
    { .num = LAYER_ABOVE, .str = c_("above") },
    { .num = LAYER_BELOW, .str = c_("below") },
    { .num = 0, .str = NULL},
};


int
str2num(xconf_enum *p, gchar *str, int defval)
{
    for (;p && p->str; p++) {
        if (!g_ascii_strcasecmp(str, p->str))
            return p->num;
    }
    return defval;
}

gchar *
num2str(xconf_enum *p, int num, gchar *defval)
{
    for (;p && p->str; p++) {
        if (num == p->num)
            return p->str;
    }
    return defval;
}


void resolve_atoms()
{
    Display *dpy;

    dpy = GDK_DPY;

    a_UTF8_STRING                = XInternAtom(dpy, "UTF8_STRING", False);
    a_XROOTPMAP_ID               = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    a_WM_STATE                   = XInternAtom(dpy, "WM_STATE", False);
    a_WM_CLASS                   = XInternAtom(dpy, "WM_CLASS", False);
    a_WM_DELETE_WINDOW           = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_WM_PROTOCOLS               = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_NET_WORKAREA               = XInternAtom(dpy, "_NET_WORKAREA", False);
    a_NET_CLIENT_LIST            = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    a_NET_CLIENT_LIST_STACKING   = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
    a_NET_NUMBER_OF_DESKTOPS     = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    a_NET_CURRENT_DESKTOP        = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    a_NET_DESKTOP_NAMES          = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
    a_NET_DESKTOP_GEOMETRY       = XInternAtom(dpy, "_NET_DESKTOP_GEOMETRY", False);
    a_NET_ACTIVE_WINDOW          = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    a_NET_SUPPORTED              = XInternAtom(dpy, "_NET_SUPPORTED", False);
    a_NET_WM_DESKTOP             = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    a_NET_WM_STATE               = XInternAtom(dpy, "_NET_WM_STATE", False);
    a_NET_WM_STATE_SKIP_TASKBAR  = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    a_NET_WM_STATE_SKIP_PAGER    = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    a_NET_WM_STATE_STICKY        = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    a_NET_WM_STATE_HIDDEN        = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    a_NET_WM_STATE_SHADED        = XInternAtom(dpy, "_NET_WM_STATE_SHADED", False);
    a_NET_WM_STATE_ABOVE         = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    a_NET_WM_STATE_BELOW         = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    a_NET_WM_WINDOW_TYPE         = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);

    a_NET_WM_WINDOW_TYPE_DESKTOP = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a_NET_WM_WINDOW_TYPE_DOCK    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a_NET_WM_WINDOW_TYPE_TOOLBAR = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    a_NET_WM_WINDOW_TYPE_MENU    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    a_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    a_NET_WM_WINDOW_TYPE_SPLASH  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    a_NET_WM_WINDOW_TYPE_DIALOG  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    a_NET_WM_WINDOW_TYPE_NORMAL  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    a_NET_WM_NAME                = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_NET_WM_VISIBLE_NAME        = XInternAtom(dpy, "_NET_WM_VISIBLE_NAME", False);
    a_NET_WM_STRUT               = XInternAtom(dpy, "_NET_WM_STRUT", False);
    a_NET_WM_STRUT_PARTIAL       = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    a_NET_WM_ICON                = XInternAtom(dpy, "_NET_WM_ICON", False);
    a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR
                                 = XInternAtom(dpy, "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);

    return;
}


void fb_init()
{
    resolve_atoms();
    icon_theme = gtk_icon_theme_get_default();
}

void fb_free()
{
    // MUST NOT be ref'd or unref'd
    // g_object_unref(icon_theme);
}

void
Xclimsg(Window win, long type, long l0, long l1, long l2, long l3, long l4)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.send_event = True;
    xev.display = GDK_DPY;
    xev.message_type = type;
    xev.format = 32;
    xev.data.l[0] = l0;
    xev.data.l[1] = l1;
    xev.data.l[2] = l2;
    xev.data.l[3] = l3;
    xev.data.l[4] = l4;
    XSendEvent(GDK_DPY, GDK_ROOT_WINDOW(), False,
          (SubstructureNotifyMask | SubstructureRedirectMask),
          (XEvent *) & xev);
}

void
Xclimsgwm(Window win, Atom type, Atom arg)
{
    XClientMessageEvent xev;

    xev.type = ClientMessage;
    xev.window = win;
    xev.message_type = type;
    xev.format = 32;
    xev.data.l[0] = arg;
    xev.data.l[1] = GDK_CURRENT_TIME;
    XSendEvent(GDK_DPY, win, False, 0L, (XEvent *) &xev);
}


void *
get_utf8_property(Window win, Atom atom)
{

    Atom type;
    int format;
    gulong nitems;
    gulong bytes_after;
    gchar  *retval;
    int result;
    guchar *tmp = NULL;

    type = None;
    retval = NULL;
    result = XGetWindowProperty (GDK_DPY, win, atom, 0, G_MAXLONG, False,
          a_UTF8_STRING, &type, &format, &nitems,
          &bytes_after, &tmp);
    if (result != Success)
        return NULL;
    if (tmp) {
        if (type == a_UTF8_STRING && format == 8 && nitems != 0)
            retval = g_strndup ((gchar *)tmp, nitems);
        XFree (tmp);
    }
    return retval;

}

char **
get_utf8_property_list(Window win, Atom atom, int *count)
{
    Atom type;
    int format, i;
    gulong nitems;
    gulong bytes_after;
    gchar *s, **retval = NULL;
    int result;
    guchar *tmp = NULL;

    *count = 0;
    result = XGetWindowProperty(GDK_DPY, win, atom, 0, G_MAXLONG, False,
          a_UTF8_STRING, &type, &format, &nitems,
          &bytes_after, &tmp);
    if (result != Success || type != a_UTF8_STRING || tmp == NULL)
        return NULL;

    if (nitems) {
        gchar *val = (gchar *) tmp;
        DBG("res=%d(%d) nitems=%d val=%s\n", result, Success, nitems, val);
        for (i = 0; i < nitems; i++) {
            if (!val[i])
                (*count)++;
        }
        retval = g_new0 (char*, *count + 2);
        for (i = 0, s = val; i < *count; i++, s = s +  strlen (s) + 1) {
            retval[i] = g_strdup(s);
        }
        if (val[nitems-1]) {
            result = nitems - (s - val);
            DBG("val does not ends by 0, moving last %d bytes\n", result);
            memmove(s - 1, s, result);
            val[nitems-1] = 0;
            DBG("s=%s\n", s -1);
            retval[i] = g_strdup(s - 1);
            (*count)++;
        }
    }
    XFree (tmp);

    return retval;

}

void *
get_xaproperty (Window win, Atom prop, Atom type, int *nitems)
{
    Atom type_ret;
    int format_ret;
    unsigned long items_ret;
    unsigned long after_ret;
    unsigned char *prop_data;

    prop_data = NULL;
    if (XGetWindowProperty (GDK_DPY, win, prop, 0, 0x7fffffff, False,
              type, &type_ret, &format_ret, &items_ret,
              &after_ret, &prop_data) != Success)
        return NULL;
    DBG("win=%x prop=%d type=%d rtype=%d rformat=%d nitems=%d\n", win, prop,
            type, type_ret, format_ret, items_ret);

    if (nitems)
        *nitems = items_ret;
    return prop_data;
}

static char*
text_property_to_utf8 (const XTextProperty *prop)
{
  char **list;
  int count;
  char *retval;

  list = NULL;
  count = gdk_text_property_to_utf8_list_for_display (gdk_display_get_default(),
                                          gdk_x11_xatom_to_atom (prop->encoding),
                                          prop->format,
                                          prop->value,
                                          prop->nitems,
                                          &list);

  DBG("count=%d\n", count);
  if (count == 0)
    return NULL;

  retval = list[0];
  list[0] = g_strdup (""); /* something to free */

  g_strfreev (list);

  return retval;
}

char *
get_textproperty(Window win, Atom atom)
{
    XTextProperty text_prop;
    char *retval;

    if (XGetTextProperty(GDK_DPY, win, &text_prop, atom)) {
        DBG("format=%d enc=%d nitems=%d value=%s   \n",
              text_prop.format,
              text_prop.encoding,
              text_prop.nitems,
              text_prop.value);
        retval = text_property_to_utf8 (&text_prop);
        if (text_prop.nitems > 0)
            XFree (text_prop.value);
        return retval;

    }
    return NULL;
}


guint
get_net_number_of_desktops()
{
    guint desknum;
    guint32 *data;

    data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_NUMBER_OF_DESKTOPS,
          XA_CARDINAL, 0);
    if (!data)
        return 0;

    desknum = *data;
    XFree (data);
    return desknum;
}


guint
get_net_current_desktop ()
{
    guint desk;
    guint32 *data;

    data = get_xaproperty (GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, XA_CARDINAL, 0);
    if (!data)
        return 0;

    desk = *data;
    XFree (data);
    return desk;
}

guint
get_net_wm_desktop(Window win)
{
    guint desk = 0;
    guint *data;

    data = get_xaproperty (win, a_NET_WM_DESKTOP, XA_CARDINAL, 0);
    if (data) {
        desk = *data;
        XFree (data);
    } else
        DBG("can't get desktop num for win 0x%lx", win);
    return desk;
}

void
get_net_wm_state(Window win, net_wm_state *nws)
{
    Atom *state;
    int num3;


    bzero(nws, sizeof(*nws));
    if (!(state = get_xaproperty(win, a_NET_WM_STATE, XA_ATOM, &num3)))
        return;

    DBG( "%x: netwm state = { ", (unsigned int)win);
    while (--num3 >= 0) {
        if (state[num3] == a_NET_WM_STATE_SKIP_PAGER) {
            DBGE("NET_WM_STATE_SKIP_PAGER ");
            nws->skip_pager = 1;
        } else if (state[num3] == a_NET_WM_STATE_SKIP_TASKBAR) {
            DBGE("NET_WM_STATE_SKIP_TASKBAR ");
            nws->skip_taskbar = 1;
        } else if (state[num3] == a_NET_WM_STATE_STICKY) {
            DBGE("NET_WM_STATE_STICKY ");
            nws->sticky = 1;
        } else if (state[num3] == a_NET_WM_STATE_HIDDEN) {
            DBGE("NET_WM_STATE_HIDDEN ");
            nws->hidden = 1;
        } else if (state[num3] == a_NET_WM_STATE_SHADED) {
            DBGE("NET_WM_STATE_SHADED ");
            nws->shaded = 1;
        } else {
            DBGE("... ");
        }
    }
    XFree(state);
    DBGE( "}\n");
    return;
}




void
get_net_wm_window_type(Window win, net_wm_window_type *nwwt)
{
    Atom *state;
    int num3;


    bzero(nwwt, sizeof(*nwwt));
    if (!(state = get_xaproperty(win, a_NET_WM_WINDOW_TYPE, XA_ATOM, &num3)))
        return;

    DBG( "%x: netwm state = { ", (unsigned int)win);
    while (--num3 >= 0) {
        if (state[num3] == a_NET_WM_WINDOW_TYPE_DESKTOP) {
            DBG("NET_WM_WINDOW_TYPE_DESKTOP ");
            nwwt->desktop = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DOCK) {
            DBG( "NET_WM_WINDOW_TYPE_DOCK ");
            nwwt->dock = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_TOOLBAR) {
            DBG( "NET_WM_WINDOW_TYPE_TOOLBAR ");
            nwwt->toolbar = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_MENU) {
            DBG( "NET_WM_WINDOW_TYPE_MENU ");
            nwwt->menu = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_UTILITY) {
            DBG( "NET_WM_WINDOW_TYPE_UTILITY ");
            nwwt->utility = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_SPLASH) {
            DBG( "NET_WM_WINDOW_TYPE_SPLASH ");
            nwwt->splash = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_DIALOG) {
            DBG( "NET_WM_WINDOW_TYPE_DIALOG ");
            nwwt->dialog = 1;
        } else if (state[num3] == a_NET_WM_WINDOW_TYPE_NORMAL) {
            DBG( "NET_WM_WINDOW_TYPE_NORMAL ");
            nwwt->normal = 1;
        } else {
            DBG( "... ");
        }
    }
    XFree(state);
    DBG( "}\n");
    return;
}




static void
calculate_width(int scrw, int wtype, int allign, int xmargin,
      int *panw, int *x)
{
    DBG("scrw=%d\n", scrw);
    DBG("IN panw=%d\n", *panw);
    //scrw -= 2;
    if (wtype == WIDTH_PERCENT) {
        /* sanity check */
        if (*panw > 100)
            *panw = 100;
        else if (*panw < 0)
            *panw = 1;
        *panw = ((gfloat) scrw * (gfloat) *panw) / 100.0;
    }
    if (*panw > scrw)
        *panw = scrw;

    if (allign != ALLIGN_CENTER) {
        if (xmargin > scrw) {
            ERR( "xmargin is bigger then edge size %d > %d. Ignoring xmargin\n",
                  xmargin, scrw);
            xmargin = 0;
        }
        if (wtype == WIDTH_PERCENT)
            //*panw = MAX(scrw - xmargin, *panw);
            ;
        else
            *panw = MIN(scrw - xmargin, *panw);
    }
    DBG("OUT panw=%d\n", *panw);
    if (allign == ALLIGN_LEFT)
        *x += xmargin;
    else if (allign == ALLIGN_RIGHT) {
        *x += scrw - *panw - xmargin;
        if (*x < 0)
            *x = 0;
    } else if (allign == ALLIGN_CENTER)
        *x += (scrw - *panw) / 2;
    return;
}


void
calculate_position(panel *np)
{
    int positionSet = 0;
    int sswidth, ssheight, minx, miny;


    /* If a Xinerama head was specified on the command line, then
     * calculate the location based on that.  Otherwise, just use the
     * screen dimensions. */
    if(np->xineramaHead != FBPANEL_INVALID_XINERAMA_HEAD) {
      GdkDisplay *dpy = gdk_display_get_default();
      int nDisplay = gdk_display_get_n_monitors(dpy);
      GdkRectangle rect;

      if(np->xineramaHead < nDisplay) {
        GdkMonitor *mon = gdk_display_get_monitor(dpy, np->xineramaHead);
        gdk_monitor_get_geometry(mon, &rect);
        minx = rect.x;
        miny = rect.y;
        sswidth = rect.width;
        ssheight = rect.height;
        positionSet = 1;
      }
    }

    if (!positionSet) {
        GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
        GdkRectangle rect;
        gdk_monitor_get_geometry(mon, &rect);
        minx = rect.x;
        miny = rect.y;
        sswidth  = rect.width;
        ssheight = rect.height;
    }

    np->screenRect.x = minx;
    np->screenRect.y = miny;
    np->screenRect.width = sswidth;
    np->screenRect.height = ssheight;

    if (np->edge == EDGE_TOP || np->edge == EDGE_BOTTOM) {
        np->aw = np->width;
        np->ax = minx;
        calculate_width(sswidth, np->widthtype, np->allign, np->xmargin,
              &np->aw, &np->ax);
        np->ah = np->height;
        np->ah = MIN(PANEL_HEIGHT_MAX, np->ah);
        np->ah = MAX(PANEL_HEIGHT_MIN, np->ah);
        if (np->edge == EDGE_TOP)
            np->ay = np->ymargin;
        else
            np->ay = ssheight - np->ah - np->ymargin;

    } else {
        np->ah = np->width;
        np->ay = miny;
        calculate_width(ssheight, np->widthtype, np->allign, np->xmargin,
              &np->ah, &np->ay);
        np->aw = np->height;
        np->aw = MIN(PANEL_HEIGHT_MAX, np->aw);
        np->aw = MAX(PANEL_HEIGHT_MIN, np->aw);
        if (np->edge == EDGE_LEFT)
            np->ax = np->ymargin;
        else
            np->ax = sswidth - np->aw - np->ymargin;
    }
    if (!np->aw)
        np->aw = 1;
    if (!np->ah)
        np->ah = 1;

    /*
    if (!np->visible) {
        DBG("pushing of screen dx=%d dy=%d\n", np->ah_dx, np->ah_dy);
        np->ax += np->ah_dx;
        np->ay += np->ah_dy;
    }
    */
    DBG("x=%d y=%d w=%d h=%d\n", np->ax, np->ay, np->aw, np->ah);
    return;
}



gchar *
expand_tilda(gchar *file)
{
    if (!file)
        return NULL;
    RET((file[0] == '~') ?
        g_strdup_printf("%s%s", getenv("HOME"), file+1)
        : g_strdup(file));

}


void
get_button_spacing(GtkRequisition *req, GtkContainer *parent, gchar *name)
{
    GtkWidget *b;
    //gint focus_width;
    //gint focus_pad;

    b = gtk_button_new();
    gtk_widget_set_name(GTK_WIDGET(b), name);
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_can_default(b, FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (b), 0);

    if (parent)
        gtk_container_add(parent, b);

    gtk_widget_show(b);
    gtk_widget_get_preferred_size(b, req, NULL);

    gtk_widget_destroy(b);
    return;
}


guint32
gcolor2rgb24(GdkRGBA *color)
{
    guint32 i;

    i  = ((guint32)(color->red   * 255)) & 0xFF;
    i <<= 8;
    i |= ((guint32)(color->green * 255)) & 0xFF;
    i <<= 8;
    i |= ((guint32)(color->blue  * 255)) & 0xFF;
    DBG("i=%x\n", i);
    return i;
}

gchar *
gdk_color_to_RRGGBB(GdkRGBA *color)
{
    static gchar str[10]; // #RRGGBB + \0
    g_snprintf(str, sizeof(str), "#%02x%02x%02x",
        (int)(color->red * 255), (int)(color->green * 255), (int)(color->blue * 255));
    return str;
}

gchar *
indent(int level)
{
    static gchar *space[] = {
        "",
        "    ",
        "        ",
        "            ",
        "                ",
    };

    if (level > sizeof(space))
        level = sizeof(space);
    return space[level];
}

