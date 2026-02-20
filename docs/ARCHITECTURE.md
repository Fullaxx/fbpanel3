# fbpanel3 — Architecture Overview

## 1. High-Level Structure

fbpanel3 is a single-process GTK3 desktop panel.  The binary acts as both
the executable **and** as a shared library: it is linked with
`-Wl,--export-dynamic` so that dynamically-loaded plugin `.so` files can
call symbols from the panel core at runtime (via `gmodule`).

```
fbpanel binary  ←→  gtk_main() event loop
       ↕
 plugin .so files  (dlopen'd by GModule; call back into panel symbols)
```

The overall structure is:
```
main()
 ├─ gtk_init()          — initialise GTK3
 ├─ fb_init()           — intern X11 atoms, create FbEv singleton
 ├─ xconf_new_from_file() — parse ~/.config/fbpanel/<profile>
 ├─ panel_start_gui()   — allocate panel struct, build GTK widget tree,
 │    ├─ load each plugin (plugin_load / plugin_start)
 │    └─ gtk_widget_show_all()
 ├─ gtk_main()          — GTK event loop (runs until force_quit)
 └─ panel_stop()        — destructor chain, restart loop if needed
```

---

## 2. Widget Hierarchy

Every visual element in the panel lives in this containment tree:

```
GtkWindow  (panel->topgwin)
  type_hint = GDK_WINDOW_TYPE_HINT_DOCK
  resizable = FALSE
  └─ GtkBgbox  (panel->bbox)
       CSS name "panel-bg"; has_window=TRUE
       Draws the panel background (wallpaper / tint / CSS)
       └─ GtkBox  (panel->lbox, horizontal or vertical)
            orientation = p->orientation
            └─ GtkBgbox  (panel->bbox itself, same pointer)
                 └─ GtkBox  (panel->box)
                      spacing = p->spacing
                      ├─ GtkBgbox  plugin[0]->pwid
                      │    └─ [plugin-specific child widgets]
                      ├─ GtkBgbox  plugin[1]->pwid
                      │    └─ [plugin-specific child widgets]
                      └─ ...
```

> **Note:** The `panel->lbox` and `panel->bbox` relationship is slightly
> interleaved in the code — `bbox` is the direct child of `topgwin`, and
> `box` is the child of `bbox`.  The key point is that **every plugin
> receives a `GtkBgbox` (`pwid`) that the panel creates and owns**.

---

## 3. Plugin Loading Lifecycle

```
panel_start_gui()
  └─ for each plugin config block:
       plugin_load(type)
         └─ class_get(type)
              if not found → try GModule:
                g_module_open(".../fbpanel/<type>.so")
                → module ctor() fires → class_register(class_ptr)
         └─ allocate priv_size bytes (first sizeof(plugin_instance) = public)
         └─ create GtkBgbox pwid, add to panel->box
       plugin_start(instance)
         └─ instance->class->constructor(instance)
              plugin populates pwid with its own children

panel_stop()
  └─ for each loaded plugin:
       plugin_stop(instance)
         └─ instance->class->destructor(instance)
              plugin frees private resources
              (must NOT destroy pwid — panel owns it)
       plugin_put(instance)
         └─ g_object_unref(pwid) triggers GTK recursive destroy
```

Plugin `.so` files define a file-scope `static plugin_class *class_ptr`
and use the `PLUGIN` macro (in `plugin.h`) which installs
`__attribute__((constructor))` and `__attribute__((destructor))` handlers
that call `class_register` / `class_unregister` automatically on
`dlopen` / `dlclose`.

---

## 4. Event System (FbEv)

FbEv is a GObject singleton that carries EWMH (Extended Window Manager
Hints) state and emits GObject signals when that state changes.

```
X11 PropertyNotify on root window
  → panel_event_filter()  (GDK root-window filter installed at startup)
    → identifies which _NET_* atom changed
    → fb_ev_trigger(fbev, EV_<signal>)
      → g_signal_emit() on the FbEv singleton
        → all connected plugin callbacks fire
```

Signals carried by FbEv:

| Signal constant              | Fired when                              |
|------------------------------|-----------------------------------------|
| `EV_CURRENT_DESKTOP`         | `_NET_CURRENT_DESKTOP` changes          |
| `EV_NUMBER_OF_DESKTOPS`      | `_NET_NUMBER_OF_DESKTOPS` changes       |
| `EV_DESKTOP_NAMES`           | `_NET_DESKTOP_NAMES` changes            |
| `EV_ACTIVE_WINDOW`           | `_NET_ACTIVE_WINDOW` changes            |
| `EV_CLIENT_LIST`             | `_NET_CLIENT_LIST` changes              |
| `EV_CLIENT_LIST_STACKING`    | `_NET_CLIENT_LIST_STACKING` changes     |

FbEv caches the queried values and invalidates them when the corresponding
signal fires; accessors (`fb_ev_current_desktop()`, etc.) re-fetch from X11
on the next call after invalidation.

---

## 5. Background Rendering

Transparent panel backgrounds work via the FbBg / GtkBgbox pair:

```
_XROOTPMAP_ID property on root window
  → FbBg reads the Pixmap ID
  → fb_bg_ensure_cache():
       creates cairo_xlib_surface for the Pixmap
       copies it to a CPU cairo_image_surface (bg->cache)
  → fb_bg_get_xroot_pix_for_win(bg, widget):
       translates widget coords to root coords
       crops the cached image to the widget area
       returns a new cairo_image_surface (caller owns it)
  → GtkBgbox stores the surface in priv->pixmap
  → gtk_bgbox_draw() paints priv->pixmap, then tint overlay, then children
```

When the wallpaper changes (`fb_bg_notify_changed_bg()`), FbBg emits the
`"changed"` GObject signal.  All GtkBgbox instances connected to that
signal call `gtk_bgbox_set_background()` to refresh their cached slice.

Background modes (`BG_*` enum in `gtkbgbox.h`):

| Mode         | Behaviour                                                    |
|--------------|--------------------------------------------------------------|
| `BG_NONE`    | No background set yet (transient state during init)          |
| `BG_STYLE`   | GTK3 CSS style context draws the background                  |
| `BG_ROOT`    | Wallpaper slice + optional tint via cairo                    |
| `BG_INHERIT` | Inherit from parent widget (currently a stub)                |

---

## 6. Screen Resize Handling

fbpanel responds to screen layout changes in two ways:

1. **`GdkScreen::monitors-changed`** — fired by GDK when the X11 RandR
   extension reports a monitor change.  Does not require a window manager.
   Handler: `panel_screen_changed()` in `panel.c`.
   Action: recalculate `screenRect`, call `calculate_position()`, move/resize
   the panel window, reset the WM strut.

2. **`_NET_DESKTOP_GEOMETRY` atom change** — fired by a window manager
   (via EWMH) when the desktop size changes.
   Detected by `panel_event_filter()` in `panel.c`.
   Action: sets `force_quit = RESTART` → exits `gtk_main()` → panel restarts.

The `monitors_sid` field in the `panel` struct holds the GLib signal
handler ID for the `monitors-changed` connection so it can be disconnected
in `panel_stop()`.

---

## 7. Config System

Config files use a simple block/key=value text format parsed into an
**xconf** tree (see `docs/XCONF_REFERENCE.md`).

```
Global config:  ~/.config/fbpanel/<profile>
Typical layout:
  Global {
      edge       = bottom
      widthtype  = percent
      width      = 100
      ...
  }
  Plugin {
      type = taskbar
      Config {
          ...
      }
  }
  Plugin {
      type = dclock
      ...
  }
```

The panel reads the root `xconf *xc`, stores it in `panel->xc`, and passes
each `Plugin {}` sub-tree to the corresponding `plugin_instance->xc` so
each plugin reads only its own config keys via the `XCG` macro.

Config is written back via `xconf_save_to_profile()` when the user closes
the Preferences dialog.

---

## 8. Autohide

When `panel->autohide` is set, the panel uses two GLib timeout callbacks:

- **`ah_state`** — function pointer to the current hide-state handler
  (`HIDDEN`, `WAITING`, or `VISIBLE`).
- **`hide_tout`** — GLib timeout source ID (removed with `g_source_remove`
  in `ah_stop()`).

The panel window is not actually unmapped — it is moved off-screen by
`ah_dx` / `ah_dy` pixels, leaving `height_when_hidden` pixels visible.

---

## 9. Key Files at a Glance

| File                  | Role                                                        |
|-----------------------|-------------------------------------------------------------|
| `panel/panel.c`       | `main()`, `panel_start_gui()`, X11 event filter, restart loop |
| `panel/panel.h`       | `panel` struct, atom externs, enum constants                |
| `panel/plugin.c`      | Plugin loader, class registry, `plugin_load/start/stop/put` |
| `panel/plugin.h`      | `plugin_class`, `plugin_instance`, `PLUGIN` macro           |
| `panel/xconf.c/.h`    | Config tree: parse, get, set, save                          |
| `panel/ev.c/.h`       | FbEv GObject: EWMH state cache and signals                  |
| `panel/bg.c/.h`       | FbBg GObject: root pixmap cache for transparent backgrounds |
| `panel/gtkbgbox.c/.h` | GtkBgbox widget: background painting + child allocation     |
| `panel/gtkbar.c/.h`   | GtkBar widget: fixed-height task button container           |
| `panel/misc.c/.h`     | X11 helpers, position calculation, colour utilities         |
| `panel/widgets.c/.h`  | Widget factory: calendar popup, image buttons               |
| `panel/gconf*.c`      | Preferences dialog (GTK3 UI for editing panel config)       |
| `panel/run.c/.h`      | Simple "Run" command launcher dialog                        |
