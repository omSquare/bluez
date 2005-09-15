/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2005  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 *  CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
 *  COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
 *  SOFTWARE IS DISCLAIMED.
 *
 *
 *  $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <sys/socket.h>
#include <sys/syslog.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <dbus/dbus.h>

#include "glib-ectomy.h"

#include "hcid.h"

static DBusConnection *connection;

#define TIMEOUT (30 * 1000)		/* 30 seconds */

#define SERVICE_NAME "org.bluez.PinAgent"
#define INTERFACE_NAME SERVICE_NAME
#define REQUEST_NAME "PinRequest"
#define PATH_NAME "/org/bluez/PinAgent"

#define WRONG_ARGS_ERROR "org.bluez.Error.WrongArgs"

struct pin_request {
	int dev;
	bdaddr_t bda;
};

static void reply_handler_function(DBusPendingCall *call, void *user_data)
{
	struct pin_request *req = (struct pin_request *) user_data;
	pin_code_reply_cp pr;
	DBusMessage *message;
	DBusMessageIter iter;
	int arg_type;
	int msg_type;
	size_t len;
	char *pin;
	const char *error_msg;
        
	message = dbus_pending_call_steal_reply(call);
        
	if (message) {
		msg_type = dbus_message_get_type(message);
		dbus_message_iter_init(message, &iter);
		
		if (msg_type == DBUS_MESSAGE_TYPE_ERROR) {
			dbus_message_iter_get_basic(&iter, &error_msg);

			/* handling WRONG_ARGS_ERROR, DBUS_ERROR_NO_REPLY, DBUS_ERROR_SERVICE_UNKNOWN */
			syslog(LOG_ERR, "%s: %s", dbus_message_get_error_name(message), error_msg);
			hci_send_cmd(req->dev, OGF_LINK_CTL,
					OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);
		} else {
			/* check signature */
			arg_type = dbus_message_iter_get_arg_type(&iter);
			if (arg_type != DBUS_TYPE_STRING) {
				syslog(LOG_ERR, "Wrong reply signature: expected PIN");
				hci_send_cmd(req->dev, OGF_LINK_CTL,
						OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);
			} else {
				dbus_message_iter_get_basic(&iter, &pin);
				len = strlen(pin);
				
				memset(&pr, 0, sizeof(pr));
				bacpy(&pr.bdaddr, &req->bda);
				memcpy(pr.pin_code, pin, len);
				pr.pin_len = len;
				hci_send_cmd(req->dev, OGF_LINK_CTL, OCF_PIN_CODE_REPLY,
						PIN_CODE_REPLY_CP_SIZE, &pr);
			}
                }

		dbus_message_unref(message);
	}

	dbus_pending_call_unref(call);
}

static void free_pin_req(void *req)
{
	free(req);
}

void hcid_dbus_request_pin(int dev, struct hci_conn_info *ci)
{
	DBusMessage *message;
	DBusPendingCall *pending = NULL;
	struct pin_request *req;
	uint8_t *addr = (uint8_t *) &ci->bdaddr;

	message = dbus_message_new_method_call(SERVICE_NAME, PATH_NAME,
						INTERFACE_NAME, REQUEST_NAME);
	if (message == NULL) {
		syslog(LOG_ERR, "Couldn't allocate D-BUS message");
		goto failed;
	}

	req = malloc(sizeof(*req));
	req->dev = dev;
	bacpy(&req->bda, &ci->bdaddr);

	dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &ci->out,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&addr, sizeof(bdaddr_t), DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
					&pending, TIMEOUT) == FALSE) {
		syslog(LOG_ERR, "D-BUS send failed");
		goto failed;
	}

	dbus_pending_call_set_notify(pending, reply_handler_function,
							req, free_pin_req);

	dbus_connection_flush(connection);

	dbus_message_unref(message);

	return;

failed:
	dbus_message_unref(message);
	hci_send_cmd(dev, OGF_LINK_CTL,
				OCF_PIN_CODE_NEG_REPLY, 6, &ci->bdaddr);
}

void hcid_dbus_inquiry_start(bdaddr_t *local)
{
	DBusMessage *message;
	char *local_addr;	
	bdaddr_t tmp;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	message = dbus_message_new_signal("/org/bluez/DevAgent",
				"org.bluez.DevAgent", "InquiryStart");
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry start message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &local_addr,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry start message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);

	return;
}

void hcid_dbus_inquiry_complete(bdaddr_t *local)
{
	DBusMessage *message;
	char *local_addr;
	bdaddr_t tmp;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	message = dbus_message_new_signal("/org/bluez/DevAgent",
				"org.bluez.DevAgent", "InquiryComplete");
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry complete message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &local_addr,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry complete message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);

	return;
}

void hcid_dbus_inquiry_result(bdaddr_t *local, bdaddr_t *peer, uint32_t class, int8_t rssi)
{
	DBusMessage *message;
	char *local_addr, *peer_addr;
	bdaddr_t tmp;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	message = dbus_message_new_signal("/org/bluez/DevAgent",
				"org.bluez.DevAgent", "InquiryResult");
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry result message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &local_addr,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_UINT32, &class,
					DBUS_TYPE_INT32, &rssi,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry result message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);

	return;
}

void hcid_dbus_remote_name(bdaddr_t *local, bdaddr_t *peer, char *name)
{
	DBusMessage *message;
	char *local_addr, *peer_addr;
	bdaddr_t tmp;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	message = dbus_message_new_signal("/org/bluez/DevAgent",
				"org.bluez.DevAgent", "RemoteName");
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &local_addr,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS remote name message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);

	return;
}

void hcid_dbus_conn_complete(bdaddr_t *local, bdaddr_t *peer)
{
}

void hcid_dbus_disconn_complete(bdaddr_t *local, bdaddr_t *peer, uint8_t reason)
{
}

gboolean watch_func(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	DBusWatch *watch = (DBusWatch *) data;
	int flags = 0;

	if (cond & G_IO_IN)  flags |= DBUS_WATCH_READABLE;
	if (cond & G_IO_OUT) flags |= DBUS_WATCH_WRITABLE;
	if (cond & G_IO_HUP) flags |= DBUS_WATCH_HANGUP;
	if (cond & G_IO_ERR) flags |= DBUS_WATCH_ERROR;

	dbus_watch_handle(watch, flags);

	dbus_connection_ref(connection);

	/* Dispatch messages */
	while (dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS);

	dbus_connection_unref(connection);

	return TRUE;
}

dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
	GIOCondition cond = G_IO_HUP | G_IO_ERR;
	GIOChannel *io;
	guint id;
	int fd, flags;

	if (!dbus_watch_get_enabled(watch))
		return TRUE;

	fd = dbus_watch_get_fd(watch);
	io = g_io_channel_unix_new(fd);
	flags = dbus_watch_get_flags(watch);

	if (flags & DBUS_WATCH_READABLE) cond |= G_IO_IN;
	if (flags & DBUS_WATCH_WRITABLE) cond |= G_IO_OUT;

	id = g_io_add_watch(io, cond, watch_func, watch);

	dbus_watch_set_data(watch, (void *) id, NULL);

	return TRUE;
}

static void remove_watch(DBusWatch *watch, void *data)
{
	guint id = (guint) dbus_watch_get_data(watch);

	dbus_watch_set_data(watch, NULL, NULL);

	if (id)
		g_io_remove_watch(id);
}

static void watch_toggled(DBusWatch *watch, void *data)
{
	/* Because we just exit on OOM, enable/disable is
	 * no different from add/remove
	 */
	if (dbus_watch_get_enabled(watch))
		add_watch(watch, data);
	else
		remove_watch(watch, data);
}

gboolean hcid_dbus_init(void)
{
	DBusError error;

	dbus_error_init(&error);

	connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		fprintf(stderr, "Failed to open connection to system message bus: %s\n",
			error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	dbus_connection_set_watch_functions(connection,
		add_watch, remove_watch, watch_toggled, NULL, NULL);

	return TRUE;
}
