/**
 * @file panel.h
 * @brief fbpanel core types, enums, atoms, and public API.
 *
 * This header is the central include for the entire codebase.  It defines:
 *
 *   - Panel layout enums (ALLIGN, EDGE, WIDTH, HEIGHT, POS, LAYER)
 *   - The panel struct with all geometry, config, and widget fields
 *   - EWMH helper structs (net_wm_state, net_wm_window_type)
 *   - Extern declarations for all globally-interned X11 atoms
 *   - Extern declarations for global singletons (fbev, icon_theme, etc.)
 *   - Public panel API (panel_set_wm_strut, ah_start, ah_stop, etc.)
 *
 * STARTUP AND LIFETIME
 * --------------------
 * One panel struct is allocated per run of the main loop:
 *   main() -> panel_start() -> panel_parse_global() -> panel_start_gui()
 * After gtk_main() returns, panel_stop() tears everything down,
 * then g_free(p) releases the struct.
 *
 * If SIGUSR1 arrives, gtk_main() returns with force_quit==0 and the loop
 * restarts (hot-reload).  SIGUSR2 sets force_quit=1, which exits cleanly.
 *
 * See also: docs/ARCHITECTURE.md sec.1 (startup), docs/MEMORY_MODEL.md sec.2.
 */

#ifndef PANEL_H
#define PANEL_H


#include <X11/Xlib.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include <libintl.h>
/** gettext translation macro. */
#define _(String) gettext(String)
/** Compile-time string constant; skips gettext lookup (used in enum tables). */
#define c_(String) String

#include "config.h"

#include "bg.h"
#include "ev.h"
#include "xconf.h"

/* -------------------------------------------------------------------------
 * Layout enums
 * All values start at 0; used with str2num/num2str and the *_enum tables
 * defined in misc.c.
 * ------------------------------------------------------------------------- */

/** Panel alignment along its edge. */
enum { ALLIGN_CENTER, ALLIGN_LEFT, ALLIGN_RIGHT  };

/** Edge of the screen the panel occupies. */
enum { EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT, EDGE_TOP };

/** How the panel's width (or height for vertical panels) is specified. */
enum { WIDTH_PERCENT, WIDTH_REQUEST, WIDTH_PIXEL };

/** How the panel's thickness is specified. */
enum { HEIGHT_PIXEL, HEIGHT_REQUEST };

/** Plugin position within panel->box (for future use; currently not fully implemented). */
enum { POS_NONE, POS_START, POS_END };

/** Autohide state machine states (used in GdkWindow visibility tracking). */
enum { HIDDEN, WAITING, VISIBLE };

/** Desktop layer preference for the panel window. */
enum { LAYER_ABOVE, LAYER_BELOW };

/** Default panel thickness in pixels if not specified in config. */
#define PANEL_HEIGHT_DEFAULT  26
/** Maximum allowed panel thickness (clamped in panel_parse_global). */
#define PANEL_HEIGHT_MAX      200
/** Minimum allowed panel thickness (clamped in panel_parse_global). */
#define PANEL_HEIGHT_MIN      16

/** Prefix for built-in image assets (e.g. icons bundled with fbpanel). */
#define IMGPREFIX  DATADIR "/images"

/**
 * panel - the central panel instance struct.
 *
 * One instance is allocated per main-loop iteration (g_new0) in main().
 * All widget, config, and runtime state is stored here.
 *
 * WIDGET OWNERSHIP (all widgets owned by GTK parent-child tree):
 *   topgwin owns bbox (via gtk_container_add)
 *   bbox owns lbox
 *   lbox owns box
 *   box owns plugin pwids (added by plugin_start)
 *   menu is an independent widget; destroyed explicitly in panel_stop()
 *
 * LIFETIME:
 *   Allocated: main() -> g_new0(panel,1)
 *   Initialised: panel_parse_global() sets all config fields
 *   GUI created: panel_start_gui() creates widget tree
 *   Running: gtk_main()
 *   Torn down: panel_stop() -> plugin_stop/put, gtk_widget_destroy(topgwin)
 *   Freed: xconf_del(p->xc, FALSE); g_free(p)
 */
typedef struct _panel
{
    GtkWidget *topgwin;       /**< Top-level GtkWindow (DOCK hint, no decorations).
                               *   Valid from panel_start_gui(); destroyed in panel_stop().
                               *   Owns bbox as its single child. */
    Window topxwin;           /**< X11 window ID of topgwin; valid after gtk_widget_realize()
                               *   in panel_start_gui(). Used for XChangeProperty, XSelectInput. */
    GtkWidget *lbox;          /**< Primary layout GtkBox (horizontal or vertical depending on edge).
                               *   Child of bbox; owned by bbox. Contains box as its child. */
    GtkWidget *bbox;          /**< GtkBgbox filling topgwin (widget name "panel-bg").
                               *   Child of topgwin; owned by topgwin.
                               *   In transparent mode, holds a BG_ROOT background. */
    GtkWidget *box;           /**< Plugin container GtkBox; child of lbox.
                               *   All plugin pwids are packed into this box. */
    GtkWidget *menu;          /**< Context menu (right-click, Ctrl+click).
                               *   Independent widget (not in widget tree); destroyed
                               *   explicitly by gtk_widget_destroy(p->menu) in panel_stop(). */
    GtkRequisition requisition; /**< Unused; kept for potential future use. */
    GtkWidget *(*my_box_new) (GtkOrientation orientation, gint spacing);
                              /**< Box factory: hbox_new (horizontal) or vbox_new (vertical),
                               *   chosen in panel_parse_global() based on panel edge. */
    GtkWidget *(*my_separator_new) ();
                              /**< Separator factory: vseparator_new or hseparator_new,
                               *   matching my_box_new orientation. */

    FbBg *bg;                 /**< FbBg singleton ref; acquired with fb_bg_get_for_display()
                               *   when transparent=1 in panel_start_gui().  NOT directly
                               *   unref'd in panel_stop() -- the bbox widget's finalize
                               *   releases the ref when topgwin is destroyed. */
    int alpha;                /**< Background tint opacity 0-255 (0 = no tint). */
    guint32 tintcolor;        /**< Tint colour as packed 0xRRGGBB (computed from gtintcolor). */
    GdkRGBA gtintcolor;       /**< Tint colour as GdkRGBA (parsed from tintcolor_name). */
    gchar *tintcolor_name;    /**< (transfer none) Raw pointer into xconf tree via
                               *   XCG(xc,"tintcolor",&p->tintcolor_name,str).
                               *   Do NOT g_free(). */

    int ax, ay, aw, ah;      /**< Desired geometry: X, Y, width, height (set by
                               *   calculate_position(); applied with gtk_window_move/resize).
                               *   ah is used as the minimum-height floor for gtk_widget_set_size_request. */
    int cx, cy, cw, ch;      /**< Current geometry as last reported by GDK configure-event.
                               *   Used by panel_configure_event() to detect size/position changes. */
    int allign, edge;         /**< Alignment (ALLIGN_xxx) and edge (EDGE_xxx) from config. */
    int xmargin, ymargin;     /**< Margins in pixels from the alignment edge and from screen edge. */
    GtkOrientation orientation; /**< GTK_ORIENTATION_HORIZONTAL (top/bottom) or VERTICAL (left/right).
                               *   Derived from edge in panel_parse_global(). */
    int widthtype, width;     /**< Width type (WIDTH_xxx) and value from config. */
    int heighttype, height;   /**< Height type (always HEIGHT_PIXEL after parse; see BUG-014)
                               *   and panel thickness in pixels. */
    int round_corners_radius; /**< Corner rounding radius in pixels (if round_corners set). */
    int max_elem_height;      /**< Maximum height for plugin content areas.  Defaults to
                               *   p->height when unconfigured (see panel_parse_global). */

    int xineramaHead;         /**< Xinerama/RandR monitor index from --xineramaHead flag,
                               *   or FBPANEL_INVALID_XINERAMA_HEAD (-1) for primary monitor. */
    GdkRectangle screenRect;  /**< Geometry of the target monitor (set by calculate_position).
                               *   x/y = monitor origin; width/height = monitor size. */

    gint self_destroy;        /**< Unused flag; kept for potential use. */
    gint setdocktype;         /**< If non-zero, sets GDK_WINDOW_TYPE_HINT_DOCK on topgwin. */
    gint setstrut;            /**< If non-zero, calls panel_set_wm_strut() to reserve screen space. */
    gint round_corners;       /**< If non-zero, applies rounded-corner shape mask to topgwin. */
    gint transparent;         /**< If non-zero, uses BG_ROOT background (root pixmap slice). */
    gint autohide;            /**< If non-zero, activates autohide (mouse-proximity show/hide). */
    gint ah_far;              /**< Autohide: non-zero when mouse is "far enough" from panel. */
    gint layer;               /**< Layer preference: LAYER_ABOVE or LAYER_BELOW. */
    gint setlayer;            /**< If non-zero, applies the layer hint to topgwin. */

    int ah_dx, ah_dy;         /**< Autohide: pixel offsets to shift panel off-screen when hiding. */
    int height_when_hidden;   /**< Panel thickness (in pixels) while autohide is hidden state. */
    guint hide_tout;          /**< Unused; autohide timer IDs are in static mwid/hpid. */

    int spacing;              /**< Pixel spacing between plugin widgets in box. */

    gulong monitors_sid;      /**< GdkScreen::monitors-changed signal handler ID.
                               *   Connected in panel_start_gui(); disconnected in panel_stop(). */

    guint desknum;            /**< Number of virtual desktops (from _NET_NUMBER_OF_DESKTOPS). */
    guint curdesk;            /**< Current desktop index (from _NET_CURRENT_DESKTOP). */
    guint32 *workarea;        /**< Reserved; workarea query is commented out. */

    int plug_num;             /**< Number of plugins loaded (not actively maintained). */
    GList *plugins;           /**< List of plugin_instance*; each freed by delete_plugin
                               *   in panel_stop().  After panel_stop, this is NULL. */

    gboolean (*ah_state)(struct _panel *);
                              /**< Function pointer to the active autohide state handler:
                               *   ah_state_visible, ah_state_waiting, or ah_state_hidden. */

    xconf *xc;                /**< (transfer full) Root xconf node from xconf_new_from_file().
                               *   Freed after panel_stop() via xconf_del(p->xc, FALSE). */
} panel;


/**
 * net_wm_state - _NET_WM_STATE bitfield parsed from an X11 client window.
 *
 * Populated by get_net_wm_state() (misc.c).  All fields are 1-bit booleans
 * set when the corresponding _NET_WM_STATE_xxx atom appears in the property.
 */
typedef struct {
    unsigned int modal : 1;
    unsigned int sticky : 1;
    unsigned int maximized_vert : 1;
    unsigned int maximized_horz : 1;
    unsigned int shaded : 1;
    unsigned int skip_taskbar : 1;  /**< Window requests to be excluded from taskbar. */
    unsigned int skip_pager : 1;    /**< Window requests to be excluded from pager. */
    unsigned int hidden : 1;
    unsigned int fullscreen : 1;
    unsigned int above : 1;
    unsigned int below : 1;
} net_wm_state;

/**
 * net_wm_window_type - _NET_WM_WINDOW_TYPE bitfield parsed from an X11 client window.
 *
 * Populated by get_net_wm_window_type() (misc.c).  All fields are 1-bit booleans
 * set when the corresponding _NET_WM_WINDOW_TYPE_xxx atom appears in the property.
 */
typedef struct {
    unsigned int desktop : 1;
    unsigned int dock : 1;
    unsigned int toolbar : 1;
    unsigned int menu : 1;
    unsigned int utility : 1;
    unsigned int splash : 1;
    unsigned int dialog : 1;
    unsigned int normal : 1;
} net_wm_window_type;

/**
 * command - a named callback entry in the commands[] table.
 * @name: Command name string (used for menu labels, etc.).
 * @cmd:  Callback invoked when the command is selected.
 */
typedef struct {
    char *name;
    void (*cmd)(void);
} command;

/** Global command table; defined in gconf_panel.c. */
extern command commands[];

/** Currently active profile name; set from --profile argument. */
extern gchar *cprofile;

/* -------------------------------------------------------------------------
 * X11 Atom externs
 * All atoms are interned by fb_init() -> resolve_atoms() at startup.
 * Valid for the lifetime of the X server connection; never need freeing.
 * ------------------------------------------------------------------------- */

extern Atom a_UTF8_STRING;
extern Atom a_XROOTPMAP_ID;

extern Atom a_WM_STATE;
extern Atom a_WM_CLASS;
extern Atom a_WM_DELETE_WINDOW;
extern Atom a_WM_PROTOCOLS;
extern Atom a_NET_WORKAREA;
extern Atom a_NET_CLIENT_LIST;
extern Atom a_NET_CLIENT_LIST_STACKING;
extern Atom a_NET_NUMBER_OF_DESKTOPS;
extern Atom a_NET_CURRENT_DESKTOP;
extern Atom a_NET_DESKTOP_NAMES;
extern Atom a_NET_DESKTOP_GEOMETRY;
extern Atom a_NET_ACTIVE_WINDOW;
extern Atom a_NET_CLOSE_WINDOW;
extern Atom a_NET_SUPPORTED;
extern Atom a_NET_WM_STATE;
extern Atom a_NET_WM_STATE_SKIP_TASKBAR;
extern Atom a_NET_WM_STATE_SKIP_PAGER;
extern Atom a_NET_WM_STATE_STICKY;
extern Atom a_NET_WM_STATE_HIDDEN;
extern Atom a_NET_WM_STATE_SHADED;
extern Atom a_NET_WM_STATE_ABOVE;
extern Atom a_NET_WM_STATE_BELOW;

/**
 * _NET_WM_STATE action constants (sent as l[0] in an Xclimsg ClientMessage).
 * Not atoms — plain integer values per the EWMH spec.
 */
#define a_NET_WM_STATE_REMOVE        0    /* remove/unset property */
#define a_NET_WM_STATE_ADD           1    /* add/set property */
#define a_NET_WM_STATE_TOGGLE        2    /* toggle property  */

extern Atom a_NET_WM_WINDOW_TYPE;
extern Atom a_NET_WM_WINDOW_TYPE_DESKTOP;
extern Atom a_NET_WM_WINDOW_TYPE_DOCK;
extern Atom a_NET_WM_WINDOW_TYPE_TOOLBAR;
extern Atom a_NET_WM_WINDOW_TYPE_MENU;
extern Atom a_NET_WM_WINDOW_TYPE_UTILITY;
extern Atom a_NET_WM_WINDOW_TYPE_SPLASH;
extern Atom a_NET_WM_WINDOW_TYPE_DIALOG;
extern Atom a_NET_WM_WINDOW_TYPE_NORMAL;

extern Atom a_NET_WM_DESKTOP;
extern Atom a_NET_WM_NAME;
extern Atom a_NET_WM_VISIBLE_NAME;
extern Atom a_NET_WM_STRUT;
extern Atom a_NET_WM_STRUT_PARTIAL;
extern Atom a_NET_WM_ICON;
extern Atom a_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR;


/* -------------------------------------------------------------------------
 * xconf_enum table externs
 * Defined in misc.c; used by XCG with the 'enum' specifier.
 * ------------------------------------------------------------------------- */
extern xconf_enum allign_enum[];
extern xconf_enum edge_enum[];
extern xconf_enum widthtype_enum[];
extern xconf_enum heighttype_enum[];
extern xconf_enum bool_enum[];
extern xconf_enum pos_enum[];
extern xconf_enum layer_enum[];

/** Legacy verbosity flag; not actively used (log_level replaces it). */
extern int verbose;
/** Set to 1 by SIGUSR2 / panel_destroy_event to prevent main-loop restart. */
extern gint force_quit;
/** FbEv singleton; created in panel_start(), unref'd in panel_stop(). */
extern FbEv *fbev;
/** Default GTK icon theme; set in fb_init(); borrowed ref -- do NOT unref. */
extern GtkIconTheme *icon_theme;

/**
 * FBPANEL_WIN - look up the GdkWindow for an X11 window ID.
 * @win: X11 Window to look up.
 *
 * Returns (transfer none) the GdkWindow* associated with @win on the default
 * display, or NULL if not managed by GDK.  Used by plugins (tray, taskbar)
 * to check whether an X11 window is a GDK-managed window.
 */
#define FBPANEL_WIN(win)  gdk_x11_window_lookup_for_display(gdk_display_get_default(), win)

/**
 * panel_set_wm_strut - set _NET_WM_STRUT_PARTIAL and _NET_WM_STRUT on topgwin.
 * @p: Panel instance.
 *
 * Reserves screen space for the panel by setting the strut properties on
 * p->topxwin.  The strut direction and magnitude are derived from p->edge,
 * p->aw/ah, and p->ymargin.  No-ops if the window is not yet mapped or if
 * p->autohide is set (autohiding panels do not need a strut).
 */
void panel_set_wm_strut(panel *p);

/**
 * panel_get_profile - return the active profile name.
 *
 * Returns: (transfer none) static string; do NOT g_free().
 */
gchar *panel_get_profile(void);

/**
 * panel_get_profile_file - return the full path to the active profile config file.
 *
 * Returns: (transfer none) string allocated in main(); do NOT g_free().
 */
gchar *panel_get_profile_file(void);

/**
 * ah_start - begin autohide behaviour for the panel.
 * @p: Panel with autohide=1.
 *
 * Starts the mouse-watch timer (g_timeout_add every PERIOD ms) and
 * transitions to ah_state_visible immediately.  Should be called after
 * the window is mapped (from panel_mapped callback).
 */
void ah_start(panel *p);

/**
 * ah_stop - stop all autohide timers.
 * @p: Panel instance.
 *
 * Removes both the mouse-watch timer (mwid) and the hide-delay timer (hpid)
 * if they are active.  Safe to call when autohide is not running.
 */
void ah_stop(panel *p);

/** Sentinel value for p->xineramaHead indicating "use primary monitor". */
#define FBPANEL_INVALID_XINERAMA_HEAD (-1)

#endif /* PANEL_H */
