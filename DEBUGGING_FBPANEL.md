# fbpanel3 — Debugging Guide

## Quick-start: reproduce and diagnose a crash

### 1. Build with AddressSanitizer

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
cmake --build build -j$(nproc)
```

### 2. Install plugins to the baked-in path

The binary searches for plugins at the compiled-in `LIBDIR` (default
`/usr/local/lib/fbpanel`).  Copy the ASAN-built `.so` files there:

```bash
cmake --install build --prefix /tmp/fbtest
sudo cp /tmp/fbtest/lib/fbpanel/*.so /usr/local/lib/fbpanel/
# Or just symlink the build directory:
# sudo ln -sfn $(pwd)/build /usr/local/lib/fbpanel
```

### 3. Run with ASAN on the live display

```bash
# Set a root background so transparent panels don't stay black:
xsetroot -display :1 -solid '#2e3436'

DISPLAY=:1 \
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1 \
  /tmp/fbtest/bin/fbpanel 2>&1 | tee /tmp/fbpanel_asan.log
```

Use the helper script `scripts/run_asan.sh` for all of this in one step.

### 4. Trigger a resize to reproduce resize-related crashes

```bash
scripts/resize_cycle.sh   # resizes the display several times
```

Or manually:
```bash
DISPLAY=:1 xrandr --fb 1920x1080
sleep 1
DISPLAY=:1 xrandr --fb 1280x800
```

---

## Known bugs and fixes

### FIXED v8.3.23 — Double-free in menu_create_item (crash after ~30 s)

**Symptom:** `free(): double free detected in tcache 2` / Abort.
Appears ~30 s after startup (on first `rebuild_menu` timeout) or whenever
the system menu is rebuilt.

**Root cause:**
```c
/* menu.c — menu_create_item() */
XCG(xc, "icon", &iname, str);   /* iname = raw ptr into xconf tree */
g_free(iname);                   /* BUG: frees xconf-owned memory  */
```
`XCG(..., str)` returns the raw `x->value` pointer (not a copy).
`g_free(iname)` immediately frees the xconf node's value string.
When `menu_destroy()` → `xconf_del(m->xc)` later frees the same node,
the same pointer is freed again.

**Fix:** Remove `g_free(iname); iname = NULL;` from `menu_create_item`.
The xconf tree owns those strings; `xconf_del` frees them on destruction.

**ASAN output that identified this:**
```
==N==ERROR: AddressSanitizer: attempting double-free on 0x...
    #1 xconf_del /panel/xconf.c:91              ← second free
    #4 menu_destroy /plugins/menu/menu.c:186
    ...
freed by thread T0 here:
    #1 menu_create_item /plugins/menu/menu.c:84 ← first free
    ...
previously allocated by thread T0 here:
    #3 xconf_new /panel/xconf.c:30
    #4 xconf_new_from_systemmenu ...
```

---

### FIXED v8.3.24 — "Negative content height" GTK warning during resize

**Symptom:** After a screen resize, dozens of:
```
Gtk-WARNING: Negative content height -1 (allocation 9, extents 5x5)
             while allocating gadget (node button, owner GtkButton)
```

**Root cause:** Two interacting issues:

1. **Missing monitors-changed handler**: fbpanel only watched `_NET_DESKTOP_GEOMETRY`
   (a WM-set atom) to detect screen changes.  In bare-X / Xvfb environments
   without a WM, the panel never repositioned after an xrandr resize.

2. **No minimum height on GtkWindow**: GTK3's internal `GtkWindow::monitors-changed`
   handler fires **before** ours and calls `gtk_widget_queue_resize`.  During that
   transient layout pass the window's minimum size is determined by content —
   GtkBar reports 2 px minimum when there are no taskbar tasks.  The window
   shrank to ~9 px momentarily, giving every GtkButton an allocation of 9 px,
   which is less than the 5+5=10 px CSS extents → warning.

**Fix (v8.3.24 — panel/panel.c):**
- Connect to `GdkScreen::monitors-changed` signal so the panel repositions
  on any screen geometry change, not just WM-signalled ones.
- Call `gtk_widget_set_size_request(topgwin, -1, p->ah)` to set a hard
  minimum height that GTK cannot override during queue_resize.

**Verified clean:** ASAN build, 4-resolution resize cycle + 35 s
`rebuild_menu` timeout → 0 GTK warnings, 0 ASAN errors.

---

## xconf str vs strdup: ownership rule

| Macro                        | Ownership      | Safe to g_free? |
|------------------------------|----------------|-----------------|
| `XCG(xc, "k", &v, str)`     | xconf owns it  | **NO**          |
| `XCG(xc, "k", &v, strdup)`  | caller owns it | YES             |

Rule: **never `g_free` a pointer obtained via `XCG(..., str)`**.

---

## No windows in taskbar (bare X / no WM)

**Symptom:** fbpanel starts, no task buttons appear even when windows exist.

**Root cause:** The taskbar reads `_NET_CLIENT_LIST` from the root window,
which is populated by the window manager.  In a bare Xvfb/VNC session
without a WM, this property is never set → `tb_net_client_list` returns
early → no tasks.

**Fix (environment):** Start a lightweight WM before fbpanel:
```bash
openbox &   # or: jwm &  fluxbox &  twm &
fbpanel &
```

**No code change needed** — this is a deployment issue, not a code bug.

---

## Useful ASAN options

```bash
# Minimal — just catch heap errors, skip leak checks (faster):
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1

# Verbose — print full shadow map on error:
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:verbosity=1

# Keep running after first error (to see all errors):
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0
```

---

## Reading existing crash output (without ASAN)

When the crash is only `free(): double free detected in tcache 2`:

1. Build with ASAN and reproduce → get exact file/line
2. The `tcache N` bin index corresponds to an allocation size bucket;
   it does NOT directly tell you *what* was freed — use ASAN for that.

---

## Valgrind alternative (slower, no compile-time instrumentation needed)

```bash
DISPLAY=:1 valgrind --tool=memcheck --error-exitcode=1 \
  --track-origins=yes --leak-check=no \
  /tmp/fbtest/bin/fbpanel 2>&1 | tee /tmp/fbpanel_valgrind.log
```

Valgrind is ~20× slower than ASAN but works on the installed binary.
