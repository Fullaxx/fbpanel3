/**
 * @file misc.c
 * @brief fbpanel miscellaneous helpers — X11 atoms, EWMH queries, geometry (implementation).
 *
 * This file has three main responsibilities:
 *
 * 1. GLOBAL ATOM TABLE
 *    Defines and interns all X11 atoms used by the panel and its plugins.
 *    resolve_atoms() calls XInternAtom for each atom once at startup (fb_init).
 *    Atoms are valid for the lifetime of the X server connection and do not
 *    need to be freed.
 *
 *    Note: a_NET_WM_DESKTOP had a duplicate tentative definition (BUG-010, fixed).
 *
 * 2. X11 PROPERTY HELPERS
 *    All property queries use XGetWindowProperty.  The Xlib-allocated buffer
 *    returned by XGetWindowProperty is always XFree()'d internally; callers
 *    receive either:
 *      - A GLib-heap copy (g_strndup / g_strdup / g_new0) -> caller g_free()s
 *      - A raw Xlib pointer (get_xaproperty only) -> caller XFree()s
 *
 * 3. PANEL GEOMETRY
 *    calculate_position() and its helper calculate_width() convert the panel
 *    config (edge, allign, widthtype, xmargin, ymargin) into screen pixel
 *    coordinates using the GDK monitor API.
 *
 * ENUM TABLES
 * -----------
 * allign_enum, edge_enum, widthtype_enum, heighttype_enum, bool_enum,
 * pos_enum, layer_enum are defined here and used by panel.c and plugins
 * to map config file strings to integer constants via str2num/num2str.
 *
 * See also: docs/LIBRARY_USAGE.md sec.2 (Xlib/GDK boundary),
 *           docs/MEMORY_MODEL.md sec.5 (XFree vs g_free rule).
 */


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

/** Cached default GtkIconTheme; set in fb_init(); NOT ref'd (borrowed singleton). */
GtkIconTheme *icon_theme;

/* -------------------------------------------------------------------------
 * Global X11 Atom cache
 * All atoms are interned once by resolve_atoms() and remain valid for the
 * lifetime of the X server connection.
 * ------------------------------------------------------------------------- */

/** UTF8_STRING — ICCCM extended string encoding type. */
Atom a_UTF8_STRING;
/** _XROOTPMAP_ID — root window property set by wallpaper setters (e.g. feh, nitrogen). */
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
Atom a_NET_WM_NAME;
Atom a_NET_WM_VISIBLE_NAME;
Atom a_NET_WM_STRUT;
Atom a_NET_WM_STRUT_PARTIAL;
Atom a_NET_WM_ICON;
Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;

/* -------------------------------------------------------------------------
 * xconf_enum tables
 * Used by str2num/num2str for config file parsing.
 * Each table is terminated by a sentinel entry with str == NULL.
 * ------------------------------------------------------------------------- */

/** Alignment values for the panel along its edge (left / right / center). */
xconf_enum allign_enum[] = {
    { .num = ALLIGN_LEFT, .str = c_("left") },
    { .num = ALLIGN_RIGHT, .str = c_("right") },
    { .num = ALLIGN_CENTER, .str = c_("center")},
    { .num = 0, .str = NULL },
};

/** Edge values for panel placement (left / right / top / bottom). */
xconf_enum edge_enum[] = {
    { .num = EDGE_LEFT, .str = c_("left") },
    { .num = EDGE_RIGHT, .str = c_("right") },
    { .num = EDGE_TOP, .str = c_("top") },
    { .num = EDGE_BOTTOM, .str = c_("bottom") },
    { .num = 0, .str = NULL },
};

/**
 * Width type: request (shrink to content), pixel (fixed px), percent (% of screen).
 * The desc field is used in the preferences dialog combo box.
 */
xconf_enum widthtype_enum[] = {
    { .num = WIDTH_REQUEST, .str = "request" , .desc = c_("dynamic") },
    { .num = WIDTH_PIXEL, .str = "pixel" , .desc = c_("pixels") },
    { .num = WIDTH_PERCENT, .str = "percent", .desc = c_("% of screen") },
    { .num = 0, .str = NULL },
};

/** Height type: currently only "pixel" is supported. */
xconf_enum heighttype_enum[] = {
    { .num = HEIGHT_PIXEL, .str = c_("pixel") },
    { .num = 0, .str = NULL },
};

/** Boolean: "false" → 0, "true" → 1. */
xconf_enum bool_enum[] = {
    { .num = 0, .str = "false" },
    { .num = 1, .str = "true" },
    { .num = 0, .str = NULL },
};

/** Plugin position within the panel box: none / start / end. */
xconf_enum pos_enum[] = {
    { .num = POS_NONE, .str = "none" },
    { .num = POS_START, .str = "start" },
    { .num = POS_END,  .str = "end" },
    { .num = 0, .str = NULL},
};

/** Window layer preference: above / below (normal is the default). */
xconf_enum layer_enum[] = {
    { .num = LAYER_ABOVE, .str = c_("above") },
    { .num = LAYER_BELOW, .str = c_("below") },
    { .num = 0, .str = NULL},
};


/**
 * str2num - look up a string in an xconf_enum table and return its integer value.
 * @p:      NULL-sentinel xconf_enum array.
 * @str:    String to look up (case-insensitive).
 * @defval: Value to return if not found.
 *
 * Returns: Matching num, or @defval if the string is absent from the table.
 */
int
str2num(xconf_enum *p, gchar *str, int defval)
{
    for (;p && p->str; p++) {
        if (!g_ascii_strcasecmp(str, p->str))
            return p->num;
    }
    return defval;
}

/**
 * num2str - look up an integer in an xconf_enum table and return its string.
 * @p:      NULL-sentinel xconf_enum array.
 * @num:    Integer to look up.
 * @defval: String to return if not found.
 *
 * Returns: (transfer none) pointer into the enum table, or @defval.
 *          Do NOT g_free() the result.
 */
gchar *
num2str(xconf_enum *p, int num, gchar *defval)
{
    for (;p && p->str; p++) {
        if (num == p->num)
            return p->str;
    }
    return defval;
}


/**
 * resolve_atoms - intern all X11 atoms used by fbpanel.
 *
 * Called once from fb_init().  XInternAtom with create=False returns None if
 * the atom does not yet exist on the server; with create=True (False here means
 * "don't create" is FALSE, i.e. DO create).  Wait — actually False passed to
 * XInternAtom as only_if_exists means "create if not found", so all atoms are
 * guaranteed to be interned.
 *
 * Note: _NET_CLOSE_WINDOW and several other atoms are interned in resolve_atoms
 * via the global declarations but the a_NET_CLOSE_WINDOW variable is set in
 * panel.c where Xclimsg is called.  The atom table here covers everything
 * needed by misc.c callers; panel.c may intern additional atoms separately.
 */
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


/**
 * fb_init - initialise X11 atoms and the global icon theme cache.
 *
 * Must be called once after gtk_init() and before any X11 or icon operations.
 * Sets the global icon_theme to gtk_icon_theme_get_default() (borrowed ref —
 * do NOT g_object_unref; see fb_free).
 */
void fb_init()
{
    resolve_atoms();
    icon_theme = gtk_icon_theme_get_default();
}

/**
 * fb_free - no-op cleanup for fbpanel globals.
 *
 * icon_theme is a borrowed reference from gtk_icon_theme_get_default() and
 * must NOT be g_object_unref()'d.  Atoms are server-side and need no cleanup.
 */
void fb_free()
{
    // MUST NOT be ref'd or unref'd
    // g_object_unref(icon_theme);
}

/**
 * Xclimsg - send a 32-bit XClientMessage to the root window.
 * @win:  Window the message concerns (appears in xev.window).
 * @type: Message type Atom (e.g. a_NET_ACTIVE_WINDOW).
 * @l0–l4: Data words; fill unused slots with 0.
 *
 * Sends to the root window (GDK_ROOT_WINDOW()) with
 * SubstructureNotifyMask | SubstructureRedirectMask so the WM intercepts it.
 * Used for EWMH client-to-WM requests (activate, close, change desktop, etc.).
 */
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

/**
 * Xclimsgwm - send a 32-bit XClientMessage directly to a window.
 * @win:  Target window (receives the event; used for WM_PROTOCOLS messages).
 * @type: Message type Atom (e.g. a_WM_PROTOCOLS).
 * @arg:  First data word (e.g. a_WM_DELETE_WINDOW).
 *
 * Uses GDK_CURRENT_TIME as the timestamp in data.l[1] (ICCCM requirement).
 * Sent with event mask 0L (not via the root window redirect path).
 */
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


/**
 * get_utf8_property - read a UTF-8 string window property.
 * @win:  Window to query.
 * @atom: Property atom.
 *
 * Reads with type=UTF8_STRING, format=8.  The Xlib buffer is XFree()'d
 * after g_strndup() copies it to the GLib heap.
 *
 * Returns: (transfer full) gchar*; caller g_free()s.  NULL on failure.
 */
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

/**
 * get_utf8_property_list - read a NUL-separated UTF-8 string list property.
 * @win:   Window to query.
 * @atom:  Property atom (e.g. a_NET_DESKTOP_NAMES).
 * @count: Output; number of strings in the returned array.
 *
 * Reads the entire property value as a flat buffer of NUL-separated strings.
 * Each substring is g_strdup()'d into a newly allocated char** array.
 * Handles the edge case where the last string is not NUL-terminated by
 * memmove()-shifting it one byte back and adding a NUL.
 *
 * The Xlib buffer is XFree()'d internally.  The returned char** is allocated
 * with g_new0 and the strings with g_strdup — caller must g_strfreev().
 *
 * Returns: (transfer full) char**; NULL on failure.  *count is 0 on failure.
 */
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
            /* Last string not NUL-terminated: shift it one byte back into the
             * NUL that terminated the previous string, then NUL-terminate. */
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

/**
 * get_xaproperty - read any X11 window property as a raw Xlib-heap buffer.
 * @win:    Window to query.
 * @prop:   Property atom.
 * @type:   Expected type atom (e.g. XA_CARDINAL, XA_ATOM, XA_WINDOW).
 * @nitems: If non-NULL, set to the item count on success.
 *
 * Reads up to 0x7FFFFFFF items.  Unlike get_utf8_property, the return value
 * is NOT copied to the GLib heap — it points directly into X11 memory.
 *
 * Returns: (transfer full) void*; caller must XFree() the result.
 *          NULL if the property is absent or XGetWindowProperty fails.
 */
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

/**
 * text_property_to_utf8 - convert an XTextProperty to a UTF-8 string.
 * @prop: XTextProperty to convert (may be Latin-1, Compound Text, or UTF-8).
 *
 * Delegates to gdk_text_property_to_utf8_list_for_display().  Takes
 * ownership of list[0] (the first converted string) and frees the rest of
 * the list with g_strfreev() after replacing list[0] with an empty string.
 *
 * Returns: (transfer full) gchar* UTF-8 string; caller must g_free().
 *          NULL if conversion produces zero strings.
 */
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

/**
 * get_textproperty - read an ICCCM text property and return it as UTF-8.
 * @win:  Window to query.
 * @atom: Property atom (e.g. a_WM_NAME, a_WM_CLASS).
 *
 * Reads via XGetTextProperty (ICCCM), converts via text_property_to_utf8
 * (handles all ICCCM string encodings), XFree()s text_prop.value internally.
 *
 * Returns: (transfer full) gchar* UTF-8 string; caller g_free()s.
 *          NULL if the property is absent or conversion fails.
 */
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


/**
 * get_net_number_of_desktops - read _NET_NUMBER_OF_DESKTOPS from the root window.
 *
 * Queries the root window's _NET_NUMBER_OF_DESKTOPS XA_CARDINAL property.
 * XFree()s the Xlib buffer internally.
 *
 * Returns: Number of virtual desktops, or 0 if the property is absent.
 */
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


/**
 * get_net_current_desktop - read _NET_CURRENT_DESKTOP from the root window.
 *
 * Returns: Active desktop index (0-based), or 0 if absent.
 */
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

/**
 * get_net_wm_desktop - read _NET_WM_DESKTOP for a client window.
 * @win: Client window.
 *
 * Returns: Desktop index, or 0 if the property is absent.
 *          0xFFFFFFFF means "all desktops" (sticky) per EWMH spec but is
 *          not special-cased here — callers must check that value themselves.
 */
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

/**
 * get_net_wm_state - read _NET_WM_STATE atoms for a client window.
 * @win: Client window.
 * @nws: Output; zeroed (bzero) then filled with matching boolean flags.
 *
 * Reads the _NET_WM_STATE XA_ATOM list.  Recognised atoms:
 *   _NET_WM_STATE_SKIP_PAGER    → nws->skip_pager
 *   _NET_WM_STATE_SKIP_TASKBAR  → nws->skip_taskbar
 *   _NET_WM_STATE_STICKY        → nws->sticky
 *   _NET_WM_STATE_HIDDEN        → nws->hidden
 *   _NET_WM_STATE_SHADED        → nws->shaded
 *
 * Unrecognised atoms are silently ignored.  XFree()s the Xlib buffer.
 */
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




/**
 * get_net_wm_window_type - read _NET_WM_WINDOW_TYPE atoms for a client window.
 * @win:   Client window.
 * @nwwt:  Output; zeroed then filled with boolean flags.
 *
 * Reads the _NET_WM_WINDOW_TYPE XA_ATOM list.  Recognised atoms:
 *   _NET_WM_WINDOW_TYPE_DESKTOP → nwwt->desktop
 *   _NET_WM_WINDOW_TYPE_DOCK    → nwwt->dock
 *   _NET_WM_WINDOW_TYPE_TOOLBAR → nwwt->toolbar
 *   _NET_WM_WINDOW_TYPE_MENU    → nwwt->menu
 *   _NET_WM_WINDOW_TYPE_UTILITY → nwwt->utility
 *   _NET_WM_WINDOW_TYPE_SPLASH  → nwwt->splash
 *   _NET_WM_WINDOW_TYPE_DIALOG  → nwwt->dialog
 *   _NET_WM_WINDOW_TYPE_NORMAL  → nwwt->normal
 *
 * XFree()s the Xlib buffer.
 */
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




/**
 * calculate_width - compute the panel's position along one axis.
 * @scrw:    Screen dimension in pixels along the panel's edge (width or height).
 * @wtype:   WIDTH_PERCENT, WIDTH_PIXEL, or WIDTH_REQUEST.
 * @allign:  ALLIGN_LEFT, ALLIGN_RIGHT, or ALLIGN_CENTER.
 * @xmargin: Margin in pixels from the alignment edge.
 * @panw:    In/out: panel extent in pixels.  For WIDTH_PERCENT, converted from
 *           percentage to pixels.  Clamped to [0, scrw].
 * @x:       In/out: panel origin offset; adjusted based on @allign and @xmargin.
 *
 * Note: for WIDTH_PERCENT with allign != ALLIGN_CENTER, xmargin is silently
 * ignored (the clamping line is commented out).  This may be unintentional.
 * See BUG-011 in docs/BUGS_AND_ISSUES.md.
 */
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


/**
 * calculate_position - compute the panel's pixel geometry from config and monitor.
 * @np: Panel; reads xineramaHead, edge, widthtype, allign, xmargin, ymargin,
 *      width, height; writes ax, ay, aw, ah, screenRect.
 *
 * Monitor selection:
 *   If np->xineramaHead is valid (not FBPANEL_INVALID_XINERAMA_HEAD) and within
 *   the current monitor count, uses that monitor's geometry.  Otherwise falls
 *   back to the primary monitor (gdk_display_get_primary_monitor).
 *
 * Geometry calculation:
 *   Top/bottom edge: calculate_width applies to the horizontal axis.
 *     np->aw = panel width; np->ah = panel height (clamped to
 *     [PANEL_HEIGHT_MIN, PANEL_HEIGHT_MAX]).
 *     np->ax = computed X; np->ay = ymargin (top) or ssheight-ah-ymargin (bottom).
 *   Left/right edge: calculate_width applies to the vertical axis (roles swapped).
 *     np->ah = panel height; np->aw = panel width (thickness).
 *     np->ay = computed Y; np->ax = ymargin (left) or sswidth-aw-ymargin (right).
 *
 *   np->aw and np->ah are clamped to >= 1 after all calculations to prevent
 *   zero-size windows.
 */
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



/**
 * expand_tilda - expand a leading '~' in a file path to $HOME.
 * @file: Input path; may be NULL.
 *
 * Only bare '~' (current user) is expanded.  "~user" expansion is not
 * supported.  Uses getenv("HOME") — may return NULL on minimal systems.
 *
 * Returns: (transfer full) gchar*; caller must g_free().  NULL if @file is NULL.
 */
gchar *
expand_tilda(gchar *file)
{
    if (!file)
        return NULL;
    RET((file[0] == '~') ?
        g_strdup_printf("%s%s", getenv("HOME"), file+1)
        : g_strdup(file));

}


/**
 * get_button_spacing - probe a GtkButton's minimum preferred size.
 * @req:    Output; set to the button's minimum preferred size (includes
 *          internal padding and border imposed by the current GTK theme).
 * @parent: Optional container to temporarily parent the button (may be NULL).
 *          Parenting lets the button inherit CSS context for accurate sizing.
 * @name:   Widget name applied via gtk_widget_set_name(); used for CSS matching.
 *
 * Creates a temporary GtkButton, measures it, then destroys it.  The caller
 * uses the measured size to account for per-button overhead when computing
 * content area dimensions.
 */
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


/**
 * gcolor2rgb24 - convert a GdkRGBA colour to a packed 0xRRGGBB integer.
 * @color: Input; red/green/blue in [0.0, 1.0]; alpha is discarded.
 *
 * Each channel is multiplied by 255 and truncated to 8 bits.
 * Used by plugins and GtkBgbox to convert GTK colour values to the
 * 0xRRGGBB format expected by fb_pixbuf_make_back_image.
 *
 * Returns: guint32 packed as 0x00RRGGBB.
 */
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

/**
 * gdk_color_to_RRGGBB - format a GdkRGBA as a "#RRGGBB" CSS hex string.
 * @color: Input colour.
 *
 * Writes into a static 10-byte buffer — not re-entrant and invalidated by
 * the next call.  Safe in practice because fbpanel is single-threaded.
 * See BUG-012 in docs/BUGS_AND_ISSUES.md.
 *
 * Returns: (transfer none) pointer to static buffer; do NOT g_free().
 */
gchar *
gdk_color_to_RRGGBB(GdkRGBA *color)
{
    static gchar str[10]; // #RRGGBB + \0
    g_snprintf(str, sizeof(str), "#%02x%02x%02x",
        (int)(color->red * 255), (int)(color->green * 255), (int)(color->blue * 255));
    return str;
}

/**
 * indent - return a static indentation string for the given depth.
 * @level: Indentation level; clamped to [0, 4].
 *
 * Returns one of five pre-allocated static strings (0, 4, 8, 12, 16 spaces).
 *
 * Returns: (transfer none) static string; do NOT g_free().
 */
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

    if (level < 0 || level >= (int)G_N_ELEMENTS(space))
        level = G_N_ELEMENTS(space) - 1;
    return space[level];
}
