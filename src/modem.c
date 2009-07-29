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

#include <string.h>
#include <stdio.h>

#include <dbus/dbus.h>
#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "dbus-gsm.h"
#include "modem.h"
#include "driver.h"
#include "common.h"
#include "cssn.h"
#include "sim.h"

#define MODEM_INTERFACE "org.ofono.Modem"

#define MODEM_FLAG_INITIALIZING_ATTRS 1

#define ATTRIBUTE_QUERY_DELAY 0

struct ofono_modem_data {
	char      			*manufacturer;
	char      			*model;
	char      			*revision;
	char      			*serial;
	GSList          		*interface_list;
	int				flags;
	unsigned int			idlist;
	struct ofono_modem_attribute_ops	*ops;
	DBusMessage			*pending;
	guint				interface_update;
};

unsigned int modem_alloc_callid(struct ofono_modem *modem)
{
	struct ofono_modem_data *d = modem->modem_info;
	unsigned int i;

	for (i = 1; i < sizeof(d->idlist) * 8; i++) {
		if (d->idlist & (0x1 << i))
			continue;

		d->idlist |= (0x1 << i);
		return i;
	}

	return 0;
}

void modem_release_callid(struct ofono_modem *modem, int id)
{
	struct ofono_modem_data *d = modem->modem_info;

	d->idlist &= ~(0x1 << id);
}

void ofono_modem_set_userdata(struct ofono_modem *modem, void *userdata)
{
	if (modem)
		modem->userdata = userdata;
}

void *ofono_modem_userdata(struct ofono_modem *modem)
{
	if (modem)
		return modem->userdata;

	return NULL;
}

static void modem_free(gpointer data)
{
	struct ofono_modem *modem = data;
	GSList *l;

	if (modem == NULL)
		return;

	for (l = modem->modem_info->interface_list; l; l = l->next)
		g_free(l->data);

	g_slist_free(modem->modem_info->interface_list);

	g_free(modem->modem_info->manufacturer);
	g_free(modem->modem_info->serial);
	g_free(modem->modem_info->revision);
	g_free(modem->modem_info->model);

	if (modem->modem_info->pending)
		dbus_message_unref(modem->modem_info->pending);

	if (modem->modem_info->interface_update)
		g_source_remove(modem->modem_info->interface_update);

	g_free(modem->modem_info);
	g_free(modem->path);
	g_free(modem);
}

static DBusMessage *generate_properties_reply(struct ofono_modem *modem,
				DBusConnection *conn, DBusMessage *msg)
{
	struct ofono_modem_data *info = modem->modem_info;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	char **interfaces;
	int i;
	GSList *l;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	if (info->manufacturer)
		ofono_dbus_dict_append(&dict, "Manufacturer", DBUS_TYPE_STRING,
					&info->manufacturer);

	if (info->model)
		ofono_dbus_dict_append(&dict, "Model", DBUS_TYPE_STRING,
					&info->model);

	if (info->revision)
		ofono_dbus_dict_append(&dict, "Revision", DBUS_TYPE_STRING,
					&info->revision);

	if (info->serial)
		ofono_dbus_dict_append(&dict, "Serial", DBUS_TYPE_STRING,
					&info->serial);

	interfaces = g_new0(char *, g_slist_length(info->interface_list) + 1);
	for (i = 0, l = info->interface_list; l; l = l->next, i++)
		interfaces[i] = l->data;

	dbus_gsm_dict_append_array(&dict, "Interfaces", DBUS_TYPE_STRING,
					&interfaces);

	g_free(interfaces);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *modem_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;

	if (modem->modem_info->flags & MODEM_FLAG_INITIALIZING_ATTRS) {
		modem->modem_info->pending = dbus_message_ref(msg);
		return NULL;
	}

	return generate_properties_reply(modem, conn, msg);
}

static GDBusMethodTable modem_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	modem_get_properties,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable modem_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static gboolean trigger_interface_update(void *data)
{
	struct ofono_modem *modem = data;
	struct ofono_modem_data *info = modem->modem_info;
	DBusConnection *conn = ofono_dbus_get_connection();

	char **interfaces;
	GSList *l;
	int i;

	interfaces = g_new0(char *, g_slist_length(info->interface_list) + 1);
	for (i = 0, l = info->interface_list; l; l = l->next, i++)
		interfaces[i] = l->data;

	ofono_dbus_signal_array_property_changed(conn, modem->path,
						MODEM_INTERFACE,
						"Interfaces", DBUS_TYPE_STRING,
						&interfaces);

	g_free(interfaces);

	info->interface_update = 0;

	return FALSE;
}

void modem_add_interface(struct ofono_modem *modem, const char *interface)
{
	struct ofono_modem_data *info = modem->modem_info;

	info->interface_list =
		g_slist_prepend(info->interface_list, g_strdup(interface));

	if (info->interface_update == 0)
		info->interface_update =
			g_timeout_add(0, trigger_interface_update, modem);
}

void modem_remove_interface(struct ofono_modem *modem, const char *interface)
{
	struct ofono_modem_data *info = modem->modem_info;

	GSList *found = g_slist_find_custom(info->interface_list,
						interface,
						(GCompareFunc) strcmp);

	if (!found) {
		ofono_error("Interface %s not found on the interface_list",
				interface);
		return;
	}

	g_free(found->data);

	info->interface_list =
		g_slist_remove(info->interface_list, found->data);

	if (info->interface_update == 0)
		info->interface_update =
			g_timeout_add(0, trigger_interface_update, modem);
}

static void finish_attr_query(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;

	modem->modem_info->flags &= ~MODEM_FLAG_INITIALIZING_ATTRS;

	if (!modem->modem_info->pending)
		return;

	reply = generate_properties_reply(modem, conn,
						modem->modem_info->pending);

	if (reply)
		g_dbus_send_message(conn, reply);

	dbus_message_unref(modem->modem_info->pending);
	modem->modem_info->pending = NULL;
}

static void query_serial_cb(const struct ofono_error *error,
				const char *serial, void *user)
{
	struct ofono_modem *modem = user;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		modem->modem_info->serial = g_strdup(serial);

	finish_attr_query(modem);
}

static gboolean query_serial(gpointer user)
{
	struct ofono_modem *modem = user;

	if (!modem->modem_info->ops->query_serial) {
		finish_attr_query(modem);
		return FALSE;
	}

	modem->modem_info->ops->query_serial(modem, query_serial_cb, modem);

	return FALSE;
}

static void query_revision_cb(const struct ofono_error *error,
				const char *revision, void *user)
{
	struct ofono_modem *modem = user;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		modem->modem_info->revision = g_strdup(revision);

	g_timeout_add(0, query_serial, modem);
}

static gboolean query_revision(gpointer user)
{
	struct ofono_modem *modem = user;

	if (!modem->modem_info->ops->query_revision) {
		g_timeout_add(0, query_serial, modem);
		return FALSE;
	}

	modem->modem_info->ops->query_revision(modem, query_revision_cb, modem);

	return FALSE;
}

static void query_model_cb(const struct ofono_error *error,
				const char *model, void *user)
{
	struct ofono_modem *modem = user;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		modem->modem_info->model = g_strdup(model);

	g_timeout_add(0, query_revision, modem);
}

static gboolean query_model(gpointer user)
{
	struct ofono_modem *modem = user;

	if (!modem->modem_info->ops->query_model) {
		/* If model is not supported, don't bother querying revision */
		g_timeout_add(0, query_serial, modem);
		return FALSE;
	}

	modem->modem_info->ops->query_model(modem, query_model_cb, modem);

	return FALSE;
}

static void query_manufacturer_cb(const struct ofono_error *error,
					const char *manufacturer, void *user)
{
	struct ofono_modem *modem = user;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		modem->modem_info->manufacturer = g_strdup(manufacturer);

	g_timeout_add(0, query_model, modem);
}

static gboolean query_manufacturer(gpointer user)
{
	struct ofono_modem *modem = user;

	if (!modem->modem_info->ops->query_manufacturer) {
		g_timeout_add(0, query_model, modem);
		return FALSE;
	}

	modem->modem_info->ops->query_manufacturer(modem, query_manufacturer_cb,
							modem);

	return FALSE;
}

struct ofono_modem *modem_create(int id, struct ofono_modem_attribute_ops *ops)
{
	char path[MAX_DBUS_PATH_LEN];
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem;

	modem = g_try_new0(struct ofono_modem, 1);
	if (modem == NULL)
		return modem;

	modem->modem_info = g_try_new0(struct ofono_modem_data, 1);
	if (modem->modem_info == NULL) {
		g_free(modem);
		return NULL;
	}

	modem->id = id;
	modem->modem_info->ops = ops;

	snprintf(path, MAX_DBUS_PATH_LEN, "/modem%d", modem->id);
	modem->path = g_strdup(path);

	if (!g_dbus_register_interface(conn, path, MODEM_INTERFACE,
			modem_methods, modem_signals, NULL,
			modem, modem_free)) {
		ofono_error("Modem interface init failed on path %s", path);
		modem_free(modem);
		return NULL;
	}

	ofono_sim_manager_init(modem);
	ofono_cssn_init(modem);

	modem->modem_info->flags |= MODEM_FLAG_INITIALIZING_ATTRS;
	g_timeout_add(ATTRIBUTE_QUERY_DELAY, query_manufacturer, modem);

	return modem;
}

void modem_remove(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	/* Need to make a copy to keep gdbus happy */
	char *path = g_strdup(modem->path);

	ofono_debug("Removing modem: %s", modem->path);

	ofono_cssn_exit(modem);
	ofono_sim_manager_exit(modem);

	g_dbus_unregister_interface(conn, path, MODEM_INTERFACE);

	g_free(path);
}
