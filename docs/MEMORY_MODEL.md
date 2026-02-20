# fbpanel3 — Memory Model and Ownership Rules

This document describes who owns each allocation in fbpanel3, when it must
be freed, and which allocator/deallocator to use.  Violating these rules
is the most common source of crashes (double-free, use-after-free) and
leaks in the codebase.

---

## 1. GObject Reference Counting

fbpanel3 uses two GObject singletons:

### FbBg (background monitor)

```c
FbBg *fb_bg_get_for_display(void);   // (transfer full) — caller must g_object_unref
```

`fb_bg_get_for_display()` returns the global `default_bg` singleton:
- First call: allocates a new FbBg; sets `default_bg`; returns it with
  ref-count == 1.
- Subsequent calls: calls `g_object_ref(default_bg)` and returns it with
  ref-count incremented.

Each caller that receives this pointer **must** eventually call
`g_object_unref()`.  GtkBgbox does this in `gtk_bgbox_finalize()`.

`fb_bg_finalize()` sets `default_bg = NULL`, so the singleton can be
re-created after the last unref.

### FbEv (event bus)

```c
FbEv *fb_ev_new(void);   // (transfer full) — caller must g_object_unref
```

Created once in `panel.c` and stored in the global `FbEv *fbev`.  Never
re-created during normal operation.  Destroyed when the panel exits.

---

## 2. GtkWidget Parent-Owns-Child Rule

When a GtkWidget is added to a container:
```c
gtk_container_add(GTK_CONTAINER(parent), child);
gtk_box_pack_start(GTK_BOX(box), child, ...);
```
The container takes ownership of the child.  **Do not** call
`gtk_widget_destroy()` or `g_object_unref()` on the child directly
unless you first remove it from its parent (which transfers ownership back).

**Implication for plugins**: The panel creates `plugin_instance->pwid`
(a GtkBgbox) and adds it to `panel->box`.  The plugin must **not** destroy
`pwid` in its destructor.  When `panel_stop()` calls `plugin_put()`, the
panel calls `g_object_unref(pwid)` which triggers GTK's recursive destroy.

**Implication for plugin children**: Any widget a plugin creates and adds
to `pwid` is owned by `pwid`.  The plugin destructor generally does not
need to destroy child widgets — they are destroyed automatically when `pwid`
is destroyed.

---

## 3. cairo_surface_t Lifecycle

| Creation site | Owner | Freed by |
|---|---|---|
| `fb_bg_get_xroot_pix_for_win(bg, widget)` | Caller receives **(transfer full)** | Caller must `cairo_surface_destroy()` |
| `fb_bg_get_xroot_pix_for_area(bg, x, y, w, h)` | Caller receives **(transfer full)** | Caller must `cairo_surface_destroy()` |
| `GtkBgboxPrivate->pixmap` | GtkBgbox (private state) | `gtk_bgbox_set_background()` or `gtk_bgbox_finalize()` |
| `FbBg->cache` (internal) | FbBg | `fb_bg_changed()` (invalidate) or `fb_bg_finalize()` |

The pattern for `GtkBgboxPrivate->pixmap`:
```c
// Always destroy old surface before assigning a new one:
if (priv->pixmap) {
    cairo_surface_destroy(priv->pixmap);
    priv->pixmap = NULL;
}
priv->pixmap = fb_bg_get_xroot_pix_for_win(priv->bg, widget);  // transfer full
```

**Rule**: Every `cairo_surface_create*()` call must be balanced by exactly
one `cairo_surface_destroy()`.

---

## 4. xconf Ownership — the str vs strdup Rule

The `xconf` tree stores all config values as `gchar *` strings inside the
node.  There are two ways to read a string from the tree:

### `XCG(xc, "key", &var, str)` — borrow (transfer none)

```c
gchar *val = NULL;
XCG(xc, "filename", &val, str);
// val points into xconf-owned memory
// DO NOT g_free(val)
// val is valid as long as xc is alive
```

Expands to `xconf_get_str(xconf_find(xc, "filename", 0), &val)`.
`xconf_get_str()` sets `*val = x->value` — a raw pointer into the tree.

### `XCG(xc, "key", &var, strdup)` — copy (transfer full)

```c
gchar *val = NULL;
XCG(xc, "filename", &val, strdup);
// val is a g_strdup'd copy
// YOU MUST g_free(val) when done
```

Expands to `xconf_get_strdup(xconf_find(xc, "filename", 0), &val)`.
`xconf_get_strdup()` sets `*val = g_strdup(x->value)`.

### The double-free case study (v8.3.23 bug)

In `menu.c`, before v8.3.23, the code was:
```c
gchar *iname = NULL;
XCG(xc, "icon", &iname, str);   // <-- transfer NONE: iname points into xconf
// ... later ...
g_free(iname);                   // BUG: freeing xconf-owned memory!
```
This caused a crash approximately 30 seconds after startup when the menu
was rebuilt and the stale xconf node was accessed.

**Fix**: either switch to `strdup` (and add the matching `g_free`), or
remove the `g_free` entirely.

### xconf_del() frees everything recursively

```c
void xconf_del(xconf *x, gboolean sons_only);
```

`xconf_del(xc, FALSE)` frees `xc->name`, `xc->value`, all son nodes
recursively, and finally `xc` itself.  After this call, **any pointer
obtained via `XCG(..., str)`** is a dangling pointer.

---

## 5. X11 vs GLib Allocations — XFree vs g_free

X11 functions that return dynamically allocated data use the X11 heap,
which is managed by the X client library (Xlib).  You **must** free X11
heap data with `XFree()`, not `g_free()` or `free()`.

GLib/GTK functions return data from the GLib heap, freed with `g_free()`.

| Function | Return type | Free with |
|---|---|---|
| `XGetWindowProperty()` | `unsigned char *` (via `prop` out-param) | `XFree(prop)` |
| `get_xaproperty()` (misc.c wrapper) | `void *` | `XFree()` |
| `get_utf8_property()` (misc.c) | `void *` | `g_free()` — uses `g_malloc` internally |
| `get_utf8_property_list()` (misc.c) | `char **` | `g_strfreev()` |
| `XGetTextProperty()` | `XTextProperty` | `XFree(prop.value)` |

**Rule**: look at the implementation to determine which allocator was used.
If the data came from Xlib (XGetWindowProperty, XFetch*, etc.) → use `XFree`.
If the data came from GLib (g_strdup, g_new, g_malloc) → use `g_free`.

---

## 6. GLib Allocations

Standard GLib allocation patterns used throughout the codebase:

| Allocator | Deallocator |
|---|---|
| `g_new0(Type, n)` | `g_free()` |
| `g_strdup(s)` | `g_free()` |
| `g_strdup_printf(fmt, ...)` | `g_free()` |
| `g_strndup(s, n)` | `g_free()` |
| `g_strv` functions | `g_strfreev()` |
| `g_list_*` | `g_list_free()` (elements freed separately) |
| `g_slist_*` | `g_slist_free()` (elements freed separately) |

---

## 7. Signal Handler Cleanup

Connecting a GObject signal registers a handler that will fire until it
is explicitly disconnected.  Failing to disconnect handlers causes:
- Use-after-free if the callback accesses a destroyed object.
- Memory leaks from the signal connection data.

**Pattern for timeouts**:
```c
guint timeout_id = g_timeout_add(ms, callback, data);
// ... later, in destructor:
g_source_remove(timeout_id);
```

**Pattern for GObject signals**:
```c
gulong sid = g_signal_connect(source, "signal", G_CALLBACK(cb), data);
// ... later, in destructor:
g_signal_handler_disconnect(source, sid);
```

**Pattern for GDK window filters**:
```c
gdk_window_add_filter(win, filter_func, data);
// ... later, in destructor:
gdk_window_remove_filter(win, filter_func, data);
```

**Pattern for GdkScreen signals** (panel.c):
```c
panel->monitors_sid = g_signal_connect(screen, "monitors-changed",
    G_CALLBACK(panel_screen_changed), p);
// ... in panel_stop():
g_signal_handler_disconnect(screen, p->monitors_sid);
```

---

## 8. Known Leaks

### BUG-001: FbEv::desktop_names not freed in finalize

**File**: `panel/ev.c`, `fb_ev_finalize()`

`ev->desktop_names` (`char **`, a `g_strv`) is freed in `ev_desktop_names()`
(the signal handler) when the signal fires, but **not** in `fb_ev_finalize()`.
If the signal never fires before the panel exits, `desktop_names` leaks.

Since FbEv is a singleton destroyed exactly once on exit, this is a minor
leak with no practical impact.

### BUG-002: FbBg::gc conditional free in finalize

**File**: `panel/bg.c`, `fb_bg_finalize()`

`XFreeGC()` is called unconditionally for `bg->gc`.  However `bg->gc` is
always initialised in `fb_bg_init()`, so this is safe.  The `default_bg`
guard (`if (default_bg != NULL)`) from historical code was removed; the
current version sets `default_bg = NULL` unconditionally in finalize.

No active bug, but the interaction between `default_bg = NULL` in finalize
and `fb_bg_get_for_display()` (which checks `!default_bg`) is worth noting:
once finalize runs, the singleton can be re-created.
