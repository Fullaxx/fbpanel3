# fbpanel3 — Plugin Architecture

This document is the authoritative reference for how fbpanel's plugin system
works, how plugins are loaded and unloaded, and what the rules are for writing
a well-behaved plugin.

---

## Guiding principle

> **A plugin must never prevent fbpanel from functioning.**
>
> If a plugin cannot initialise — because the required hardware is absent,
> a system resource is unavailable, a dependency library is missing, or
> configuration is invalid — it must return `0` from its constructor and
> emit a diagnostic message to stderr.  The panel will skip that plugin,
> leave an empty slot in its place, and continue loading all remaining
> plugins.
>
> A plugin must **never** call `exit()`, `abort()`, or otherwise terminate
> the process.

This requirement is enforced by the panel: `panel_parse_plugin()` in
`panel/panel.c` catches a `0` return from `plugin_start()`, logs a message,
frees the plugin instance cleanly, and continues with the next plugin.

---

## Overview

Each plugin is a POSIX shared library (`lib<type>.so`) loaded at panel startup
via `GModule` (a GLib wrapper around `dlopen`).  Plugins are not compiled into
the panel binary; the panel binary only links against the plugin API headers.

The plugin system has three layers:

```
Config file           panel.c               plugin.c
──────────────        ─────────────────     ─────────────────────────────
Plugin {         →    panel_parse_plugin  → plugin_load     (allocate)
    type = foo        reads type, expand,   plugin_start    (create widget,
    expand = true     padding from config               call constructor)
    config { … }
}                                           plugin_stop     (call destructor,
                                                         destroy widget)
                                            plugin_put      (free memory)
```

---

## Plugin class descriptor — `plugin_class`

Every plugin defines exactly one `plugin_class` struct and one
`plugin_class *class_ptr` pointing at it (both file-scope, `static`):

```c
static plugin_class class = {
    .type        = "myplugin",   /* key used in config files */
    .name        = "My Plugin",  /* human-readable label in preferences */
    .version     = "1.0",
    .description = "One-line description",
    .priv_size   = sizeof(my_priv),
    .constructor = my_constructor,
    .destructor  = my_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
```

| Field | Set by | Purpose |
|-------|--------|---------|
| `type` | plugin | Short identifier matching the `type =` key in config files |
| `name` | plugin | Human-readable name shown in the panel preferences dialog |
| `version` | plugin | Version string for display purposes |
| `description` | plugin | One-line description shown in preferences |
| `priv_size` | plugin | `sizeof(your_priv_struct)`; the panel allocates this many bytes |
| `constructor` | plugin | Called once to create widgets; return `1` on success, `0` to disable |
| `destructor` | plugin | Called once to free resources before the plugin is removed |
| `save_config` | plugin | Optional: serialize current config to a `FILE *` |
| `edit_config` | plugin | Optional: return a `GtkWidget *` for the preferences dialog |
| `fname` | panel | Path to the `.so` file (or `NULL` for built-ins); set by the loader |
| `count` | panel | Reference count; managed by `class_get`/`class_put` |
| `dynamic` | panel | `1` if loaded from a `.so` at runtime |
| `invisible` | plugin | Set to `1` if the plugin has no visible widget (e.g. `icons`) |

---

## Plugin instance — `plugin_instance`

The panel allocates `priv_size` bytes for each plugin instance.  The first
`sizeof(plugin_instance)` bytes are the public fields; the remaining bytes are
the plugin's own private struct.  Plugins access both through a single cast:

```c
typedef struct {
    plugin_instance plugin;   /* MUST be first */
    /* plugin-private fields follow: */
    GtkWidget *my_label;
    int        my_counter;
    guint      my_timer_id;
} my_priv;

static int my_constructor(plugin_instance *p) {
    my_priv *priv = (my_priv *) p;   /* safe cast: plugin is first member */
    …
}
```

Fields provided by the panel at constructor call time:

| Field | Type | Description |
|-------|------|-------------|
| `p->panel` | `panel *` | The panel this plugin lives in; provides orientation, size, geometry |
| `p->xc` | `xconf *` | Parsed config subtree for this plugin (`config { }` block) |
| `p->pwid` | `GtkWidget *` | The `GtkBgbox` container the panel created and added to the bar |
| `p->expand` | `int` | Whether to expand to fill remaining bar space |
| `p->padding` | `int` | Extra space (px) on each side of `pwid` |
| `p->border` | `int` | Border width (px) inside `pwid` |

---

## Full plugin lifecycle

### 1. Registration — on `dlopen`

When the panel calls `g_module_open()` on the plugin `.so`, the `ctor()`
function defined by the `PLUGIN` macro runs automatically (via
`__attribute__((constructor))`).  It calls `class_register(class_ptr)`, which
inserts the `plugin_class` into a global hash table keyed on `type`.

```
dlopen("lib<type>.so")
  → ctor() [attribute constructor]
    → class_register(class_ptr)
      → g_hash_table_insert(class_ht, type, class_ptr)
```

### 2. Loading — `plugin_load(type)`

Looks up the registered class, allocates `priv_size` bytes with `g_malloc0`,
sets `pp->class`, and returns the zeroed instance.  No constructor is called
yet.

### 3. Starting — `plugin_start(plugin_instance *)`

This is where the plugin's widget is created and its constructor is called:

```c
int plugin_start(plugin_instance *this) {
    /* Create the container widget and add it to the panel bar */
    this->pwid = gtk_bgbox_new();
    gtk_box_pack_start(GTK_BOX(this->panel->box), this->pwid, …);

    /* Call the plugin's constructor */
    if (!this->class->constructor(this)) {
        gtk_widget_destroy(this->pwid);
        return 0;   /* caller (panel_parse_plugin) will free and skip */
    }
    return 1;
}
```

Key points:
- `pwid` is a `GtkBgbox` (a `GtkBin` subclass with pseudo-transparency support).
  It is already added to the panel bar when the constructor is called.
- The constructor must add its own widgets as children of `p->pwid`.
- If the plugin sets `invisible = 1` in its class, a hidden `GtkBox` is used
  instead; no visible space is reserved on the bar.

### 4. Constructor contract

```
return 1   →  success: plugin is live on the panel bar
return 0   →  failure: plugin is skipped; panel continues loading
```

**A constructor that returns `0` is not a fatal error.**  The panel skips
the plugin and continues.  This is the correct way to handle any situation
where the plugin cannot function:

- Hardware not present (`/dev/mixer`, `/dev/dsp`, no battery, etc.)
- Kernel interface not available (`/proc/acpi/battery`, `/sys/class/…`)
- Required library missing or version mismatch
- Required image file not found
- Any `open()` / `ioctl()` / network call that fails at startup

**Pattern for graceful self-disabling:**
```c
static int my_constructor(plugin_instance *p) {
    int fd = open("/dev/some_device", O_RDWR);
    if (fd < 0) {
        g_message("myplugin: /dev/some_device not available — plugin disabled");
        return 0;   /* panel skips us; no crash, no exit() */
    }
    /* proceed with normal setup */
    …
    return 1;
}
```

Use `g_message()` (not `ERR()`) for the disable message so it is clearly
diagnostic rather than alarming.  `ERR()` is for genuine programming errors.

### 5. Destructor contract

```c
static void my_destructor(plugin_instance *p) {
    my_priv *priv = (my_priv *) p;
    /* Remove timers, signal handlers, close file descriptors */
    if (priv->my_timer_id)
        g_source_remove(priv->my_timer_id);
    /* Do NOT gtk_widget_destroy(p->pwid) — the panel owns that */
}
```

- Cancel all `g_timeout_add` / `g_idle_add` timers.
- Disconnect all signal handlers not auto-disconnected by widget destruction.
- Close file descriptors opened in the constructor.
- Free all heap memory not managed by GTK.
- **Do not** destroy `p->pwid` — `plugin_stop()` calls
  `gtk_widget_destroy(p->pwid)` after the destructor returns.

### 6. Stopping and unloading

```
plugin_stop(plug)
  → destructor(plug)           plugin frees its own resources
  → gtk_widget_destroy(pwid)   panel destroys the widget subtree
  → plug->panel->plug_num--

plugin_put(plug)
  → g_free(plug)               free the priv struct
  → class_put(type)            decrement class refcount; dlclose if 0
```

---

## Reading config — `XCG()`

The `plugin_instance.xc` field points to the parsed `config { }` subtree for
this plugin.  Use the `XCG()` macro to read typed values:

```c
XCG(p->xc, "key",    &field,  int);
XCG(p->xc, "flag",   &field,  enum, bool_enum);
XCG(p->xc, "color",  &field,  str);
```

Supported types: `int`, `str` (char pointer), `enum` (requires a
`xconf_enum[]` table as the fifth argument).

`bool_enum` is a pre-defined table mapping `"true"`/`"false"`/`"yes"`/`"no"` to
`1`/`0`.

---

## The panel event bus — `FbEv`

EWMH property changes on the root window are broadcast as GObject signals on
the global `fbev` object (declared in `panel.h`).  Plugins connect to these
signals instead of installing their own root-window filters:

| Signal | Fired when |
|--------|-----------|
| `current_desktop` | `_NET_CURRENT_DESKTOP` changes |
| `active_window` | `_NET_ACTIVE_WINDOW` changes |
| `number_of_desktops` | `_NET_NUMBER_OF_DESKTOPS` changes |
| `client_list` | `_NET_CLIENT_LIST` changes |
| `desktop_names` | `_NET_DESKTOP_NAMES` changes |

```c
g_signal_connect(G_OBJECT(fbev), "active_window",
    G_CALLBACK(my_active_window_cb), priv);
```

Disconnect all signal handlers in the destructor:
```c
g_signal_handlers_disconnect_by_func(G_OBJECT(fbev),
    my_active_window_cb, priv);
```

---

## Pseudo-transparent backgrounds

Each plugin's `pwid` is a `GtkBgbox`.  When the panel is configured with
`transparent = true`, the panel calls:

```c
gtk_bgbox_set_background(p->pwid, BG_INHERIT,
    p->panel->tintcolor, p->panel->alpha);
```

This causes `pwid` to sample the correct portion of the root window pixmap as
its background, creating the pseudo-transparency effect.  Plugins do not need
to do anything to opt into this — it is set up by `plugin_start()`.

---

## The PLUGIN macro

Compiled with `-DPLUGIN` (set in CMakeLists for all plugin targets), `plugin.h`
defines:

```c
static plugin_class *class_ptr;
static void ctor(void) __attribute__((constructor));
static void ctor(void) { class_register(class_ptr); }
static void dtor(void) __attribute__((destructor));
static void dtor(void) { class_unregister(class_ptr); }
```

The plugin fills in `class_ptr` at file scope:
```c
static plugin_class *class_ptr = (plugin_class *) &class;
```

**Multi-file plugins** (e.g. taskbar, split across 4 `.c` files) must `#undef
PLUGIN` before `#include "plugin.h"` in their shared internal header, and
define `ctor`/`dtor` manually in exactly one `.c` file to avoid duplicate
symbol errors.

---

## Helper sub-plugins

Some plugins act as shared widget libraries for other plugins:

| Helper | Used by | Provides |
|--------|---------|---------|
| `meter` | `battery`, `volume` | Icon-based 0–100 level widget; cycled through named icon theme icons |
| `chart` | `cpu`, `mem`, `net` | Scrolling bar-graph rendered with cairo |

A plugin that uses a helper calls `class_get("meter")` to get a pointer to the
helper's `plugin_class`, then calls its constructor/destructor manually.  It
must also call `class_put("meter")` in its own destructor to release the
reference.

The helper's constructor failure should also be treated as non-fatal — the
using plugin should return `0` from its own constructor if the helper is
unavailable, emitting a diagnostic message.

---

## Minimal working plugin

```c
#include "plugin.h"

typedef struct {
    plugin_instance plugin;   /* MUST be first */
    GtkWidget      *label;
    guint           timer_id;
} clock_priv;

static gboolean
clock_update(clock_priv *priv)
{
    time_t t = time(NULL);
    gtk_label_set_text(GTK_LABEL(priv->label), ctime(&t));
    return TRUE;
}

static int
clock_constructor(plugin_instance *p)
{
    clock_priv *priv = (clock_priv *) p;

    priv->label = gtk_label_new(NULL);
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    priv->timer_id = g_timeout_add(1000, (GSourceFunc)clock_update, priv);
    clock_update(priv);
    return 1;
}

static void
clock_destructor(plugin_instance *p)
{
    clock_priv *priv = (clock_priv *) p;
    if (priv->timer_id)
        g_source_remove(priv->timer_id);
    /* label is destroyed when pwid is destroyed */
}

static plugin_class class = {
    .type        = "myclock",
    .name        = "My Clock",
    .version     = "1.0",
    .description = "Simple text clock",
    .priv_size   = sizeof(clock_priv),
    .constructor = clock_constructor,
    .destructor  = clock_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
```

Add to `CMakeLists.txt` by appending `myclock` to the `PLUGINS` list — the
`foreach` loop handles all compilation, install, and link rules automatically.

---

## Adding a new plugin — checklist

1. Create `plugins/<type>/` directory.
2. Write `<type>.c` with `plugin_class`, `constructor`, `destructor`, and
   `plugin_class *class_ptr`.  Compile with `-DPLUGIN` (handled by CMake).
3. Add `<type>` to the `set(PLUGINS …)` list in `CMakeLists.txt`.
4. If the plugin needs `cairo-xlib`, add the extra link targets after the
   `foreach` block (see the `taskbar` and `pager` entries as examples).
5. Ensure the constructor gracefully returns `0` (with a `g_message()`) for
   any hardware or resource that might be absent.
6. Add a `Plugin { type = <type> }` block to `data/config/default.in` if the
   plugin should be part of the default panel layout.

---

## Requirements summary

| # | Requirement |
|---|-------------|
| R1 | A plugin constructor must return `1` on success and `0` on failure. |
| R2 | **A plugin must not call `exit()`, `abort()`, or otherwise terminate the process.** |
| R3 | A plugin that cannot initialise due to missing hardware or resources must return `0` and emit a diagnostic via `g_message()` to stderr. |
| R4 | The panel treats constructor `0` as non-fatal: the plugin is skipped and the panel continues. |
| R5 | The destructor must cancel all timers, disconnect all signals, and close all file descriptors opened in the constructor. |
| R6 | The destructor must not destroy `p->pwid`; that is the panel's responsibility. |
| R7 | `plugin_instance` must be the first member of the plugin's private struct. |
| R8 | Multi-file plugins must `#undef PLUGIN` in their shared internal header and register their class manually in exactly one translation unit. |
