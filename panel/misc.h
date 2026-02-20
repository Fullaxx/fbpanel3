/**
 * @file misc.h
 * @brief fbpanel miscellaneous helpers — X11 properties, EWMH, panel geometry.
 *
 * Provides three categories of utility:
 *
 * 1. ENUM HELPERS
 *    str2num / num2str — bidirectional lookup in xconf_enum tables used for
 *    config file parsing (edge, allign, widthtype, etc.).
 *
 * 2. X11 / EWMH HELPERS
 *    Wrappers around Xlib property queries (XGetWindowProperty) that return
 *    GLib-heap-allocated strings (free with g_free / g_strfreev) or X11-heap-
 *    allocated arrays (free with XFree).  The caller must use the correct
 *    allocator — mixing them causes heap corruption.
 *
 *    Ownership rule:
 *      get_utf8_property()      → (transfer full) gchar*;   caller g_free()s
 *      get_utf8_property_list() → (transfer full) char**;   caller g_strfreev()s
 *      get_textproperty()       → (transfer full) gchar*;   caller g_free()s
 *      get_xaproperty()         → (transfer full) void*;    caller XFree()s
 *      get_net_*()              → scalar guint; no heap allocation
 *
 *    Xclimsg / Xclimsgwm send XClientMessage events — no return value.
 *
 * 3. PANEL GEOMETRY
 *    calculate_position() computes np->ax/ay/aw/ah from the panel config and
 *    the geometry of the target monitor (Xinerama head or primary monitor).
 *
 * GLOBAL ATOMS
 * ------------
 * All a_NET_xxx, a_WM_xxx, a_UTF8_STRING, a_XROOTPMAP_ID atoms are defined in misc.c
 * and declared extern in panel.h (via the panel struct and the atom macros).
 * fb_init() interns them all; fb_free() is currently a no-op.
 *
 * See also: docs/LIBRARY_USAGE.md sec.2 (Xlib), docs/MEMORY_MODEL.md sec.5 (XFree rule).
 */

#ifndef MISC_H
#define MISC_H

#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <stdio.h>

/**
 * GDK_DPY - convenience macro for the X11 Display of the default GDK display.
 *
 * Equivalent to GDK_DISPLAY_XDISPLAY(gdk_display_get_default()).
 * Used throughout misc.c and plugins instead of a cached Display* variable.
 * Valid only after gtk_init() has been called.
 */
#define GDK_DPY GDK_DISPLAY_XDISPLAY(gdk_display_get_default())

#include "panel.h"

/**
 * str2num - look up a string in an xconf_enum table and return its integer value.
 * @p:      Pointer to a NULL-sentinel xconf_enum array (last entry has str==NULL).
 * @str:    String to look up; compared case-insensitively via g_ascii_strcasecmp.
 * @defval: Value to return if @str is not found in the table.
 *
 * Returns: Matching xconf_enum::num, or @defval if not found.
 *          Does not allocate memory; @str is not modified.
 */
int str2num(xconf_enum *p, gchar *str, int defval);

/**
 * num2str - look up an integer in an xconf_enum table and return its string.
 * @p:      Pointer to a NULL-sentinel xconf_enum array.
 * @num:    Integer value to find.
 * @defval: String to return if @num is not found.
 *
 * Returns: (transfer none) pointer into the xconf_enum table; do NOT g_free().
 *          Returns @defval (also transfer none) if not found.
 */
gchar *num2str(xconf_enum *p, int num, gchar *defval);


/**
 * Xclimsg - send a 32-bit XClientMessage to the root window.
 * @win:  Target window (the window the message concerns, e.g. a client window).
 * @type: Message type Atom (e.g. a_NET_ACTIVE_WINDOW).
 * @l0–l4: Five long data values; unused slots should be 0.
 *
 * Sends the event to the root window with SubstructureNotifyMask |
 * SubstructureRedirectMask so the WM sees it.  Used for EWMH requests
 * such as activating a window or closing it.
 */
extern void Xclimsg(Window win, long type, long l0, long l1, long l2, long l3, long l4);

/**
 * Xclimsgwm - send a 32-bit XClientMessage directly to a window (WM protocol).
 * @win:  Target window (receives the event directly, not via root).
 * @type: Message type Atom (e.g. a_WM_PROTOCOLS).
 * @arg:  First data word (e.g. a_WM_DELETE_WINDOW).
 *
 * Sends the event directly to @win (not via root) with no event mask (0L),
 * using GDK_CURRENT_TIME as the timestamp in data.l[1].
 * Used for ICCCM WM_PROTOCOLS messages (e.g. polite close requests).
 */
void Xclimsgwm(Window win, Atom type, Atom arg);

/**
 * get_xaproperty - read an arbitrary X11 window property as a raw byte array.
 * @win:    X11 window whose property is read.
 * @prop:   Atom identifying the property (e.g. a_NET_WM_STATE).
 * @type:   Expected property type (e.g. XA_ATOM, XA_CARDINAL, XA_WINDOW).
 * @nitems: If non-NULL, set to the number of items returned.
 *
 * Reads up to 0x7FFFFFFF items.  The property data is allocated by Xlib.
 *
 * Returns: (transfer full) void* pointing to X11-heap data; caller must XFree()
 *          the result when done.  Returns NULL if the property does not exist,
 *          is the wrong type, or XGetWindowProperty fails.
 */
extern void *get_xaproperty (Window win, Atom prop, Atom type, int *nitems);

/**
 * get_textproperty - read an ICCCM text property and convert it to UTF-8.
 * @win:  X11 window whose property is read.
 * @prop: Atom identifying the property (e.g. a_WM_CLASS, a_NET_WM_NAME).
 *
 * Calls XGetTextProperty then gdk_text_property_to_utf8_list_for_display
 * to handle Latin-1, Compound Text, and UTF-8 encodings uniformly.
 * The raw Xlib text_prop.value buffer is XFree()'d internally.
 *
 * Returns: (transfer full) gchar* in UTF-8; caller must g_free().
 *          Returns NULL if the property is absent or conversion fails.
 */
char *get_textproperty(Window win, Atom prop);

/**
 * get_utf8_property - read a UTF-8 string window property.
 * @win:  X11 window whose property is read.
 * @atom: Atom identifying the property (e.g. a_NET_WM_NAME).
 *
 * Reads the property with type UTF8_STRING and copies the value into a
 * GLib-heap buffer via g_strndup().  The Xlib buffer is XFree()'d internally.
 *
 * Returns: (transfer full) gchar* in UTF-8; caller must g_free().
 *          Returns NULL if the property is absent, is not UTF8_STRING type,
 *          or has zero items.
 */
void *get_utf8_property(Window win, Atom atom);

/**
 * get_utf8_property_list - read a multi-string UTF-8 window property.
 * @win:   X11 window whose property is read.
 * @atom:  Atom identifying the property (e.g. a_NET_DESKTOP_NAMES).
 * @count: Output; set to the number of strings in the returned array.
 *
 * The property value is a NUL-separated sequence of UTF-8 strings.  Each
 * substring is g_strdup()'d into a NULL-terminated char** array.
 * The Xlib buffer is XFree()'d internally.
 *
 * Handles malformed properties where the last string is not NUL-terminated
 * by memmove()-shifting the trailing bytes before NUL-terminating them.
 *
 * Returns: (transfer full) char**; caller must g_strfreev() the result.
 *          Returns NULL and sets *count to 0 on failure.
 */
char **get_utf8_property_list(Window win, Atom atom, int *count);

/**
 * fb_init - initialise fbpanel's X11 atoms and icon theme.
 *
 * Interns all a_NET_xxx, a_WM_xxx, a_UTF8_STRING, a_XROOTPMAP_ID atoms via
 * XInternAtom, and caches the default GtkIconTheme* as the global
 * icon_theme.  Must be called once after gtk_init() and before any
 * X11 property query or icon load.
 */
void fb_init(void);

/**
 * fb_free - release fbpanel's global X11 resources.
 *
 * Currently a no-op: icon_theme is a borrowed reference to the default
 * theme singleton (gtk_icon_theme_get_default) and must NOT be unref'd.
 * Atoms are interned for the lifetime of the X server connection.
 */
void fb_free(void);

/**
 * get_net_number_of_desktops - read _NET_NUMBER_OF_DESKTOPS from the root window.
 *
 * Returns: Number of virtual desktops reported by the WM, or 0 if the
 *          property is absent (no EWMH WM running).
 */
guint get_net_number_of_desktops();

/**
 * get_net_current_desktop - read _NET_CURRENT_DESKTOP from the root window.
 *
 * Returns: Index (0-based) of the currently active desktop, or 0 on failure.
 */
extern guint get_net_current_desktop ();

/**
 * get_net_wm_desktop - read _NET_WM_DESKTOP for a client window.
 * @win: X11 client window.
 *
 * Returns: Desktop index on which @win resides, or 0 if the property is absent.
 *          0xFFFFFFFF (all-bits-set) conventionally means "on all desktops"
 *          (sticky), but this function does not special-case that value.
 */
extern guint get_net_wm_desktop(Window win);

/**
 * get_net_wm_state - read _NET_WM_STATE for a client window into a struct.
 * @win: X11 client window.
 * @nws: Output struct (net_wm_state); zeroed before filling.
 *
 * Reads the _NET_WM_STATE atom list and sets the corresponding boolean fields
 * in @nws: skip_pager, skip_taskbar, sticky, hidden, shaded.
 * Unknown state atoms are silently ignored.
 * XFree()s the Xlib data internally.
 */
extern void get_net_wm_state(Window win, net_wm_state *nws);

/**
 * get_net_wm_window_type - read _NET_WM_WINDOW_TYPE for a client window.
 * @win:   X11 client window.
 * @nwwt:  Output struct (net_wm_window_type); zeroed before filling.
 *
 * Reads the _NET_WM_WINDOW_TYPE atom list and sets boolean fields in @nwwt:
 * desktop, dock, toolbar, menu, utility, splash, dialog, normal.
 * Unknown type atoms are silently ignored.  XFree()s the Xlib data internally.
 */
extern void get_net_wm_window_type(Window win, net_wm_window_type *nwwt);

/**
 * calculate_position - compute the panel's screen coordinates from its config.
 * @np: Panel instance; reads config fields and writes ax/ay/aw/ah/screenRect.
 *
 * Determines the target monitor rectangle from np->xineramaHead (if valid and
 * within the current monitor count) or the primary monitor otherwise.
 * Stores the monitor rect in np->screenRect.
 *
 * For top/bottom edge panels, applies calculate_width() to the horizontal axis;
 * for left/right panels, applies calculate_width() to the vertical axis.
 * np->ah (panel height/thickness) is clamped to [PANEL_HEIGHT_MIN, PANEL_HEIGHT_MAX].
 * np->aw and np->ah are clamped to >= 1 after all calculations.
 *
 * Called from panel_start_gui() on initial layout and from panel_screen_changed()
 * (the GdkScreen::monitors-changed handler) on monitor hot-plug.
 */
void calculate_position(panel *np);

/**
 * expand_tilda - expand a leading '~' in a file path to $HOME.
 * @file: Input path; may be NULL.
 *
 * If @file starts with '~', returns g_strdup_printf("%s%s", getenv("HOME"),
 * file+1).  Otherwise returns g_strdup(@file).
 *
 * Returns: (transfer full) gchar* with the expanded path; caller must g_free().
 *          Returns NULL if @file is NULL.
 *
 * Note: does not handle "~user" expansion — only bare '~' (current user).
 */
gchar *expand_tilda(gchar *file);

/**
 * get_button_spacing - measure a GtkButton's preferred size for layout purposes.
 * @req:    Output GtkRequisition; set to the button's minimum preferred size.
 * @parent: Optional GtkContainer to temporarily add the button to (may be NULL).
 * @name:   Widget name set via gtk_widget_set_name(); used to apply CSS styles.
 *
 * Creates a temporary GtkButton, optionally adds it to @parent so CSS theming
 * is applied, measures its preferred size, then destroys it.  Used by plugins
 * to determine the overhead (border, padding) a button imposes so they can
 * size their content area accordingly.
 */
void get_button_spacing(GtkRequisition *req, GtkContainer *parent, gchar *name);

/**
 * gcolor2rgb24 - convert a GdkRGBA colour to a packed 0xRRGGBB integer.
 * @color: Input colour with red/green/blue in [0.0, 1.0].
 *
 * Each channel is multiplied by 255 and truncated to 8 bits.  The alpha
 * channel is discarded.
 *
 * Returns: guint32 in the form 0x00RRGGBB.
 */
guint32 gcolor2rgb24(GdkRGBA *color);

/**
 * gdk_color_to_RRGGBB - format a GdkRGBA as a "#RRGGBB" CSS hex string.
 * @color: Input colour.
 *
 * Formats the colour into a static 10-byte buffer as "#RRGGBB\0".
 *
 * Returns: (transfer none) pointer to a static buffer; valid only until the
 *          next call to this function.  Do NOT g_free().  Not thread-safe, but
 *          fbpanel is single-threaded so this is safe in practice.
 */
gchar *gdk_color_to_RRGGBB(GdkRGBA *color);

#include "widgets.h"


/**
 * configure - open (or raise) the panel preferences dialog.
 *
 * Defined in gconf_panel.c.  Creates or presents the configuration window
 * containing global panel settings and per-plugin configuration.
 */
void configure();

/**
 * indent - return a static string of spaces for the given indentation level.
 * @level: Indentation depth (0–4).
 *
 * Returns one of five pre-allocated static strings: "", "    ", "        ",
 * "            ", "                " (0, 4, 8, 12, 16 spaces).
 *
 * WARNING: the bounds check uses sizeof(space) instead of G_N_ELEMENTS(space),
 * making it incorrect on any platform where sizeof(gchar*) != 1.  On 64-bit
 * systems sizeof(space) == 40, so levels 5–40 bypass the clamp and return
 * out-of-bounds memory.  See BUG-013 in docs/BUGS_AND_ISSUES.md.
 *
 * Returns: (transfer none) static string; do NOT g_free().
 */
gchar *indent(int level);

/**
 * get_profile_file - open the named fbpanel profile config file.
 * @profile: Profile name (used to construct the file path under ~/.config/fbpanel/).
 * @perm:    fopen() permission string (e.g. "r", "w").
 *
 * Returns: FILE* opened with fopen(); caller must fclose() when done.
 *          Returns NULL if the file cannot be opened.
 */
FILE *get_profile_file(gchar *profile, char *perm);

#endif /* MISC_H */
