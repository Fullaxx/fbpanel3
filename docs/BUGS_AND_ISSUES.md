# fbpanel3 — Bugs and Issues

This file tracks suspected bugs, ownership violations, and code smells
found during the documentation pass.  Issues are **not** fixed here —
only recorded for follow-up.

For each issue, the status will be updated when a fix is committed.

**Severity levels**:
- **critical** — crash, data loss, double-free, use-after-free
- **moderate** — memory leak, incorrect behavior under specific conditions
- **minor** — logic error with low impact, edge-case wrong result
- **cosmetic** — unused variable, dead code, misleading comment

---

### BUG-001 — FbEv::desktop_names not freed in finalize

**File**: `panel/ev.c:192` (`fb_ev_finalize`)
**Severity**: minor (leak)
**Status**: open

**Description**:
`FbEv::desktop_names` is a `char **` (a `g_strv` owned by FbEv).  It is
freed in `ev_desktop_names()` (the signal handler for `EV_DESKTOP_NAMES`)
when the signal fires.  However, `fb_ev_finalize()` does not free it:

```c
static void fb_ev_finalize(GObject *object)
{
    FbEv *ev G_GNUC_UNUSED;
    ev = FB_EV(object);
    //XFreeGC(ev->dpy, ev->gc);   // commented out
    // ev->desktop_names is NOT freed here
}
```

If `EV_DESKTOP_NAMES` never fires before the panel exits (e.g., when running
without a window manager), `desktop_names` leaks.

**Impact**: Minor — FbEv is a singleton destroyed exactly once on process
exit.  The OS reclaims the memory.

**Suspected fix**:
```c
static void fb_ev_finalize(GObject *object)
{
    FbEv *ev = FB_EV(object);
    if (ev->desktop_names) {
        g_strfreev(ev->desktop_names);
        ev->desktop_names = NULL;
    }
}
```

---

### BUG-002 — FbBg::gc freed unconditionally even if pixmap is None

**File**: `panel/bg.c:166` (`fb_bg_finalize`)
**Severity**: cosmetic / defensive hardening
**Status**: open

**Description**:
`fb_bg_finalize()` calls `XFreeGC(bg->dpy, bg->gc)` unconditionally.
`bg->gc` is always allocated in `fb_bg_init()`, so this is safe.  However
the interaction with `default_bg = NULL` is subtle:

```c
static void fb_bg_finalize(GObject *object)
{
    FbBg *bg = FB_BG(object);
    if (bg->cache) {
        cairo_surface_destroy(bg->cache);
        bg->cache = NULL;
    }
    XFreeGC(bg->dpy, bg->gc);
    default_bg = NULL;   // allows singleton re-creation after finalize
}
```

Setting `default_bg = NULL` inside finalize means that if a GtkBgbox
calls `fb_bg_get_for_display()` while another GtkBgbox is still finalizing,
a new singleton could be created mid-destruction.  In practice this cannot
happen (single-threaded GTK loop, GtkBgboxes are destroyed together), but
it is an architectural assumption worth documenting.

**Suspected fix**: No code change needed; add a comment documenting the
single-threaded assumption.

---

### BUG-003 — `ev_active_window`, `ev_client_list`, `ev_client_list_stacking` not implemented in ev.c

**File**: `panel/ev.c:292-294`
**Severity**: moderate
**Status**: open

**Description**:
Three accessor functions are declared in the `.c` file as prototypes only
but never implemented:

```c
Window fb_ev_active_window(FbEv *ev);
Window *fb_ev_client_list(FbEv *ev);
Window *fb_ev_client_list_stacking(FbEv *ev);
```

These appear at the end of `ev.c` as naked declarations (no body).  The
functions are declared in `ev.h` and may be called by plugins.  If called,
the linker will resolve them to the default symbol (undefined behaviour /
link error) or they go unresolved.

**Investigation needed**: Check whether the panel links successfully and
whether any plugin actually calls these functions.  If not called, the
declarations are dead code.  If called, they need implementations analogous
to `fb_ev_current_desktop()` and `fb_ev_number_of_desktops()`.

**Suspected fix**:
```c
Window fb_ev_active_window(FbEv *ev)
{
    if (ev->active_window == None) {
        Window *data = get_xaproperty(GDK_ROOT_WINDOW(),
                                      a_NET_ACTIVE_WINDOW, XA_WINDOW, 0);
        if (data) {
            ev->active_window = *data;
            XFree(data);
        }
    }
    return ev->active_window;
}
```

---

### BUG-004 — `gtk_bgbox_set_bg_inherit` is a stub

**File**: `panel/gtkbgbox.c:358` (`gtk_bgbox_set_bg_inherit`)
**Severity**: minor
**Status**: open

**Description**:
The `BG_INHERIT` background mode is defined in the enum but its handler
is a no-op stub:

```c
static void gtk_bgbox_set_bg_inherit(GtkWidget *widget, GtkBgboxPrivate *priv)
{
    priv = gtk_bgbox_get_instance_private(GTK_BGBOX(widget));
    return;
}
```

The function reloads `priv` from the widget (overwriting the passed-in `priv`)
and returns immediately.  Nothing is actually implemented.  If a plugin or
config sets `bg_type = BG_INHERIT`, the widget will have no background.

**Impact**: Minor — no known config uses `BG_INHERIT` in current profiles.

**Suspected fix**: Either implement (copy background from parent window) or
document as intentionally unimplemented and remove from the enum.

---

### BUG-005 — `xconf_cmp` returns FALSE when both pointers are NULL

**File**: `panel/xconf.c:374` (`xconf_cmp`)
**Severity**: minor (logic inversion)
**Status**: open

**Description**:
The `xconf_cmp()` function is documented to return TRUE if trees differ.
But when both `a` and `b` are NULL:

```c
gboolean xconf_cmp(xconf *a, xconf *b)
{
    if (!(a || b))   // both NULL → !(FALSE || FALSE) = !(FALSE) = TRUE... wait
        return FALSE;
```

Actually `!(a || b)` when both are NULL: `a || b` = `NULL || NULL` = `0`
(falsy), so `!(0)` = TRUE.  But the code returns `FALSE` here.

The intended semantics appear to be: "both NULL → trees are equal → return
FALSE (no difference)".  That is correct.  The next branch:

```c
    if (!(a && b))   // exactly one is NULL
        return TRUE;
```

`!(a && b)` when one is NULL: `a && b` = `0`, so `!(0)` = TRUE → return TRUE
(they differ).  This is also correct.

So the logic is actually correct — but the `!(a || b)` expression is
confusing.  A clearer form would be `if (!a && !b) return FALSE`.

**Impact**: Cosmetic — the logic is correct but hard to read.

---

### BUG-006 — `read_block` calls `exit(1)` on parse error

**File**: `panel/xconf.c:320` (`read_block`)
**Severity**: minor (robustness)
**Status**: open

**Description**:
When the config file parser encounters an unknown token, it calls
`exit(1)` rather than returning an error:

```c
} else {
    printf("syntax error\n");
    exit(1);
}
```

This aborts the entire panel on a malformed config file, with no cleanup.
Any open GDK connections, files, or sockets are abandoned.

**Suspected fix**: Return NULL from `read_block` and propagate the error up
to `xconf_new_from_file()`, which would then also return NULL.  Callers
would need to handle NULL gracefully.
