# fbpanel3 — GTK Widget Lifecycle

This document describes the creation, ownership transfer, and destruction of
every major widget type used by fbpanel3, with particular attention to the
custom GtkBgbox and GtkBar widgets and the GTK3 CSS gadget traps.

---

## 1. Widget Creation and Container Ownership

When a child widget is packed into a container:
```c
gtk_box_pack_start(GTK_BOX(parent), child, expand, fill, padding);
gtk_container_add(GTK_CONTAINER(parent), child);
```
the container holds a reference to the child.  The child's ref-count starts
at 1 (from `g_object_new`); `gtk_container_add` does not increment it — it
is a "sink" operation in GLib 2 parlance: the container inherits the
floating ref.

**Consequence**: after `gtk_container_add()`, the container owns the child.
Do not `g_object_unref()` or `gtk_widget_destroy()` the child directly unless
you first call `gtk_container_remove()` (which transfers ownership back to you).

**Panel-specific rule**: every `plugin_instance->pwid` is a GtkBgbox created
by the panel and added to `panel->box`.  The panel owns `pwid`.  Plugin
destructors must not destroy `pwid`.

---

## 2. GtkBgbox Lifecycle

GtkBgbox is a custom `GtkBin` subclass (`has_window = TRUE`) that paints
the panel's background (wallpaper slice or CSS).

### Private state (`GtkBgboxPrivate`)

```c
typedef struct {
    cairo_surface_t *pixmap;   // cached background slice (owned by GtkBgbox)
    guint32 tintcolor;         // tint RRGGBB for BG_ROOT mode
    gint alpha;                // tint alpha (0-255) for BG_ROOT mode
    int bg_type;               // BG_NONE/BG_STYLE/BG_ROOT/BG_INHERIT
    FbBg *bg;                  // ref-counted FbBg singleton (or NULL)
    gulong sid;                // g_signal connect id for FbBg::changed
} GtkBgboxPrivate;
```

### Creation

```c
GtkWidget *gtk_bgbox_new(void);   // returns (transfer full) floating ref
```

After creation, add to a container which sinks the floating ref.

### Realization (`gtk_bgbox_realize`)

GtkBgbox has `has_window = TRUE`, which means it needs its own GDK window.
GTK3's `gtk_widget_real_realize()` asserts `!has_window`, so it **cannot**
be called via `parent_class->realize()`.  Instead, GtkBgbox creates its
GDK child window manually, following the GtkLayout / GtkDrawingArea pattern:

```c
gtk_widget_set_realized(widget, TRUE);
window = gdk_window_new(gtk_widget_get_parent_window(widget),
                        &attributes, attributes_mask);
gtk_widget_register_window(widget, window);
gtk_widget_set_window(widget, window);
```

### Size Allocation (`gtk_bgbox_size_allocate`)

**Does NOT call `parent_class->size_allocate`.**

This is intentional.  `GtkBin::size_allocate` allocates to the single child,
but GtkBgbox handles child allocation itself (offsetting by the border width).
Calling the parent would double-allocate the child — harmlessly, but
wastefully.

If the allocation changed, GtkBgbox:
1. Moves/resizes the GDK window (`gdk_window_move_resize`).
2. Refreshes the background slice (`gtk_bgbox_set_background`).
3. Allocates the child widget.

### Finalization (`gtk_bgbox_finalize`)

```c
static void gtk_bgbox_finalize(GObject *object)
{
    // 1. Destroy cached cairo surface
    if (priv->pixmap) { cairo_surface_destroy(priv->pixmap); }
    // 2. Disconnect FbBg::changed signal
    if (priv->sid)    { g_signal_handler_disconnect(priv->bg, priv->sid); }
    // 3. Unref the FbBg singleton
    if (priv->bg)     { g_object_unref(priv->bg); }
}
```

GtkBin's finalize is called by GObject's chain after this.

---

## 3. GtkBar Lifecycle

GtkBar is a custom `GtkBox` subclass used by the taskbar plugin to arrange
task buttons with a fixed-height constraint.

### Struct layout

```c
struct _GtkBar {
    GtkBox box;         // must be first: C-style inheritance
    gint child_height;  // maximum child height (set by taskbar)
    gint child_width;   // maximum child width (set by taskbar)
    gint dimension;     // current bar dimension (cross-axis size)
    GtkOrientation orient;
};
```

### `size_allocate` parent-class requirement

**GtkBar MUST call `GTK_WIDGET_CLASS(parent_class)->size_allocate` first.**

```c
static void gtk_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GtkBar *bar = GTK_BAR(widget);

    // REQUIRED: let GtkBox update the CSS gadget (GTK3 internal machinery)
    GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation);

    // ... then do custom child allocation ...
}
```

Without this call, the GTK3 CSS gadget is not updated and you get:
```
Gtk-WARNING: Drawing a gadget with negative dimensions.
             Did you forget to allocate a size?
```
This was fixed in v8.3.7.

### Minimum-height requirement (v8.3.24)

When GtkBar has no children (empty taskbar), GTK3 reports a minimum height
of 2 px.  If the containing GtkWindow is allowed to shrink, it does, and
then GtkBar's allocation becomes 2 px, causing the "Negative content height"
warning.

Fix: always call:
```c
gtk_widget_set_size_request(topgwin, -1, p->ah);
```
after computing the panel's preferred allocation height `p->ah`.  This sets
a hard minimum height on the top-level window so GTK cannot shrink it below
the panel height.

---

## 4. Signal Cleanup on Destroy

GTK3 does **not** automatically disconnect GLib signal handlers when a widget
is destroyed.  If a plugin connects to a signal on an external object (like
`FbEv` or `FbBg`) and the plugin is destroyed first, the callback will fire
on a dangling pointer.

**Correct pattern**:
```c
// In constructor:
priv->ev_sid = g_signal_connect(fbev, "current_desktop",
    G_CALLBACK(my_ev_handler), priv);

// In destructor:
g_signal_handler_disconnect(fbev, priv->ev_sid);
```

GtkBgbox's internal `FbBg::changed` handler is correctly disconnected in
`gtk_bgbox_finalize()` via the stored `priv->sid`.

---

## 5. The CSS Gadget Trap

GTK3 internally uses "CSS gadgets" as rendering units for each widget.  The
gadget is created during `realize` and its geometry must be updated on every
`size_allocate`.

If a custom widget's `size_allocate` does **not** call
`GTK_WIDGET_CLASS(parent_class)->size_allocate`, the gadget geometry is never
updated.  The next draw call then finds a gadget with stale (possibly
negative) dimensions and logs:

```
Gtk-WARNING: Drawing a gadget with negative dimensions (w=-2 h=-2).
             Did you forget to allocate a size? (box.c:352)
```

**Affected widget**: GtkBar (subclass of GtkBox).
**Required call**: `GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation)` at the top of the override.

**Not affected**: GtkBgbox (subclass of GtkBin with `has_window=TRUE`).
GtkBin's size_allocate only calls the child — GtkBgbox handles that itself.

---

## 6. GtkWindow Sizing

The panel top-level window uses:
```c
gtk_window_set_type_hint(GTK_WINDOW(p->topgwin), GDK_WINDOW_TYPE_HINT_DOCK);
gtk_window_set_resizable(GTK_WINDOW(p->topgwin), FALSE);
gtk_widget_set_size_request(p->topgwin, p->aw, p->ah);
```

Key points:
- `resizable=FALSE` prevents the window manager from resizing the panel.
- `gtk_widget_set_size_request(topgwin, -1, p->ah)` sets the **minimum**
  window height.  GTK cannot shrink the window below this.  This is critical
  because `GtkBar` with no children reports a 2 px minimum, which would allow
  the window to collapse during a `queue_resize` cycle.
- `gtk_window_resize(topgwin, aw, ah)` is used for explicit repositioning
  (e.g., after a screen layout change).

---

## 7. Widget Lifecycle Summary Table

| Widget | Created by | Destroyed by | Parent-class size_allocate? |
|---|---|---|---|
| `topgwin` (GtkWindow) | `panel_start_gui()` | `gtk_widget_destroy(topgwin)` in `panel_stop()` | n/a |
| `bbox` (GtkBgbox) | `panel_start_gui()` | GTK recursive destroy from `topgwin` | NOT called (intentional) |
| `box` (GtkBox) | `panel_start_gui()` | GTK recursive destroy | n/a (default) |
| `pwid` (GtkBgbox per plugin) | `plugin_load()` | `plugin_put()` → `g_object_unref(pwid)` | NOT called (intentional) |
| GtkBar (taskbar) | taskbar plugin constructor | GTK recursive destroy from `pwid` | MUST be called |
| Plugin child widgets | Plugin constructor | GTK recursive destroy from `pwid` | varies |
