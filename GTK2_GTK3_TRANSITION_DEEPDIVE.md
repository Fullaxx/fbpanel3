# GTK2 → GTK3 Transition Deep-Dive

Living reference document for the fbpanel3 GTK3 port. Updated as fixes land.
Last updated: v8.3.22.

---

## 1. Background: What Changed from GTK2 to GTK3

| Area | GTK2 | GTK3 |
|------|------|------|
| Rendering | `expose_event` + `GdkDrawable` (XPixmap) | `draw` vfunc + `cairo_t *cr` |
| CSS gadgets | None (GtkStyle) | Every widget has a CSS node; `size_allocate` **MUST** call parent to allocate gadget |
| Drawing from layout | OK in GTK2 | **Illegal** — never call `begin_draw_frame` or `queue_draw` from `size_allocate` |
| Offscreen windows | Had X11 backing | `GtkOffscreenWindow` has NO X11 window; `gdk_x11_window_get_xid()` will assert |
| X11 Pixmap → surface | `GdkPixmap` wrapping | Must use `cairo_xlib_surface_create()` directly |
| `gdk_window_add_filter` | Primary event dispatch mechanism | Deprecated GTK 3.12; removed in GTK4 |
| GdkEvent field access | Direct struct field access OK | Accessor functions preferred (GTK 3.20+); direct access still works in GTK3 |
| Widget state | `GtkStateType` + `gtk_widget_set_state()` | `GtkStateFlags` + `gtk_widget_set_state_flags()` |
| GtkStyle / GdkGC | Primary styling API | Removed; use GtkStyleContext + CSS |
| Transparency | `gdk_window_set_background_pattern()` | All painting in `draw` vfunc |
| Custom container parent call | Calling parent `size_allocate` optional | **Required** to allocate CSS gadget node |
| Image menu items | `gtk_image_menu_item_*` | `gtk_menu_item_*` (image menu items removed) |
| Menu popup | `gtk_menu_popup()` | `gtk_menu_popup_at_pointer()` |
| CSS styling | `gtk_rc_parse_string()` | `GtkCssProvider` |
| Widget mapping | `GTK_WIDGET_MAPPED()` macro | Removed in GTK 3.24.49 (Trixie); use `gtk_widget_get_mapped()` |
| Display access | `GDK_DISPLAY()` | `GDK_DISPLAY_XDISPLAY(gdk_display_get_default())` |

---

## 2. Issue Catalog

### Severity Key

- **CRITICAL (C-)**: Causes crash or complete non-functionality
- **HIGH (H-)**: Deprecated; will break on newer GTK or strict builds
- **MEDIUM (M-)**: Subtle breakage or GTK3 anti-pattern
- **LOW (L-)**: Cosmetic / non-blocking

---

### CRITICAL Issues

#### C-1: `gtkbar.c` — CSS gadget never allocated → double-free on resize

- **File:** `panel/gtkbar.c`
- **Function:** `gtk_bar_size_allocate` (around line 172)
- **Status:** Fixed in v8.3.7

**Symptom:**
```
Gtk-WARNING: Drawing a gadget with negative dimensions.
Did you forget to allocate a size? (node box owner GtkBar)
free(): double free detected in tcache 2
Aborted (core dumped)
```
Triggered by any window resize while fbpanel is running.

**Root cause:**

`gtk_bar_size_allocate` overrides the `size_allocate` vfunc but never calls
`GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation)`.

In GTK3, `GtkBox::size_allocate` performs two critical operations beyond child layout:
1. Calls `gtk_css_gadget_allocate()` for the widget's own CSS node
2. Calls `gtk_widget_set_allocation(widget, …)` for the correct border-box area

When GtkBar skips the parent call, `gtk_css_gadget_allocate` is never called for the
GtkBar node. The CSS gadget's internal `GtkAllocation` stays at its previous (or
zero-initialised) value. Immediately after `gtk_widget_set_allocation` (line 182),
`gtk_widget_queue_draw(widget)` (line 190) is called — still within `size_allocate`.
GTK3 processes that draw using the CSS gadget's **stale allocation**, sees 0×0 or
negative dimensions, emits the "negative dimensions" warning, then corrupts the CSS
gadget data → double-free → abort.

Additionally, calling `gtk_widget_queue_draw` from within `size_allocate` itself is a
GTK3 anti-pattern (see GTK3 Drawing Rule in MEMORY.md).

**Fix:**

```c
static void
gtk_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GtkBar *bar;
    GList *children, *l;
    GtkAllocation child_allocation;
    gint nvis_children, tmp, rows, cols, dim;

    DBG("a.w=%d  a.h=%d\n", allocation->width, allocation->height);

    /* Let the parent handle CSS gadget allocation and gtk_widget_set_allocation.
     * GtkBox will lay children linearly first; we immediately override below. */
    GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation);

    bar = GTK_BAR(widget);
    nvis_children = 0;
    children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (l = children; l; l = l->next) {
        if (gtk_widget_get_visible(GTK_WIDGET(l->data)))
            nvis_children++;
    }

    /* No gtk_widget_queue_draw here — GTK3 does it automatically after
     * allocation changes. No gtk_widget_set_allocation either — parent did it. */

    dim = MIN(bar->dimension, nvis_children);
    if (nvis_children == 0) {
        g_list_free(children);
        return;
    }
    /* ... rest of grid layout unchanged ... */
    g_list_free(children);
}
```

Specifically:
1. Add `GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation)` as the
   **first** statement.
2. Remove `gtk_widget_set_allocation(widget, allocation)` — parent handles it.
3. Remove `gtk_widget_queue_draw(widget)` — GTK3 queues a draw automatically.

---

### HIGH Issues

#### H-1: `gdk_window_add_filter` deprecated — taskbar, gtkbgbox, pager, icons

- **Deprecated:** GTK 3.12; **removed in GTK4**
- **Impact on GTK 3.24.x:** Still works but produces deprecation warnings in strict
  builds; will break entirely on GTK4
- **Status:** Fixed across all affected files

**Fixed files:**

| File | Fix | Version |
|------|-----|---------|
| `panel/gtkbgbox.c` | Removed `gdk_window_add_filter(window, ...)` + `gtk_bgbox_event_filter`; ConfigureNotify is handled by GTK3 automatically (see M-3) | v8.3.8 |
| `plugins/taskbar/taskbar_ui.c` | Replaced `gdk_window_add_filter(NULL, ...)` with per-window `gdk_window_add_filter(tk->gdkwin, ...)` using `gdk_x11_window_foreign_new_for_display`; filter added in `tk_build_gui`, removed in `del_task` | v8.3.9 |
| `plugins/pager/pager.c` | Same per-window pattern: `task.gdkwin` field, filter added in `do_net_client_list_stacking`, removed in `task_remove_stale`/`task_remove_all` | v8.3.22 |
| `plugins/icons/icons.c` | Same per-window pattern: `task.gdkwin` field, filter added in `do_net_client_list`, removed in `free_task` | v8.3.22 |

**Per-window filter pattern (from taskbar, replicated to pager/icons):**
```c
/* When adding a tracked window: */
tk->gdkwin = gdk_x11_window_foreign_new_for_display(
        gdk_display_get_default(), tk->win);
if (tk->gdkwin)
    gdk_window_add_filter(tk->gdkwin, (GdkFilterFunc)my_event_filter, priv);

/* When removing a tracked window: */
if (tk->gdkwin) {
    gdk_window_remove_filter(tk->gdkwin, (GdkFilterFunc)my_event_filter, priv);
    g_object_unref(tk->gdkwin);
}
```

#### H-2: `taskbar_size_alloc` calls `gtk_widget_queue_resize` from `size-allocate` signal

- **File:** `plugins/taskbar/taskbar_ui.c` (line 395)
- **Status:** Fixed in v8.3.10

`taskbar_size_alloc` is connected to the `size-allocate` signal of `p->pwid`. Inside
it calls `gtk_bar_set_dimension` → `gtk_widget_queue_resize(GTK_WIDGET(tb->bar))`.

Calling `queue_resize` from within a `size-allocate` signal handler asks GTK to redo
layout while layout is already in progress. GTK3.20+ guards against infinite loops but
can produce incorrect final state or stale allocations.

**Fix:** Store the desired dimension and apply it via `g_idle_add` callback, or
restructure so `gtk_bar_set_dimension` is called from the constructor/configure path
rather than from the `size-allocate` signal.

---

### MEDIUM Issues

#### M-1: `panel_size_alloc` signal handler signature mismatch

- **File:** `panel/panel.c` (line ~184)
- **Status:** Fixed in v8.3.11

The `size-allocate` signal passes `GtkAllocation *` but the handler uses `GdkRectangle *`.
Both have identical struct layout (4 `gint`s), so it works today. However, GObject signal
marshaling can be strict about type in debug builds or future GTK versions.

**Fix:** Change parameter type to `GtkAllocation *`.

#### M-2: Tray plugin force-reflow hack

- **File:** `plugins/tray/main.c` (lines 39-40)
- **Status:** Fixed in v8.3.11

Uses `gtk_events_pending()` + `gtk_main_iteration()` inside a callback — a GTK2-era
workaround to force a synchronous relayout. In GTK3, this can cause nested event
processing (reentrancy) during signal callbacks.

**Fix:** Replace with `gtk_widget_queue_resize()`.

#### M-3: `gtkbgbox.c` — ConfigureNotify filter now redundant

- **File:** `panel/gtkbgbox.c` (line 185, `gtk_bgbox_realize`)
- **Status:** Fixed in v8.3.8

`gdk_window_add_filter(window, gtk_bgbox_event_filter, widget)` catches `ConfigureNotify`
to call `gtk_widget_queue_draw`. The `draw` vfunc now handles all painting; GTK3 already
queues a draw on configure.

**Fix:**

Remove from `gtk_bgbox_realize`:
```c
gdk_window_add_filter(window, (GdkFilterFunc) gtk_bgbox_event_filter, widget);
```

Remove the entire `gtk_bgbox_event_filter` function (currently around lines 127-142).

If ConfigureNotify-triggered redraws are still needed, implement in the already-wired
`gtk_bgbox_configure_event` vfunc (currently has only a `DBG` + `return FALSE` body):
```c
static gboolean
gtk_bgbox_configure_event(GtkWidget *widget, GdkEventConfigure *e)
{
    gtk_widget_queue_draw(widget);
    return FALSE;
}
```
Note: GTK3 already queues a redraw on configure in most cases, so this may not be
necessary. Test without it first.

#### M-4: `bg.c` — multiple `cairo_xlib_surface_create` calls per resize

- **File:** `panel/bg.c` (`fb_bg_get_xroot_pix_for_win`)
- **Status:** Future work (no version assigned)

Called once per plugin widget during each `fb_bg_notify_changed_bg` emission. With 20
plugins, this means 20 X11 round-trips per resize to copy the wallpaper.

**Fix:** Cache the last-copied pixmap keyed on root pixmap ID + screen size; invalidate
on the "changed" signal from `FbBg`.

#### M-5: `pager.c` — WM_HINTS icon loading

- **File:** `plugins/pager/pager.c` (lines 511-577)
- **Status:** Fixed — implemented using `cairo_xlib_surface_create`

`_wnck_gdk_pixbuf_get_from_pixmap` has a full implementation: it uses
`cairo_xlib_surface_create` (colour pixmaps) and
`cairo_xlib_surface_create_for_bitmap` (1-bit masks), copies to a
`cairo_image_surface_t`, then calls `gdk_pixbuf_get_from_surface`. Both
`get_netwm_icon` (_NET_WM_ICON) and `get_wm_icon` (WM_HINTS pixmap/mask) are
implemented and called from `do_net_client_list_stacking`. This was a stub in
the original GTK3 port but is now complete.

---

### LOW Issues

#### L-1: Panel help text says "GTK2+"

- **File:** `panel/panel.c` (line ~799)
- **Fix:** Change to "GTK3"

#### L-2: `pager.c` wallpaper rendering stubbed out

- **File:** `plugins/pager/pager.c` (`desk_draw_bg`, lines 351-355)
- Desktop thumbnails don't show wallpaper.
- **Fix:** Low priority; implement using root pixmap copy if desired.

---

## 3. File-by-File Status Table

| File | GTK2 APIs remaining | Known issues | Status |
|------|--------------------|--------------|--------|
| `panel/bg.c` | None | M-4 (efficiency) | Patched v8.3.6; M-4 future work |
| `panel/bg.h` | None | None | Clean |
| `panel/gtkbar.c` | None | ~~C-1~~ (fixed v8.3.7) | Clean |
| `panel/gtkbar.h` | None | None | Clean |
| `panel/gtkbgbox.c` | None | ~~H-1/M-3~~ (fixed v8.3.8) | Clean |
| `panel/panel.c` | None | ~~M-1, L-1~~ (fixed v8.3.11) | Clean |
| `panel/plugin.c` | None | None | Clean |
| `panel/plugin.h` | None | None | Clean |
| `panel/misc.c` | None | None | Clean |
| `panel/widgets.c` | None | None | Clean |
| `panel/ev.c` | None | None | Clean |
| `plugins/taskbar/taskbar_ui.c` | None | ~~H-1~~ (fixed v8.3.9), ~~H-2~~ (fixed v8.3.10) | Clean |
| `plugins/taskbar/taskbar_task.c` | None | None | Clean |
| `plugins/taskbar/taskbar_net.c` | None | None | Clean |
| `plugins/taskbar/taskbar.c` | None | None | Clean |
| `plugins/tray/main.c` | None | ~~M-2~~ (fixed v8.3.11) | Clean |
| `plugins/pager/pager.c` | None | ~~H-1~~ (fixed v8.3.22), ~~M-5~~ (implemented), L-2 | L-2 (wallpaper) is an acceptable stub |
| `plugins/icons/icons.c` | None | ~~H-1~~ (fixed v8.3.22) | Clean |
| `plugins/chart/chart.c` | None | None | Clean |
| `plugins/cpu/cpu.c` | None | None | Clean |
| `plugins/net/net.c` | None | None | Clean |
| `plugins/mem/mem.c` | None | None | Clean |
| `plugins/battery/battery.c` | None | None | Clean |

---

## 4. Prioritised Fix Sequence

| Version | Issue | Change |
|---------|-------|--------|
| **v8.3.7** | C-1 | `gtkbar.c`: call parent `size_allocate`; remove manual `gtk_widget_set_allocation` and `gtk_widget_queue_draw` |
| **v8.3.8** | H-1/M-3 | `gtkbgbox.c`: remove `gdk_window_add_filter` + `gtk_bgbox_event_filter`; GTK3 queues redraw on configure automatically |
| **v8.3.9** | H-1 | `taskbar_ui.c`: replace `gdk_window_add_filter(NULL, ...)` with per-window `gdk_window_add_filter(tk->gdkwin, ...)` |
| **v8.3.10** | H-2 | `taskbar_ui.c`: fix `queue_resize` from `size-allocate` signal via `g_idle_add` |
| **v8.3.11** | M-1, M-2, L-1 | `panel.c` signal type fix; tray `events_pending` → `queue_resize`; update help text |
| **v8.3.22** | H-1 | `pager.c`, `icons.c`: replace `gdk_window_add_filter(NULL, ...)` with per-window filters using same pattern as taskbar |

---

## 5. Implementation Notes

### gtkbar.c fix detail (v8.3.7)

The complete new `gtk_bar_size_allocate` function, with changes clearly marked:

```c
static void
gtk_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    GtkBar *bar;
    GList *children, *l;
    GtkAllocation child_allocation;
    gint nvis_children, tmp, rows, cols, dim;

    DBG("a.w=%d  a.h=%d\n", allocation->width, allocation->height);

    /* NEW: Let the parent handle CSS gadget allocation and widget allocation.
     * GtkBox will lay children linearly first; we immediately override below
     * with the grid layout. GTK3 is fine with children being reallocated. */
    GTK_WIDGET_CLASS(parent_class)->size_allocate(widget, allocation);

    /* REMOVED: gtk_widget_set_allocation(widget, allocation) — parent did it */
    /* REMOVED: gtk_widget_queue_draw(widget) — GTK3 does this automatically
     *          when allocation changes, and calling it here is illegal in GTK3 */

    bar = GTK_BAR(widget);
    nvis_children = 0;
    children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (l = children; l; l = l->next) {
        if (gtk_widget_get_visible(GTK_WIDGET(l->data)))
            nvis_children++;
    }

    dim = MIN(bar->dimension, nvis_children);
    if (nvis_children == 0) {
        g_list_free(children);
        return;
    }
    /* ... rest of grid layout logic unchanged ... */
    g_list_free(children);
}
```

### gtkbgbox.c filter removal (v8.3.8)

**In `gtk_bgbox_realize`**, remove:
```c
gdk_window_add_filter(window, (GdkFilterFunc) gtk_bgbox_event_filter, widget);
```

**Remove the entire `gtk_bgbox_event_filter` function** (currently around lines 127-142).

**Optionally** update `gtk_bgbox_configure_event` (currently a no-op with just DBG + return):
```c
static gboolean
gtk_bgbox_configure_event(GtkWidget *widget, GdkEventConfigure *e)
{
    gtk_widget_queue_draw(widget);
    return FALSE;
}
```
Note: GTK3 already queues a redraw on configure in most cases, so this may not be
necessary. Test without it first.

---

## 6. Build + Verification Checklist

Run after each fix before tagging a release:

### Build check
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -j$(nproc) 2>&1 | grep -E '(error:|warning:.*deprecated)'
```
Expected: 0 errors, 0 deprecated warnings.

### Quick smoke test (local Xvfb)
```bash
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 ./build/fbpanel 2>&1 | head -20
```
Expected: No assertion failures, no "negative dimensions" warning.

### VNC container test (matches CI distros)
```bash
# Replace 'noble' with any of: noble jammy focal trixie bookworm bullseye
docker run --rm -it ghcr.io/fullaxx/fbpanel_builder:noble \
    bash -c "apt-get install -y xvfb xdotool 2>/dev/null | tail -1 && \
             Xvfb :1 -screen 0 1280x800x24 & sleep 1 && \
             DISPLAY=:1 /path/to/fbpanel 2>&1"
```

### Resize stress test (VNC container — catches C-1)
After fbpanel is running:
```bash
xdotool search --name fbpanel | head -1 | xargs -I{} xdotool windowsize {} 1920 30
xdotool search --name fbpanel | head -1 | xargs -I{} xdotool windowsize {} 1280 30
```
Repeat several times. Expected: no crash, no "Drawing a gadget with negative dimensions"
in stderr.

### Success criteria

- [ ] No `gdk_x11_window_get_xid` assertion failures in stderr
- [ ] No "Drawing a gadget with negative dimensions" warnings
- [ ] No `free(): double free detected` / `Aborted` after resize
- [ ] Taskbar shows open windows and updates when windows open/close
- [ ] All 6 distro builds pass in GitHub Actions
- [ ] 0 deprecated API warnings in build output

---

## 7. Previously Fixed Issues (for reference)

These were resolved during the v8.3.0–v8.3.6 work and are noted here as examples of
the GTK2→GTK3 patterns that occurred throughout the codebase.

| Issue | File | Fix | Version |
|-------|------|-----|---------|
| `GDK_DISPLAY()` → GDK3 accessor | multiple | `GDK_DISPLAY_XDISPLAY(gdk_display_get_default())` | v8.3.0 |
| `GTK_WIDGET_MAPPED` removed in GTK 3.24.49 | taskbar | `gtk_widget_get_mapped()` | v8.3.1 |
| `gtk_image_menu_item_*` removed | multiple | `gtk_menu_item_*` | v8.3.0 |
| `gtk_menu_popup` removed | multiple | `gtk_menu_popup_at_pointer()` | v8.3.0 |
| `gtk_rc_parse_string` removed | panel | `GtkCssProvider` | v8.3.0 |
| `gtk_widget_set_state` + `GtkStateType` | multiple | `gtk_widget_set_state_flags` + `GtkStateFlags` | v8.3.0 |
| `gdk_window_set_background_pattern` | gtkbgbox | cairo `draw()` vfunc | v8.3.2 |
| `GtkOffscreenWindow` used for X11 Pixmap | bg.c | `cairo_xlib_surface_create()` directly | v8.3.5 |
| `gdk_window_begin_draw_frame` from size_allocate | gtkbar | Removed; draw in `draw` vfunc | v8.3.3 |
| `GdkPixmap` wrapping | bg.c | `cairo_xlib_surface_create()` | v8.3.6 |
| Missing `${CAIRO_XLIB_LIBRARIES}` link | CMakeLists.txt | Added to fbpanel target | v8.3.6 |
| `.inc` files missing DBG header | battery | Check .inc includes before removing headers | v8.3.0 |

---

*End of document. Update the status table and fix sequence when new issues are found or
fixes are landed.*
