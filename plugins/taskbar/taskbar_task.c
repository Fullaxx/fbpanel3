/**
 * @file taskbar_task.c
 * @brief Taskbar plugin — task lifecycle, name management, and icon loading.
 *
 * VISIBILITY FILTER
 * -----------------
 * task_visible() decides whether a task button should be shown.  A task is
 * visible when:
 *   - show_all_desks is set, OR the task is sticky (desktop == -1), OR
 *     the task's desktop matches the current desktop (tb->cur_desk)
 *   AND
 *   - show_iconified is set (when the task is minimised), OR
 *     show_mapped is set (when the task is not minimised).
 *
 * ACCEPT FILTERS
 * --------------
 * accept_net_wm_state():
 *   Rejects windows with _NET_WM_STATE_SKIP_TASKBAR.
 *   Optionally rejects _NET_WM_STATE_SKIP_PAGER windows (if accept_skip_pager is set).
 *
 * accept_net_wm_window_type():
 *   Rejects desktop, dock, and splash windows.
 *
 * NAME MANAGEMENT
 * ---------------
 * tk->name  = " Title "  (g_strdup_printf with leading/trailing spaces, for display)
 * tk->iname = "[Title]"  (g_strdup_printf with brackets, for iconified display)
 *
 * Both are allocated together and freed together in tk_free_names().
 * A debug counter (tb->alloc_no) tracks outstanding allocations.
 * Name source priority: _NET_WM_NAME (UTF-8) > XA_WM_NAME (ICCCM text).
 *
 * ICON LOADING
 * ------------
 * tk_update_icon() tries three sources in order:
 *   1. get_netwm_icon() — reads _NET_WM_ICON (ARGB32 pixel data via XGetWindowProperty).
 *      Data format: [width, height, ARGB pixels...].  Validates dimensions (16-256).
 *      Converts ARGB -> RGBA bytes (argbdata_to_pixdata).  Scales to iconsize x iconsize.
 *      Data is (transfer full) XFree'd.
 *
 *   2. get_wm_icon() — reads WM_HINTS icon_pixmap/icon_mask via XGetWMHints.
 *      Uses cairo-xlib (_wnck_gdk_pixbuf_get_from_pixmap) to convert X11 Pixmap
 *      to GdkPixbuf (replaces the GTK2 GdkColormap path).  Applies mask via
 *      apply_mask() to create RGBA with transparency.  Scales to iconsize x iconsize.
 *
 *   3. get_generic_icon() — returns g_object_ref(tb->gen_pixbuf), the default.xpm
 *      fallback icon.
 *
 * The old tk->pixbuf ref is g_object_unref'd whenever tk->pixbuf changes.
 *
 * URGENCY AND FLASH
 * -----------------
 * tk_has_urgency() reads XGetWMHints and checks the XUrgencyHint flag.
 * tk_flash_window() installs a g_timeout_add callback (on_flash_win) that
 * toggles GTK_STATE_FLAG_SELECTED on the button at the cursor blink interval.
 * tk_unflash_window() removes the timeout and resets the state.
 *
 * WINDOW ACTIVATION
 * -----------------
 * tk_raise_window() first switches desktops if needed (Xclimsg _NET_CURRENT_DESKTOP),
 * then either sends _NET_ACTIVE_WINDOW (if use_net_active) or calls XRaiseWindow
 * + XSetInputFocus directly.
 */

#include "taskbar_priv.h"

/**
 * task_visible - determine whether a task button should be visible.
 * @tb: Taskbar instance.
 * @tk: Task to evaluate.
 *
 * Returns: non-zero (TRUE) if the task should be shown; 0 (FALSE) if hidden.
 */
int
task_visible(taskbar_priv *tb, task *tk)
{
    DBG("%lx: desktop=%d iconified=%d \n", tk->win, tk->desktop, tk->iconified);
    RET( (tb->show_all_desks || tk->desktop == -1
            || (tk->desktop == tb->cur_desk))
        && ((tk->iconified && tb->show_iconified)
            || (!tk->iconified && tb->show_mapped)) );
}

/**
 * accept_net_wm_state - check whether a window's WM state passes the taskbar filter.
 * @nws:               Pointer to the window's net_wm_state bitfield.
 * @accept_skip_pager: If non-zero, reject windows with skip_pager set.
 *
 * Returns: non-zero if the window should appear in the taskbar; 0 to reject it.
 */
int
accept_net_wm_state(net_wm_state *nws, int accept_skip_pager)
{
    DBG("accept_skip_pager=%d  skip_taskbar=%d skip_pager=%d\n",
        accept_skip_pager,
        nws->skip_taskbar,
        nws->skip_pager);

    return !(nws->skip_taskbar || (accept_skip_pager && nws->skip_pager));
}

/**
 * accept_net_wm_window_type - check whether a window type passes the taskbar filter.
 * @nwwt: Pointer to the window's net_wm_window_type bitfield.
 *
 * Rejects desktop, dock, and splash window types.
 *
 * Returns: non-zero if the window should appear in the taskbar; 0 to reject it.
 */
int
accept_net_wm_window_type(net_wm_window_type *nwwt)
{
    DBG("desktop=%d dock=%d splash=%d\n",
        nwwt->desktop, nwwt->dock, nwwt->splash);

    return !(nwwt->desktop || nwwt->dock || nwwt->splash);
}



/**
 * tk_free_names - free the name and iname strings for a task.
 * @tk: Task whose names are freed.
 *
 * Frees tk->name and tk->iname (both g_strdup'd) and sets both to NULL.
 * Decrements tb->alloc_no to track outstanding allocations.
 * Logs a debug warning if exactly one of name/iname is NULL (partial alloc).
 * No-op if both are already NULL.
 */
void
tk_free_names(task *tk)
{
    if ((!tk->name) != (!tk->iname)) {
        DBG("tk names partially allocated \ntk->name=%s\ntk->iname %s\n",
                tk->name, tk->iname);
    }
    if (tk->name && tk->iname) {
        g_free(tk->name);
        g_free(tk->iname);
        tk->name = tk->iname = NULL;
        tk->tb->alloc_no--;
    }
    return;
}

/**
 * tk_get_names - read the window title and populate tk->name / tk->iname.
 * @tk: Task whose title is re-read.
 *
 * Frees any existing names (tk_free_names), then tries:
 *   1. get_utf8_property(a_NET_WM_NAME) — UTF-8 title (transfer full g_free).
 *   2. get_textproperty(XA_WM_NAME) — ICCCM text (transfer full g_free).
 * If a name is found, formats tk->name as " Title " and tk->iname as "[Title]"
 * (both g_strdup_printf'd; transfers full ownership to the task).
 * The raw name string from the property query is g_free'd after formatting.
 * Increments tb->alloc_no.
 */
void
tk_get_names(task *tk)
{
    char *name;

    tk_free_names(tk);
    name = get_utf8_property(tk->win,  a_NET_WM_NAME);
    DBG("a_NET_WM_NAME:%s\n", name);
    if (!name) {
        name = get_textproperty(tk->win,  XA_WM_NAME);
        DBG("XA_WM_NAME:%s\n", name);
    }
    if (name) {
        tk->name = g_strdup_printf(" %s ", name);
        tk->iname = g_strdup_printf("[%s]", name);
        g_free(name);
        tk->tb->alloc_no++;
    }
    return;
}

/**
 * tk_set_names - apply the current name to the task button's label and tooltip.
 * @tk: Task to update.
 *
 * Sets the GtkLabel text to tk->iname if iconified, otherwise tk->name.
 * (No-op if icons_only is set — the label widget does not exist.)
 * Sets the tooltip on the button to tk->name if tooltips is enabled.
 */
void
tk_set_names(task *tk)
{
    char *name;

    name = tk->iconified ? tk->iname : tk->name;
    if (!tk->tb->icons_only)
        gtk_label_set_text(GTK_LABEL(tk->label), name);
    if (tk->tb->tooltips)
        gtk_widget_set_tooltip_text(tk->button, tk->name);
    return;
}



/**
 * find_task - look up a task by X11 window ID.
 * @tb:  Taskbar instance.
 * @win: X11 window to find.
 *
 * Returns: (transfer none) task pointer if found; NULL if not in task_list.
 */
task *
find_task (taskbar_priv * tb, Window win)
{
    return g_hash_table_lookup(tb->task_list, &win);
}


/**
 * del_task - destroy a task and optionally remove it from the hash table.
 * @tb:   Taskbar instance.
 * @tk:   Task to destroy.
 * @hdel: If non-zero, also remove @tk from tb->task_list (via g_hash_table_remove).
 *        Pass hdel=0 when called from a g_hash_table_foreach_remove callback,
 *        since the hash table manages removal in that case.
 *
 * Sequence:
 *   1. Cancel flash timeout (g_source_remove tk->flash_timeout).
 *   2. Remove GDK filter and unref GdkWindow.
 *   3. gtk_widget_destroy(tk->button) — removes from bar widget tree.
 *   4. Decrement tb->num_tasks.
 *   5. tk_free_names.
 *   6. Clear tb->focused if it pointed to this task.
 *   7. Optionally g_hash_table_remove.
 *   8. g_free(tk).
 */
void
del_task (taskbar_priv * tb, task *tk, int hdel)
{
    DBG("deleting(%d)  %08x %s\n", hdel, tk->win, tk->name);
    if (tk->flash_timeout)
        g_source_remove(tk->flash_timeout);
    if (tk->gdkwin) {
        gdk_window_remove_filter(tk->gdkwin,
                (GdkFilterFunc)tb_event_filter, tb);
        g_object_unref(tk->gdkwin);
    }
    gtk_widget_destroy(tk->button);
    tb->num_tasks--;
    tk_free_names(tk);
    if (tb->focused == tk)
        tb->focused = NULL;
    if (hdel)
        g_hash_table_remove(tb->task_list, &tk->win);
    g_free(tk);
    return;
}



/**
 * task_remove_every - GHRFunc callback to remove all tasks (used on destructor).
 * @win: Hash table key (unused).
 * @tk:  Task value.
 *
 * Calls del_task with hdel=0 (hash table manages the removal itself).
 * Returns TRUE to signal the hash table to remove this entry.
 */
gboolean
task_remove_every(Window *win, task *tk)
{
    del_task(tk->tb, tk, 0);
    return TRUE;
}

/**
 * task_remove_stale - GHRFunc callback to remove tasks with zero refcount.
 * @win:  Hash table key (unused).
 * @tk:   Task value.
 * @data: Unused.
 *
 * Called by tb_net_client_list after iterating the new _NET_CLIENT_LIST.
 * Windows still in the list had their refcount incremented; windows that
 * disappeared did not.  Decrements refcount; if it reaches 0, removes the task.
 *
 * Returns TRUE to remove the entry; FALSE to keep it.
 */
gboolean
task_remove_stale(Window *win, task *tk, gpointer data)
{
    if (tk->refcount-- == 0) {
        //DBG("tb_net_list <del>: 0x%x %s\n", tk->win, tk->name);
        del_task(tk->tb, tk, 0);
        return TRUE;
    }
    return FALSE;
}

/**
 * _wnck_gdk_pixbuf_get_from_pixmap - read an X11 Pixmap into a GdkPixbuf via cairo-xlib.
 * @dest:    Optional destination pixbuf (if non-NULL, the pixels are copied here and
 *           @dest is returned; otherwise a new pixbuf is allocated).
 * @xpixmap: X11 Pixmap handle to read.
 * @src_x, @src_y: Source rectangle origin within @xpixmap.
 * @dest_x, @dest_y: Destination rectangle origin within @dest.
 * @width, @height: Rectangle dimensions.
 *
 * Uses XGetGeometry to determine the pixmap depth, XGetVisualInfo to find a
 * matching Visual, and cairo_xlib_surface_create to wrap the pixmap.
 * Then gdk_pixbuf_get_from_surface extracts RGBA pixels.
 *
 * This replaces the GTK2 GdkColormap/GdkPixmap path which was removed in GTK3.
 *
 * Returns: (transfer full) GdkPixbuf; caller must g_object_unref.
 *          Returns NULL on any failure (XGetGeometry, XGetVisualInfo, cairo error).
 */
/* GTK3: GdkPixmap/GdkColormap removed; use cairo-xlib to read X11 pixmaps. */
static GdkPixbuf*
_wnck_gdk_pixbuf_get_from_pixmap (GdkPixbuf *dest,
                                  Pixmap     xpixmap,
                                  int        src_x,
                                  int        src_y,
                                  int        dest_x,
                                  int        dest_y,
                                  int        width,
                                  int        height)
{
    Window       root_win;
    int          px, py;
    unsigned int pw, ph, pbw, pdepth;
    XVisualInfo  vinfo_tmpl, *vinfo;
    int          nvisuals;
    cairo_surface_t *surface;
    GdkPixbuf   *pixbuf;

    if (!XGetGeometry(GDK_DPY, xpixmap, &root_win,
                      &px, &py, &pw, &ph, &pbw, &pdepth))
        return NULL;

    vinfo_tmpl.screen = DefaultScreen(GDK_DPY);
    vinfo_tmpl.depth  = pdepth;
    vinfo = XGetVisualInfo(GDK_DPY, VisualScreenMask | VisualDepthMask,
                           &vinfo_tmpl, &nvisuals);
    if (!vinfo || !nvisuals)
    {
        if (vinfo) XFree(vinfo);
        return NULL;
    }

    surface = cairo_xlib_surface_create(GDK_DPY, xpixmap,
                                        vinfo[0].visual, pw, ph);
    XFree(vinfo);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surface);
        return NULL;
    }

    pixbuf = gdk_pixbuf_get_from_surface(surface, src_x, src_y, width, height);
    cairo_surface_destroy(surface);

    if (pixbuf && dest)
    {
        gdk_pixbuf_copy_area(pixbuf, 0, 0, width, height, dest, dest_x, dest_y);
        g_object_unref(pixbuf);
        return dest;
    }
    return pixbuf;
}

/**
 * apply_mask - apply a 1-bit pixmap mask to a pixbuf's alpha channel.
 * @pixbuf: Source pixbuf (may not have alpha).
 * @mask:   Mask pixbuf (RGB; pixels with R==0 are transparent, non-zero are opaque).
 *
 * Creates a new RGBA pixbuf from @pixbuf with alpha added, then writes 0 or 255
 * into the alpha channel based on the mask's red channel.
 * Processes the overlapping area (MIN of both dimensions).
 *
 * Returns: (transfer full) new RGBA GdkPixbuf; caller must g_object_unref.
 */
static GdkPixbuf*
apply_mask (GdkPixbuf *pixbuf,
            GdkPixbuf *mask)
{
  int w, h;
  int i, j;
  GdkPixbuf *with_alpha;
  guchar *src;
  guchar *dest;
  int src_stride;
  int dest_stride;

  w = MIN (gdk_pixbuf_get_width (mask), gdk_pixbuf_get_width (pixbuf));
  h = MIN (gdk_pixbuf_get_height (mask), gdk_pixbuf_get_height (pixbuf));

  with_alpha = gdk_pixbuf_add_alpha (pixbuf, FALSE, 0, 0, 0);

  dest = gdk_pixbuf_get_pixels (with_alpha);
  src = gdk_pixbuf_get_pixels (mask);

  dest_stride = gdk_pixbuf_get_rowstride (with_alpha);
  src_stride = gdk_pixbuf_get_rowstride (mask);

  i = 0;
  while (i < h)
    {
      j = 0;
      while (j < w)
        {
          guchar *s = src + i * src_stride + j * 3;
          guchar *d = dest + i * dest_stride + j * 4;

          /* s[0] == s[1] == s[2], they are 255 if the bit was set, 0
           * otherwise
           */
          if (s[0] == 0)
            d[3] = 0;   /* transparent */
          else
            d[3] = 255; /* opaque */

          ++j;
        }

      ++i;
    }

  return with_alpha;
}


/**
 * free_pixels - GdkPixbuf pixel data destructor callback.
 * @pixels: The pixel buffer to free (allocated by argbdata_to_pixdata).
 * @data:   Unused.
 *
 * Called by GdkPixbuf when the pixbuf is finalized.
 */
static void
free_pixels (guchar *pixels, gpointer data)
{
    g_free (pixels);
    return;
}


/**
 * argbdata_to_pixdata - convert _NET_WM_ICON ARGB data to RGBA byte array.
 * @argb_data: Array of gulong values in ARGB format (as returned by XGetWindowProperty
 *             for XA_CARDINAL).  On 64-bit systems each gulong is 8 bytes but only the
 *             lower 32 bits are used (Xlib pads CARDINAL to sizeof(long)).
 * @len:       Number of gulong values (pixels) to convert.
 *
 * Each ARGB value (0xAARRGGBB) is converted to RGBA bytes [R, G, B, A] by
 * shifting: rgba = (argb << 8) | (argb >> 24), then writing the four bytes.
 *
 * Returns: (transfer full) guchar* RGBA pixel array of length @len * 4;
 *          caller must g_free (via the free_pixels destructor passed to
 *          gdk_pixbuf_new_from_data).
 */
static guchar *
argbdata_to_pixdata (gulong *argb_data, int len)
{
    guchar *p, *ret;
    int i;

    ret = p = g_new (guchar, len * 4);
    if (!ret)
        return NULL;
    /* One could speed this up a lot. */
    i = 0;
    while (i < len) {
        guint32 argb;
        guint32 rgba;

        argb = argb_data[i];
        rgba = (argb << 8) | (argb >> 24);

        *p = rgba >> 24;
        ++p;
        *p = (rgba >> 16) & 0xff;
        ++p;
        *p = (rgba >> 8) & 0xff;
        ++p;
        *p = rgba & 0xff;
        ++p;

        ++i;
    }
    return ret;
}




/**
 * get_netwm_icon - load and scale a window's _NET_WM_ICON ARGB icon.
 * @tkwin: X11 client window.
 * @iw:   Desired icon width in pixels.
 * @ih:   Desired icon height in pixels.
 *
 * Reads XA_CARDINAL data from _NET_WM_ICON.  Data format:
 *   data[0] = width, data[1] = height, data[2..] = ARGB pixel values.
 * Validates that dimensions are in range [16, 256].
 * Converts via argbdata_to_pixdata, wraps in a GdkPixbuf (with free_pixels
 * destructor), and scales to iw x ih if necessary.
 *
 * The raw Xlib data is (transfer full) XFree'd internally.
 *
 * Returns: (transfer full) scaled GdkPixbuf; caller must g_object_unref.
 *          NULL on failure (property absent, dimensions out of range, alloc error).
 */
static GdkPixbuf *
get_netwm_icon(Window tkwin, int iw, int ih)
{
    gulong *data;
    GdkPixbuf *ret = NULL;
    int n;
    guchar *p;
    GdkPixbuf *src;
    int w, h;

    data = get_xaproperty(tkwin, a_NET_WM_ICON, XA_CARDINAL, &n);
    if (!data)
        return NULL;

    /* loop through all icons in data to find best fit */
    if (0) {
        gulong *tmp;
        int len;

        len = n/sizeof(gulong);
        tmp = data;
        while (len > 2) {
            int size = tmp[0] * tmp[1];
            DBG("sub-icon: %dx%d %d bytes\n", tmp[0], tmp[1], size * 4);
            len -= size + 2;
            tmp += size;
        }
    }

    if (0) {
        int i, j, nn;

        nn = MIN(10, n);
        p = (guchar *) data;
        for (i = 0; i < nn; i++) {
            for (j = 0; j < sizeof(gulong); j++)
                ERR("%02x ", (guint) p[i*sizeof(gulong) + j]);
            ERR("\n");
        }
    }

    /* check that data indeed represents icon in w + h + ARGB[] format
     * with 16x16 dimension at least */
    if (n < (16 * 16 + 1 + 1)) {
        ERR("win %lx: icon is too small or broken (size=%d)\n", tkwin, n);
        goto out;
    }
    w = data[0];
    h = data[1];
    /* check that sizes are in 64-256 range */
    if (w < 16 || w > 256 || h < 16 || h > 256) {
        ERR("win %lx: icon size (%d, %d) is not in 64-256 range\n",
            tkwin, w, h);
        goto out;
    }

    DBG("orig  %dx%d dest %dx%d\n", w, h, iw, ih);
    p = argbdata_to_pixdata(data + 2, w * h);
    if (!p)
        goto out;
    src = gdk_pixbuf_new_from_data (p, GDK_COLORSPACE_RGB, TRUE,
        8, w, h, w * 4, free_pixels, NULL);
    if (src == NULL)
        goto out;
    ret = src;
    if (w != iw || h != ih) {
        ret = gdk_pixbuf_scale_simple(src, iw, ih, GDK_INTERP_HYPER);
        g_object_unref(src);
    }

out:
    XFree(data);
    return ret;
}

/**
 * get_wm_icon - load and scale a window's WM_HINTS icon pixmap/mask.
 * @tkwin: X11 client window.
 * @iw:   Desired icon width in pixels.
 * @ih:   Desired icon height in pixels.
 *
 * Reads XGetWMHints for icon_pixmap and icon_mask.
 * Converts the icon_pixmap to a GdkPixbuf via _wnck_gdk_pixbuf_get_from_pixmap
 * (cairo-xlib path).  If a mask pixmap is present, applies it via apply_mask()
 * to create RGBA transparency.  Scales the result to iw x ih.
 *
 * All intermediate pixbufs are g_object_unref'd; the XWMHints struct is XFree'd.
 *
 * Returns: (transfer full) scaled RGBA GdkPixbuf; caller must g_object_unref.
 *          NULL if WM_HINTS is absent, icon_pixmap is None, or any conversion fails.
 */
static GdkPixbuf *
get_wm_icon(Window tkwin, int iw, int ih)
{
    XWMHints *hints;
    Pixmap xpixmap = None, xmask = None;
    Window win;
    unsigned int w, h;
    int sd;
    GdkPixbuf *ret, *masked, *pixmap, *mask = NULL;

    hints = XGetWMHints(GDK_DPY, tkwin);
    DBG("\nwm_hints %s\n", hints ? "ok" : "failed");
    if (!hints)
        return NULL;

    if ((hints->flags & IconPixmapHint))
        xpixmap = hints->icon_pixmap;
    if ((hints->flags & IconMaskHint))
        xmask = hints->icon_mask;
    DBG("flag=%ld xpixmap=%lx flag=%ld xmask=%lx\n",
        (hints->flags & IconPixmapHint), xpixmap,
        (hints->flags & IconMaskHint),  xmask);
    XFree(hints);
    if (xpixmap == None)
        return NULL;

    if (!XGetGeometry (GDK_DPY, xpixmap, &win, &sd, &sd, &w, &h,
              (guint *)&sd, (guint *)&sd)) {
        DBG("XGetGeometry failed for %x pixmap\n", (unsigned int)xpixmap);
        return NULL;
    }
    DBG("tkwin=%x icon pixmap w=%d h=%d\n", tkwin, w, h);
    pixmap = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xpixmap, 0, 0, 0, 0, w, h);
    if (!pixmap)
        return NULL;
    if (xmask != None && XGetGeometry (GDK_DPY, xmask,
              &win, &sd, &sd, &w, &h, (guint *)&sd, (guint *)&sd)) {
        mask = _wnck_gdk_pixbuf_get_from_pixmap (NULL, xmask, 0, 0, 0, 0, w, h);

        if (mask) {
            masked = apply_mask (pixmap, mask);
            g_object_unref (G_OBJECT (pixmap));
            g_object_unref (G_OBJECT (mask));
            pixmap = masked;
        }
    }
    if (!pixmap)
        return NULL;
    ret = gdk_pixbuf_scale_simple (pixmap, iw, ih, GDK_INTERP_TILES);
    g_object_unref(pixmap);

    return ret;
}

/**
 * get_generic_icon - return a ref to the taskbar's generic fallback icon.
 * @tb: Taskbar instance (owns gen_pixbuf).
 *
 * The generic icon is created from default.xpm in taskbar_build_gui.
 * Returns a new reference; caller must g_object_unref.
 *
 * Returns: (transfer full) g_object_ref'd GdkPixbuf; always non-NULL.
 */
static GdkPixbuf*
get_generic_icon(taskbar_priv *tb)
{
    g_object_ref(tb->gen_pixbuf);
    return tb->gen_pixbuf;
}

/**
 * tk_update_icon - load the best available icon for a task.
 * @tb: Taskbar instance.
 * @tk: Task whose icon is updated.
 * @a:  Atom hint: a_NET_WM_ICON or None -> try _NET_WM_ICON first;
 *      XA_WM_HINTS -> skip directly to WM_HINTS (used when only WM_HINTS changed).
 *
 * Tries icon sources in priority order (see file-level docblock).
 * If tk->pixbuf changes, the old ref is g_object_unref'd.
 */
void
tk_update_icon (taskbar_priv *tb, task *tk, Atom a)
{
    GdkPixbuf *pixbuf;

    DBG("%lx: ", tk->win);
    pixbuf = tk->pixbuf;
    if (a == a_NET_WM_ICON || a == None) {
        tk->pixbuf = get_netwm_icon(tk->win, tb->iconsize, tb->iconsize);
        tk->using_netwm_icon = (tk->pixbuf != NULL);
        DBGE("netwm_icon=%d ", tk->using_netwm_icon);
    }
    if (!tk->using_netwm_icon) {
        tk->pixbuf = get_wm_icon(tk->win, tb->iconsize, tb->iconsize);
        DBGE("wm_icon=%d ", (tk->pixbuf != NULL));
    }
    if (!tk->pixbuf) {
        tk->pixbuf = get_generic_icon(tb); // always exists
        DBGE("generic_icon=1");
    }
    if (pixbuf != tk->pixbuf) {
        if (pixbuf)
            g_object_unref(pixbuf);
    }
    DBGE(" %dx%d \n", gdk_pixbuf_get_width(tk->pixbuf),
        gdk_pixbuf_get_height(tk->pixbuf));
    return;
}

/**
 * on_flash_win - toggle button state for urgency flash animation.
 * @tk: Task to flash.
 *
 * Called by g_timeout_add at the GTK cursor blink interval.
 * Toggles between GTK_STATE_FLAG_SELECTED and tb->normal_state.
 *
 * Returns: TRUE (keep the timeout running).
 */
static gboolean
on_flash_win( task *tk )
{
    tk->flash_state = !tk->flash_state;
    gtk_widget_set_state_flags(tk->button,
          tk->flash_state ? GTK_STATE_FLAG_SELECTED : tk->tb->normal_state, TRUE);
    gtk_widget_queue_draw(tk->button);
    return TRUE;
}

/**
 * tk_flash_window - start urgency flash animation for a task.
 * @tk: Task to start flashing.
 *
 * Installs a g_timeout_add callback (on_flash_win) at the GTK cursor blink
 * interval.  No-op if flash_timeout is already set (already flashing).
 * Sets tk->flash = 1 and starts with the current flash_state inverted.
 */
void
tk_flash_window( task *tk )
{
    gint interval;
    tk->flash = 1;
    tk->flash_state = !tk->flash_state;
    if (tk->flash_timeout)
        return;
    g_object_get( gtk_widget_get_settings(tk->button),
          "gtk-cursor-blink-time", &interval, NULL );
    tk->flash_timeout = g_timeout_add(interval, (GSourceFunc)on_flash_win, tk);
}

/**
 * tk_unflash_window - stop urgency flash animation for a task.
 * @tk: Task to stop flashing.
 *
 * Removes the flash timeout (g_source_remove) and resets flash/flash_state to 0.
 * The button state is NOT reset here — tb_display() will update it on the next redraw.
 */
void
tk_unflash_window( task *tk )
{
    tk->flash = tk->flash_state = 0;
    if (tk->flash_timeout) {
        g_source_remove(tk->flash_timeout);
        tk->flash_timeout = 0;
    }
}

/**
 * tk_has_urgency - check whether a window has the XUrgencyHint flag set.
 * @tk: Task to check.
 *
 * Reads XGetWMHints and tests the XUrgencyHint bit.  Sets tk->urgency as a
 * side effect (to 0 before checking, then to 1 if the hint is found).
 * XWMHints is XFree'd internally.
 *
 * Returns: non-zero if the window has the urgency hint; 0 otherwise.
 */
gboolean
tk_has_urgency( task* tk )
{
    XWMHints* hints;

    tk->urgency = 0;
    hints = XGetWMHints(GDK_DPY, tk->win);
    if (hints) {
        if (hints->flags & XUrgencyHint) /* Got urgency hint */
            tk->urgency = 1;
        XFree( hints );
    }
    return tk->urgency;
}

/**
 * tk_raise_window - activate and raise a client window.
 * @tk:   Task to raise.
 * @time: X11 timestamp for the activation request (from the button event).
 *
 * If the window is on a different desktop, sends a _NET_CURRENT_DESKTOP
 * ClientMessage first and XSync's to ensure the WM processes it before the
 * activation request.
 *
 * Activation method:
 *   - If use_net_active: sends _NET_ACTIVE_WINDOW ClientMessage (source=2=pager).
 *   - Otherwise: calls XRaiseWindow + XSetInputFocus directly.
 */
void
tk_raise_window( task *tk, guint32 time )
{
    if (tk->desktop != -1 && tk->desktop != tk->tb->cur_desk){
        Xclimsg(GDK_ROOT_WINDOW(), a_NET_CURRENT_DESKTOP, tk->desktop,
            0, 0, 0, 0);
        XSync (GDK_DPY, False);
    }
    if(use_net_active) {
        Xclimsg(tk->win, a_NET_ACTIVE_WINDOW, 2, time, 0, 0, 0);
    }
    else {
        XRaiseWindow (GDK_DPY, tk->win);
        XSetInputFocus (GDK_DPY, tk->win, RevertToNone, CurrentTime);
    }
    DBG("XRaiseWindow %x\n", tk->win);
}
