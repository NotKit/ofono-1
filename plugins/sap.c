/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010-2011  ProFUSION embedded systems
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include "bluetooth.h"

#ifndef DBUS_TYPE_UNIX_FD
#define DBUS_TYPE_UNIX_FD -1
#endif

static DBusConnection *connection;
static GHashTable *modem_hash = NULL;
static struct ofono_modem *sap_hw_modem = NULL;
static struct bluetooth_sap_driver *sap_hw_driver = NULL;

struct sap_data {
	char *server_path;
	struct ofono_modem *hw_modem;
	struct bluetooth_sap_driver *sap_driver;
};

int bluetooth_sap_client_register(struct bluetooth_sap_driver *sap,
					struct ofono_modem *modem)
{
	if (sap_hw_modem != NULL)
		return -EPERM;

	sap_hw_modem = modem;
	sap_hw_driver = sap;

	return 0;
}

static void sap_remove_modem(struct ofono_modem *modem)
{
	struct sap_data *data = ofono_modem_get_data(modem);

	g_free(data->server_path);
	g_free(data);

	ofono_modem_set_data(modem, NULL);

	ofono_modem_remove(modem);
}

void bluetooth_sap_client_unregister(struct ofono_modem *modem)
{
	GHashTableIter iter;
	gpointer key, value;

	if (sap_hw_modem == NULL)
		return;

	g_hash_table_iter_init(&iter, modem_hash);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_iter_remove(&iter);
		sap_remove_modem(value);
	}

	sap_hw_modem = NULL;
	sap_hw_driver = NULL;
}

static int sap_probe(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void sap_remove(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

/* power up hardware */
static int sap_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static int sap_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	return 0;
}

static void sap_pre_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void sap_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static int bluetooth_sap_probe(const char *device, const char *dev_addr,
				const char *adapter_addr, const char *alias)
{
	struct ofono_modem *modem;
	struct sap_data *data;
	char buf[256];

	if (sap_hw_modem == NULL)
		return -ENODEV;

	/* We already have this device in our hash, ignore */
	if (g_hash_table_lookup(modem_hash, device) != NULL)
		return -EALREADY;

	ofono_info("Using device: %s, devaddr: %s, adapter: %s",
			device, dev_addr, adapter_addr);

	strcpy(buf, "sap/");
	bluetooth_create_path(dev_addr, adapter_addr, buf + 4,
						sizeof(buf) - 4);

	modem = ofono_modem_create(buf, "sap");
	if (modem == NULL)
		return -ENOMEM;

	data = g_try_new0(struct sap_data, 1);
	if (data == NULL)
		goto free;

	data->server_path = g_strdup(device);
	if (data->server_path == NULL)
		goto free;

	ofono_modem_set_data(modem, data);
	ofono_modem_set_name(modem, alias);
	ofono_modem_register(modem);

	g_hash_table_insert(modem_hash, g_strdup(device), modem);

	return 0;

free:
	g_free(data);
	ofono_modem_remove(modem);

	return -ENOMEM;
}

static void bluetooth_sap_remove(const char *prefix)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("%s", prefix);

	if (modem_hash == NULL || prefix == NULL)
		return;

	g_hash_table_iter_init(&iter, modem_hash);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (g_str_has_prefix((char *)key, prefix) == FALSE)
			continue;

		g_hash_table_iter_remove(&iter);
		sap_remove_modem(value);
	}
}

static void bluetooth_sap_set_alias(const char *device, const char *alias)
{
	struct ofono_modem *modem;

	if (device == NULL || alias == NULL)
		return;

	modem =	g_hash_table_lookup(modem_hash, device);
	if (modem == NULL)
		return;

	ofono_modem_set_name(modem, alias);
}

static struct ofono_modem_driver sap_driver = {
	.name		= "sap",
	.probe		= sap_probe,
	.remove		= sap_remove,
	.enable		= sap_enable,
	.disable	= sap_disable,
	.pre_sim	= sap_pre_sim,
	.post_sim	= sap_post_sim,
};

static struct bluetooth_profile sap = {
	.name		= "sap",
	.probe		= bluetooth_sap_probe,
	.remove		= bluetooth_sap_remove,
	.set_alias	= bluetooth_sap_set_alias,
};

static int sap_init(void)
{
	int err;

	if (DBUS_TYPE_UNIX_FD < 0)
		return -EBADF;

	connection = ofono_dbus_get_connection();

	err = ofono_modem_driver_register(&sap_driver);
	if (err < 0)
		return err;

	err = bluetooth_register_uuid(SAP_UUID, &sap);
	if (err < 0) {
		ofono_modem_driver_unregister(&sap_driver);
		return err;
	}

	modem_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	return 0;
}

static void sap_exit(void)
{
	DBG("");

	bluetooth_unregister_uuid(SAP_UUID);
	ofono_modem_driver_unregister(&sap_driver);
	g_hash_table_destroy(modem_hash);
	modem_hash = NULL;
}

OFONO_PLUGIN_DEFINE(sap, "Sim Access Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, sap_init, sap_exit)
