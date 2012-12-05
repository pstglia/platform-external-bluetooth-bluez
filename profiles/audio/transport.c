/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <errno.h>

#include <bluetooth/uuid.h>

#include <glib.h>
#include <gdbus/gdbus.h>

#include "../src/adapter.h"
#include "../src/device.h"
#include "../src/dbus-common.h"

#include "log.h"
#include "error.h"
#include "device.h"
#include "avdtp.h"
#include "media.h"
#include "transport.h"
#include "a2dp.h"
#include "sink.h"
#include "source.h"
#include "avrcp.h"

#define MEDIA_TRANSPORT_INTERFACE "org.bluez.MediaTransport"

typedef enum {
	TRANSPORT_LOCK_READ = 1,
	TRANSPORT_LOCK_WRITE = 1 << 1,
} transport_lock_t;

typedef enum {
	TRANSPORT_STATE_IDLE,		/* Not acquired and suspended */
	TRANSPORT_STATE_PENDING,	/* Playing but not acquired */
	TRANSPORT_STATE_REQUESTING,	/* Acquire in progress */
	TRANSPORT_STATE_ACTIVE,		/* Acquired and playing */
	TRANSPORT_STATE_SUSPENDING,     /* Release in progress */
} transport_state_t;

static char *str_state[] = {
	"TRANSPORT_STATE_IDLE",
	"TRANSPORT_STATE_PENDING",
	"TRANSPORT_STATE_REQUESTING",
	"TRANSPORT_STATE_ACTIVE",
	"TRANSPORT_STATE_SUSPENDING",
};

struct media_request {
	DBusMessage		*msg;
	guint			id;
};

struct media_owner {
	struct media_transport	*transport;
	struct media_request	*pending;
	char			*name;
	transport_lock_t	lock;
	guint			watch;
};

struct a2dp_transport {
	struct avdtp		*session;
	uint16_t		delay;
	uint16_t		volume;
};

struct media_transport {
	char			*path;		/* Transport object path */
	struct audio_device	*device;	/* Transport device */
	struct media_endpoint	*endpoint;	/* Transport endpoint */
	GSList			*owners;	/* Transport owners */
	uint8_t			*configuration; /* Transport configuration */
	int			size;		/* Transport configuration size */
	int			fd;		/* Transport file descriptor */
	uint16_t		imtu;		/* Transport input mtu */
	uint16_t		omtu;		/* Transport output mtu */
	transport_lock_t	lock;
	transport_state_t	state;
	guint			hs_watch;
	guint			source_watch;
	guint			sink_watch;
	guint			(*resume) (struct media_transport *transport,
					struct media_owner *owner);
	guint			(*suspend) (struct media_transport *transport,
					struct media_owner *owner);
	void			(*cancel) (struct media_transport *transport,
								guint id);
	GDestroyNotify		destroy;
	void			*data;
};

static const char *lock2str(transport_lock_t lock)
{
	if (lock == 0)
		return "";
	else if (lock == TRANSPORT_LOCK_READ)
		return "r";
	else if (lock == TRANSPORT_LOCK_WRITE)
		return "w";
	else
		return "rw";
}

static transport_lock_t str2lock(const char *str)
{
	transport_lock_t lock = 0;

	if (g_strstr_len(str, -1, "r") != NULL)
		lock |= TRANSPORT_LOCK_READ;

	if (g_strstr_len(str, -1, "w") != NULL)
		lock |= TRANSPORT_LOCK_WRITE;

	return lock;
}

static const char *state2str(transport_state_t state)
{
	switch (state) {
	case TRANSPORT_STATE_IDLE:
	case TRANSPORT_STATE_REQUESTING:
		return "idle";
	case TRANSPORT_STATE_PENDING:
		return "pending";
	case TRANSPORT_STATE_ACTIVE:
	case TRANSPORT_STATE_SUSPENDING:
		return "active";
	}

	return NULL;
}

static gboolean state_in_use(transport_state_t state)
{
	switch (state) {
	case TRANSPORT_STATE_IDLE:
	case TRANSPORT_STATE_PENDING:
		return FALSE;
	case TRANSPORT_STATE_REQUESTING:
	case TRANSPORT_STATE_ACTIVE:
	case TRANSPORT_STATE_SUSPENDING:
		return TRUE;
	}

	return FALSE;
}

static void transport_set_state(struct media_transport *transport,
							transport_state_t state)
{
	transport_state_t old_state = transport->state;
	const char *str;

	if (old_state == state)
		return;

	transport->state = state;

	DBG("State changed %s: %s -> %s", transport->path, str_state[old_state],
							str_state[state]);

	str = state2str(state);

	if (g_strcmp0(str, state2str(old_state)) != 0)
		g_dbus_emit_property_changed(btd_get_dbus_connection(),
						transport->path,
						MEDIA_TRANSPORT_INTERFACE,
						"State");
}

void media_transport_destroy(struct media_transport *transport)
{
	char *path;

	if (transport->sink_watch)
		sink_remove_state_cb(transport->sink_watch);

	if (transport->source_watch)
		source_remove_state_cb(transport->source_watch);

	path = g_strdup(transport->path);
	g_dbus_unregister_interface(btd_get_dbus_connection(), path,
						MEDIA_TRANSPORT_INTERFACE);

	g_free(path);
}

static struct media_request *media_request_create(DBusMessage *msg, guint id)
{
	struct media_request *req;

	req = g_new0(struct media_request, 1);
	req->msg = dbus_message_ref(msg);
	req->id = id;

	DBG("Request created: method=%s id=%u", dbus_message_get_member(msg),
									id);

	return req;
}

static void media_request_reply(struct media_request *req, int err)
{
	DBusMessage *reply;

	DBG("Request %s Reply %s", dbus_message_get_member(req->msg),
							strerror(err));

	if (!err)
		reply = g_dbus_create_reply(req->msg, DBUS_TYPE_INVALID);
	else
		reply = g_dbus_create_error(req->msg,
						ERROR_INTERFACE ".Failed",
						"%s", strerror(err));

	g_dbus_send_message(btd_get_dbus_connection(), reply);
}

static gboolean media_transport_release(struct media_transport *transport,
							transport_lock_t lock)
{
	transport->lock &= ~lock;

	if (lock & TRANSPORT_LOCK_READ)
		DBG("Transport %s: read lock released", transport->path);

	if (lock & TRANSPORT_LOCK_WRITE)
		DBG("Transport %s: write lock released", transport->path);

	return TRUE;
}

static void media_owner_remove(struct media_owner *owner)
{
	struct media_transport *transport = owner->transport;
	struct media_request *req = owner->pending;

	if (!req)
		return;

	DBG("Owner %s Request %s", owner->name,
					dbus_message_get_member(req->msg));

	if (req->id)
		transport->cancel(transport, req->id);

	owner->pending = NULL;
	if (req->msg)
		dbus_message_unref(req->msg);

	g_free(req);
}

static void media_owner_free(struct media_owner *owner)
{
	DBG("Owner %s", owner->name);

	media_owner_remove(owner);

	g_free(owner->name);
	g_free(owner);
}

static void media_transport_remove(struct media_transport *transport,
						struct media_owner *owner)
{
	DBG("Transport %s Owner %s", transport->path, owner->name);

	media_transport_release(transport, owner->lock);

	/* Reply if owner has a pending request */
	if (owner->pending)
		media_request_reply(owner->pending, EIO);

	transport->owners = g_slist_remove(transport->owners, owner);

	if (owner->watch)
		g_dbus_remove_watch(btd_get_dbus_connection(), owner->watch);

	media_owner_free(owner);

	/* Suspend if there is no longer any owner */
	if (transport->owners == NULL && state_in_use(transport->state))
		transport->suspend(transport, NULL);
}

static gboolean media_transport_set_fd(struct media_transport *transport,
					int fd, uint16_t imtu, uint16_t omtu)
{
	if (transport->fd == fd)
		return TRUE;

	transport->fd = fd;
	transport->imtu = imtu;
	transport->omtu = omtu;

	info("%s: fd(%d) ready", transport->path, fd);

	return TRUE;
}

static void a2dp_resume_complete(struct avdtp *session,
				struct avdtp_error *err, void *user_data)
{
	struct media_owner *owner = user_data;
	struct media_request *req = owner->pending;
	struct media_transport *transport = owner->transport;
	struct a2dp_sep *sep = media_endpoint_get_sep(transport->endpoint);
	struct avdtp_stream *stream;
	int fd;
	uint16_t imtu, omtu;
	gboolean ret;

	req->id = 0;

	if (err)
		goto fail;

	stream = a2dp_sep_get_stream(sep);
	if (stream == NULL)
		goto fail;

	ret = avdtp_stream_get_transport(stream, &fd, &imtu, &omtu, NULL);
	if (ret == FALSE)
		goto fail;

	media_transport_set_fd(transport, fd, imtu, omtu);

	if ((owner->lock & TRANSPORT_LOCK_READ) == 0)
		imtu = 0;

	if ((owner->lock & TRANSPORT_LOCK_WRITE) == 0)
		omtu = 0;

	ret = g_dbus_send_reply(btd_get_dbus_connection(), req->msg,
						DBUS_TYPE_UNIX_FD, &fd,
						DBUS_TYPE_UINT16, &imtu,
						DBUS_TYPE_UINT16, &omtu,
						DBUS_TYPE_INVALID);
	if (ret == FALSE)
		goto fail;

	media_owner_remove(owner);

	transport_set_state(transport, TRANSPORT_STATE_ACTIVE);

	return;

fail:
	media_transport_remove(transport, owner);
}

static guint resume_a2dp(struct media_transport *transport,
				struct media_owner *owner)
{
	struct a2dp_transport *a2dp = transport->data;
	struct media_endpoint *endpoint = transport->endpoint;
	struct audio_device *device = transport->device;
	struct a2dp_sep *sep = media_endpoint_get_sep(endpoint);

	if (a2dp->session == NULL) {
		a2dp->session = avdtp_get(&device->src, &device->dst);
		if (a2dp->session == NULL)
			return 0;
	}

	if (state_in_use(transport->state))
		goto done;

	if (a2dp_sep_lock(sep, a2dp->session) == FALSE)
		return 0;

	if (transport->state == TRANSPORT_STATE_IDLE)
		transport_set_state(transport, TRANSPORT_STATE_REQUESTING);

done:
	return a2dp_resume(a2dp->session, sep, a2dp_resume_complete, owner);
}

static void a2dp_suspend_complete(struct avdtp *session,
				struct avdtp_error *err, void *user_data)
{
	struct media_owner *owner = user_data;
	struct media_transport *transport = owner->transport;
	struct a2dp_transport *a2dp = transport->data;
	struct a2dp_sep *sep = media_endpoint_get_sep(transport->endpoint);

	/* Release always succeeds */
	if (owner->pending) {
		owner->pending->id = 0;
		media_request_reply(owner->pending, 0);
		media_owner_remove(owner);
	}

	a2dp_sep_unlock(sep, a2dp->session);
	transport_set_state(transport, TRANSPORT_STATE_IDLE);
	media_transport_remove(transport, owner);
}

static guint suspend_a2dp(struct media_transport *transport,
						struct media_owner *owner)
{
	struct a2dp_transport *a2dp = transport->data;
	struct media_endpoint *endpoint = transport->endpoint;
	struct a2dp_sep *sep = media_endpoint_get_sep(endpoint);

	if (owner != NULL)
		return a2dp_suspend(a2dp->session, sep, a2dp_suspend_complete,
									owner);

	transport_set_state(transport, TRANSPORT_STATE_IDLE);
	a2dp_sep_unlock(sep, a2dp->session);

	return 0;
}

static void cancel_a2dp(struct media_transport *transport, guint id)
{
	a2dp_cancel(transport->device, id);
}

static void media_owner_exit(DBusConnection *connection, void *user_data)
{
	struct media_owner *owner = user_data;

	owner->watch = 0;

	media_owner_remove(owner);

	media_transport_remove(owner->transport, owner);
}

static gboolean media_transport_acquire(struct media_transport *transport,
							transport_lock_t lock)
{
	if (transport->lock & lock)
		return FALSE;

	transport->lock |= lock;

	if (lock & TRANSPORT_LOCK_READ)
		DBG("Transport %s: read lock acquired", transport->path);

	if (lock & TRANSPORT_LOCK_WRITE)
		DBG("Transport %s: write lock acquired", transport->path);


	return TRUE;
}

static void media_transport_add(struct media_transport *transport,
					struct media_owner *owner)
{
	DBG("Transport %s Owner %s", transport->path, owner->name);
	transport->owners = g_slist_append(transport->owners, owner);
	owner->transport = transport;
	owner->watch = g_dbus_add_disconnect_watch(btd_get_dbus_connection(),
							owner->name,
							media_owner_exit,
							owner, NULL);
}

static struct media_owner *media_owner_create(DBusMessage *msg,
						transport_lock_t lock)
{
	struct media_owner *owner;

	owner = g_new0(struct media_owner, 1);
	owner->name = g_strdup(dbus_message_get_sender(msg));
	owner->lock = lock;

	DBG("Owner created: sender=%s accesstype=%s", owner->name,
							lock2str(lock));

	return owner;
}

static void media_owner_add(struct media_owner *owner,
						struct media_request *req)
{
	DBG("Owner %s Request %s", owner->name,
					dbus_message_get_member(req->msg));

	owner->pending = req;
}

static struct media_owner *media_transport_find_owner(
					struct media_transport *transport,
					const char *name)
{
	GSList *l;

	for (l = transport->owners; l; l = l->next) {
		struct media_owner *owner = l->data;

		if (g_strcmp0(owner->name, name) == 0)
			return owner;
	}

	return NULL;
}

static DBusMessage *acquire(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_transport *transport = data;
	struct media_owner *owner;
	struct media_request *req;
	const char *accesstype, *sender;
	transport_lock_t lock;
	guint id;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &accesstype,
				DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	sender = dbus_message_get_sender(msg);

	owner = media_transport_find_owner(transport, sender);
	if (owner != NULL)
		return btd_error_not_authorized(msg);

	lock = str2lock(accesstype);
	if (lock == 0)
		return btd_error_invalid_args(msg);

	if (transport->state != TRANSPORT_STATE_PENDING &&
				g_strstr_len(accesstype, -1, "?") != NULL)
		return btd_error_failed(msg, "Transport not playing");

	if (media_transport_acquire(transport, lock) == FALSE)
		return btd_error_not_authorized(msg);

	owner = media_owner_create(msg, lock);
	id = transport->resume(transport, owner);
	if (id == 0) {
		media_transport_release(transport, lock);
		media_owner_free(owner);
		return btd_error_not_authorized(msg);
	}

	req = media_request_create(msg, id);
	media_owner_add(owner, req);
	media_transport_add(transport, owner);

	return NULL;
}

static DBusMessage *release(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_transport *transport = data;
	struct media_owner *owner;
	const char *accesstype, *sender;
	transport_lock_t lock;
	struct media_request *req;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_STRING, &accesstype,
				DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	sender = dbus_message_get_sender(msg);

	owner = media_transport_find_owner(transport, sender);
	if (owner == NULL)
		return btd_error_not_authorized(msg);

	lock = str2lock(accesstype);

	if (owner->lock == lock) {
		guint id;

		/* Not the last owner, no need to suspend */
		if (g_slist_length(transport->owners) != 1) {
			media_transport_remove(transport, owner);
			return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
		}

		if (owner->pending) {
			const char *member;

			member = dbus_message_get_member(owner->pending->msg);
			/* Cancel Acquire request if that exist */
			if (g_str_equal(member, "Acquire"))
				media_owner_remove(owner);
			else
				return btd_error_in_progress(msg);
		}

		transport_set_state(transport, TRANSPORT_STATE_SUSPENDING);

		id = transport->suspend(transport, owner);
		if (id == 0) {
			media_transport_remove(transport, owner);
			return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
		}

		req = media_request_create(msg, id);
		media_owner_add(owner, req);

		return NULL;
	} else if ((owner->lock & lock) == lock) {
		media_transport_release(transport, lock);
		owner->lock &= ~lock;
	} else
		return btd_error_not_authorized(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static gboolean get_device(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	const char *path = device_get_path(transport->device->btd_dev);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	return TRUE;
}

static gboolean get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	const char *uuid = media_endpoint_get_uuid(transport->endpoint);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

	return TRUE;
}

static gboolean get_codec(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	uint8_t codec = media_endpoint_get_codec(transport->endpoint);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &codec);

	return TRUE;
}

static gboolean get_configuration(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	DBusMessageIter array;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&transport->configuration,
						transport->size);

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static gboolean get_state(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	const char *state = state2str(transport->state);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &state);

	return TRUE;
}

static gboolean delay_exists(const GDBusPropertyTable *property, void *data)
{
	struct media_transport *transport = data;
	struct a2dp_transport *a2dp = transport->data;

	return a2dp->delay != 0;
}

static gboolean get_delay(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	struct a2dp_transport *a2dp = transport->data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT16, &a2dp->delay);

	return TRUE;
}

static gboolean volume_exists(const GDBusPropertyTable *property, void *data)
{
	struct media_transport *transport = data;
	struct a2dp_transport *a2dp = transport->data;

	return a2dp->volume <= 127;
}

static gboolean get_volume(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct media_transport *transport = data;
	struct a2dp_transport *a2dp = transport->data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT16, &a2dp->volume);

	return TRUE;
}

static void set_volume(const GDBusPropertyTable *property,
			DBusMessageIter *iter, GDBusPendingPropertySet id,
			void *data)
{
	struct media_transport *transport = data;
	struct a2dp_transport *a2dp = transport->data;
	uint16_t volume;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_UINT16)
		return g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");

	dbus_message_iter_get_basic(iter, &volume);

	if (volume > 127)
		return g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");

	if (a2dp->volume != volume)
		avrcp_set_volume(transport->device, volume);

	g_dbus_pending_property_success(id);
}

static const GDBusMethodTable transport_methods[] = {
	{ GDBUS_ASYNC_METHOD("Acquire",
			GDBUS_ARGS({ "access_type", "s" }),
			GDBUS_ARGS({ "fd", "h" }, { "mtu_r", "q" },
							{ "mtu_w", "q" } ),
			acquire) },
	{ GDBUS_ASYNC_METHOD("Release",
			GDBUS_ARGS({ "access_type", "s" }), NULL,
			release ) },
	{ },
};

static const GDBusPropertyTable transport_properties[] = {
	{ "Device", "o", get_device },
	{ "UUID", "s", get_uuid },
	{ "Codec", "y", get_codec },
	{ "Configuration", "ay", get_configuration },
	{ "State", "s", get_state },
	{ "Delay", "q", get_delay, NULL, delay_exists },
	{ "Volume", "q", get_volume, set_volume, volume_exists },
	{ }
};

static void destroy_a2dp(void *data)
{
	struct a2dp_transport *a2dp = data;

	if (a2dp->session)
		avdtp_unref(a2dp->session);

	g_free(a2dp);
}

static void media_transport_free(void *data)
{
	struct media_transport *transport = data;
	GSList *l = transport->owners;

	while (l) {
		struct media_owner *owner = l->data;
		l = l->next;
		media_transport_remove(transport, owner);
	}

	g_slist_free(transport->owners);

	if (transport->destroy != NULL)
		transport->destroy(transport->data);

	g_free(transport->configuration);
	g_free(transport->path);
	g_free(transport);
}

static void transport_update_playing(struct media_transport *transport,
							gboolean playing)
{
	DBG("%s State=%s Playing=%d", transport->path,
					str_state[transport->state], playing);

	if (playing == FALSE) {
		if (transport->state == TRANSPORT_STATE_PENDING)
			transport_set_state(transport, TRANSPORT_STATE_IDLE);
		else if (transport->state == TRANSPORT_STATE_ACTIVE) {
			/* Remove all owners */
			while (transport->owners != NULL) {
				struct media_owner *owner;

				owner = transport->owners->data;
				media_transport_remove(transport, owner);
			}
		}
	} else if (transport->state == TRANSPORT_STATE_IDLE)
		transport_set_state(transport, TRANSPORT_STATE_PENDING);
}

static void sink_state_changed(struct audio_device *dev,
						sink_state_t old_state,
						sink_state_t new_state,
						void *user_data)
{
	struct media_transport *transport = user_data;

	if (dev != transport->device)
		return;

	if (new_state == SINK_STATE_PLAYING)
		transport_update_playing(transport, TRUE);
	else
		transport_update_playing(transport, FALSE);
}

static void source_state_changed(struct audio_device *dev,
						source_state_t old_state,
						source_state_t new_state,
						void *user_data)
{
	struct media_transport *transport = user_data;

	if (dev != transport->device)
		return;

	if (new_state == SOURCE_STATE_PLAYING)
		transport_update_playing(transport, TRUE);
	else
		transport_update_playing(transport, FALSE);
}

struct media_transport *media_transport_create(struct media_endpoint *endpoint,
						struct audio_device *device,
						uint8_t *configuration,
						size_t size)
{
	struct media_transport *transport;
	const char *uuid;
	static int fd = 0;

	transport = g_new0(struct media_transport, 1);
	transport->device = device;
	transport->endpoint = endpoint;
	transport->configuration = g_new(uint8_t, size);
	memcpy(transport->configuration, configuration, size);
	transport->size = size;
	transport->path = g_strdup_printf("%s/fd%d",
				device_get_path(device->btd_dev), fd++);
	transport->fd = -1;

	uuid = media_endpoint_get_uuid(endpoint);
	if (strcasecmp(uuid, A2DP_SOURCE_UUID) == 0 ||
			strcasecmp(uuid, A2DP_SINK_UUID) == 0) {
		struct a2dp_transport *a2dp;

		a2dp = g_new0(struct a2dp_transport, 1);
		a2dp->volume = -1;

		transport->resume = resume_a2dp;
		transport->suspend = suspend_a2dp;
		transport->cancel = cancel_a2dp;
		transport->data = a2dp;
		transport->destroy = destroy_a2dp;

		if (strcasecmp(uuid, A2DP_SOURCE_UUID) == 0)
			transport->sink_watch = sink_add_state_cb(
							sink_state_changed,
							transport);
		else
			transport->source_watch = source_add_state_cb(
							source_state_changed,
							transport);
	} else
		goto fail;

	if (g_dbus_register_interface(btd_get_dbus_connection(),
				transport->path, MEDIA_TRANSPORT_INTERFACE,
				transport_methods, NULL, transport_properties,
				transport, media_transport_free) == FALSE) {
		error("Could not register transport %s", transport->path);
		goto fail;
	}

	return transport;

fail:
	media_transport_free(transport);
	return NULL;
}

const char *media_transport_get_path(struct media_transport *transport)
{
	return transport->path;
}

void media_transport_update_delay(struct media_transport *transport,
							uint16_t delay)
{
	struct a2dp_transport *a2dp = transport->data;

	/* Check if delay really changed */
	if (a2dp->delay == delay)
		return;

	a2dp->delay = delay;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
					transport->path,
					MEDIA_TRANSPORT_INTERFACE, "Delay");
}

struct audio_device *media_transport_get_dev(struct media_transport *transport)
{
	return transport->device;
}

void media_transport_update_volume(struct media_transport *transport,
								uint8_t volume)
{
	struct a2dp_transport *a2dp = transport->data;

	/* Check if volume really changed */
	if (a2dp->volume == volume)
		return;

	a2dp->volume = volume;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
					transport->path,
					MEDIA_TRANSPORT_INTERFACE, "Volume");
}
