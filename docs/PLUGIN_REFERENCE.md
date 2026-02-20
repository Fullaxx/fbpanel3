# fbpanel3 — Plugin Reference

This document provides a per-plugin summary: what each plugin does, which
config keys it reads, what widgets it creates, and any notable lifecycle
considerations.

For the plugin loading/unloading lifecycle see `docs/ARCHITECTURE.md §3`.
For xconf ownership rules (str vs strdup) see `docs/XCONF_REFERENCE.md §4`.

---

## Plugin API Quick Reference

Every plugin is a shared library (`.so`) that exports a `plugin_class`
struct via the `PLUGIN` macro.  The panel calls:
1. `constructor(plugin_instance *)` — plugin populates `p->pwid`
2. `destructor(plugin_instance *)` — plugin frees private state (not `pwid`)

Plugins must not call `gtk_widget_destroy(p->pwid)` — the panel owns it.

---

## Complex Plugins

### taskbar — Window Taskbar

**Files**: `plugins/taskbar/taskbar.c`, `taskbar_net.c`, `taskbar_task.c`,
`taskbar_ui.c`, `taskbar_priv.h`

**Description**: Displays a button for each open window (respecting EWMH).
Buttons can show icons, window titles, or both.

**Config keys**:
| Key | Type | Default | Description |
|---|---|---|---|
| `tooltips` | bool | true | Show window title in tooltip |
| `iconsonly` | bool | false | Show icons only, no text |
| `acceptskippager` | bool | false | Show windows with _NET_WM_STATE_SKIP_PAGER |
| `showiconified` | bool | true | Show iconified (minimised) windows |
| `showalldesks` | bool | false | Show windows from all desktops |
| `showmapped` | bool | true | Show mapped (visible) windows |
| `usemousewheel` | bool | false | Cycle windows with mouse wheel |
| `useurgencyhint` | bool | true | Flash button when window requests urgency |
| `maxtaskwidth` | int | 200 | Maximum width per task button (pixels) |

**Main widgets created**: `GtkBar` (inside `pwid`) containing one
`GtkButton` per visible window.

**Key lifecycle notes**:
- Installs a GDK filter on the root window to receive `_NET_CLIENT_LIST`,
  `_NET_ACTIVE_WINDOW`, and other EWMH property change events.
- Also connects to `FbEv` signals (`client_list`, `active_window`, etc.).
- GDK filter must be removed in the destructor.
- Task buttons hold `GdkPixbuf *` icon references; these are unreffed when
  the task is destroyed.
- Uses `GtkBar` which **requires** calling `parent_class->size_allocate` —
  see `docs/GTK_WIDGET_LIFECYCLE.md §3`.

---

### menu — Application Menu

**Files**: `plugins/menu/menu.c`, `menu.h`, `system_menu.c`

**Description**: Builds a hierarchical popup menu from the xconf config
tree or from the FreeDesktop application database.

**Config keys** (top-level):
| Key | Type | Description |
|---|---|---|
| `iconsize` | int | Icon size for menu items (pixels) |

**Per-item config** (inside menu item `{ }` blocks):
| Key | Type | Description |
|---|---|---|
| `name` | str | Display name of the menu item |
| `image` | str | Path to image file for item icon |
| `icon` | str | Named icon (from icon theme) |
| `action` | str | Shell command to execute |
| `command` | str | Special command (e.g., `logout`) |

**Main widgets created**: `GtkButton` (the menu button in `pwid`);
`GtkMenu` (popup, created lazily with a 35-second rebuild timeout).

**Key lifecycle notes**:
- Menu is rebuilt periodically via a GLib timeout.  The timeout ID must be
  removed in the destructor with `g_source_remove()`.
- xconf string ownership: item names read with `XCG(..., str)` are borrowed
  pointers.  **Must not** be `g_free`'d.  (This was the double-free bug
  fixed in v8.3.23.)
- `system_menu.c` reads `/usr/share/applications/*.desktop` files.

---

### pager — Virtual Desktop Pager

**File**: `plugins/pager/pager.c`

**Description**: Displays a miniature thumbnail of each virtual desktop,
showing window positions as rectangles.  Optionally renders the desktop
wallpaper.

**Config keys**:
| Key | Type | Default | Description |
|---|---|---|---|
| `showwallpaper` | bool | false | Show desktop wallpaper in thumbnails |

**Main widgets created**: One `GtkDrawingArea` per desktop inside a
`GtkBox` in `pwid`.

**Key lifecycle notes**:
- Installs per-window GDK filters to track window position changes.
  Filters are removed from each window in the destructor.
- Uses `cairo` to render thumbnail rectangles.
- Connects to FbEv signals for desktop count, current desktop changes.

---

### tray — System Tray (Notification Area)

**Files**: `plugins/tray/main.c`, `eggtraymanager.c`, `eggtraymanager.h`,
`fixedtip.c`, `fixedtip.h`, `egg-marshal.c`, `eggmarshalers.h`

**Description**: Implements the freedesktop.org System Tray Protocol
(XEMBED).  Manages a `_NET_SYSTEM_TRAY_S0` selection owner and accepts
dock requests from system tray icon clients.

**Config keys**: None.

**Main widgets created**: `GtkBox` (horizontal) in `pwid`; each tray icon
is a `GtkSocket` (XEMBED container).

**Key lifecycle notes**:
- `EggTrayManager` is a GObject that manages the systray manager window.
- Acquiring the `_NET_SYSTEM_TRAY_S0` selection means only one tray can
  be active per display.
- On plugin destructor, releases the selection and destroys icon sockets.
- `fixedtip` is a custom tooltip implementation for tray icons that do not
  use GTK tooltips.

---

### launchbar — Application Launcher

**File**: `plugins/launchbar/launchbar.c`

**Description**: Displays a horizontal row of icon buttons, each configured
to run a shell command when clicked.

**Per-button config** (inside `Button { }` blocks):
| Key | Type | Description |
|---|---|---|
| `image` | str | (transfer none) Path to image file |
| `icon` | str | (transfer none) Named icon from icon theme |
| `action` | str | (transfer none) Shell command to execute |
| `tooltip` | str | (transfer none) Tooltip text |

**Main widgets created**: `GtkBox` (in `pwid`) containing one `GtkButton`
per launcher entry.

**Key lifecycle notes**:
- Config strings read via `XCG(..., str)` are borrowed pointers valid only
  while the xconf tree is alive.  They are used only during construction.
- Icon theme fallback: tries named icon first, then image file path,
  then a default icon.
- `g_signal_connect` on icon-theme `"changed"` signal refreshes icons.
  Handler must be disconnected in the destructor.

---

### icons — Window Icon Override

**File**: `plugins/icons/icons.c`

**Description**: An invisible plugin that intercepts `_NET_CLIENT_LIST`
changes and replaces the icon of matching windows.  Useful for applications
that do not set their own icon.

**Per-rule config** (inside `icon { }` blocks):
| Key | Type | Description |
|---|---|---|
| `appname` | str | Match window by `WM_NAME` |
| `classname` | str | Match window by `WM_CLASS` |
| `image` | str | Path to replacement image |
| `icon` | str | Named icon to use as replacement |

**Top-level config**:
| Key | Type | Description |
|---|---|---|
| `defaulticon` | str | Default icon for windows with no icon |

**Main widgets created**: None (invisible plugin; `plugin_class.invisible = 0`
is unused here — it creates a minimal GtkBgbox but paints nothing visible).

**Key lifecycle notes**:
- Connects to FbEv `client_list` signal.
- Installs per-window GDK filter to detect `_NET_WM_ICON` property changes
  and re-apply the override.
- Filters must be removed in the destructor.

---

## Simple Plugins

### battery — Battery Usage (Graphical)

**Files**: `plugins/battery/battery.c`, `main.c`, `os_linux.c.inc`,
`power_supply.c`, `power_supply.h`

**Description**: Displays battery charge level as a graphical bar or icon.
Reads `/sys/class/power_supply/` on Linux.

**Config keys**: None exposed at top level (defaults used).

**Main widget**: `GtkDrawingArea` with a custom cairo draw handler.

---

### batterytext — Battery Usage (Text)

**File**: `plugins/batterytext/batterytext.c`

**Description**: Displays battery charge level as a text label.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `DesignCapacity` | bool | Use design capacity instead of last-full |
| `PollingTimeMs` | int | Polling interval in milliseconds |
| `TextSize` | str | CSS font size string (e.g. `"small"`) |
| `BatteryPath` | str | Path to power supply in `/sys/class/power_supply/` |

**Main widget**: `GtkLabel`.

---

### chart — Chart Base Widget

**Files**: `plugins/chart/chart.c`, `chart.h`

**Description**: Shared scrolling chart widget used by `cpu`, `net`, `mem`,
and `mem2`.  Not useful as a standalone plugin.

**Config keys**: None (used as a library by other plugins).

---

### cpu — CPU Usage Chart

**File**: `plugins/cpu/cpu.c`

**Description**: Scrolling chart of CPU usage (user + system + iowait).

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `Color` | str | Chart line colour (`#RRGGBB`) |

**Main widget**: Chart widget (from `chart.c`).

---

### dclock — Digital Clock

**File**: `plugins/dclock/dclock.c`

**Description**: Displays the current time using `strftime` format strings,
with an optional tooltip showing a secondary time format.  Clicking the
clock runs an optional action command.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `ClockFmt` | str | `strftime` format for the clock label |
| `TooltipFmt` | str | `strftime` format for the tooltip |
| `ShowSeconds` | bool | Show seconds in display |
| `HoursView` | enum | `12` or `24` hour format |
| `Action` | str | Shell command to run on click |
| `Color` | str | Label text colour (`#RRGGBB`) |

**Main widget**: `GtkLabel` updated via a 1-second `g_timeout_add`.

---

### deskno — Desktop Number (v1)

**File**: `plugins/deskno/deskno.c`

**Description**: Shows the current virtual desktop number as a text label.
Connects to FbEv `current_desktop` signal.

**Config keys**: None.

**Main widget**: `GtkLabel`.

---

### deskno2 — Desktop Number (v2)

**File**: `plugins/deskno2/deskno2.c`

**Description**: Shows the current virtual desktop number.  Variant of
`deskno` with slightly different styling.

**Config keys**: None.

**Main widget**: `GtkLabel`.

---

### genmon — Generic Monitor

**File**: `plugins/genmon/genmon.c`

**Description**: Periodically runs a shell command and displays its output
as a label in the panel.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `Command` | str | Shell command to run |
| `TextSize` | str | CSS font size string |
| `TextColor` | str | Label text colour |
| `PollingTime` | int | Polling interval (seconds) |
| `MaxTextLength` | int | Maximum characters to display |

**Main widget**: `GtkLabel`.

---

### image — Static Image

**File**: `plugins/image/image.c`

**Description**: Displays a static image file in the panel, with an optional
tooltip.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `image` | str | Path to image file |
| `tooltip` | str | Tooltip text |

**Main widget**: `GtkImage`.

---

### mem — Memory Usage (Text/Chart)

**File**: `plugins/mem/mem.c`

**Description**: Displays memory and optionally swap usage as a text label.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `ShowSwap` | bool | Also show swap usage |

**Main widget**: `GtkLabel`.

---

### mem2 — Memory Usage (Chart)

**File**: `plugins/mem2/mem2.c`

**Description**: Scrolling chart of memory and swap usage.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `MemColor` | str | Memory bar colour (`#RRGGBB`) |
| `SwapColor` | str | Swap bar colour (`#RRGGBB`) |

**Main widget**: Chart widget (from `chart.c`).

---

### meter — Meter Base Widget

**Files**: `plugins/meter/meter.c`, `meter.h`

**Description**: Shared vertical meter widget.  Used internally by battery
plugins.

---

### net — Network Usage Chart

**File**: `plugins/net/net.c`

**Description**: Scrolling chart of network interface transmit/receive rates.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `interface` | str | Network interface name (e.g. `eth0`) |
| `RxLimit` | int | Maximum receive rate (bytes/s) for scaling |
| `TxLimit` | int | Maximum transmit rate (bytes/s) for scaling |
| `TxColor` | str | Transmit chart colour (`#RRGGBB`) |
| `RxColor` | str | Receive chart colour (`#RRGGBB`) |

**Main widget**: Chart widget (from `chart.c`).

---

### separator — Separator Line

**File**: `plugins/separator/separator.c`

**Description**: Draws a vertical (or horizontal) separator line between
other panel plugins.

**Config keys**: None.

**Main widget**: `GtkSeparator`.

---

### space — Empty Space

**File**: `plugins/space/space.c`

**Description**: Occupies a configurable amount of empty space in the panel.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `size` | int | Number of pixels to occupy |

**Main widget**: `GtkDrawingArea` (invisible, sized to `size` pixels).

---

### tclock — Text Clock

**File**: `plugins/tclock/tclock.c`

**Description**: Like `dclock` but uses a different default format and
supports an optional calendar popup on click.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `ClockFmt` | str | `strftime` format for display |
| `TooltipFmt` | str | `strftime` format for tooltip |
| `Action` | str | Shell command to run on click |
| `ShowCalendar` | bool | Show calendar popup on click |
| `ShowTooltip` | bool | Show tooltip |

**Main widget**: `GtkLabel`.

---

### user — User Photo and Menu

**File**: `plugins/user/user.c`

**Description**: Displays the current user's avatar (from `/etc/faces/` or
Gravatar) and shows a pop-up menu of user actions on click.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `image` | str | Path to avatar image |
| `icon` | str | Named icon for avatar |
| `gravataremail` | str | Email for Gravatar avatar lookup |

**Main widget**: `GtkButton` with `GtkImage`.

---

### volume — OSS Volume Control

**File**: `plugins/volume/volume.c`

**Description**: Scroll wheel on the plugin adjusts the OSS (Open Sound System)
master volume.  Displays a speaker icon.

**Config keys**: None.

**Main widget**: `GtkImage`.

---

### wincmd — Show Desktop Button

**File**: `plugins/wincmd/wincmd.c`

**Description**: Sends EWMH commands to all windows (show desktop, shade,
iconify) when clicked or middle-clicked.

**Config keys**:
| Key | Type | Description |
|---|---|---|
| `Button1` | enum | Action for left click (`shade`/`iconify`/`none`) |
| `Button2` | enum | Action for middle click |
| `Icon` | str | Named icon for button |
| `Image` | str | Path to image file for button |
| `tooltip` | str | Tooltip text |

**Main widget**: `GtkButton` with icon/image.
