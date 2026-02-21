/**
 * @file eggtraymanager.c
 * @brief EggTrayManager — XEMBED system tray protocol implementation.
 *
 * Copyright (C) 2002 Anders Carlsson <andersca@gnu.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * XEMBED SYSTRAY PROTOCOL
 * -----------------------
 * The freedesktop.org System Tray Protocol works as follows:
 *
 *  1. The tray manager (this code) takes ownership of the X selection
 *     "_NET_SYSTEM_TRAY_S{n}" using a GtkInvisible widget's GDK window.
 *
 *  2. The manager broadcasts a MANAGER ClientMessage to the root window
 *     so that waiting systray applications discover the new owner.
 *
 *  3. Systray applications send a _NET_SYSTEM_TRAY_OPCODE ClientMessage
 *     with opcode SYSTEM_TRAY_REQUEST_DOCK (data.l[2] = their X window ID).
 *
 *  4. The manager creates a GtkSocket, calls gtk_socket_add_id() to XEMBED
 *     the application's window, and emits "tray_icon_added".
 *
 *  5. When the application exits, GtkSocket emits "plug_removed", which
 *     removes the socket from the hash table and emits "tray_icon_removed".
 *     Returning FALSE from the handler lets GTK destroy the socket.
 *
 *  6. Balloon messages arrive in chunks of 20 bytes as
 *     _NET_SYSTEM_TRAY_MESSAGE_DATA ClientMessages; they are reassembled
 *     into a PendingMessage and "message_sent" is emitted when complete.
 *
 *  7. If another manager takes the selection (SelectionClear event),
 *     "lost_selection" is emitted and egg_tray_manager_unmanage() is called.
 *
 * GDK WINDOW FILTER
 * -----------------
 * egg_tray_manager_window_filter() is installed on the GtkInvisible's GDK
 * window.  It intercepts ClientMessage events for the opcode and message-data
 * atoms, and SelectionClear for manager handover.  The filter is removed in
 * egg_tray_manager_unmanage().
 *
 * GTK3 CHANGES
 * ------------
 * Two GTK2 functions are no longer available:
 *  - expose_event   -> "draw" signal handler (egg_tray_manager_socket_exposed)
 *  - gdk_window_set_back_pixmap -> no-op stub (make_socket_transparent)
 * Both stubs are present for API completeness; the draw handler returns FALSE
 * and the transparency stub is a no-op because GTK3 compositing handles it.
 *
 * HASH TABLE
 * ----------
 * manager->socket_table maps X window ID (as GINT_TO_POINTER) -> GtkSocket*.
 * Keys and values are NOT owned by the table (no destroy notifiers).
 * The socket is the sole reference holder; it is destroyed by returning FALSE
 * from egg_tray_manager_plug_removed().
 */

/* eggtraymanager.c
 * Copyright (C) 2002 Anders Carlsson <andersca@gnu.org>
 */

#include <string.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include "eggtraymanager.h"
#include "eggmarshalers.h"

#define GDK_DPY GDK_DISPLAY_XDISPLAY(gdk_display_get_default())

//#define DEBUGPRN
#include "dbg.h"

/** Signal IDs for the five EggTrayManager signals. */
enum
{
  TRAY_ICON_ADDED,
  TRAY_ICON_REMOVED,
  MESSAGE_SENT,
  MESSAGE_CANCELLED,
  LOST_SELECTION,
  LAST_SIGNAL
};

/**
 * PendingMessage - in-flight balloon message being assembled from 20-byte chunks.
 *
 * Balloon messages (SYSTEM_TRAY_BEGIN_MESSAGE + SYSTEM_TRAY_MESSAGE_DATA) arrive
 * in at most 20-byte pieces.  Each piece is appended to str until remaining_len
 * reaches zero, at which point "message_sent" is emitted and the PendingMessage
 * is freed.
 */
typedef struct
{
  long id;            /**< Message ID assigned by the sending application. */
  long len;           /**< Total message length in bytes. */
  long remaining_len; /**< Bytes still expected (len - bytes received so far). */
  long timeout;       /**< Display timeout (ms) requested by the application. */
  Window window;      /**< X window of the sending application. */
  char *str;          /**< Buffer of length len+1 (NUL-terminated); (transfer full, g_free). */
} PendingMessage;

static GObjectClass *parent_class = NULL;
static guint manager_signals[LAST_SIGNAL] = { 0 };

/** Systray opcode values sent in _NET_SYSTEM_TRAY_OPCODE ClientMessages. */
#define SYSTEM_TRAY_REQUEST_DOCK    0   /**< Application requests XEMBED docking. */
#define SYSTEM_TRAY_BEGIN_MESSAGE   1   /**< Start of a balloon message. */
#define SYSTEM_TRAY_CANCEL_MESSAGE  2   /**< Cancel a pending balloon message. */

static gboolean egg_tray_manager_check_running_xscreen (Screen *xscreen);

static void egg_tray_manager_init (EggTrayManager *manager);
static void egg_tray_manager_class_init (EggTrayManagerClass *klass);

static void egg_tray_manager_finalize (GObject *object);

static void egg_tray_manager_unmanage (EggTrayManager *manager);

/**
 * egg_tray_manager_get_type - GObject type registration for EggTrayManager.
 *
 * Uses a static GType (once-initialised pattern).  Not thread-safe, but
 * fbpanel is single-threaded (GTK main loop).
 *
 * Returns: GType for EggTrayManager.
 */
GType
egg_tray_manager_get_type (void)
{
  static GType our_type = 0;

  if (our_type == 0)
    {
      static const GTypeInfo our_info =
      {
	sizeof (EggTrayManagerClass),
	(GBaseInitFunc) NULL,
	(GBaseFinalizeFunc) NULL,
	(GClassInitFunc) egg_tray_manager_class_init,
	NULL, /* class_finalize */
	NULL, /* class_data */
	sizeof (EggTrayManager),
	0,    /* n_preallocs */
	(GInstanceInitFunc) egg_tray_manager_init
      };

      our_type = g_type_register_static (G_TYPE_OBJECT, "EggTrayManager", &our_info, 0);
    }

  return our_type;

}

/**
 * egg_tray_manager_init - GObject instance initialiser.
 * @manager: New instance to initialise. (transfer none)
 *
 * Allocates the socket_table hash table (direct hash, Window as key).
 */
static void
egg_tray_manager_init (EggTrayManager *manager)
{
  manager->socket_table = g_hash_table_new (NULL, NULL);
}

/**
 * egg_tray_manager_class_init - GObject class initialiser.
 * @klass: Class being initialised. (transfer none)
 *
 * Sets the finalize vfunc and registers the five signals:
 *  - "tray_icon_added"   — G_SIGNAL_RUN_LAST; param: GTK_TYPE_SOCKET
 *  - "tray_icon_removed" — G_SIGNAL_RUN_LAST; param: GTK_TYPE_SOCKET
 *  - "message_sent"      — G_SIGNAL_RUN_LAST; params: SOCKET, STRING, LONG, LONG
 *  - "message_cancelled" — G_SIGNAL_RUN_LAST; params: SOCKET, LONG
 *  - "lost_selection"    — G_SIGNAL_RUN_LAST; no params
 */
static void
egg_tray_manager_class_init (EggTrayManagerClass *klass)
{
    GObjectClass *gobject_class;

    parent_class = g_type_class_peek_parent (klass);
    gobject_class = (GObjectClass *)klass;

    gobject_class->finalize = egg_tray_manager_finalize;

    manager_signals[TRAY_ICON_ADDED] =
        g_signal_new ("tray_icon_added",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, tray_icon_added),
              NULL, NULL,
              g_cclosure_marshal_VOID__OBJECT,
              G_TYPE_NONE, 1,
              GTK_TYPE_SOCKET);

    manager_signals[TRAY_ICON_REMOVED] =
        g_signal_new ("tray_icon_removed",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, tray_icon_removed),
              NULL, NULL,
              g_cclosure_marshal_VOID__OBJECT,
              G_TYPE_NONE, 1,
              GTK_TYPE_SOCKET);
    manager_signals[MESSAGE_SENT] =
        g_signal_new ("message_sent",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, message_sent),
              NULL, NULL,
              _egg_marshal_VOID__OBJECT_STRING_LONG_LONG,
              G_TYPE_NONE, 4,
              GTK_TYPE_SOCKET,
              G_TYPE_STRING,
              G_TYPE_LONG,
              G_TYPE_LONG);
    manager_signals[MESSAGE_CANCELLED] =
        g_signal_new ("message_cancelled",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, message_cancelled),
              NULL, NULL,
              _egg_marshal_VOID__OBJECT_LONG,
              G_TYPE_NONE, 2,
              GTK_TYPE_SOCKET,
              G_TYPE_LONG);
    manager_signals[LOST_SELECTION] =
        g_signal_new ("lost_selection",
              G_OBJECT_CLASS_TYPE (klass),
              G_SIGNAL_RUN_LAST,
              G_STRUCT_OFFSET (EggTrayManagerClass, lost_selection),
              NULL, NULL,
              g_cclosure_marshal_VOID__VOID,
              G_TYPE_NONE, 0);

}

/**
 * egg_tray_manager_finalize - GObject finalize; calls unmanage().
 * @object: GObject being finalised. (transfer none)
 *
 * Calls egg_tray_manager_unmanage() to release the X selection and GDK
 * filter before chaining to parent_class->finalize.
 * egg_tray_manager_unmanage() is idempotent (checks manager->invisible != NULL).
 */
static void
egg_tray_manager_finalize (GObject *object)
{
  EggTrayManager *manager;

  manager = EGG_TRAY_MANAGER (object);

  egg_tray_manager_unmanage (manager);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**
 * egg_tray_manager_new - allocate a new EggTrayManager.
 *
 * Returns: (transfer full) new EggTrayManager via g_object_new.
 *          Must be freed with g_object_unref().
 */
EggTrayManager *
egg_tray_manager_new (void)
{
  EggTrayManager *manager;

  manager = g_object_new (EGG_TYPE_TRAY_MANAGER, NULL);

  return manager;
}

/**
 * egg_tray_manager_plug_removed - GtkSocket "plug_removed" signal handler.
 * @socket:  GtkSocket whose embedded window was withdrawn. (transfer none)
 * @manager: EggTrayManager. (transfer none)
 *
 * Removes the window from socket_table, clears the "egg-tray-child-window"
 * object data (g_free's the Window*), and emits "tray_icon_removed".
 *
 * Returns FALSE so GTK destroys the socket widget (standard convention for
 * "plug_removed" handlers that do not want to re-use the empty socket).
 */
static gboolean
egg_tray_manager_plug_removed (GtkSocket       *socket,
    EggTrayManager  *manager)
{
    Window *window;

    window = g_object_get_data (G_OBJECT (socket), "egg-tray-child-window");

    g_hash_table_remove (manager->socket_table, GINT_TO_POINTER (*window));
    g_object_set_data (G_OBJECT (socket), "egg-tray-child-window",
        NULL);

    g_signal_emit (manager, manager_signals[TRAY_ICON_REMOVED], 0, socket);

    /* This destroys the socket. */
    return FALSE;
}

/**
 * egg_tray_manager_socket_exposed - GtkSocket "draw" signal handler (stub).
 * @widget:    The GtkSocket. (transfer none)
 * @cr:        Cairo context. (transfer none)
 * @user_data: Unused.
 *
 * No-op stub replacing the GTK2 "expose_event" handler.  Returns FALSE to
 * allow further drawing.
 */
static gboolean
egg_tray_manager_socket_exposed (GtkWidget *widget,
      cairo_t   *cr,
      gpointer   user_data)
{
    return FALSE;
}

/**
 * egg_tray_manager_make_socket_transparent - make a GtkSocket background transparent (stub).
 * @widget:    The GtkSocket. (transfer none)
 * @user_data: Unused.
 *
 * In GTK2 this set a NULL back-pixmap for X compositing transparency.
 * In GTK3 the GDK window default is already transparent; this is a no-op.
 * Called from the "realize" and "style_updated" signals.
 */
static void
egg_tray_manager_make_socket_transparent (GtkWidget *widget,
      gpointer   user_data)
{
    GdkWindow *win = gtk_widget_get_window(widget);
    if (!gtk_widget_get_has_window(widget) || win == NULL)
        return;
    /* GTK3: gdk_window_set_background_pattern removed; NULL pattern is default */
    (void)win;
}

/**
 * egg_tray_manager_socket_style_set - "style_updated" signal handler for GtkSocket.
 * @widget:         GtkSocket. (transfer none)
 * @previous_style: Previous GtkStyle (unused in GTK3). (transfer none)
 * @user_data:      Unused.
 *
 * Re-applies the transparent background after a theme change, if the widget
 * window is realised.
 */
static void
egg_tray_manager_socket_style_set (GtkWidget *widget,
      GtkStyle  *previous_style,
      gpointer   user_data)
{
    if (gtk_widget_get_window(widget) == NULL)
        return;
    egg_tray_manager_make_socket_transparent(widget, user_data);
}

/**
 * egg_tray_manager_handle_dock_request - handle SYSTEM_TRAY_REQUEST_DOCK.
 * @manager: EggTrayManager. (transfer none)
 * @xevent:  ClientMessage with data.l[2] = application's X window ID. (transfer none)
 *
 * Creates a GtkSocket to host the application's XEMBED window:
 *  1. Allocate socket, set app-paintable and EXPOSURE_MASK.
 *  2. Connect transparency/draw signals.
 *  3. Store the application window ID as "egg-tray-child-window" object data.
 *  4. Emit "tray_icon_added" so main.c can pack the socket into its GtkBar.
 *  5. If the socket was added to a window (gtk_widget_get_toplevel returns a
 *     GtkWindow): call gtk_socket_add_id() to begin XEMBED; insert into
 *     socket_table; connect "plug_removed".
 *  6. If the socket was NOT added to a window: emit "tray_icon_removed" and
 *     gtk_widget_destroy() to clean up.
 */
static void
egg_tray_manager_handle_dock_request(EggTrayManager *manager,
    XClientMessageEvent  *xevent)
{
    GtkWidget *socket;
    Window *window;

    socket = gtk_socket_new ();
    gtk_widget_set_app_paintable (socket, TRUE);
    gtk_widget_add_events (socket, GDK_EXPOSURE_MASK);

    g_signal_connect (socket, "realize",
          G_CALLBACK (egg_tray_manager_make_socket_transparent), NULL);
    g_signal_connect (socket, "draw",
          G_CALLBACK (egg_tray_manager_socket_exposed), NULL);
    g_signal_connect_after (socket, "style_updated",
          G_CALLBACK (egg_tray_manager_socket_style_set), NULL);
    gtk_widget_show (socket);


    /* We need to set the child window here
     * so that the client can call _get functions
     * in the signal handler
     */
    window = g_new (Window, 1);
    *window = xevent->data.l[2];
    DBG("plug window %lx\n", *window);
    g_object_set_data_full (G_OBJECT (socket), "egg-tray-child-window",
        window, g_free);
    g_signal_emit(manager, manager_signals[TRAY_ICON_ADDED], 0,
        socket);
    /* Add the socket only if it's been attached */
    if (GTK_IS_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(socket)))) {
        GtkRequisition req;
        XWindowAttributes wa;

        DBG("socket has window. going on\n");
        gtk_socket_add_id(GTK_SOCKET (socket), xevent->data.l[2]);
        g_signal_connect(socket, "plug_removed",
              G_CALLBACK(egg_tray_manager_plug_removed), manager);

        gdk_x11_display_error_trap_push(gdk_display_get_default());
        XGetWindowAttributes(GDK_DPY, *window, &wa);
        if (gdk_x11_display_error_trap_pop(gdk_display_get_default())) {
            ERR("can't embed window %lx\n", xevent->data.l[2]);
            goto error;
        }
        g_hash_table_insert(manager->socket_table,
            GINT_TO_POINTER(xevent->data.l[2]), socket);
        req.width = req.height = 1;
        gtk_widget_get_preferred_size(socket, &req, NULL);
        return;
    }
error:
    DBG("socket has NO window. destroy it\n");
    g_signal_emit(manager, manager_signals[TRAY_ICON_REMOVED], 0,
        socket);
    gtk_widget_destroy(socket);
    return;
}

/**
 * pending_message_free - free a PendingMessage and its string.
 * @message: PendingMessage to free. (transfer full)
 */
static void
pending_message_free (PendingMessage *message)
{
  g_free (message->str);
  g_free (message);
}

/**
 * egg_tray_manager_handle_message_data - handle _NET_SYSTEM_TRAY_MESSAGE_DATA.
 * @manager: EggTrayManager. (transfer none)
 * @xevent:  ClientMessage with up to 20 bytes of balloon message data. (transfer none)
 *
 * Finds the matching PendingMessage by window ID, appends up to 20 bytes of
 * data.  When remaining_len reaches 0, the full message is complete:
 * looks up the socket in socket_table, emits "message_sent", removes the
 * PendingMessage from manager->messages, and frees it.
 */
static void
egg_tray_manager_handle_message_data (EggTrayManager       *manager,
				       XClientMessageEvent  *xevent)
{
  GList *p;
  int len;

  /* Try to see if we can find the
   * pending message in the list
   */
  for (p = manager->messages; p; p = p->next)
    {
      PendingMessage *msg = p->data;

      if (xevent->window == msg->window)
	{
	  /* Append the message */
	  len = MIN (msg->remaining_len, 20);

	  memcpy ((msg->str + msg->len - msg->remaining_len),
		  &xevent->data, len);
	  msg->remaining_len -= len;

	  if (msg->remaining_len == 0)
	    {
	      GtkSocket *socket;

	      socket = g_hash_table_lookup (manager->socket_table, GINT_TO_POINTER (msg->window));

	      if (socket)
		{
		  g_signal_emit (manager, manager_signals[MESSAGE_SENT], 0,
				 socket, msg->str, msg->id, msg->timeout);
		}
	      manager->messages = g_list_remove_link (manager->messages,
						      p);

	      pending_message_free (msg);
	    }

	  return;
	}
    }
}

/**
 * egg_tray_manager_handle_begin_message - handle SYSTEM_TRAY_BEGIN_MESSAGE.
 * @manager: EggTrayManager. (transfer none)
 * @xevent:  ClientMessage with timeout, length, and message ID in data.l. (transfer none)
 *
 * If the same message ID from the same window is already queued, removes it
 * (replacement semantics).  Allocates a new PendingMessage and prepends it
 * to manager->messages.  Data arrives via subsequent MESSAGE_DATA events.
 */
static void
egg_tray_manager_handle_begin_message (EggTrayManager       *manager,
				       XClientMessageEvent  *xevent)
{
  GList *p;
  PendingMessage *msg;

  /* Check if the same message is
   * already in the queue and remove it if so
   */
  for (p = manager->messages; p; p = p->next)
    {
      PendingMessage *msg = p->data;

      if (xevent->window == msg->window &&
	  xevent->data.l[4] == msg->id)
	{
	  /* Hmm, we found it, now remove it */
	  pending_message_free (msg);
	  manager->messages = g_list_remove_link (manager->messages, p);
	  break;
	}
    }

  /* Now add the new message to the queue */
  msg = g_new0 (PendingMessage, 1);
  msg->window = xevent->window;
  msg->timeout = xevent->data.l[2];
  msg->len = xevent->data.l[3];
  msg->id = xevent->data.l[4];
  msg->remaining_len = msg->len;
  msg->str = g_malloc (msg->len + 1);
  msg->str[msg->len] = '\0';
  manager->messages = g_list_prepend (manager->messages, msg);
}

/**
 * egg_tray_manager_handle_cancel_message - handle SYSTEM_TRAY_CANCEL_MESSAGE.
 * @manager: EggTrayManager. (transfer none)
 * @xevent:  ClientMessage with data.l[2] = message ID to cancel. (transfer none)
 *
 * Looks up the GtkSocket for the sending window and emits "message_cancelled"
 * with the message ID if found.
 *
 * Note: does NOT remove the message from manager->messages if it is still
 * pending (only the complete-and-delivered messages are removed from the list
 * in handle_message_data).  For partial messages, the cancel is emitted but
 * the PendingMessage remains, which is a minor leak.
 */
static void
egg_tray_manager_handle_cancel_message (EggTrayManager       *manager,
					XClientMessageEvent  *xevent)
{
  GtkSocket *socket;

  socket = g_hash_table_lookup (manager->socket_table, GINT_TO_POINTER (xevent->window));

  if (socket)
    {
      g_signal_emit (manager, manager_signals[MESSAGE_CANCELLED], 0,
		     socket, xevent->data.l[2]);
    }
}

/**
 * egg_tray_manager_handle_event - dispatch a _NET_SYSTEM_TRAY_OPCODE ClientMessage.
 * @manager: EggTrayManager. (transfer none)
 * @xevent:  ClientMessage event (transfer none)
 *
 * Dispatches on data.l[1]:
 *  - SYSTEM_TRAY_REQUEST_DOCK    -> handle_dock_request; returns REMOVE.
 *  - SYSTEM_TRAY_BEGIN_MESSAGE   -> handle_begin_message; returns REMOVE.
 *  - SYSTEM_TRAY_CANCEL_MESSAGE  -> handle_cancel_message; returns REMOVE.
 *  - other                       -> returns CONTINUE.
 *
 * Returns: GDK_FILTER_REMOVE if the event was handled; GDK_FILTER_CONTINUE
 *          for unrecognised opcodes.
 */
static GdkFilterReturn
egg_tray_manager_handle_event (EggTrayManager       *manager,
			       XClientMessageEvent  *xevent)
{
  switch (xevent->data.l[1])
    {
    case SYSTEM_TRAY_REQUEST_DOCK:
      egg_tray_manager_handle_dock_request (manager, xevent);
      return GDK_FILTER_REMOVE;

    case SYSTEM_TRAY_BEGIN_MESSAGE:
      egg_tray_manager_handle_begin_message (manager, xevent);
      return GDK_FILTER_REMOVE;

    case SYSTEM_TRAY_CANCEL_MESSAGE:
      egg_tray_manager_handle_cancel_message (manager, xevent);
      return GDK_FILTER_REMOVE;
    default:
      break;
    }

  return GDK_FILTER_CONTINUE;
}

/**
 * egg_tray_manager_window_filter - GDK window filter for the selection owner window.
 * @xev:   Raw X11 event. (transfer none)
 * @event: GDK event wrapper (unused). (transfer none)
 * @data:  EggTrayManager. (transfer none)
 *
 * Installed on the GtkInvisible's GDK window via gdk_window_add_filter().
 * Handles:
 *  - ClientMessage with opcode_atom message_type: dispatches to handle_event.
 *  - ClientMessage with message_data_atom: appends balloon message data.
 *  - SelectionClear: another manager took the selection; emits "lost_selection"
 *    and calls egg_tray_manager_unmanage().
 *
 * Returns: GDK_FILTER_REMOVE for handled events; GDK_FILTER_CONTINUE for all others.
 */
static GdkFilterReturn
egg_tray_manager_window_filter (GdkXEvent *xev, GdkEvent *event, gpointer data)
{
  XEvent *xevent = (GdkXEvent *)xev;
  EggTrayManager *manager = data;

  if (xevent->type == ClientMessage)
    {
      if (xevent->xclient.message_type == manager->opcode_atom)
	{
	  return egg_tray_manager_handle_event (manager, (XClientMessageEvent *)xevent);
	}
      else if (xevent->xclient.message_type == manager->message_data_atom)
	{
	  egg_tray_manager_handle_message_data (manager, (XClientMessageEvent *)xevent);
	  return GDK_FILTER_REMOVE;
	}
    }
  else if (xevent->type == SelectionClear)
    {
      g_signal_emit (manager, manager_signals[LOST_SELECTION], 0);
      egg_tray_manager_unmanage (manager);
    }

  return GDK_FILTER_CONTINUE;
}

/**
 * egg_tray_manager_unmanage - release the systray selection and GDK filter.
 * @manager: EggTrayManager. (transfer none)
 *
 * Idempotent: returns immediately if manager->invisible is NULL (already
 * unmanaged).
 *
 * Releases the _NET_SYSTEM_TRAY_S{n} X selection (XSetSelectionOwner to None),
 * removes the GDK window filter, sets manager->invisible to NULL (before destroy,
 * to handle potential re-entrancy), and destroys + unrefs the invisible widget.
 *
 * Called from egg_tray_manager_finalize() and from the SelectionClear handler.
 */
static void
egg_tray_manager_unmanage (EggTrayManager *manager)
{
  Display *display;
  guint32 timestamp;
  GtkWidget *invisible;

  if (manager->invisible == NULL)
    return;

  invisible = manager->invisible;
  g_assert (GTK_IS_INVISIBLE (invisible));
  g_assert (gtk_widget_get_realized (invisible));
  g_assert (GDK_IS_WINDOW (gtk_widget_get_window (invisible)));

  display = GDK_WINDOW_XDISPLAY (gtk_widget_get_window (invisible));

  if (XGetSelectionOwner (display, manager->selection_atom) ==
      GDK_WINDOW_XID (gtk_widget_get_window (invisible)))
    {
      timestamp = gdk_x11_get_server_time (gtk_widget_get_window (invisible));
      XSetSelectionOwner (display, manager->selection_atom, None, timestamp);
    }

  gdk_window_remove_filter (gtk_widget_get_window (invisible), egg_tray_manager_window_filter, manager);

  manager->invisible = NULL; /* prior to destroy for reentrancy paranoia */
  gtk_widget_destroy (invisible);
  g_object_unref (G_OBJECT (invisible));
}

/**
 * egg_tray_manager_manage_xscreen - internal implementation of manage_screen.
 * @manager: EggTrayManager. (transfer none)
 * @xscreen: X11 Screen* to manage.
 *
 * Creates a GtkInvisible on the GdkScreen corresponding to @xscreen, realizes
 * it to obtain a GDK window, then:
 *  1. Constructs the selection atom name "_NET_SYSTEM_TRAY_S{n}".
 *  2. Calls XSetSelectionOwner to take ownership.
 *  3. Verifies ownership was granted.
 *  4. Broadcasts a MANAGER ClientMessage to RootWindowOfScreen.
 *  5. Interns opcode and message-data atoms.
 *  6. Installs egg_tray_manager_window_filter on the invisible's GDK window.
 *
 * On success: manager->invisible is set; returns TRUE.
 * On failure: invisible is destroyed; returns FALSE.
 *
 * Returns: TRUE if the manager is now active; FALSE if another owner exists.
 */
static gboolean
egg_tray_manager_manage_xscreen (EggTrayManager *manager, Screen *xscreen)
{
  GtkWidget *invisible;
  char *selection_atom_name;
  guint32 timestamp;
  GdkScreen *screen;

  g_return_val_if_fail (EGG_IS_TRAY_MANAGER (manager), FALSE);
  g_return_val_if_fail (manager->screen == NULL, FALSE);

  /* If there's already a manager running on the screen
   * we can't create another one.
   */
  screen = gdk_display_get_default_screen (gdk_x11_lookup_xdisplay (DisplayOfScreen (xscreen)));

  invisible = gtk_invisible_new_for_screen (screen);
  gtk_widget_realize (invisible);

  gtk_widget_add_events (invisible, GDK_PROPERTY_CHANGE_MASK | GDK_STRUCTURE_MASK);

  selection_atom_name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d",
					 XScreenNumberOfScreen (xscreen));
  manager->selection_atom = XInternAtom (DisplayOfScreen (xscreen), selection_atom_name, False);

  g_free (selection_atom_name);

  timestamp = gdk_x11_get_server_time (gtk_widget_get_window (invisible));
  XSetSelectionOwner (DisplayOfScreen (xscreen), manager->selection_atom,
		      GDK_WINDOW_XID (gtk_widget_get_window (invisible)), timestamp);

  /* Check if we were could set the selection owner successfully */
  if (XGetSelectionOwner (DisplayOfScreen (xscreen), manager->selection_atom) ==
      GDK_WINDOW_XID (gtk_widget_get_window (invisible)))
    {
      XClientMessageEvent xev;

      /* Announce new systray manager to all windows (MANAGER ClientMessage). */
      xev.type = ClientMessage;
      xev.window = RootWindowOfScreen (xscreen);
      xev.message_type = XInternAtom (DisplayOfScreen (xscreen), "MANAGER", False);

      xev.format = 32;
      xev.data.l[0] = timestamp;
      xev.data.l[1] = manager->selection_atom;
      xev.data.l[2] = GDK_WINDOW_XID (gtk_widget_get_window (invisible));
      xev.data.l[3] = 0;	/* manager specific data */
      xev.data.l[4] = 0;	/* manager specific data */

      XSendEvent (DisplayOfScreen (xscreen),
		  RootWindowOfScreen (xscreen),
		  False, StructureNotifyMask, (XEvent *)&xev);

      manager->invisible = invisible;
      g_object_ref (G_OBJECT (manager->invisible));

      manager->opcode_atom = XInternAtom (DisplayOfScreen (xscreen),
					  "_NET_SYSTEM_TRAY_OPCODE",
					  False);

      manager->message_data_atom = XInternAtom (DisplayOfScreen (xscreen),
						"_NET_SYSTEM_TRAY_MESSAGE_DATA",
						False);

      /* Add a window filter */
      gdk_window_add_filter (gtk_widget_get_window (invisible), egg_tray_manager_window_filter, manager);
      return TRUE;
    }
  else
    {
      gtk_widget_destroy (invisible);

      return FALSE;
    }
}

/**
 * egg_tray_manager_manage_screen - take the systray selection on a GdkScreen.
 * @manager: EggTrayManager. (transfer none)
 * @screen:  GdkScreen to manage. (transfer none)
 *
 * Calls egg_tray_manager_manage_xscreen with the underlying X11 Screen.
 * Requires GDK_IS_SCREEN(screen) and manager->screen == NULL.
 *
 * Returns: TRUE on success; FALSE if selection ownership failed.
 */
gboolean
egg_tray_manager_manage_screen (EggTrayManager *manager,
				GdkScreen      *screen)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), FALSE);
  g_return_val_if_fail (manager->screen == NULL, FALSE);

  return egg_tray_manager_manage_xscreen (manager,
					  GDK_SCREEN_XSCREEN (screen));
}

/**
 * egg_tray_manager_check_running_xscreen - internal implementation of check_running.
 * @xscreen: X11 Screen* to check.
 *
 * Returns: TRUE if _NET_SYSTEM_TRAY_S{n} has a current owner on @xscreen.
 */
static gboolean
egg_tray_manager_check_running_xscreen (Screen *xscreen)
{
  Atom selection_atom;
  char *selection_atom_name;

  selection_atom_name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d",
					 XScreenNumberOfScreen (xscreen));
  selection_atom = XInternAtom (DisplayOfScreen (xscreen), selection_atom_name, False);
  g_free (selection_atom_name);

  if (XGetSelectionOwner (DisplayOfScreen (xscreen), selection_atom))
    return TRUE;
  else
    return FALSE;
}

/**
 * egg_tray_manager_check_running - test whether a systray manager owns the selection.
 * @screen: GdkScreen to check. (transfer none)
 *
 * Returns: TRUE if _NET_SYSTEM_TRAY_S{n} has an X selection owner on @screen.
 */
gboolean
egg_tray_manager_check_running (GdkScreen *screen)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), FALSE);

  return egg_tray_manager_check_running_xscreen (GDK_SCREEN_XSCREEN (screen));
}

/**
 * egg_tray_manager_get_child_title - read the application's _NET_WM_NAME.
 * @manager: EggTrayManager. (transfer none)
 * @child:   GtkSocket of the embedded application. (transfer none)
 *
 * Retrieves the "egg-tray-child-window" Window ID from the socket's object
 * data, then reads _NET_WM_NAME (UTF8_STRING) via XGetWindowProperty.
 * Uses gdk_x11_display_error_trap to handle windows that have gone away.
 *
 * Returns: (transfer full) g_strndup'd title string; caller must g_free.
 *          NULL on error, invalid UTF-8, or missing property.
 */
char *
egg_tray_manager_get_child_title (EggTrayManager *manager,
				  EggTrayManagerChild *child)
{
  Window *child_window;
  Atom utf8_string, atom, type;
  int result;
  gchar *val, *retval;
  int format;
  gulong nitems;
  gulong bytes_after;
  guchar *tmp = NULL;

  g_return_val_if_fail (EGG_IS_TRAY_MANAGER (manager), NULL);
  g_return_val_if_fail (GTK_IS_SOCKET (child), NULL);

  child_window = g_object_get_data (G_OBJECT (child),
        "egg-tray-child-window");

  utf8_string = XInternAtom (GDK_DPY, "UTF8_STRING", False);
  atom = XInternAtom (GDK_DPY, "_NET_WM_NAME", False);

  gdk_x11_display_error_trap_push(gdk_display_get_default());

  result = XGetWindowProperty (GDK_DPY, *child_window, atom, 0,
        G_MAXLONG, False, utf8_string, &type, &format, &nitems,
        &bytes_after, &tmp);
  val = (gchar *) tmp;
  if (gdk_x11_display_error_trap_pop(gdk_display_get_default()) || result != Success || type != utf8_string)
    return NULL;

  if (format != 8 || nitems == 0) {
      if (val)
          XFree (val);
      return NULL;
  }

  if (!g_utf8_validate (val, nitems, NULL))
    {
      XFree (val);
      return NULL;
    }

  retval = g_strndup (val, nitems);

  XFree (val);

  return retval;

}
