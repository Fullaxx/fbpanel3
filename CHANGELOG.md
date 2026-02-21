## Version: 8.3.44
* bugfix: fix BUG-014 and BUG-015 in panel.c and misc.h:
  - BUG-014 (panel.c): remove the unconditional `p->heighttype = HEIGHT_PIXEL`
    assignment in panel_parse_global() that followed the XCG config read.
    The override meant the config file's "heighttype" value was always
    silently discarded; HEIGHT_REQUEST mode (auto-size to content) could
    never be activated from config. The default is already set to HEIGHT_PIXEL
    earlier in the function, so removing the override has no effect for
    standard configs that do not specify a heighttype.
  - BUG-015 (misc.h): update the configure() prototype from the old-style
    `void configure()` (unspecified parameters) to the correct prototyped
    form `void configure(xconf *xc)`, matching the gconf_panel.c definition.
    The mismatch was latent: any call via the misc.h prototype with no
    arguments would silently pass garbage as xc.

## Version: 8.3.43
* bugfix: fix BUG-007 — rename default_plugin_edit_config to
  default_plugin_instance_edit_config in plugin.c to match the declaration
  in plugin.h. The two names were mismatched: plugin.h declared
  default_plugin_instance_edit_config but plugin.c defined
  default_plugin_edit_config. Any code calling through the header-declared
  name would fail to link (or resolve to the wrong symbol via
  --export-dynamic). No callers exist today, but the mismatch was a latent
  linker bug for any new code that calls the preferences-page fallback.

## Version: 8.3.42
* bugfix: implement three missing accessor function bodies in panel/ev.c
  (BUG-003). fb_ev_active_window(), fb_ev_client_list(), and
  fb_ev_client_list_stacking() were declared in ev.h but their definitions
  in ev.c were bare prototypes with no body — calling them would resolve
  to an undefined symbol. All three now follow the same lazy-fetch pattern
  as fb_ev_current_desktop()/fb_ev_number_of_desktops(): the cached value
  is refetched from X11 via get_xaproperty() when invalidated by the
  corresponding EV_* signal, and the result is cached until the next signal.
  The Window* accessors return transfer-none pointers into FbEv's own storage
  (X11 heap); callers must not XFree them.

## Version: 8.3.41
* bugfix: fix three bugs in panel core files (no behaviour change for
  normal use; all fixes correct latent errors):
  - BUG-001 (ev.c): add g_strfreev(ev->desktop_names) in fb_ev_finalize
    so the cached desktop-name array is freed on panel exit even when the
    EV_DESKTOP_NAMES signal never fired.
  - BUG-010 (misc.c): remove duplicate tentative definition of
    a_NET_WM_DESKTOP (second copy was dead weight; both referred to the
    same storage per C99 but was confusing).
  - BUG-013 (misc.c): fix indent() bounds check — was using sizeof(space)
    (40 on 64-bit) instead of G_N_ELEMENTS(space) (5), causing out-of-bounds
    array access for level >= 5.  Changed to clamp negative values too.

## Version: 8.3.40
* docs: add light file-level, constructor, and destructor comments to all 19
  simple plugins (battery, batterytext, chart, cpu, dclock, deskno, deskno2,
  genmon, image, mem, mem2, meter, net, separator, space, tclock, user, volume,
  wincmd).  Each file receives a docblock describing purpose, config keys, main
  widget, and any non-obvious ownership notes.  Constructor and destructor
  functions receive brief parameter and return-value annotations.  Struct
  fields are annotated with ownership (transfer-none xconf pointers vs
  transfer-full allocated values) where behaviour is non-obvious.  Records
  BUG-025 (image.c: g_free on transfer-none XCG str tooltip causes double-free
  on panel exit) and BUG-026 (deskno2.c: fmt field declared but never used).
  No logic changes.

## Version: 8.3.39
* docs: add full Doxygen-style comments to plugins/launchbar/launchbar.c and
  plugins/icons/icons.c.  Documents launchbar btn/launchbar_priv struct fields
  (action ownership: expand_tilda transfer-full stored, iname transfer-none raw
  xconf pointer passed into fb_button_new), drag-and-drop TARGET_URILIST and
  TARGET_MOZ_URL handlers, Ctrl+RMB discard_release_event bitfield guard, icon
  size CSS injection, and the read_button config-key loop.  Documents icons
  plugin struct fields (wmpix_t ch: g_strdup'd g_free'd by drop_config;
  task ch: XGetClassHint X11 heap XFree'd by free_task), the pixbuf2argb
  [width, height, ARGB...] format, set_icon_maybe 3-step decision (user >
  native > default), get_user_icon NULL-wildcard matching, the two-pass stale-
  removal pattern in do_net_client_list, and the invisible=1 plugin design.
  Records BUG-024: task_remove_stale/task_remove_every missing third GHRFunc
  parameter (cast suppresses mismatch; formally UB, harmless on x86/x86-64).
  No logic changes.

## Version: 8.3.38
* docs: add full Doxygen-style comments to plugins/tray/ (main.c,
  eggtraymanager.h, eggtraymanager.c, fixedtip.c).  Documents the XEMBED
  system tray protocol (selection ownership, MANAGER ClientMessage broadcast,
  SYSTEM_TRAY_REQUEST_DOCK handling, balloon message assembly from 20-byte
  chunks, SelectionClear handover), EggTrayManager struct fields (opcode/
  selection/message_data atoms, invisible widget lifetime, socket_table
  ownership), all five signals (tray_icon_added/removed, message_sent/
  cancelled, lost_selection), GtkSocket XEMBED lifecycle, the GTK3 port
  stubs (expose/transparency no-ops), tray_priv struct fields, GtkBar
  dimension management in tray_size_alloc, FbBg background change handling,
  and fixed-position balloon tooltip positioning and GTK widget lifetime.
  No logic changes.

## Version: 8.3.37
* docs: add full Doxygen-style comments to plugins/pager/pager.c.  Documents
  task and desk struct fields (ownership, lifecycle, GDK filter, cairo surface
  management), the two-pass stale-removal pattern in do_net_client_list_stacking,
  the per-window GDK filter (pager_event_filter, PropertyNotify/ConfigureNotify
  dispatch), the desk drawing pipeline (backing surface, dirty-flag scheduling,
  desk_clear_pixmap, task_update_pix cairo rendering with GTK style colours),
  the wallpaper stub (desk_draw_bg is a no-op in the GTK3 port), dynamic desk
  creation/removal in pager_rebuild_all, and the FbEv signal connections.
  Records BUG-018 through BUG-023 (unused task fields, swapped scale naming,
  gen_pixbuf leak, unused pager_priv::dirty, desk::first stub, task_remove_stale
  pixbuf leak).  No logic changes.

## Version: 8.3.36
* docs: add full Doxygen-style comments to plugins/menu/ (menu.h, menu.c,
  system_menu.c).  Documents menu_priv struct fields (ownership of GtkMenu,
  GtkWidget button, xconf tree, timer IDs), the xconf tree expansion pipeline
  (menu_expand_xc deep-copy with systemmenu/include expansion), menu item
  construction (node dispatcher for separator/item/menu, xconf str ownership,
  expand_tilda pattern), rebuild lifecycle (schedule_rebuild_menu 2-second
  delay, rebuild_menu, check_system_menu 30-second poll), the XDG desktop
  scanner (category hash table, directory deduplication sentinel, .desktop
  filter rules, Exec format-code stripping, icon extension stripping), and
  the goto-retry empty-category pruning loop.  Records BUG-017 (do_app_file
  infinite loop when Exec ends with bare '%').  No logic changes.

## Version: 8.3.35
* docs: add full Doxygen-style comments to plugins/taskbar/ (all 5 files).
  Documents task and taskbar_priv struct fields (ownership, lifetime), the
  multi-TU plugin design (PLUGIN macro suppression, manual class registration),
  task lifecycle (create/destroy/stale-removal), icon loading priority chain
  (netwm -> wm_hints -> xpm fallback), cairo custom button rendering, all click
  callbacks (LMB raise/iconify, MMB shade, RMB menu, Ctrl+RMB panel menu),
  drag-over activation delay, deferred bar dimension update via idle callback,
  per-window GDK filter lifecycle, and the EWMH event handlers.  No logic changes.

## Version: 8.3.34
* docs: add full Doxygen-style comments to panel/gconf.h, gconf.c, gconf_panel.c,
  gconf_plugins.c, run.h, and run.c.  Documents the gconf_block row-layout helper
  (struct fields, ownership, lifecycle, signal wiring), all four edit-widget
  factories (int/enum/boolean/color) with xconf ownership semantics, the
  preferences dialog lifecycle (xconf snapshot pattern, two-copy change detection,
  static block pointers, conditional sensitivity), and the two run_app helpers.
  Logs BUG-014 (p->heighttype overwritten unconditionally), BUG-015
  (configure() signature mismatch), and BUG-016 (alpha not applied to initial
  color button display).  No logic changes.

## Version: 8.3.33
* docs: add full Doxygen-style comments to panel/panel.h and panel/panel.c.
  Documents the full startup sequence (xconf parse -> panel_start_gui ->
  plugin loading -> gtk_main), SIGUSR1 hot-reload and SIGUSR2 shutdown flows,
  EWMH event dispatch via panel_event_filter (PropertyNotify on root window),
  the three-state autohide state machine (visible/waiting/hidden), the
  panel_start_gui 11-step construction sequence, panel_parse_global with
  BUG-014 cross-reference (heighttype forced to HEIGHT_PIXEL at line 738),
  panel_stop full teardown sequence, and the main() restart loop.
  Annotates all panel struct fields with ownership and lifetime comments.
  No logic changes.

## Version: 8.3.32
* docs: add full Doxygen-style comments to panel/misc.h and panel/misc.c.
  Documents the global X11 atom table (resolve_atoms), all EWMH/ICCCM property
  query functions with their memory-ownership rules (XFree vs g_free boundary),
  the six xconf_enum tables (allign/edge/widthtype/heighttype/bool/pos/layer),
  the calculate_width/calculate_position geometry pipeline, expand_tilda,
  get_button_spacing, gcolor2rgb24, gdk_color_to_RRGGBB, and indent.
  Logs BUG-010 (a_NET_WM_DESKTOP duplicate tentative definition), BUG-011
  (xmargin ignored for percent-width non-centered panels), BUG-012
  (gdk_color_to_RRGGBB static buffer), and BUG-013 (indent uses sizeof instead
  of G_N_ELEMENTS — out-of-bounds for level > 4).  No logic changes.

## Version: 8.3.31
* docs: add full Doxygen-style comments to panel/widgets.h and panel/widgets.c.
  Documents all four public factories (fb_pixbuf_new, fb_image_new,
  fb_button_new, fb_create_calendar) with (transfer full) ownership annotations,
  the fb_image_conf_t internal struct (three-pixbuf triple, icon-theme signal
  lifecycle, iname/fname g_strdup copies), the additive-blend highlight
  algorithm in fb_pixbuf_make_back_image, the press-shrink compositing in
  fb_pixbuf_make_press_image, the g_signal_connect_swapped event routing on
  GtkBgbox, and the enter/leave vs button press return-value semantics.
  Logs BUG-008 (wasteful pixbuf rebuild for plain images on theme change) and
  BUG-009 (label parameter silently ignored in fb_button_new).  No logic changes.

## Version: 8.3.30
* docs: add full Doxygen-style comments to panel/gtkbgbox.h and panel/gtkbgbox.c.
  Documents GtkBgboxPrivate fields (pixmap ownership, FbBg singleton ref, signal
  handler ID), the four background modes (BG_NONE/BG_STYLE/BG_ROOT/BG_INHERIT),
  the manual GDK window creation in realize (required because has_window=TRUE
  prevents calling parent_class->realize), the intentional omission of
  parent_class->size_allocate (GtkBin would double-allocate the child harmlessly
  but wastefully), the three-layer draw sequence (root pixmap slice → tint
  overlay → child rendering), the memcmp optimisation in size_allocate, and
  BUG-004 cross-reference on the gtk_bgbox_set_bg_inherit stub.  No logic changes.

## Version: 8.3.29
* docs: add full Doxygen-style comments to panel/gtkbar.h and panel/gtkbar.c.
  Documents the grid layout algorithm (rows×cols from dimension and N visible
  children), the empty-bar 2×2 minimum and its relation to the v8.3.24
  "Negative content height" fix, the critical parent-class size_allocate
  requirement (v8.3.7 fix), the unused MAX_CHILD_SIZE macro, the child-cell
  clamping logic, and the prohibition on calling queue_draw from size_allocate.
  No logic changes.

## Version: 8.3.28
* docs: add full Doxygen-style comments to panel/plugin.h and panel/plugin.c.
  Documents the class registry (class_ht hash table, built-in vs dynamic
  classes, double-open/close unload pattern), the full instance lifecycle
  (plugin_load → plugin_start → plugin_stop → plugin_put), pwid ownership
  rules (plugin must not destroy it), the invisible plugin placeholder
  pattern, and the PLUGIN macro mechanics. Adds BUG-007 to
  docs/BUGS_AND_ISSUES.md (default_plugin_edit_config name mismatch between
  header declaration and implementation). No logic changes.

## Version: 8.3.27
* docs: add full Doxygen-style comments to panel/ev.h and panel/ev.c.
  Documents the EWMH event bus signal flow, the lazy invalidation pattern
  for all six cached properties, X11 vs GLib heap ownership for the cached
  arrays (XFree vs g_strfreev), the legacy unused fields in _FbEv, and
  cross-references to BUG-001 (desktop_names finalize leak) and BUG-003
  (three unimplemented accessor bodies). No logic changes.

## Version: 8.3.26
* docs: add full Doxygen-style comments to panel/bg.h and panel/bg.c.
  Documents the FbBg singleton pattern, the internal root-pixmap cache
  design (cache hit/miss logic, why xlib surfaces are transient), surface
  ownership (transfer full to callers), finalize behaviour, and cross-
  references to BUG-002 (default_bg singleton assumption). No logic changes.

## Version: 8.3.25
* docs: add full Doxygen-style comments to panel/xconf.h and panel/xconf.c.
  Documents every function's contract, parameter ownership (transfer none vs
  transfer full), and the str-vs-strdup ownership rule.  Adds a cross-reference
  to BUG-005 (xconf_cmp readability) and BUG-006 (exit(1) on parse error).
  No logic changes.

## Version: 8.3.24
* panel: respond to screen size changes without requiring a window manager.
  Connect to GdkScreen::monitors-changed (fired by GDK's built-in RandR
  event handling) to reposition and resize the panel window immediately when
  xrandr changes the display geometry.  Previously the panel only reacted to
  _NET_DESKTOP_GEOMETRY atom changes (set by WMs), leaving the panel
  off-screen in bare-X / Xvfb environments after a resize and producing
  spurious "Negative content height" GTK warnings from transiently mis-sized
  GtkButton allocations during the resize event.

## Version: 8.3.23
* menu: fix double-free crash triggered ~30 s after startup (or on first
  system-menu rebuild).  In menu_create_item(), iname was obtained via
  XCG(xc, "icon", &iname, str) which returns a raw pointer into the xconf
  tree (not a copy).  The subsequent g_free(iname) freed the xconf node's
  value in-place; when menu_destroy() later called xconf_del(m->xc), the
  same memory was freed again → "double free detected in tcache 2" / Abort.
  Fix: remove g_free(iname) — the xconf tree owns and frees that string.
  (Also remove the now-redundant iname = NULL.)
  Identified with AddressSanitizer; confirmed by ASAN backtrace showing
  first free at menu.c:84, second free at xconf.c:91.

## Version: 8.3.22
* pager, icons: replace deprecated global gdk_window_add_filter(NULL, ...)
  with per-window GDK filters, matching the pattern already used by the
  taskbar plugin since v8.3.x.  Each tracked window gets a GdkWindow
  wrapper (gdk_x11_window_foreign_new_for_display) and a per-window filter
  at task creation; the filter is removed and the GdkWindow unreffed when
  the task is freed.  No more global event interception; fully GTK3-correct.

## Version: 8.3.21
* docs: add PLUGIN_ARCHITECTURE.md — authoritative reference for the
  plugin system covering: lifecycle (load/start/stop/unload), the
  plugin_class and plugin_instance contracts, constructor return value
  semantics, the XCG() config macro, the FbEv event bus, pseudo-
  transparent backgrounds, the PLUGIN macro, helper sub-plugins (meter,
  chart), a minimal working example, and a checklist for adding a new
  plugin.  Includes a formal requirements table (R1–R8) codifying that
  plugins must never call exit() and must gracefully self-disable with
  a g_message() when hardware or resources are unavailable.

## Version: 8.3.20
* panel: make plugin start failures non-fatal (skip plugin, continue).
  Previously panel_parse_plugin() called exit(1) when any plugin's
  constructor returned 0, killing the entire panel.  Replace exit(1)
  with plugin_put(plug) + return so the failed plugin is cleanly
  unloaded and the panel continues with the remaining plugins.
* volume: change hard ERR to g_message when /dev/mixer is unavailable.
  In containers or systems without OSS/ALSA mixer support the volume
  plugin now silently disables itself instead of triggering the (now
  non-fatal) failure path.  The default config no longer needs to
  comment out the volume plugin on audio-less systems.

## Version: 8.3.19
* taskbar: replace CSS button styling with cairo draw handler (R-1 final fix).
  Three successive CSS attempts (v8.3.12, v8.3.17, v8.3.18) all produced no
  visible change in the VNC test containers — GTK3's CSS cascade is unreliable
  in minimal/themeless environments regardless of provider priority.
  Replace the CSS approach with a "draw" signal handler on each task button.
  The handler paints a light-gray gradient background and rounded border with
  cairo, then propagates drawing to the button's child (icon+label) and returns
  TRUE to suppress GTK's default button rendering.  This bypasses the theme/CSS
  pipeline entirely and guarantees visible buttons in all environments.

## Version: 8.3.18
* taskbar: fix CSS button styling by using a direct style class instead of
  a descendant selector (R-1 root-cause fix).
  The selector "#taskbar button" failed to match because GTK3's CSS node
  propagation does not reliably cross the GtkBar (box) node that sits
  between the named ancestor and the button nodes.  Switch to a ".tb-button"
  class added directly to each button's GtkStyleContext via
  gtk_style_context_add_class() in tk_build_gui(), with the CSS provider
  targeting ".tb-button" instead.  This guarantees the rules apply regardless
  of widget hierarchy depth.

## Version: 8.3.17
* taskbar: increase button brightness so buttons are clearly visible on a
  dark/transparent panel without a GTK theme (R-1 follow-up).
  The v8.3.12 CSS colors (#585858–#707070 on a #333333 panel) were too
  dark to read in practice.  Lightened to #888888–#aaaaaa for normal
  state; label text changed to #111111 (dark on light).  Added
  box-shadow:none to suppress any theme shadow that could obscure the
  background gradient.  Active (focused) buttons remain a distinctly
  darker shade for contrast.

## Version: 8.3.16
* bg.c: cache the full root pixmap as a CPU-side cairo_image_surface_t (M-4).
  Previously fb_bg_get_xroot_pix_for_win() and fb_bg_get_xroot_pix_for_area()
  each performed 1 XGetGeometry + 1 cairo_xlib_surface_create + 1 full X→CPU
  pixel transfer per call.  With N plugins sharing a transparent background,
  each wallpaper change triggered N independent X11 round-trips.
  The new fb_bg_ensure_cache() helper fills the cache once on the first call
  after a wallpaper change (or at startup), keyed on the root Pixmap ID.
  Subsequent per-widget calls crop from the in-memory cache with no additional
  X11 round-trips.  Cache is freed and reset in fb_bg_changed() and
  fb_bg_finalize().

## Version: 8.3.15
* pager: implement WM_HINTS icon loading from X Pixmaps (M-5).
  _wnck_gdk_pixbuf_get_from_pixmap was a GTK3-port stub that always
  returned NULL because gdk_pixbuf_get_from_drawable() was removed in GTK3.
  Windows with only WM_HINTS icons (no _NET_WM_ICON) showed the generic
  fallback icon in the pager thumbnail instead of their actual icon.
  Replace the stub with a cairo-xlib implementation:
  - Full-depth icon pixmap: cairo_xlib_surface_create() → copy to image
    surface → gdk_pixbuf_get_from_surface().
  - 1-bit mask bitmap: cairo_xlib_surface_create_for_bitmap() → render
    white-on-black onto an RGB image surface (black=transparent,
    white=opaque, as expected by the existing apply_mask() function).
  Add cairo-xlib include and link targets for the pager plugin in
  CMakeLists.txt (previously only taskbar had them).

## Version: 8.3.14
* Fix transparent panel appearing black when no wallpaper pixmap is set
  (_XROOTPMAP_ID atom absent, e.g. in a bare Xvfb session):
  - bg.c: emit a one-time g_message() when _XROOTPMAP_ID is not found at
    startup, telling the user to run 'xsetroot' or a wallpaper setter.
  - gtkbgbox.c (gtk_bgbox_draw): when BG_ROOT mode is active but the root
    pixmap is NULL, call gtk_render_background() as a CSS fallback instead
    of painting nothing (which left the widget transparent/black).
  - panel.c: name the panel bbox widget "panel-bg" and apply a
    #333333 dark-gray background rule at GTK_STYLE_PROVIDER_PRIORITY_FALLBACK.
    When a wallpaper IS present the BG_ROOT path paints the root pixmap
    directly (CSS is bypassed); when no pixmap is available the fallback
    CSS makes the panel visibly dark rather than invisible.

## Version: 8.3.13
* dclock: add source-tree fallback path for dclock_glyphs.png so the plugin
  works when fbpanel is run directly from the build directory without installing.
  Try IMGPREFIX (installed path) first; if the file is not found there, try
  SRCIMGPREFIX (CMAKE_SOURCE_DIR/data/images, baked in at compile time).
  Also emit a clear ERR() message when both paths fail instead of returning 0
  silently.  Added SRCIMGPREFIX define to config.h.in.

## Version: 8.3.12
* taskbar: expand GtkCssProvider CSS to give task buttons a theme-independent
  appearance.  In GTK3, GtkButton renders entirely via CSS; without a desktop
  theme installed (e.g. in a bare Xvfb session) buttons appear as solid black
  transparent rectangles.  Apply explicit background gradient, border, and
  border-radius for normal, hover (:hover), and focused-window (:active) states,
  plus a near-white label colour, so the taskbar is legible in any environment.

## Version: 8.3.11
* panel.c: fix panel_size_alloc signal handler to use GtkAllocation *
  instead of GdkRectangle * (identical layout but wrong type for the
  size-allocate signal; can confuse GObject marshaling in debug builds).
* tray/main.c: replace GTK2-era reflow hack in tray_bg_changed
  (hide + gtk_events_pending/gtk_main_iteration + show) with a single
  gtk_widget_queue_resize() call.  The old pattern forces nested event
  processing which can cause reentrancy in GTK3 signal callbacks.
* panel.c: update --help text from "GTK2+" to "GTK3".

## Version: 8.3.10
* Fix taskbar_size_alloc calling gtk_widget_queue_resize from within a
  size-allocate signal handler.  gtk_bar_set_dimension (called from the
  handler) internally calls gtk_widget_queue_resize, asking GTK to redo
  layout while layout is already in progress.  Fix: store the desired
  dimension and apply it via g_idle_add so it runs after the current
  layout pass completes.  Cancel the idle in the destructor.

## Version: 8.3.9
* Replace deprecated global gdk_window_add_filter(NULL,...) in taskbar with
  per-window filters on each tracked client window.  tb_event_filter only
  handles PropertyNotify on tracked windows; root-window property events
  already flow through fbev signals.  A GdkWindow wrapper is created via
  gdk_x11_window_foreign_new_for_display() when a window is tracked and
  released (with the filter removed) in del_task when the window is removed.

## Version: 8.3.8
* Remove deprecated gdk_window_add_filter() from gtkbgbox.c: the
  gtk_bgbox_event_filter function caught ConfigureNotify X events to call
  gtk_widget_queue_draw, but GTK3 already queues redraws on configure and
  all painting is handled by the draw vfunc.  Remove the filter, the filter
  function, and the now-unused gdkx.h include.

## Version: 8.3.7
* Fix "Drawing a gadget with negative dimensions (node box owner GtkBar)" warning
  and subsequent double-free / Abort on window resize: gtk_bar_size_allocate()
  was not calling the parent GtkBox size_allocate vfunc, so GTK3 never allocated
  the CSS gadget node for GtkBar.  Fix: call
  GTK_WIDGET_CLASS(parent_class)->size_allocate() first; remove the now-redundant
  manual gtk_widget_set_allocation() and gtk_widget_queue_draw() calls.

## Version: 8.3.6
* Fix double free / crash on VNC resize: fb_bg_composite() called
  gdk_window_begin_draw_frame() from within the size_allocate chain, corrupting
  GDK's internal paint_stack.  Fix: remove fb_bg_composite() entirely; move
  tint overlay painting into gtk_bgbox_draw() using the cairo_t provided by
  GTK3's draw vfunc.
* Fix "gdk_x11_window_get_xid: assertion 'GDK_IS_X11_WINDOW' failed" (3x at
  startup): fb_bg_get_xroot_pix_for_win() and fb_bg_get_xroot_pix_for_area()
  used gtk_offscreen_window_new() to obtain an X11 drawable — offscreen windows
  have no X11 backing in GTK3.  Rewritten to use cairo_xlib_surface_create()
  to wrap the X11 root pixmap and blit the relevant slice into a new image
  surface.  Add cairo-xlib to fbpanel binary link targets.

## Version: 8.3.5
* Fix segfault in fb_bg_composite() (triggered by transparent=true in default
  config): missing gdk_window_end_draw_frame() left the window locked; second
  call returned NULL drawing context; cairo_paint_with_alpha(NULL,...) crashed
  at address 0x4.  Also fix NULL rgba passed to gdk_cairo_set_source_rgba()
  (now built from tintcolor) and remove incorrect cairo_destroy() on a context
  owned by the drawing context.
* Fix gdk_pixbuf_scale_simple dest_width > 0 assertion: clamp taskbar iconsize
  to a minimum of 1 when panel height equals button minimum height in GTK3.
* Fix gtk_widget_set_halign on NULL: gtk_button_new() has no child in GTK3;
  remove the call on gtk_bin_get_child() immediately after button creation.


## Version: 8.3.4
* Fix segfault at startup (address 0x20 = NULL->type in class_register):
  taskbar is split across 4 TUs all compiled with -DPLUGIN; the PLUGIN macro
  in plugin.h emits a constructor-attribute ctor() with a static class_ptr in
  every TU — 3 of the 4 have class_ptr==NULL, so class_register(NULL) crashes.
  Fix: #undef PLUGIN in taskbar_priv.h and register the class manually in
  taskbar.c only.


## Version: 8.3.3
* Fix crash on startup: gtk_bgbox_realize() called parent class realize() which
  chains to gtk_widget_real_realize() — that function asserts the widget does
  NOT have its own GDK window, but GtkBgbox sets has_window=TRUE.
  Fix: create the GDK child window explicitly in gtk_bgbox_realize(), following
  the GTK3 GtkLayout/GtkDrawingArea pattern, without calling parent realize().


## Version: 8.3.2
* Fix crash on startup: remove GTK2 'size-request' signal connection (signal
  does not exist in GTK3 for GtkWindow, causing a fatal assertion in realize)
* Replace with GTK3-correct 'size-allocate' handler that preserves
  widthtype=request / heighttype=request dynamic sizing behaviour
* Fix CMakeLists.txt install rule: COPYING was renamed to LICENSE


## Version: 8.3.1
* Fix build error: restore #include "dbg.h" in battery.c for DBG() calls
  in the included os_linux.c.inc (caught by gcc on Debian trixie)


## Version: 8.3.0
* Remove 756 ENTER/RET macro invocations (vestigial GTK1-era tracing)
* Remove all #if 0 dead-code blocks (8 GTK2-era blocks across 5 files)
* Remove unused menu_pos() function and extern panel *the_panel from misc.c
* Split taskbar.c (1422 lines) into taskbar.c / taskbar_net.c / taskbar_task.c / taskbar_ui.c + shared taskbar_priv.h
* Split misc.c: image/button/calendar widgets moved to panel/widgets.c + widgets.h
* Document plugin.h: lifecycle comments, field annotations, minimal example
* Add CLAUDE.md: instructions for version/changelog discipline in Claude Code sessions
* Remove #include "dbg.h" from 10 files that no longer use any DBG/ERR macros


## Version: 8.2.0
* Fix all deferred GTK3 deprecated-API warnings (zero warnings achieved):
  - gdk_window_set_background_pattern → cairo draw() vfunc in gtkbgbox.c
  - gtk_image_menu_item_* → gtk_menu_item_* in panel.c, taskbar.c, menu.c
  - gtk_menu_popup → gtk_menu_popup_at_pointer (panel.c, taskbar.c, menu.c)
  - gtk_rc_parse_string → GtkCssProvider (taskbar.c, launchbar.c)
  - gtk_widget_set_state / GtkStateType → gtk_widget_set_state_flags / GtkStateFlags (taskbar.c)
  - gdk_display_get_pointer → GdkSeat API (panel.c)
  - gdk_display_get_screen → gdk_display_get_default_screen (eggtraymanager.c)
  - gtk_color_button_get_alpha → GdkRGBA.alpha (gconf.c)
  - gtk_misc_set_alignment/padding → gtk_widget_set_halign/valign (meter.c)
  - gtk_alignment_new → halign/valign on child widget (launchbar.c)
  - gtk_window_set_wmclass removed (panel.c)
* Add libcairo2-dev to all 6 fbpanel_builder Dockerfiles


## Version: 8.1.0
* Fix GTK3 deprecated-API warnings across panel/ and plugins/:
  - GdkPixmap / gdk_pixmap_* → cairo_surface_t (bg.c)
  - gdk_screen_* monitor API → GdkMonitor API (misc.c, panel.c)
  - gtk_statusbar_* → removed dead statusbar code
  - gdk_pixbuf_xlib_* → gdk-pixbuf-xlib still used in plugin.c, icons.c, pager.c
  - gtk_style_* → GtkStyleContext API
  - gtk_calendar_display_options → gtk_calendar_set_display_options
  - fb_create_calendar dedup: moved to misc.c, removed per-plugin copies in dclock/tclock
  - taskbar icon loading: GdkPixmap → cairo-xlib surface → GdkPixbuf


## Version: 8.0

* Port from GTK2 to GTK3 (3.0+, tested up to 3.24.49)
* Merge fbpanel_eleksir v7.2 improvements (CMake build, launchbar, translations)
* Replace all removed GTK2 APIs: GdkColormap, GdkPixmap, GdkDrawable, GdkBitmap
* Replace GDK_DISPLAY() with GDK_DISPLAY_XDISPLAY(gdk_display_get_default())
* Replace widget->window with gtk_widget_get_window()
* Replace expose_event with draw signal and cairo-based rendering
* Replace GDK_WINDOW_XWINDOW with GDK_WINDOW_XID
* Replace gdk_xid_table_lookup with gdk_x11_window_lookup_for_display()
* Replace GTK_WIDGET_MAPPED with gtk_widget_get_mapped() (removed in GTK 3.24.49)
* Replace gtk_calendar_display_options with gtk_calendar_set_display_options()
* Replace GdkColor/gdk_color_parse with GdkRGBA/gdk_rgba_parse
* Replace deprecated GTK_WIDGET_UNSET_FLAGS with gtk_widget_set_can_focus/default()
* Replace gtk_hbox_new/gtk_vbox_new with gtk_box_new()
* Remove www/ (old project website)
* Build tested on Ubuntu 20.04/22.04/24.04, Debian 11/12/13


## Version: 7.2
* Add cmake rules to build fbpanel
* Get rid of previous python2-bsed build system
* Update documetation
* Extend max amount of buttons 20->40 on launchbar


## Version: 7.1

Date: 2020-06-07 20:20:00

* Reimplement 'Xinerama support' patch for 7.0
* Fix some deprecation warnings
* Added plugin to display battery usage in text form
* Added Italian translation
* Added icon drawing support to the pager plugin
* Some other assorted fixes


## Version: 7.0

Date: 2015-12-05 08:25:36

* [#12] fix menu position for top panel
* [#11] new plugin: user menu with gravatar icon
* [#8] Fix for issue #5 (make battery plugin work with /sys)
* [#6] Rounded corners don't work with widthtype=request
* [#5] make battery plugin work with /sys
* [#4] update README
* [#2] Include option for vertical (y) and horizontal (x) margin

[#12]: https://github.com/aanatoly/fbpanel/issues/12
[#11]: https://github.com/aanatoly/fbpanel/issues/11
[#8]: https://github.com/aanatoly/fbpanel/pull/8
[#6]: https://github.com/aanatoly/fbpanel/issues/6
[#5]: https://github.com/aanatoly/fbpanel/issues/5
[#4]: https://github.com/aanatoly/fbpanel/issues/4
[#2]: https://github.com/aanatoly/fbpanel/issues/2


## Version: 6.2

* 3367953: 'move to desktop' item in taskbar menu


## Version: 6.1

New Features:

* 2977832: meter plugin  - base plugin for icons slide show
* 2977833: battery plugin
* 2981332: volume plugin
* 2981313: Enhancements to 'tclock' plugin - calendar and transparency
* multiline taskbar: new config MinTaskHeight was added to set minimal
  task/row height
* multiline launchbar: row height is MaxIconSize
* scrolling on panel changes desktops
* dclock vertical layout was implemented. It still draws digits
  horizontaly if there is enough space
* new global config MaxEelemHeight was added to limit plugin elements
  (eg icons, messages) height
* 993836: add GTK frame to non-transparent chart plugins

Fixed Bugs:

* 2990621: add charging icons to battery plugin
* 2993878: set menu icons size from panel config not from gtk rc
* fixed locale/NLS issues with configure
* chart class struct was made static
* 2979388: configure broken - problems in busybox environments
* fixing variable name check in configure
* 2985792: Menu disappears too quickly
* 2990610: panel with autohide disappears when positioned at (0,0)
* 2990620: fix dclock for vertical orientation
* 2991081: do not autohide panel when menu is open
* 3002021: reduce sensitive area of hidden panel


## Version: 6.0

* adding xlogout script
* fixing cpu and net plugins to recover after /proc read errors
* menu: new code to build system menu
* GUI configurator code was reworked
* common API to run external programs was added
* new configuration system - xconf - was introduced
* adding png icons for reboot and shutdown. They may be missing in some icon themes.
* all svg icons were removed
* automatic profile created
* show calendar as default action in dclock
* fixed 'toggle iconfig all' algorithm
* 2863566: Allow seconds in dclock plugin
* 2972256: rebuild code upon makefile changes
* 2965428: fbpanel dissapears when configuring via GUI
* 2958238: 5.6 has bugs in configure script and fails to load plugins
* 2953357: Crashes when staring Opera


## Version: 5.8

* moving config dir ~/.config
* automatic new profile creation
* removing app categories default icons
* dclock plugin pops up calendar if no action was set
* net plugin got detailed tooltip and color configs
* cpu plugin got detailed tooltip and color configs
* mem plugin was made
* allocating plugin's private section as part of instance malloc
* drag and drop fix in launchbar
* Fixed "2891558: widthtype=request does not work"


## Version: 5.7

* XRandR support (dynamic desktop geometry changes)
* Fixed "2891558: widthtype=request does not work"
* configurator redraws panel on global changes
* fixing 'toggle iconify all' algorithm


## Version: 5.6

* genmon plugin - displays command output in a panel
* CFLAGS propagation fix

## Version: 5.5

* adding static build option for debugin purposes e.g to use with valgrind
* ability to set CFLAGS from command line was added. 
  make CFLAGS=bla-bla works correctly
* fixing memory leaks in taskbar, menu and icons plugin

## Version: 5.4

* fb_image and icon loading code refactoring
* chart: making frame around a chart more distinguishable
* taskbar: enable tooltips in IconsOnly mode
* taskbar: build tooltips from text rather then from markup


## Version: 5.3

* when no icon exists in a theme, missing-image icon is substituted. theme-avare
* prevent duplicate entries in menu
* menu plugin uses simple icons, and rebuild entire menu upon theme change, rather then creating many heavy theme-aware icons, and let them update
* cpu, net plugins: linux specific code was put into ifdefs and stub for another case was created
* system menu icon was renamed to logo.png from star.png
* strip moved to separete target and not done automatically after compile
* by default make leaves output as is, to see summaries only run 'make Q=1'
* enbling dependency checking by default
* adding svn ebuild fbpanel-2009.ebuild
* adding tooltips to cpu and net plugins
* BgBox use BG_STYLE by default
* close_profile function was added to group relevant stuff
* autohide was simplified. Now it hides completly and ignoress heightWhenHidden


## Version: 5.2

* fixing segfault in menu plugin
* extra spaces in lunchbar plugin were removed
* replacing obsolete GtkTooltips with GtkTooltip
* plugins' install path is set to LIBDIR/fbpanel instead of LIBEXECDIR/fbpanel
* fixing short flash of wrong background on startup


## Version: 5.1

* Tooltips can have mark-uped text, like '<b>T</b>erminal'
* Cpu plugin is fixed and working
* Added general chart plugin (used by cpu and net monitors)
* Code layout was changed, new configure system and new makefiles set was adopted
* fixed segfault in taskbar plugin
* background pixmap drawing speed ups and bugfixes

## Version: 4.13

New Features:

* support for "above all" and "below all" layering states. Global section
  was added string variable
      Layer = None | Above | Below
* to speed start-up, panel does not have window icon, only configuator window has
* Control-Button3 click launches configureation dialog
* taskbar was changed to propagate Control-Button3 clicks to parent window i.e to panel
* launchbar was changed to propagate Control-Button3 clicks to parent window
* pager was changed to propagate Control-Button3 clicks to parent window
* dclock was changed to propagate Control-Button3 clicks to parent window
* menu was changed to propagate Control-Button3 clicks to parent window
* normal support for round corners. Config file gets new global integer option - RoundCornersRadius
* system tray transparency fix
* clock startup delay was removed
* menu: fixed segfault caused by timeout func that used stale pointer


## Version: 4.12

New Features:

* smooth icon theme change without panel reload
* autohide. Config section is part of 'Global' section

```ini
    autoHide = false
    heightWhenHidden = 2
```

* 3 sec delayed menu creation to improve start-up time

Fixed Bugs:

* icons, taskbar do not free all tasks when destroyed


## Version: 4.11

Fixed Bugs:

* black background when no bg pixmap and transparency is unset


## Version: 4.10

New Features:

* tclock: dclock was renamed to tclock = text clock
* dclock: digital blue clock. adopted from blueclock by Jochen Baier <email@Jochen-Baier.de>
* dclock: custom clock color can be set with 'color' option

```json
  Plugin {
     type = dclock
     config {
         TooltipFmt = %A %x
         Action = xterm &
         color = wheat
     }
  }
```

* menu: items are sorted by name
* menu: icon size set to 22
* launchbar: drag-n-drop now accepts urls draged from web browsers
* style changes are grouped and only last of them is processed

Fixed Bugs:

* menu: forgoten g_free's were added
* 1723786: linkage problems with --as-needed flag
* 1724852: crash if root bg is not set
* WM_STATE usage is dropped. NET_WM_STATE is used instead. affected plugins are
  taskbar and pager
* fixed bug where pager used unupdated panel->desknum instead of pager->desknum
* all Cardinal vars were changed to guint from int
* bug in Makefile.common that generated wrong names in *.dep files
* style changes are grouped and only last of them is processed


## Version: 4.9

* new menu placement to not cover panel; used in menu and taskbar
* taskbar: icons were added to task's menu (raise, iconify, close)
* access to WM_HINTS is done via XGetWMHints only and not via get_xa_property; in taskbar it fixes failure to see existing icon pixmap
* 1704709: config checks for installed devel packages


## Version: 4.8

* help text in configurator was made selectable
* pager shows desktop wallpaper
* expanding tilda (~) in action field in config files
* menu icons size was set to 24 from 22 to avoid scaling
* avoid re-moving panel to same position
* plugins section in configurator dialog suggests to edit config manually
* taskbar vertical layout was fixed
* taskbar 'icons only' mode was optimized
* fbpanel config window has nice "star" icon


## Version: 4.7

New Feature

* Build application menu from *.desktop files
* Using themed icons. Change icon theme and see fbpanel updates itself
* default config files were updates to use new functionality


## Version: 4.6

New Features

* [ 1295234 ] Detect Window "Urgency".
* Raise window when drag target is over its name in taskbar
* fixing meory leaks from XGetWindowProperty.
* fix urgency code to catch up urgency of new windows
* taskbar: correct position of task's label
* taskbar: remove extra spaces
* taskbar: do not create event box beneath task button
* taskbar: use default expose method in gtk_bar
* taskbar; use default expose method in task button
* taskbar: cleaning up dnd code
* launchbar: visual feedback on button press


## Version: 4.5

Fixed bugs

* Makefile.common overwrite/ignore CFLAGS and LDFLAGS env. variables
* rebuild dependancy Makefiles (*.dep) if their prerequisits were changed
* fixing gcc-4.1 compile warnings about signess
* removing tar from make's recursive goals
* fixing NET_WM_STRUT code to work on 64 bit platforms

New features

* porting plugins/taskbar to 64 bit
* porting plugins/icons to 64 bit
* adding LDFLAGS=-Wl,-O1 to Makefile
* adding deskno2 plugin; it shows current desktop name and allow to scroll over available desktops
* applying patch [ 1062173 ] NET_ACTIVE_WINDOW support
* hiding tray when there are no tray icons
* remove extra space around tray
* using new icons from etiquette theme. droping old ones


## Version: 4.4

New Feature

* 64-bit awarenes


## Version: 4.3

New Feature

* [1208377] raise and iconify windows with mouse wheel
* [1210550] makefile option to compile plugins statically
* makefile help was added. run 'make help' to get it
* deskno gui changes

Fixed Bugs

* deskno can't be staticaly compiled
* typo fixes
* Makefile errors for shared and static plugin build

## Version: 4.2

Fixed Bugs

* [1161921] menu image is too small
* [1106944] ERROR used before int declaration breaks build
* [1106946] -isystem needs space?
* [1206383] makefile fails if CFLAGS set on command line
* [1206385] DnD in launchbar fails if url has a space
* fixed typos in error messages

New Feature

* New code for panel's buttons. Affected plugins are wincmd, launchbar and menu
* Depreceted option menu widget was replaced by combo box
* sys tray is packed into shadowed in frame
* pad is inserted betwean tasks in a taskbar
* clock was made flat

## Version: 4.1

New Feature

* gui configuration utility
* transparency bug fixes

## Version: 4.0

New Feature

* plugins get root events via panel's proxy rather then directly
* added configure option to disable cpu plugin compilation

## Version: 3.18

New Feature

* [ 1071997 ] deskno - plugin that displays current workspace number

Fixed Bugs

* [ 1067515 ] Fixed bug with cpu monitor plugin


## Version: 3.17

Fixed Bugs

* [ 1063620 ] 3.16 crashes with gaim 1.0.2 sys tray applet

New Feature

* [ 1062524 ] CPU usage monitor


## Version: 3.16
New Feature
* taskbar does not change window icons anymore.
* invisible (no-gui) plugin type was introduced
* icons plugin was implemented. it is invisible plugin used to  changes
  window icons with desktop-wide effect.


## Version: 3.15

Fixed Bugs

* [ 1061036 ] segfault if tray restarted


## Version: 3.14

New Feature
* [ 1010699 ] A space-filler plugin
* [ 1057046 ] transparency support
* all static plugins were converted to dlls
* added -verbose command line option

Fixed Bugs

* dynamic module load fix

## Version: 3.13

New Feature

* [ 953451 ] Add include functionality for menu config file.

Fixed Bugs

* [ 1055257 ] crash with nautilus+openbox

## Version: 3.12

New Features

* [ 976592 ] Right-click Context menu for the taskbar


## Version: 3.11

* fixed [ 940441 ] pager loose track of windows


## Version: 3.10

* fix for "996174: dclock's 'WARNING **: Invalid UTF8 string'"
* config file fix


## Version: 3.9

* fix bg change in non transparent mode
* enable icon only in taskbar
* ensure all-desktop presence if starting before wm (eg openbox)
* wincmd segfault fix


## Version: 3.8

* warnings clean-up
* X11 memory leacher was fixed
* taskbar can be set to show only mapped/iconified and wins from other desktops
* transparency initial support
* gtkbar was ported to gtk2, so fbpanel is compiled with GTK_DISABLE_DEPRECETED
* initial dll support


## Version: 3.7

* rounded corners (optional)
* taskbar view fix


## Version: 3.6

* taskbar icon size fix
* menu icon size fix
* pager checks for drawable pixmap


## Version: 3.5

* Drag-n-Drop for launchbar
* menu plugin
* removed limith for max task size in taskbar


## Version: 3.4

* gtk2.2 linkage fix
* strut fix
* launchbar segfault on wrong config fix
* '&' at the end of action var in launchbar config is depreciated


## Version: 3.3

* taskbar icon size fix


## Version: 3.2

* scroll mouse in pager changes desktops
* packaging and makefiles now are ready for system wide install additionally ./configure was implemented
* systray checks for another tray already running


## Version: 3.1

* improving icon quility in taskbar
* system tray (aka notification area) support
* NET_WM_STRUT_PARTIAL and NET_WM_STRUT were implmented
* taskbar update icon image on every icon change


## Version: 3.0

* official version bump :-)


## Version: 3.0-rc-1

* porting to GTK2+. port is based on phako's patch "[ 678749 ] make it compile and work with gtk2"


## Version: 2.2

* support for XEmbed docklets via gtktray utility


## Version: 2.1

* tray plugin was written
* documentation update
* web site update


## Version: 2.0

* complete engine rewrite
* new plugin API
* pager fixes


## Version: 1.4

* bug-fixes for pager plugin


## Version: 1.3

* middle-click in taskbar will toggle shaded state of a window
* added image plugin - this is simple plugin that just shows an image
* pager eye-candy fixes
* close_module function update


## Version: 1.2

* we've got new module - pager! Yeeaa-Haa!!
* segfault on wrong config file was fixed


## Version: 1.1

* parsing engine was rewritten
* modules' static variables were converted to mallocs
* configurable size and postion of a panel
* ability to specify what modules to load
* '~' is accepted in config files


## Version: 1.0

* 1.0-rc2 was released as 1.0


## Version: 1.0-rc2

* taskbar config file was added an option to switch tooltips on/off
* added tooltips to taskbar (thanks to Joe MacDonald joe@deserted.net)


## Version: 1.0-rc1

* copyright comments were changed


## Version: 1.0-rc0

* added _NET_WM_STRUT support
* panel now is unfocusable. this fixes iconify bug under sawfish
* panel's height is calculated at run-time, instead of fixed 22


## Version: 0.11

* improved EWMH/NETWM support
* added openbox support
* added clock customization (thanks to Tooar tooar@gmx.net)
* README was rewrited
* bug fixes
