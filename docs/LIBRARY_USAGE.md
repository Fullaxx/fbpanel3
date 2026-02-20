# fbpanel3 — Library Usage

This document describes how each external library is used in fbpanel3,
where the abstraction boundaries are, and which panel files are responsible
for each layer.

---

## 1. Layer Diagram

```
┌─────────────────────────────────────────────────────┐
│              Application Logic                       │
│   panel/panel.c, panel/plugin.c, plugins/*.c         │
├─────────────────────────────────────────────────────┤
│              Plugin API                              │
│   panel/plugin.h, panel/plugin.c                    │
├─────────────────────────────────────────────────────┤
│              Panel Core                              │
│   xconf, ev, bg, gtkbgbox, gtkbar, widgets, misc    │
├─────────────────────────────────────────────────────┤
│              GTK3 / GObject / GLib                   │
│   gtk, gdk, glib, gobject, gmodule, gio             │
├─────────────────────────────────────────────────────┤
│              Cairo                                   │
│   cairo, cairo-xlib                                 │
├─────────────────────────────────────────────────────┤
│              X11 (Xlib)                              │
│   Display, Window, Atom, Pixmap, GC, XEvent         │
└─────────────────────────────────────────────────────┘
```

fbpanel3 bypasses GTK's abstraction in several places to reach directly into
GDK-X11 or Xlib — this is necessary for EWMH (Extended Window Manager Hints)
and transparent background support.

---

## 2. Xlib (X11)

### What it provides
- Raw X11 window management, property access, and event dispatch.
- Atoms, Windows (XIDs), Pixmaps, GCs.

### Where it is used
| File | Usage |
|---|---|
| `panel/misc.c` | `XGetWindowProperty`, `XSendEvent`, `XTranslateCoordinates` |
| `panel/bg.c` | `XGetWindowProperty` (root pixmap), `XCreateGC`, `XFreeGC`, `XGetGeometry` |
| `panel/ev.c` | `XGetWindowProperty` (EWMH atom values) |
| `panel/panel.c` | `XInternAtom` (all `a_NET_*` atoms), root-window GDK filter |
| `plugins/taskbar/` | `XGetWindowProperty`, `XSendEvent`, window state queries |
| `plugins/pager/` | Per-window GDK filter, `XGetWindowProperty` |
| `plugins/tray/` | XEMBED protocol, `XSendEvent` |

### Key rules
- **X11 heap → `XFree()`**: any data returned by `XGetWindowProperty()` or
  similar Xlib calls must be freed with `XFree()`, not `g_free()` or `free()`.
- **GDK_DPY macro**: `GDK_DISPLAY_XDISPLAY(gdk_display_get_default())` — use
  this to get the `Display *` without storing it.
- **GDK_ROOT_WINDOW()**: `gdk_x11_get_default_root_xwindow()` — the X11
  root window XID.
- **XID vs GdkWindow**: Xlib uses `Window` (a `long`); GDK uses `GdkWindow`.
  Convert with `GDK_WINDOW_XID(gdk_win)` and `FBPANEL_WIN(xid)` (a macro in
  `panel.h` wrapping `gdk_x11_window_lookup_for_display()`).

### Atoms
All `a_NET_*` atoms are interned once at startup in `fb_init()` and declared
`extern` in `panel.h`.  Never intern atoms in plugin code — use the globals.

---

## 3. GDK (GIMP Drawing Kit)

GDK is GTK3's portability layer between the application and the windowing
system.  In fbpanel3, GDK is used for:

- **GDK window filters**: `gdk_window_add_filter()` / `gdk_window_remove_filter()`
  install raw `XEvent` handlers at the GDK level.
- **X11-GDK bridge**: `gdk_x11_*` functions to cross between GDK and Xlib
  types (`gdk_x11_window_lookup_for_display()`, `GDK_WINDOW_XID()`, etc.).
- **Screen information**: `gdk_screen_get_monitor_geometry()`,
  `gdk_display_get_n_monitors()` for multi-head layout.
- **GdkRGBA**: colour representation, converted to `guint32` via
  `gcolor2rgb24()` in `misc.c`.

### GDK Filter pattern

```c
// Install a GDK filter on a specific window (GTK3-correct approach):
gdk_window_add_filter(gdk_win, my_filter, user_data);

// In destructor:
gdk_window_remove_filter(gdk_win, my_filter, user_data);

// Filter function signature:
GdkFilterReturn my_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data);
```

The deprecated `gdk_window_add_filter(NULL, ...)` (global filter) was removed
in v8.3.22 in favour of per-window filters.

---

## 4. GTK3

GTK3 provides the widget toolkit.  fbpanel3 targets GTK 3.0+ (tested to 3.24).

### What it provides
- Widget hierarchy, event routing, CSS styling.
- `GtkWindow`, `GtkBox`, `GtkLabel`, `GtkButton`, `GtkImage`, etc.
- `GtkStyleContext` and CSS providers for theming.
- `GtkIconTheme` for named icon lookup.

### Custom GTK3 widgets in fbpanel3
| Widget | Base class | Purpose |
|---|---|---|
| `GtkBgbox` | `GtkBin` | Background painting (wallpaper, CSS), used for panel bbox and all plugin pwid containers |
| `GtkBar` | `GtkBox` | Fixed-height task button container used by the taskbar plugin |

### CSS theming
Plugins and the panel itself apply CSS via:
```c
GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
// The panel installs a provider at PRIORITY_FALLBACK for dark background
// fallback when no wallpaper is set.
```

### GtkIconTheme
The global `GtkIconTheme *icon_theme` (declared in `panel.h`, initialised
at startup) is used by multiple plugins for loading named icons:
```c
GdkPixbuf *pb = gtk_icon_theme_load_icon(icon_theme, name, size,
                                          GTK_ICON_LOOKUP_USE_BUILTIN, NULL);
```

### Important GTK3 deprecation boundary
fbpanel3 maintains **zero deprecated-API warnings** (enforced by the build).
Key deprecated patterns that were removed:
- `gtk_misc_set_alignment` → `gtk_widget_set_halign` / `gtk_widget_set_valign`
- `gtk_widget_modify_bg` → CSS
- `GdkColor` → `GdkRGBA`
- `gdk_window_add_filter(NULL, ...)` → per-window `gdk_window_add_filter`

---

## 5. GLib / GObject

GLib is the foundational C utility library.  GObject is GLib's object system.

### GLib usage in fbpanel3
| Feature | Usage |
|---|---|
| `g_new0`, `g_free` | Memory allocation |
| `g_strdup`, `g_strdup_printf` | String duplication |
| `g_strfreev` | Free NULL-terminated string arrays |
| `GSList` | Singly-linked list for xconf sons |
| `GList` | Doubly-linked list for plugin list |
| `g_signal_connect/disconnect` | GObject signals |
| `g_timeout_add` / `g_source_remove` | Timer callbacks |
| `g_source_remove` | Cancel a timer |
| `GModule` | Dynamic plugin loading (`gmodule-2.0`) |
| `g_type_register_static` | Register custom GObject types |
| `G_DEFINE_TYPE_WITH_CODE` | Macro for GObject subclassing |

### GObject signals in fbpanel3
Both `FbBg` and `FbEv` are custom GObject types that emit typed signals.
Plugins connect to these signals to receive EWMH state changes and
background change notifications.

```c
// FbBg emits "changed" when the root pixmap changes:
g_signal_connect(priv->bg, "changed", G_CALLBACK(on_bg_changed), widget);

// FbEv emits per-event signals (fb_ev_trigger calls g_signal_emit):
g_signal_connect(fbev, "current_desktop", G_CALLBACK(on_desktop_change), priv);
```

---

## 6. GModule (dynamic loading)

GModule wraps `dlopen`/`dlsym`/`dlclose` portably.  fbpanel3 uses it to
load plugin `.so` files at runtime.

```c
GModule *module = g_module_open(path, G_MODULE_BIND_LAZY);
// The module's __attribute__((constructor)) fires during g_module_open,
// calling class_register(class_ptr) automatically.
```

The panel binary is linked with `-Wl,--export-dynamic` so plugin `.so` files
can resolve symbols from the panel binary (e.g., `class_register`, `fbev`,
`icon_theme`, all `a_NET_*` atoms, `calculate_position`, etc.).

`g_module_close()` is called when a plugin is unloaded (not yet fully
implemented — most plugins live for the panel's lifetime).

---

## 7. Cairo / cairo-xlib

Cairo is the 2D vector graphics library used for background rendering and
some plugin drawing (taskbar buttons, pager).

### Cairo surface types used in fbpanel3

| Surface type | Created by | Purpose |
|---|---|---|
| `cairo_xlib_surface` | `cairo_xlib_surface_create()` in `bg.c` | Wrap the X11 root pixmap for reading |
| `cairo_image_surface` (RGB24) | `cairo_image_surface_create()` | CPU-side copy of the root pixmap (FbBg cache); plugin background slices |

### The cairo-xlib boundary

Direct `cairo_xlib_surface_create()` calls are confined to `panel/bg.c`.
All other code receives CPU-side `cairo_image_surface_t` pointers.

This boundary matters because:
- `cairo_xlib_surface` operations require an X connection and a drawable to
  exist for the surface's lifetime.
- `cairo_image_surface` is CPU-only, can be safely stored and passed around.

### Key cairo pattern in fbpanel3

```c
// Create an xlib surface to READ the root pixmap (temporary):
cairo_surface_t *xlib = cairo_xlib_surface_create(dpy, pixmap, visual, w, h);

// Blit it into a CPU image surface (persistent cache):
cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
cairo_t *cr = cairo_create(img);
cairo_set_source_surface(cr, xlib, 0, 0);
cairo_paint(cr);
cairo_destroy(cr);
cairo_surface_destroy(xlib);   // xlib surface no longer needed

// Later: crop the cached image for a plugin's area:
cairo_surface_t *slice = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
cr = cairo_create(slice);
cairo_set_source_surface(cr, img, -px, -py);
cairo_paint(cr);
cairo_destroy(cr);
// slice is returned to the caller (transfer full)
```

---

## 8. libintl (gettext)

Internationalisation uses GNU gettext:
```c
#include <libintl.h>
#define _(String) gettext(String)
```

Translation catalogues are in `po/` for French (`fr_FR`), Italian (`it_IT`),
and Russian (`ru_RU`).

---

## 9. Distinguishing GDK-level from X11-level

A common source of confusion is knowing whether an operation is GDK-level
or X11-level.  Use this heuristic:

| If it uses... | It is... | Notes |
|---|---|---|
| `GdkWindow *` | GDK level | Managed by GDK, ref-counted |
| `Window` (a `long`) | X11 level | Raw XID, no GDK tracking |
| `GdkEvent *` | GDK level | GDK's event wrapper |
| `XEvent *` | X11 level | Raw Xlib event |
| `gdk_window_*()` | GDK level | — |
| `XGet*Property()`, `XSend*()` | X11 level | Memory freed with `XFree` |
| `gdk_x11_*()` | Bridge | Converts between GDK and X11 types |
| `GDK_WINDOW_XID(w)` | Bridge | GdkWindow → Window (XID) |
| `FBPANEL_WIN(xid)` | Bridge | Window → GdkWindow (lookup) |
