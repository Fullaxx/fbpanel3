#include "taskbar_priv.h"

int
task_visible(taskbar_priv *tb, task *tk)
{
    DBG("%lx: desktop=%d iconified=%d \n", tk->win, tk->desktop, tk->iconified);
    RET( (tb->show_all_desks || tk->desktop == -1
            || (tk->desktop == tb->cur_desk))
        && ((tk->iconified && tb->show_iconified)
            || (!tk->iconified && tb->show_mapped)) );
}

int
accept_net_wm_state(net_wm_state *nws, int accept_skip_pager)
{
    DBG("accept_skip_pager=%d  skip_taskbar=%d skip_pager=%d\n",
        accept_skip_pager,
        nws->skip_taskbar,
        nws->skip_pager);

    return !(nws->skip_taskbar || (accept_skip_pager && nws->skip_pager));
}

int
accept_net_wm_window_type(net_wm_window_type *nwwt)
{
    DBG("desktop=%d dock=%d splash=%d\n",
        nwwt->desktop, nwwt->dock, nwwt->splash);

    return !(nwwt->desktop || nwwt->dock || nwwt->splash);
}



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



task *
find_task (taskbar_priv * tb, Window win)
{
    return g_hash_table_lookup(tb->task_list, &win);
}


void
del_task (taskbar_priv * tb, task *tk, int hdel)
{
    DBG("deleting(%d)  %08x %s\n", hdel, tk->win, tk->name);
    if (tk->flash_timeout)
        g_source_remove(tk->flash_timeout);
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



gboolean
task_remove_every(Window *win, task *tk)
{
    del_task(tk->tb, tk, 0);
    return TRUE;
}

/* tell to remove element with zero refcount */
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


static void
free_pixels (guchar *pixels, gpointer data)
{
    g_free (pixels);
    return;
}


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

static GdkPixbuf*
get_generic_icon(taskbar_priv *tb)
{
    g_object_ref(tb->gen_pixbuf);
    return tb->gen_pixbuf;
}

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

static gboolean
on_flash_win( task *tk )
{
    tk->flash_state = !tk->flash_state;
    gtk_widget_set_state_flags(tk->button,
          tk->flash_state ? GTK_STATE_FLAG_SELECTED : tk->tb->normal_state, TRUE);
    gtk_widget_queue_draw(tk->button);
    return TRUE;
}

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

void
tk_unflash_window( task *tk )
{
    tk->flash = tk->flash_state = 0;
    if (tk->flash_timeout) {
        g_source_remove(tk->flash_timeout);
        tk->flash_timeout = 0;
    }
}

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
