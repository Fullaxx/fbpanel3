/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* eggtraymanager.h
 * Copyright (C) 2002 Anders Carlsson <andersca@gnu.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file eggtraymanager.h
 * @brief EggTrayManager — XEMBED system tray manager GObject.
 *
 * EggTrayManager implements the freedesktop.org System Tray Protocol
 * Specification.  It acquires the _NET_SYSTEM_TRAY_S{n} X selection for
 * the given screen and handles three types of ClientMessage events:
 *
 *  - SYSTEM_TRAY_REQUEST_DOCK: a systray application requests embedding.
 *    Creates a GtkSocket, calls gtk_socket_add_id() to perform XEMBED, and
 *    emits "tray_icon_added" with the socket.
 *
 *  - SYSTEM_TRAY_BEGIN_MESSAGE / SYSTEM_TRAY_MESSAGE_DATA: a systray
 *    application sends a balloon message (multi-part, 20 bytes per message).
 *    Reassembled into a PendingMessage, then "message_sent" is emitted.
 *
 *  - SYSTEM_TRAY_CANCEL_MESSAGE: cancels a pending balloon message;
 *    emits "message_cancelled".
 *
 * SelectionClear events (another manager taking over) emit "lost_selection"
 * and call egg_tray_manager_unmanage() to release resources.
 *
 * XEMBED CHILD WINDOW
 * -------------------
 * The X window ID of the embedded application is stored as GObject data
 * on the GtkSocket under the key "egg-tray-child-window" (a g_new'd Window*,
 * freed with g_free via g_object_set_data_full).  It is used as the hash
 * table key in socket_table.
 *
 * SIGNALS
 * -------
 * - "tray_icon_added"   (EggTrayManager*, GtkSocket*) — new icon embedded.
 * - "tray_icon_removed" (EggTrayManager*, GtkSocket*) — icon withdrew.
 * - "message_sent"      (EggTrayManager*, GtkSocket*, const gchar* msg,
 *                        glong id, glong timeout)      — balloon message.
 * - "message_cancelled" (EggTrayManager*, GtkSocket*, glong id) — cancel.
 * - "lost_selection"    (EggTrayManager*)             — another manager took over.
 */

#ifndef __EGG_TRAY_MANAGER_H__
#define __EGG_TRAY_MANAGER_H__

#include <gtk/gtkwidget.h>
#include <gdk/gdkx.h>

G_BEGIN_DECLS

#define EGG_TYPE_TRAY_MANAGER			(egg_tray_manager_get_type ())
#define EGG_TRAY_MANAGER(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), EGG_TYPE_TRAY_MANAGER, EggTrayManager))
#define EGG_TRAY_MANAGER_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), EGG_TYPE_TRAY_MANAGER, EggTrayManagerClass))
#define EGG_IS_TRAY_MANAGER(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), EGG_TYPE_TRAY_MANAGER))
#define EGG_IS_TRAY_MANAGER_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), EGG_TYPE_TRAY_MANAGER))
#define EGG_TRAY_MANAGER_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), EGG_TYPE_TRAY_MANAGER, EggTrayManagerClass))

typedef struct _EggTrayManager	     EggTrayManager;
typedef struct _EggTrayManagerClass  EggTrayManagerClass;

/**
 * EggTrayManagerChild - opaque type alias for GtkSocket; used in signal signatures.
 *
 * Callers receive a GtkSocket* wherever this type appears.  The actual
 * GtkSocket is the XEMBED host for one systray application window.
 */
typedef struct _EggTrayManagerChild  EggTrayManagerChild;

/**
 * EggTrayManager - XEMBED system tray manager instance.
 *
 * Manages the _NET_SYSTEM_TRAY_S{n} X selection for one GdkScreen and handles
 * dock requests, balloon messages, and selection-clear events.
 */
struct _EggTrayManager
{
  GObject parent_instance;

  Atom opcode_atom;       /**< _NET_SYSTEM_TRAY_OPCODE atom; used to identify
                           *   incoming ClientMessage dock/message requests. */
  Atom selection_atom;    /**< _NET_SYSTEM_TRAY_S{n} atom; the X selection
                           *   this manager owns on the managed screen. */
  Atom message_data_atom; /**< _NET_SYSTEM_TRAY_MESSAGE_DATA atom; identifies
                           *   balloon message continuation ClientMessages. */

  GtkWidget *invisible;   /**< GtkInvisible used to hold the X selection;
                           *   has the GDK window filter installed.
                           *   NULL after unmanage. */
  GdkScreen *screen;      /**< Screen being managed; set during manage_screen.
                           *   Not used internally (assigned but not read after init). */

  GList *messages;        /**< List of PendingMessage* for in-progress balloon
                           *   messages (multi-part, 20 bytes each).
                           *   Freed by pending_message_free when complete or cancelled. */
  GHashTable *socket_table; /**< Window (X ID as GINT_TO_POINTER) -> GtkSocket*.
                             *   Allows looking up the socket for balloon messages
                             *   and cancel requests. */
};

/**
 * EggTrayManagerClass - EggTrayManager GObject class.
 *
 * Provides GObject class infrastructure and virtual signal handlers.
 */
struct _EggTrayManagerClass
{
  GObjectClass parent_class;

  /** Emitted when a systray application successfully docks. @child is the GtkSocket. */
  void (* tray_icon_added)   (EggTrayManager      *manager,
			      EggTrayManagerChild *child);
  /** Emitted when a docked application's window is withdrawn. @child is the GtkSocket. */
  void (* tray_icon_removed) (EggTrayManager      *manager,
			      EggTrayManagerChild *child);

  /** Emitted when a complete balloon message arrives. @message is NUL-terminated text. */
  void (* message_sent)      (EggTrayManager      *manager,
			      EggTrayManagerChild *child,
			      const gchar         *message,
			      glong                id,
			      glong                timeout);

  /** Emitted when an application cancels a pending balloon message by @id. */
  void (* message_cancelled) (EggTrayManager      *manager,
			      EggTrayManagerChild *child,
			      glong                id);

  /** Emitted when this manager loses the _NET_SYSTEM_TRAY_S{n} selection. */
  void (* lost_selection)    (EggTrayManager      *manager);
};

GType           egg_tray_manager_get_type        (void);

/**
 * egg_tray_manager_check_running - test whether a systray manager is already active.
 * @screen: GdkScreen to check. (transfer none)
 *
 * Returns: TRUE if _NET_SYSTEM_TRAY_S{n} has a selection owner on @screen.
 */
gboolean        egg_tray_manager_check_running   (GdkScreen           *screen);

/**
 * egg_tray_manager_new - allocate a new EggTrayManager.
 *
 * Returns: (transfer full) new EggTrayManager; must be g_object_unref'd.
 *          Call egg_tray_manager_manage_screen() before use.
 */
EggTrayManager *egg_tray_manager_new             (void);

/**
 * egg_tray_manager_manage_screen - take the systray selection on a screen.
 * @manager: EggTrayManager to activate. (transfer none)
 * @screen:  GdkScreen to manage. (transfer none)
 *
 * Creates a GtkInvisible to hold the X selection, sends a MANAGER
 * ClientMessage to the root window announcing ownership, and installs
 * the GDK window filter for incoming ClientMessages.
 *
 * Returns: TRUE on success; FALSE if the selection could not be taken.
 */
gboolean        egg_tray_manager_manage_screen   (EggTrayManager      *manager,
						  GdkScreen           *screen);

/**
 * egg_tray_manager_get_child_title - read _NET_WM_NAME from a docked icon.
 * @manager: EggTrayManager. (transfer none)
 * @child:   GtkSocket of the docked application. (transfer none)
 *
 * Reads the _NET_WM_NAME UTF-8 property from the embedded window.
 *
 * Returns: (transfer full) newly g_strndup'd title string; caller must g_free.
 *          NULL if the property is absent, not UTF-8, or an X error occurs.
 */
char           *egg_tray_manager_get_child_title (EggTrayManager      *manager,
						  EggTrayManagerChild *child);

G_END_DECLS

#endif /* __EGG_TRAY_MANAGER_H__ */
