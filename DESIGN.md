# fbpanel3 — Source Code Design Reference

This document describes what every source file does and what it is responsible
for. It also documents the shared headers, global variables, and macros that
connect the codebase together.

---

## Repository layout

```
fbpanel3/
├── panel/           Core panel binary (compiled into the fbpanel executable)
├── plugins/         Plugin shared libraries (one subdirectory per plugin)
├── data/            Config files, man page templates, default XPM icon
├── po/              Gettext translation files (.po / .mo)
└── exec/            Helper shell scripts installed alongside the binary
```

---

## Core panel — `panel/`

### `panel.c` / `panel.h`  (973 / 198 lines)

The main panel binary entry point and orchestrator.

**Responsibilities:**
- Parses command-line arguments and locates the profile config file.
- Creates the top-level `GtkWindow` for the panel bar and sets its X11
  window type/strut properties so window managers treat it as a dock.
- Calls `fb_init()` to intern X11 atoms, initialise GtkIconTheme and the
  event object.
- Reads the profile config (`xconf` tree), then instantiates and starts all
  plugins listed under the `Plugin { }` blocks.
- Enters the GTK main loop.
- On exit, calls `fb_free()` and cleans up all plugin instances.

**Key types defined in `panel.h`:**

| Type | Purpose |
|------|---------|
| `panel` | Single global struct holding all panel state: geometry, GTK widgets, plugin list, orientation, monitor info |
| `net_wm_state` | Bitfield parsed from `_NET_WM_STATE` property |
| `net_wm_window_type` | Bitfield parsed from `_NET_WM_WINDOW_TYPE` |
| `xconf_enum` | Map between a string token and an integer constant used by `xconf` |

**Key globals exported from `panel.h`:**

| Symbol | Type | Meaning |
|--------|------|---------|
| `icon_theme` | `GtkIconTheme *` | The default GTK icon theme; used by all icon-loading code |
| `fbev` | `FbEv *` | The panel event bus GObject (see `ev.h`) |
| `the_panel` | `panel *` | Pointer to the single panel instance |

---

### `misc.c` / `misc.h`  (715 / 50 lines)

A collection of X11 and panel utility functions shared across the panel and
all plugins.

**Responsibilities:**
- Defines and interns all X11 atoms (`a_NET_*`, `a_WM_*`, etc.) via
  `resolve_atoms()` called from `fb_init()`.
- Provides the `GDK_DPY` macro for convenient access to the X11 `Display *`.
- X11 helpers: `Xclimsg()` (send `ClientMessage`), `Xclimsgwm()`,
  `get_xaproperty()`, `get_utf8_property()`, `get_utf8_property_list()`,
  `get_textproperty()`.
- EWMH helpers: `get_net_current_desktop()`, `get_net_wm_desktop()`,
  `get_net_wm_state()`, `get_net_wm_window_type()`, `get_net_number_of_desktops()`.
- Panel geometry: `calculate_position()` (places the panel window on screen
  according to edge/allign/margin/size config).
- String utilities: `expand_tilda()`, `indent()`.
- Color utilities: `gcolor2rgb24()`, `gdk_color_to_RRGGBB()`.
- GTK helper: `get_button_spacing()` (measures the extra space a button
  widget adds around its child, used to size taskbar buttons correctly).
- `#include "widgets.h"` — re-exports the widget API so callers only need
  `#include "misc.h"`.

---

### `widgets.c` / `widgets.h`  (327 / 30 lines)

GTK widget factory functions extracted from misc.c.

**Responsibilities:**
- `fb_pixbuf_new()` — creates a `GdkPixbuf` from an icon name and/or file
  path, with optional "missing-image" fallback.
- `fb_image_new()` — creates a `GtkImage` that automatically reloads its
  pixbuf whenever the GTK icon theme changes (connects to the `changed`
  signal of `GtkIconTheme`).
- `fb_button_new()` — creates a `GtkBgbox` containing an `fb_image`. Adds
  hover-highlight (brightened pixbuf) and a press-shrink animation.
- `fb_create_calendar()` — creates a floating, undecorated top-level window
  containing a `GtkCalendar`. Used by the `dclock` and `tclock` plugins.

---

### `plugin.c` / `plugin.h`  (224 / 143 lines)

Plugin registry and lifecycle management.

**Responsibilities:**
- Maintains a global registry of `plugin_class` descriptors (both built-in
  and dynamically loaded).
- `class_register()` / `class_unregister()` — called automatically by the
  `ctor()`/`dtor()` shared-library constructor/destructor macros defined in
  `plugin.h`.
- `plugin_load()` — allocates `priv_size` bytes for a plugin instance, sets
  the standard `plugin_instance` fields (`panel`, `xc`, `pwid`), then calls
  the plugin's `constructor`.
- `plugin_put()` / `plugin_stop()` — call the plugin's `destructor` and free
  the instance.

**Key types (see `plugin.h` for full documentation):**

| Type | Purpose |
|------|---------|
| `plugin_class` | Static descriptor: type string, name, priv_size, constructor/destructor function pointers |
| `plugin_instance` | Per-instance state: pointer to class, panel, xconf subtree, pwid GtkWidget |

The `PLUGIN` preprocessor macro (defined when compiling a plugin) expands to
shared-library constructor/destructor functions that call `class_register` /
`class_unregister` automatically on `dlopen`/`dlclose`.

---

### `panel.h` — also declares EWMH Atoms

All X11 atoms are declared `extern Atom a_NET_*` in `panel.h` and defined in
`misc.c:resolve_atoms()`. Any file that includes `panel.h` has access to the
full atom set.

---

### `ev.c` / `ev.h`  (294 / 79 lines)

The EWMH event bus.

**Responsibilities:**
- Defines `FbEv`, a custom `GObject` subclass that acts as a GObject signal
  bus for panel-wide EWMH events.
- Receives X11 `PropertyNotify` events on the root window via a GDK event
  filter and translates them into GObject signals:
  `current_desktop`, `active_window`, `number_of_desktops`, `client_list`,
  `desktop_names`.
- Plugins connect to these signals to be notified of desktop/window changes
  without each needing their own root-window filter.

The global `fbev` (declared in `panel.h`) is the single `FbEv` instance.

---

### `bg.c` / `bg.h`  (300 / 66 lines)

Root window background reading.

**Responsibilities:**
- Reads the current desktop wallpaper from the `_XROOTPMAP_ID` X11 property.
- Provides a `cairo_surface_t *` of the root window so that pseudo-transparent
  panel widgets can sample the correct background pixels.

---

### `gtkbgbox.c` / `gtkbgbox.h`  (354 / 72 lines)

A custom GTK widget for pseudo-transparent panel backgrounds.

**Responsibilities:**
- `GtkBgbox` is a `GtkEventBox` subclass.
- Overrides the GTK3 `draw` vfunc to paint its background with a
  `cairo_surface_t` tiled from the root window (via `FbBg`), optionally
  tinted with a colour and alpha.
- `gtk_bgbox_set_background()` sets the background mode: solid colour,
  tinted root pixmap, or inherited from parent.

---

### `gtkbar.c` / `gtkbar.h`  (239 / 75 lines)

A flow-wrapping layout container for the taskbar and launchbar.

**Responsibilities:**
- `GtkBar` is a `GtkBox` subclass.
- Arranges children in rows/columns, wrapping to the next row when the panel
  dimension changes (e.g. when the taskbar gets taller during size-allocate).
- `gtk_bar_set_dimension()` sets the current number of columns (horizontal)
  or rows (vertical) to use.

---

### `xconf.c` / `xconf.h`  (395 / 52 lines)

The config file parser.

**Responsibilities:**
- Parses fbpanel's whitespace-delimited, brace-nested config format.
- `xconf_find()` / `XCG()` macro — type-safe helpers to read typed values
  (int, string, enum) from the parsed tree.
- The parsed tree is passed to each plugin as `plugin_instance.xc`.

---

### `gconf.c` / `gconf.h`  (235 / 27 lines)
### `gconf_panel.c`  (420 lines)
### `gconf_plugins.c`  (120 lines)

The GTK preferences dialog ("Configure Panel").

**Responsibilities:**
- `gconf.c` — dialog skeleton and shared helpers.
- `gconf_panel.c` — the "Panel" tab: edge, alignment, size, transparency,
  font/colour options, autohide.
- `gconf_plugins.c` — the "Plugins" tab: plugin list, add/remove, reorder.

---

### `run.c` / `run.h`  (44 / 9 lines)

Shell command execution helper.

**Responsibilities:**
- `run_app()` — spawns a shell command asynchronously via `g_spawn_command_line_async()`.
  Used by launchbar and menu plugins.

---

### `dbg.h`  (30 lines)

Debug/trace macro definitions.

**Responsibilities:**
- `DBG(fmt, ...)` — prints a debug message with file/line prefix when
  `DEBUGPRN` is `#define`d in the including file; otherwise expands to nothing.
- `ERR(fmt, ...)` — same as `DBG` but always prints (goes to stderr).
- `DBGE(fmt, ...)` — debug variant that continues on same line (no newline added).

Including `dbg.h` is optional. Files that have no debug logging at all do
not include it.

---

## Plugin system — `plugins/`

Each plugin is a standalone shared library. The panel loads them via
`GModule` (dlopen). Plugins export a single `plugin_class *class_ptr` symbol
and call `class_register()` on load via the `PLUGIN` macro in `plugin.h`.

All plugins include `"plugin.h"` (which includes `"panel.h"` and `"misc.h"`).

### `plugins/taskbar/` — Window Taskbar

The most complex plugin. Split into four source files with a shared internal
header:

| File | Responsibility |
|------|---------------|
| `taskbar_priv.h` | Internal header: `task` and `taskbar_priv` structs, constants, cross-file forward declarations |
| `taskbar.c` (113 lines) | Plugin lifecycle: `taskbar_constructor`, `taskbar_destructor`, `plugin_class` |
| `taskbar_task.c` (509 lines) | Task object lifecycle: alloc/free, name management, icon loading (cairo-xlib, NETWM icon, WM hints), flash/urgency |
| `taskbar_net.c` (233 lines) | EWMH event handlers: `tb_net_client_list`, `tb_net_active_window`, `tb_net_current_desktop`, `tb_propertynotify`, `tb_event_filter`, `net_active_detect` |
| `taskbar_ui.c` (445 lines) | Widget construction: `tk_build_gui` (per-task button), `taskbar_build_gui` (bar container), context menu (`tb_make_menu`), all GTK event callbacks |

The taskbar also uses `cairo-xlib` (linked separately in CMakeLists) to
read X11 `Pixmap` data for legacy WM_HINTS icons.

---

### `plugins/launchbar/` — Application Launcher

A row of icon buttons. Each button runs a configured command on click and
accepts file/URL drops (drag-and-drop).

---

### `plugins/menu/` — Application Menu

Reads a config file defining a hierarchical menu of application launchers.

| File | Responsibility |
|------|---------------|
| `menu.c` | Plugin lifecycle, menu widget construction from xconf config |
| `system_menu.c` | Reads a freedesktop.org `.menu` file and builds the system applications submenu |
| `menu.h` | Shared types between menu.c and system_menu.c |

---

### `plugins/pager/` — Virtual Desktop Pager

Draws a miniature representation of all virtual desktops and the windows
on each. Allows clicking to switch desktops. Uses `gdk-pixbuf-xlib` to
composite window thumbnails.

---

### `plugins/tray/` — System Tray (XEMBED)

Implements the freedesktop.org System Tray Protocol.

| File | Responsibility |
|------|---------------|
| `main.c` | Plugin lifecycle; creates the tray socket widget |
| `eggtraymanager.c/.h` | Core XEMBED tray manager (adapted from GNOME's libegg) |
| `fixedtip.c/.h` | Custom tooltip window for tray icons |
| `eggmarshalers.c/.h` / `eggmarshalers.c.inc` | GObject signal marshallers (auto-generated) |

---

### `plugins/battery/` — Battery Level (meter-based)

Reads battery status from `/sys/class/power_supply` (preferred) or legacy
`/proc/acpi/battery`.

| File | Responsibility |
|------|---------------|
| `battery.c` | Plugin lifecycle, polls OS; uses meter API to display level |
| `main.c` | Entry point / plugin class registration |
| `os_linux.c.inc` | Linux-specific battery reading (included by battery.c) |
| `power_supply.c/.h` | Parses `/sys/class/power_supply` device entries |

---

### `plugins/batterytext/` — Battery Level (text)

Same battery data as `battery` plugin but renders as a text label with
percentage and charging indicator rather than an icon meter.

---

### `plugins/meter/` — Generic Meter Widget

A reusable helper plugin (not loaded directly by users). Provides
`meter_class` which wraps a `GtkImage` that cycles through a named set of
icon theme icons to represent a 0–100 level. Used as a base by `battery`.

---

### `plugins/cpu/` — CPU Usage

Polls `/proc/stat`, computes CPU load percentage, and renders a `chart`
(bar graph) widget.

---

### `plugins/chart/` — Bar Graph Widget

Reusable bar graph widget. Renders a scrolling histogram of sampled values
into a `GtkDrawingArea` using cairo.

---

### `plugins/mem/` — Memory Usage (bar)

Polls `/proc/meminfo`, renders a `chart` of RAM usage.

---

### `plugins/mem2/` — Memory Usage (text)

Same source as `mem` but renders as a text label.

---

### `plugins/net/` — Network Traffic

Polls `/proc/net/dev`, renders upload/download rates as a dual `chart`.

---

### `plugins/dclock/` — Digital Clock

Renders a `GtkLabel` with strftime-formatted time. Clicking opens the
`fb_create_calendar()` floating calendar window.

---

### `plugins/tclock/` — Analog Clock

Draws an analog clock face onto a `GtkDrawingArea` using cairo.

---

### `plugins/icons/` — Desktop Icons (window list icons)

Renders the icons of all open windows as a compact row. Uses
`gdk-pixbuf-xlib` to read `_NET_WM_ICON` and `WM_HINTS` icon data.

---

### `plugins/deskno/` — Desktop Number

Shows the current virtual desktop number (and optionally its name) as a
formatted `GtkLabel`. Clicking cycles to the next desktop.

---

### `plugins/deskno2/`

Variant of `deskno` with a different label format.

---

### `plugins/image/` — Static Image

Displays a static icon (from the icon theme or a file path). No interactive
behaviour.

---

### `plugins/separator/` — Separator Line

Draws a vertical or horizontal separator line to visually divide plugin
groups on the panel bar.

---

### `plugins/space/` — Spacer

An invisible expanding widget used to push other plugins to one side of the
panel.

---

### `plugins/volume/` — Audio Volume Control

Reads and sets the ALSA master volume. Renders current volume level;
scroll-wheel adjusts the volume.

---

### `plugins/wincmd/` — Window Commands

Sends EWMH window commands (shade, iconify, close) to windows matching a
configurable X11 class hint pattern.

---

### `plugins/genmon/` — Generic Monitor

Runs an external command at a configurable interval and displays its
stdout output as a label. A simple way to add custom status text.

---

### `plugins/user/` — User Info

Displays the current user name and optionally a hostname label.

---

## Headers that glue the codebase together

### `panel/panel.h` — The central shared header

Every plugin and panel source file includes this. It defines:

- `panel` struct (entire panel state)
- `net_wm_state`, `net_wm_window_type` structs (EWMH property bitfields)
- `xconf_enum` (string↔int mapping for config parsing)
- All `extern Atom a_NET_*` declarations
- `extern GtkIconTheme *icon_theme`
- `extern panel *the_panel`
- `extern FbEv *fbev`
- `extern GtkIconTheme *icon_theme`
- `FBPANEL_WIN(xid)` macro — tests whether an X11 window ID belongs to the
  panel itself (used in taskbar to skip the panel window)
- `c_()` macro — marks a string for gettext translation

### `panel/misc.h` — X11 / utility API

Included by all plugins that need X11 property access or panel geometry.
Also re-exports `widgets.h` via `#include "widgets.h"` so callers get
`fb_pixbuf_new`, `fb_image_new`, `fb_button_new`, `fb_create_calendar`
without a separate include.

**Key macro defined here:**
```c
#define GDK_DPY  GDK_DISPLAY_XDISPLAY(gdk_display_get_default())
```
Used everywhere a raw `Display *` is needed for X11 calls.

### `panel/plugin.h` — Plugin API

Defines `plugin_class` and `plugin_instance`, the complete plugin contract.
Also defines the `PLUGIN` macro that plugins use to auto-register on dlopen.

See the comment block at the top of `plugin.h` for full lifecycle
documentation and a minimal example.

### `panel/xconf.h` — Config parser API + `XCG()` macro

The `XCG(xc, "key", &field, type)` macro is used in every plugin
constructor to read typed values from the plugin's config subtree:
```c
XCG(xc, "maxtaskwidth", &tb->task_width_max, int);
XCG(xc, "iconsonly",    &tb->icons_only,     enum, bool_enum);
XCG(xc, "action",       &action,             str);
```

### `panel/dbg.h` — Debug trace macros

`DBG()` and `ERR()` are used throughout. Compiled out (no-op) unless
`#define DEBUGPRN` is active in the including file.

### `plugins/taskbar/taskbar_priv.h` — Taskbar-internal shared header

Used only within `plugins/taskbar/`. Defines `task` and `taskbar_priv`
structs and all constants shared across the four taskbar source files.
Not part of the public plugin API.

### `plugins/chart/chart.h` — Chart widget API

Used by `cpu`, `mem`, `net` plugins to share the same bar-graph rendering
widget without code duplication.

### `plugins/meter/meter.h` — Meter widget API

Used by the `battery` plugin to drive the icon-based level meter widget.

---

## Atom naming convention

All X11 atom variables follow the pattern:
- `a_NET_*` — EWMH `_NET_*` atoms (e.g. `a_NET_WM_DESKTOP`)
- `a_WM_*` — ICCCM `WM_*` atoms (e.g. `a_WM_DELETE_WINDOW`)
- `a_KDE_*` — KDE-specific atoms

All are defined in `misc.c:resolve_atoms()` and declared extern in `panel.h`.
