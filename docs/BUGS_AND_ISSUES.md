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
**Status**: fixed (v8.3.41)

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
**Status**: fixed (v8.3.49) — comment added documenting the single-threaded assumption; cross-reference cleaned up

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
**Status**: fixed (v8.3.42)

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
**Status**: resolved in v8.3.50

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

**Resolution (v8.3.50)**: Documented as intentionally unimplemented.  Removed the
dead `priv` re-fetch.  The draw handler falls back to CSS rendering when
`priv->pixmap == NULL`, which is acceptable for current use.  No config currently
uses `BG_INHERIT`.

---

### BUG-005 — `xconf_cmp` returns FALSE when both pointers are NULL

**File**: `panel/xconf.c:374` (`xconf_cmp`)
**Severity**: minor (logic inversion)
**Status**: fixed (v8.3.47)

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
**Status**: fixed (v8.3.45)

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

---

### BUG-007 — `default_plugin_edit_config` name mismatch between header and implementation

**File**: `panel/plugin.h:123` and `panel/plugin.c:202`
**Severity**: moderate (unresolved symbol in any caller)
**Status**: fixed (v8.3.43)

**Description**:
`plugin.h` declares:
```c
GtkWidget *default_plugin_instance_edit_config(plugin_instance *pl);
```
but `plugin.c` defines:
```c
GtkWidget *default_plugin_edit_config(plugin_instance *pl)
```
The names differ (`default_plugin_instance_edit_config` vs
`default_plugin_edit_config`).  Any code that calls the header-declared
name will fail to link (or silently resolve to an unrelated symbol via
`--export-dynamic`).

**Reproduction**:
Search for callers of `default_plugin_instance_edit_config` in the codebase.
If none exist, the declaration is dead code.  If callers exist, they
get an unresolved symbol at link time.

**Suspected fix**: Rename the definition in `plugin.c` to match the header
declaration (`default_plugin_instance_edit_config`), or vice versa.  Then
verify all callers use the consistent name.

---

### BUG-008 — `fb_image_icon_theme_changed` rebuilds highlight/press pixbufs for plain images

**File**: `panel/widgets.c:fb_image_icon_theme_changed`
**Severity**: cosmetic (wasted allocation)
**Status**: fixed (v8.3.48)

**Description**:
When the icon theme changes, `fb_image_icon_theme_changed` always rebuilds
`pix[1]` (highlight) and `pix[2]` (press) via `fb_pixbuf_make_back_image`
and `fb_pixbuf_make_press_image`.  For images created with `fb_image_new`
directly (not wrapped by `fb_button_new`), `conf->hicolor` is 0 (zero-
initialised by `g_new0`).  `fb_pixbuf_make_back_image` with `hicolor == 0`
adds zero to every channel — producing a no-op RGBA copy.  These copies are
never displayed for plain images (no enter/leave handlers are connected).

**Reproduction**:
Any icon-theme change when a plain `fb_image_new` image is live.

**Suspected fix**:
Guard the pix[1]/pix[2] rebuild in `fb_image_icon_theme_changed` with
`if (conf->hicolor)` (or check whether enter/leave handlers are connected).

---

### BUG-009 — `fb_button_new` `label` parameter is silently ignored

**File**: `panel/widgets.c:fb_button_new` and `panel/widgets.h`
**Severity**: minor (undocumented missing feature)
**Status**: open

**Description**:
`fb_button_new` accepts a `gchar *label` (documented as label text) but never
uses it.  The original code has a `FIXME` comment acknowledging this.  Any
plugin passing a non-NULL label string gets no text displayed.

**Reproduction**:
Call `fb_button_new(..., "My Label")` and observe that no label appears.

**Suspected fix**:
Either implement label support (add a GtkLabel sibling to the GtkImage inside
the GtkBgbox, possibly using a GtkBox to stack them), or remove the parameter
from the API and update all callers.

---

### BUG-010 — `a_NET_WM_DESKTOP` declared twice in misc.c

**File**: `panel/misc.c:45` and `panel/misc.c:63`
**Severity**: cosmetic
**Status**: fixed (v8.3.41)

**Description**:
`a_NET_WM_DESKTOP` appears as two separate tentative definitions at file scope.
In C99 multiple tentative definitions of the same name refer to the same storage
and are silently merged by the linker — no crash or incorrect behaviour results.
However the duplication is confusing and masks the fact that `a_NET_CLOSE_WINDOW`
is declared in the global section but never given an `XInternAtom` call in
`resolve_atoms()` (it is interned elsewhere in panel.c).

**Suspected fix**: Remove the duplicate `Atom a_NET_WM_DESKTOP;` declaration.

---

### BUG-011 — `xmargin` silently ignored for percent-width non-centered panels

**File**: `panel/misc.c:calculate_width`
**Severity**: minor (surprising behaviour)
**Status**: open

**Description**:
In `calculate_width`, when `wtype == WIDTH_PERCENT` and `allign != ALLIGN_CENTER`,
the xmargin-adjustment line is commented out and replaced with a bare `;`:
```c
if (wtype == WIDTH_PERCENT)
    //*panw = MAX(scrw - xmargin, *panw);
    ;
```
This means xmargin has no effect on the panel width for percent-width left/right-
aligned panels.  The xmargin value is still used to compute the panel's X position
(further down in the function), so the panel is shifted but not shrunk to avoid
overlapping its own margin.

**Suspected fix**: Determine the intended behaviour (should percent panels honour
xmargin for width clamping?) and either restore the commented line or document
why the no-op is intentional.

---

### BUG-012 — `gdk_color_to_RRGGBB` returns a static buffer

**File**: `panel/misc.c:gdk_color_to_RRGGBB`
**Severity**: cosmetic (not thread-safe; safe in single-threaded use)
**Status**: open

**Description**:
`gdk_color_to_RRGGBB` writes into a `static gchar str[10]` and returns a pointer
to it.  The return value is invalidated by the next call to this function.
In a multi-threaded context this would be a data race; in fbpanel's single-threaded
GTK main loop it is safe.  However callers that store the pointer across GTK
iterations may see stale data.

**Suspected fix**: Return a `g_strdup_printf`-allocated string (transfer full) and
update all callers to `g_free()` the result, or document the static-buffer
semantics clearly at each call site.

---

### BUG-013 — `indent()` uses `sizeof(space)` instead of `G_N_ELEMENTS(space)`

**File**: `panel/misc.c:indent`
**Severity**: minor (potential out-of-bounds array access)
**Status**: fixed (v8.3.41)

**Description**:
```c
static gchar *space[] = { "", "    ", "        ", "            ", "                " };
if (level > sizeof(space))
    level = sizeof(space);
return space[level];
```
`sizeof(space)` is the byte size of the pointer array: 5 × 8 = 40 on 64-bit.
The array has only 5 elements (indices 0–4).  Any `level` in the range 5–40
bypasses the clamp and `space[level]` reads beyond the array — undefined behaviour.

**Reproduction**:
Call `indent(5)` on a 64-bit build.

**Suspected fix**:
```c
if (level >= G_N_ELEMENTS(space))
    level = G_N_ELEMENTS(space) - 1;
return space[level];
```

---

### BUG-014 — `p->heighttype` overwritten to HEIGHT_PIXEL unconditionally

**File**: `panel/panel.c:1095` (`panel_parse_global`)
**Severity**: minor (config option rendered ineffective)
**Status**: fixed (v8.3.44)

**Description**:
`panel_parse_global` reads `heighttype` from the xconf config at line 1053:
```c
XCG(xc, "heighttype", &p->heighttype, enum, heighttype_enum);
```
But immediately after the edge-based orientation setup (line 1095):
```c
p->heighttype = HEIGHT_PIXEL;
```
This unconditional assignment overwrites whatever the config file specified.
The `if (p->heighttype == HEIGHT_PIXEL)` block that follows will therefore
always be entered, making `HEIGHT_REQUEST` and any other heighttype values
impossible to use through the config.

**Impact**: Minor — since `HEIGHT_PIXEL` is the only working mode in practice,
normal users are unaffected.  The `HEIGHT_REQUEST` mode (auto-size to content)
is silently broken.

**Suspected fix**: Remove line 1095 or move the height clamping inside a
`if (p->heighttype == HEIGHT_PIXEL)` guard without the unconditional override.

---

### BUG-015 — `configure()` signature mismatch between misc.h and gconf_panel.c

**File**: `panel/misc.h:311` (declaration) and `panel/gconf_panel.c:413` (definition)
**Severity**: minor (latent linkage issue)
**Status**: fixed (v8.3.44)

**Description**:
`misc.h` declares:
```c
void configure();
```
but `gconf_panel.c` defines:
```c
void configure(xconf *xc)
```

The two signatures do not match.  In C, `void configure()` is an old-style
declaration meaning "takes an unspecified number of arguments" — it does not
conflict with a definition that takes `xconf *xc`.  However, any call site that
uses the `void configure()` prototype will pass no arguments, while the
implementation expects one.  The only known call site is `panel.c:panel_make_menu`
which calls `configure(p->xc)` directly without going through the misc.h
prototype (it includes gconf.h or panel.h, not the misc.h stub), so this is
currently harmless.

**Impact**: Minor — could cause a crash or silent misuse if new code calls
`configure()` with no arguments through the misc.h prototype.

**Suspected fix**: Update `misc.h` to declare `void configure(xconf *xc)` and
add `#include "xconf.h"` (or `"gconf.h"`) to misc.h if needed, or remove the
declaration from misc.h entirely since it is already declared via gconf.h.

---

### BUG-016 — `gconf_edit_color` alpha not applied to initial button display

**File**: `panel/gconf.c:gconf_edit_color` (lines 218-220)
**Severity**: cosmetic
**Status**: fixed (v8.3.46)

**Description**:
When `xc_alpha` is provided, the function reads the stored alpha value and
scales it from 0-255 to 0-0xFFFF:

```c
xconf_get_int(xc_alpha, &a);
a <<= 8; /* scale to 0..FFFF from 0..FF */
gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w), &c);  // c.alpha unchanged!
```

The variable `a` is computed but never written into `c.alpha`.  The second
`gtk_color_chooser_set_rgba` call resets the colour to `c` which still has its
original alpha (from `gdk_rgba_parse`).  So the colour button displays the
wrong alpha on initial load.

The `a` value is only stored as object data (`g_object_set_data(... "alpha" ...)`)
for use by the save callback, not for display initialisation.

**Suspected fix**:
```c
c.alpha = a / 65535.0;   /* convert 0..FFFF back to 0.0..1.0 */
gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w), &c);
```

---

### BUG-017 — `do_app_file` infinite loop when Exec ends with `%`

**File**: `plugins/menu/system_menu.c:do_app_file` (Exec format-code stripping loop)
**Severity**: minor (hang under rare edge-case)
**Status**: open

**Description**:
The loop that strips XDG Exec format codes (`%f`, `%u`, etc.) by replacing them
with spaces uses `strchr` to find the next `%`:

```c
while ((dot = strchr(action, '%'))) {
    if (dot[1] != '\0')
        dot[0] = dot[1] = ' ';
}
```

When the Exec string ends with a lone `%` (i.e., `dot[1] == '\0'`), the `if`
condition is false so `dot[0]` is not replaced.  The next call to `strchr`
finds the same `%` at the same position, resulting in an infinite loop.

This can be triggered by a malformed or hand-crafted `.desktop` file with an
`Exec` value ending in `%`.

**Suspected fix**:
Replace the while-loop approach with a forward scan that advances past the
format code regardless:

```c
for (dot = action; (dot = strchr(dot, '%')) != NULL; ) {
    if (dot[1] != '\0') {
        dot[0] = dot[1] = ' ';
        dot += 2;
    } else {
        dot[0] = ' ';
        dot += 1;
    }
}
```

---

### BUG-018 — `pager_priv::task` name and iname fields declared but never written

**File**: `plugins/pager/pager.c:struct _task`
**Severity**: cosmetic
**Status**: open

**Description**:
The `task` struct declares `char *name, *iname` fields, presumably for the
window title (analogous to the same fields in taskbar's task struct).  However,
no code in pager.c ever assigns to them.  They are always NULL (zeroed by
g_new0).  They are also never freed (harmless since always NULL, but misleading).

**Suspected fix**: Remove the fields from the struct; or populate them if
a window-title tooltip is ever added to the pager.

---

### BUG-019 — `desk::scalew` and `desk::scaleh` field names are swapped

**File**: `plugins/pager/pager.c:desk_configure_event` (scale assignment)
**Severity**: cosmetic (no visual impact)
**Status**: open

**Description**:
In `desk_configure_event()`:
```c
d->scalew = (gfloat)h / (gfloat)geom.height;  /* uses desk HEIGHT for WIDTH scale */
d->scaleh = (gfloat)w / (gfloat)geom.width;   /* uses desk WIDTH for HEIGHT scale */
```
`scalew` (which ought to be the x/width scale) is set from `desk_h/screen_h`,
and `scaleh` (which ought to be the y/height scale) is set from `desk_w/screen_w`.
The names are backwards.

In `task_update_pix()`, scalew is used to scale x and width, and scaleh is
used to scale y and height, which is also backwards by name but consistent
with the desk_configure_event assignment.

Since the desk is sized so that `daw/dah == screen_w/screen_h`, the two scale
values are numerically equal, so there is no visual error.

**Suspected fix**: Swap the field names or the assignment expressions to match
conventional semantics.

---

### BUG-020 — `pager_priv::gen_pixbuf` not freed in `pager_destructor`

**File**: `plugins/pager/pager.c:pager_destructor`
**Severity**: moderate (leak)
**Status**: open

**Description**:
`pg->gen_pixbuf` is loaded from default.xpm in `pager_constructor()`:
```c
pg->gen_pixbuf = gdk_pixbuf_new_from_xpm_data((const char **)icon_xpm);
```
It is used as the fallback icon in `task_update_pix()`.  `pager_destructor()`
does not call `g_object_unref(pg->gen_pixbuf)`, leaking one GdkPixbuf
per pager instance lifetime.

**Suspected fix**:
```c
if (pg->gen_pixbuf)
    g_object_unref(pg->gen_pixbuf);
```
Add before or after the `XFree(pg->wins)` call in `pager_destructor()`.

---

### BUG-021 — `pager_priv::dirty` field declared but never used

**File**: `plugins/pager/pager.c:struct _pager_priv`
**Severity**: cosmetic
**Status**: open

**Description**:
`pager_priv` declares an `int dirty` field.  No code reads or writes it
after the struct is zeroed by the plugin framework allocation.  The per-desk
`desk::dirty` field is what is actually used for redraw scheduling.

**Suspected fix**: Remove the field.

---

### BUG-022 — `desk::first` set in `desk_new` but never read

**File**: `plugins/pager/pager.c:desk_new` / `struct _desk`
**Severity**: cosmetic
**Status**: open

**Description**:
`desk_new()` sets `d->first = 1` but no code ever reads `d->first`.
This appears to be a stub for initialisation logic that was never completed.

**Suspected fix**: Remove the field and the assignment.

---

### BUG-023 — `task_remove_stale` does not free `t->pixbuf`

**File**: `plugins/pager/pager.c:task_remove_stale`
**Severity**: moderate (leak)
**Status**: open

**Description**:
When a window disappears from _NET_CLIENT_LIST_STACKING, `task_remove_stale()`
is called to remove it.  It removes the GDK filter and unrefs `gdkwin`, but
does NOT call `g_object_unref(t->pixbuf)`.  If the task had a loaded icon
(from get_netwm_icon or get_wm_icon), that GdkPixbuf leaks.

By contrast, `task_remove_all()` (called at destructor time) does free pixbuf:
```c
if (t->pixbuf != NULL)
    g_object_unref(t->pixbuf);
```

**Reproduction**: Open a window with an icon (e.g. a terminal with a custom
icon), close it, and repeat.  Each close leaks one GdkPixbuf.

**Suspected fix**: Add the same pixbuf free to `task_remove_stale()` before
the `g_free(t)` call.

---

### BUG-024 — `task_remove_stale` / `task_remove_every` in icons.c missing third `GHRFunc` parameter

**File**: `plugins/icons/icons.c:task_remove_stale`, `task_remove_every`
**Severity**: minor (undefined behaviour, harmless in practice)
**Status**: open

**Description**:
`g_hash_table_foreach_remove()` requires a `GHRFunc` with signature:
```c
gboolean (*GHRFunc)(gpointer key, gpointer value, gpointer user_data);
```
Both `task_remove_stale()` and `task_remove_every()` in `icons.c` are defined
with only **two** parameters (key, value) — the `user_data` parameter is
absent:
```c
static gboolean task_remove_stale(gpointer key, gpointer value) { ... }
static gboolean task_remove_every (gpointer key, gpointer value) { ... }
```
The C cast in the `g_hash_table_foreach_remove()` call silences the
compiler mismatch warning.  Because GLib passes `user_data` via register/stack
and neither function reads it, this is harmless on all common x86/x86-64
ABIs, but it is formally undefined behaviour per the C standard.

**Suspected fix**: Add the unused `gpointer user_data` third parameter to both
functions (and remove the `(GHRFunc)` cast).

---

### BUG-025 — `image_constructor` frees a transfer-none xconf string

**File**: `plugins/image/image.c:71-74` (`image_constructor`)
**Severity**: moderate (double-free on panel exit when tooltip is configured)
**Status**: open

**Description**:
`tooltip` is read via `XCG(p->xc, "tooltip", &tooltip, str)`, which gives a
**transfer-none** raw pointer into the xconf tree (never freed by the caller).
However, the constructor calls `g_free(tooltip)` after setting the markup:
```c
if (tooltip) {
    gtk_widget_set_tooltip_markup(img->mainw, tooltip);
    g_free(tooltip);   /* BUG: tooltip is transfer-none, owned by xconf */
}
```
The xconf tree owns the string.  When the panel exits and `xconf_del()` is
called, it attempts to free the same string a second time, causing a
heap double-free.  Under ASAN this is a hard abort; on plain glibc it
corrupts the allocator heap.

This only manifests when the `image` plugin is configured with a `tooltip`
key.

**Suspected fix**: Change `XCG(p->xc, "tooltip", &tooltip, str)` to
`XCG(p->xc, "tooltip", &tooltip, strdup)` so the caller owns the copy and
the `g_free(tooltip)` is correct.  Or remove the `g_free(tooltip)` and use
`str` without freeing.

---

### BUG-026 — `deskno2::fmt` field declared but never used

**File**: `plugins/deskno2/deskno2.c:struct deskno_priv` (labelled `deskno_priv`)
**Severity**: cosmetic
**Status**: open

**Description**:
The `deskno_priv` struct in `deskno2.c` includes a `char *fmt` field that is
neither read nor written anywhere after the struct is zero-initialised.
It appears to be a leftover from a planned format-string feature that was
never implemented.

**Suspected fix**: Remove the `fmt` field from the struct.
