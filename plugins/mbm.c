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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sms.h>
#include <ofono/log.h>

struct mbm_data {
	GAtChat *chat;
};

static int mbm_probe(struct ofono_modem *modem)
{
	struct mbm_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct mbm_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void mbm_remove(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static void mbm_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static int mbm_enable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	const char *device;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	syntax = g_at_syntax_new_gsmv1();
	data->chat = g_at_chat_new_from_tty(device, syntax);
	g_at_syntax_unref(syntax);

	if (!data->chat)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, mbm_debug, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return 0;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int mbm_disable(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_send(data->chat, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	g_at_chat_shutdown(data->chat);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	return 0;
}

static void mbm_populate(struct ofono_modem *modem)
{
	struct mbm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	ofono_netreg_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, 0, "atmodem", data->chat);
}

static struct ofono_modem_driver mbm_driver = {
	.name		= "mbm",
	.probe		= mbm_probe,
	.remove		= mbm_remove,
	.enable		= mbm_enable,
	.disable	= mbm_disable,
	.populate	= mbm_populate,
};

static int mbm_init(void)
{
	return ofono_modem_driver_register(&mbm_driver);
}

static void mbm_exit(void)
{
	ofono_modem_driver_unregister(&mbm_driver);
}

OFONO_PLUGIN_DEFINE(mbm, "Ericsson MBM modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, mbm_init, mbm_exit)
