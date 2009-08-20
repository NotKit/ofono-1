/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

#define SUPPLEMENTARY_SERVICES_INTERFACE "org.ofono.SupplementaryServices"

#define USSD_FLAG_PENDING 0x1

static GSList *g_drivers = NULL;

enum ussd_status {
	USSD_STATUS_NOTIFY = 0,
	USSD_STATUS_ACTION_REQUIRED = 1,
	USSD_STATUS_TERMINATED = 2,
	USSD_STATUS_LOCAL_CLIENT_RESPONDED = 3,
	USSD_STATUS_NOT_SUPPORTED = 4,
	USSD_STATUS_TIMED_OUT = 5,
};

enum ussd_state {
	USSD_STATE_IDLE = 0,
	USSD_STATE_ACTIVE = 1,
	USSD_STATE_USER_ACTION = 2
};

struct ofono_ussd {
	int state;
	DBusMessage *pending;
	int flags;
	GSList *ss_control_list;
	GSList *ss_passwd_list;
	const struct ofono_ussd_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct ssc_entry {
	char *service;
	void *cb;
	void *user;
	ofono_destroy_func destroy;
};

static struct ssc_entry *ssc_entry_create(const char *sc, void *cb, void *data,
						ofono_destroy_func destroy)
{
	struct ssc_entry *r;

	r = g_try_new0(struct ssc_entry, 1);

	if (!r)
		return r;

	r->service = g_strdup(sc);
	r->cb = cb;
	r->user = data;
	r->destroy = destroy;

	return r;
}

static void ssc_entry_destroy(struct ssc_entry *ca)
{
	if (ca->destroy)
		ca->destroy(ca->user);

	g_free(ca->service);
	g_free(ca);
}

static gint ssc_entry_find_by_service(gconstpointer a, gconstpointer b)
{
	const struct ssc_entry *ca = a;

	return strcmp(ca->service, b);
}

gboolean __ofono_ussd_ssc_register(struct ofono_ussd *ussd, const char *sc,
					ofono_ussd_ssc_cb_t cb, void *data,
					ofono_destroy_func destroy)
{
	struct ssc_entry *entry;

	if (!ussd)
		return FALSE;

	entry = ssc_entry_create(sc, cb, data, destroy);

	if (!entry)
		return FALSE;

	ussd->ss_control_list = g_slist_prepend(ussd->ss_control_list, entry);

	return TRUE;
}

void __ofono_ussd_ssc_unregister(struct ofono_ussd *ussd, const char *sc)
{
	GSList *l;

	if (!ussd)
		return;

	l = g_slist_find_custom(ussd->ss_control_list, sc,
				ssc_entry_find_by_service);

	if (!l)
		return;

	ssc_entry_destroy(l->data);
	ussd->ss_control_list = g_slist_remove(ussd->ss_control_list, l->data);
}

gboolean __ofono_ussd_passwd_register(struct ofono_ussd *ussd, const char *sc,
					ofono_ussd_passwd_cb_t cb, void *data,
					ofono_destroy_func destroy)
{
	struct ssc_entry *entry;

	if (!ussd)
		return FALSE;

	entry = ssc_entry_create(sc, cb, data, destroy);

	if (!entry)
		return FALSE;

	ussd->ss_passwd_list = g_slist_prepend(ussd->ss_passwd_list, entry);

	return TRUE;
}

void __ofono_ussd_passwd_unregister(struct ofono_ussd *ussd, const char *sc)
{
	GSList *l;

	if (!ussd)
		return;

	l = g_slist_find_custom(ussd->ss_passwd_list, sc,
				ssc_entry_find_by_service);

	if (!l)
		return;

	ssc_entry_destroy(l->data);
	ussd->ss_passwd_list = g_slist_remove(ussd->ss_passwd_list, l->data);
}

static gboolean recognized_passwd_change_string(struct ofono_ussd *ussd,
						int type, char *sc,
						char *sia, char *sib,
						char *sic, char *sid,
						char *dn, DBusMessage *msg)
{
	GSList *l = ussd->ss_passwd_list;

	switch (type) {
	case SS_CONTROL_TYPE_ACTIVATION:
	case SS_CONTROL_TYPE_REGISTRATION:
		break;

	default:
		return FALSE;
	}

	if (strcmp(sc, "03") || strlen(dn))
		return FALSE;

	/* If SIC & SID don't match, then we just bail out here */
	if (strcmp(sic, sid)) {
		DBusConnection *conn = ofono_dbus_get_connection();
		DBusMessage *reply = __ofono_error_invalid_format(msg);
		g_dbus_send_message(conn, reply);
		return TRUE;
	}

	while ((l = g_slist_find_custom(l, sia,
			ssc_entry_find_by_service)) != NULL) {
		struct ssc_entry *entry = l->data;
		ofono_ussd_passwd_cb_t cb = entry->cb;

		if (cb(sia, sib, sic, msg, entry->user))
			return TRUE;

		l = l->next;
	}

	return FALSE;
}

static gboolean recognized_control_string(struct ofono_ussd *ussd,
						const char *ss_str,
						DBusMessage *msg)
{
	char *str = g_strdup(ss_str);
	char *sc, *sia, *sib, *sic, *sid, *dn;
	int type;
	gboolean ret = FALSE;

	ofono_debug("parsing control string");

	if (parse_ss_control_string(str, &type, &sc,
				&sia, &sib, &sic, &sid, &dn)) {
		GSList *l = ussd->ss_control_list;

		ofono_debug("Got parse result: %d, %s, %s, %s, %s, %s, %s",
				type, sc, sia, sib, sic, sid, dn);

		/* A password change string needs to be treated separately
		 * because it uses a fourth SI and is thus not a valid
		 * control string.  */
		if (recognized_passwd_change_string(ussd, type, sc,
					sia, sib, sic, sid, dn, msg)) {
			ret = TRUE;
			goto out;
		}

		if (*sid != '\0')
			goto out;

		while ((l = g_slist_find_custom(l, sc,
				ssc_entry_find_by_service)) != NULL) {
			struct ssc_entry *entry = l->data;
			ofono_ussd_ssc_cb_t cb = entry->cb;

			if (cb(type, sc, sia, sib, sic, dn, msg, entry->user)) {
				ret = TRUE;
				goto out;
			}

			l = l->next;
		}

	}

	/* TODO: Handle all strings that control voice calls */

	/* TODO: Handle Multiple subscriber profile DN*59#SEND and *59#SEND
	 */

	/* Note: SIM PIN/PIN2 change and unblock and IMEI presentation
	 * procedures are not handled by the daemon since they are not followed
	 * by SEND and are not valid USSD requests.
	 */

out:
	g_free(str);

	return ret;
}

void ofono_ussd_notify(struct ofono_ussd *ussd, int status, const char *str)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *ussdstr = "USSD";
	const char sig[] = { DBUS_TYPE_STRING, 0 };
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter variant;

	if (status == USSD_STATUS_NOT_SUPPORTED) {
		ussd->state = USSD_STATE_IDLE;
		reply = __ofono_error_not_supported(ussd->pending);
		goto out;
	}

	if (status == USSD_STATUS_TIMED_OUT) {
		ussd->state = USSD_STATE_IDLE;
		reply = __ofono_error_timed_out(ussd->pending);
		goto out;
	}

	/* TODO: Rework this in the Agent framework */
	if (ussd->state == USSD_STATE_ACTIVE) {
		if (status == USSD_STATUS_ACTION_REQUIRED) {
			ofono_error("Unable to handle action required ussd");
			return;
		}

		reply = dbus_message_new_method_return(ussd->pending);

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
						&ussdstr);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, sig,
							&variant);

		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						&str);

		dbus_message_iter_close_container(&iter, &variant);

		ussd->state = USSD_STATE_IDLE;
	} else {
		ofono_error("Received an unsolicited USSD, ignoring for now...");
		ofono_debug("USSD is: status: %d, %s", status, str);

		return;
	}

out:
	g_dbus_send_message(conn, reply);

	dbus_message_unref(ussd->pending);
	ussd->pending = NULL;
}

static void ussd_callback(const struct ofono_error *error, void *data)
{
	struct ofono_ussd *ussd = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("ussd request failed with error: %s",
				telephony_error_to_str(error));

	ussd->flags &= ~USSD_FLAG_PENDING;

	if (!ussd->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		ussd->state = USSD_STATE_ACTIVE;
		return;
	}

	reply = __ofono_error_failed(ussd->pending);

	g_dbus_send_message(conn, reply);

	dbus_message_unref(ussd->pending);
	ussd->pending = NULL;
}

static DBusMessage *ussd_initiate(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_ussd *ussd = data;
	const char *str;

	if (ussd->flags & USSD_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (ussd->state == USSD_STATE_ACTIVE)
		return __ofono_error_busy(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &str,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (strlen(str) == 0)
		return __ofono_error_invalid_format(msg);

	ofono_debug("checking if this is a recognized control string");
	if (recognized_control_string(ussd, str, msg))
		return NULL;

	ofono_debug("No.., checking if this is a USSD string");
	if (!valid_ussd_string(str))
		return __ofono_error_invalid_format(msg);

	ofono_debug("OK, running USSD request");

	if (!ussd->driver->request)
		return __ofono_error_not_implemented(msg);

	ussd->flags |= USSD_FLAG_PENDING;
	ussd->pending = dbus_message_ref(msg);

	ussd->driver->request(ussd, str, ussd_callback, ussd);

	return NULL;
}

static void ussd_cancel_callback(const struct ofono_error *error, void *data)
{
	struct ofono_ussd *ussd = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("ussd cancel failed with error: %s",
				telephony_error_to_str(error));

	ussd->flags &= ~USSD_FLAG_PENDING;

	if (!ussd->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		ussd->state = USSD_STATE_IDLE;

		reply = dbus_message_new_method_return(ussd->pending);
	} else
		reply = __ofono_error_failed(ussd->pending);

	__ofono_dbus_pending_reply(&ussd->pending, reply);
}

static DBusMessage *ussd_cancel(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_ussd *ussd = data;

	if (ussd->flags & USSD_FLAG_PENDING)
		return __ofono_error_busy(msg);

	if (ussd->state == USSD_STATE_IDLE)
		return __ofono_error_not_active(msg);

	if (!ussd->driver->cancel)
		return __ofono_error_not_implemented(msg);

	ussd->flags |= USSD_FLAG_PENDING;
	ussd->pending = dbus_message_ref(msg);

	ussd->driver->cancel(ussd, ussd_cancel_callback, ussd);

	return NULL;
}

static GDBusMethodTable ussd_methods[] = {
	{ "Initiate",	"s",	"sv",	ussd_initiate,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ "Cancel",	"",	"",	ussd_cancel,
					G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable ussd_signals[] = {
	{ }
};

int ofono_ussd_driver_register(const struct ofono_ussd_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_ussd_driver_unregister(const struct ofono_ussd_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void ussd_unregister(struct ofono_atom *atom)
{
	struct ofono_ussd *ussd = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_slist_foreach(ussd->ss_control_list, (GFunc)ssc_entry_destroy, NULL);
	g_slist_free(ussd->ss_control_list);
	ussd->ss_control_list = NULL;

	g_slist_foreach(ussd->ss_passwd_list, (GFunc)ssc_entry_destroy, NULL);
	g_slist_free(ussd->ss_passwd_list);
	ussd->ss_passwd_list = NULL;

	ofono_modem_remove_interface(modem, SUPPLEMENTARY_SERVICES_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					SUPPLEMENTARY_SERVICES_INTERFACE);
}

static void ussd_remove(struct ofono_atom *atom)
{
	struct ofono_ussd *ussd = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (ussd == NULL)
		return;

	if (ussd->driver && ussd->driver->remove)
		ussd->driver->remove(ussd);

	g_free(ussd);
}

struct ofono_ussd *ofono_ussd_create(struct ofono_modem *modem,
					const char *driver,
					void *data)
{
	struct ofono_ussd *ussd;
	GSList *l;

	if (driver == NULL)
		return NULL;

	ussd = g_try_new0(struct ofono_ussd, 1);

	if (ussd == NULL)
		return NULL;

	ussd->driver_data = data;
	ussd->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_USSD,
						ussd_remove, ussd);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_ussd_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(ussd) < 0)
			continue;

		ussd->driver = drv;
		break;
	}

	return ussd;
}

void ofono_ussd_register(struct ofono_ussd *ussd)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(ussd->atom);
	const char *path = __ofono_atom_get_path(ussd->atom);

	if (!g_dbus_register_interface(conn, path,
					SUPPLEMENTARY_SERVICES_INTERFACE,
					ussd_methods, ussd_signals, NULL,
					ussd, NULL)) {
		ofono_error("Could not create %s interface",
				SUPPLEMENTARY_SERVICES_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, SUPPLEMENTARY_SERVICES_INTERFACE);

	__ofono_atom_register(ussd->atom, ussd_unregister);
}

void ofono_ussd_remove(struct ofono_ussd *ussd)
{
	__ofono_atom_free(ussd->atom);
}

void ofono_ussd_set_data(struct ofono_ussd *ussd, void *data)
{
	ussd->driver_data = data;
}

void *ofono_ussd_get_data(struct ofono_ussd *ussd)
{
	return ussd->driver_data;
}
