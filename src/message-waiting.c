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
#include <sys/stat.h>
#include <sys/types.h>

#include "ofono.h"

#include "driver.h"
#include "common.h"
#include "util.h"
#include "sim.h"
#include "simutil.h"
#include "smsutil.h"

#define MESSAGE_WAITING_INTERFACE "org.ofono.MessageWaiting"

struct mailbox_state {
	gboolean indication;
	unsigned char message_count;
};

struct message_waiting_data {
	struct mailbox_state messages[5];
	unsigned char efmwis_length;
	unsigned char efmbdn_length;
	unsigned char efmbdn_record_id[5];
	struct ofono_phone_number mailbox_number[5];
};

struct mbdn_set_request {
	struct ofono_modem *modem;
	struct message_waiting_data *mw;
	int mailbox;
	struct ofono_phone_number number;
	DBusMessage *msg;
};

static struct message_waiting_data *message_waiting_create()
{
	return g_try_new0(struct message_waiting_data, 1);
}

static void message_waiting_destroy(gpointer userdata)
{
	struct ofono_modem *modem = userdata;
	struct message_waiting_data *data = modem->message_waiting;

	g_free(data);

	modem->message_waiting = NULL;
}

static const char *mw_message_waiting_property_name[5] = {
	"VoicemailWaiting",
#if 0
	"FaxWaiting",
	"EmailWaiting",
	"OtherWaiting",
	"VideomailWaiting",
#endif
};

static const char *mw_message_count_property_name[5] = {
	"VoicemailMessageCount",
#if 0
	"FaxMessageCount",
	"EmailMessageCount",
	"OtherMessageCount",
	"VideomailMessageCount",
#endif
};

static const char *mw_mailbox_property_name[5] = {
	"VoicemailMailboxNumber",
#if 0
	"FaxMailboxNumber",
	"EmailMailboxNumber",
	"OtherMailboxNumber",
	"VideomailMailboxNumber",
#endif
};

static DBusMessage *mw_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_modem *modem = data;
	struct message_waiting_data *mw = modem->message_waiting;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	int i;
	dbus_bool_t indication;
	unsigned char count;
	const char *number;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			OFONO_PROPERTIES_ARRAY_SIGNATURE, &dict);

	for (i = 0; i < 5; i++) {
		if (mw_message_waiting_property_name[i]) {
			indication = mw->messages[i].indication;

			ofono_dbus_dict_append(&dict,
					mw_message_waiting_property_name[i],
					DBUS_TYPE_BOOLEAN, &indication);
		}

		if (mw_message_count_property_name[i]) {
			count = mw->messages[i].message_count;

			ofono_dbus_dict_append(&dict,
					mw_message_count_property_name[i],
					DBUS_TYPE_BYTE, &count);
		}

		if (mw_mailbox_property_name[i]) {
			number = phone_number_to_string(&mw->mailbox_number[i]);

			ofono_dbus_dict_append(&dict,
					mw_mailbox_property_name[i],
					DBUS_TYPE_STRING, &number);
		}
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void mbdn_set_cb(struct ofono_modem *modem, int ok, void *data)
{
	struct mbdn_set_request *req = data;
	struct ofono_phone_number *old = &req->mw->mailbox_number[req->mailbox];
	const char *property;
	DBusMessage *reply = NULL;

	if (!ok) {
		if (req->msg)
			reply = __ofono_error_failed(req->msg);

		goto out;
	}

	if (g_str_equal(req->number.number, old->number) &&
			req->number.type == old->type)
		goto out;

	memcpy(old, &req->number, sizeof(struct ofono_phone_number));

	property = mw_mailbox_property_name[req->mailbox];

	if (property) {
		DBusConnection *conn = ofono_dbus_get_connection();
		const char *number;

		number = phone_number_to_string(old);

		ofono_dbus_signal_property_changed(conn, req->modem->path,
						MESSAGE_WAITING_INTERFACE,
						property, DBUS_TYPE_STRING,
						&number);
	}

	if (req->msg)
		reply = dbus_message_new_method_return(req->msg);

out:
	if (req->msg && reply)
		__ofono_dbus_pending_reply(&req->msg, reply);

	g_free(req);
}

static DBusMessage *set_mbdn(struct ofono_modem *modem, int mailbox,
			const char *number, DBusMessage *msg)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct mbdn_set_request *req;
	unsigned char efmbdn[255];

	if (modem->message_waiting->efmbdn_record_id[mailbox] == 0) {
		if (msg)
			return __ofono_error_failed(msg);

		return NULL;
	}

	req = g_new0(struct mbdn_set_request, 1);

	req->modem = modem;
	req->mw = modem->message_waiting;
	req->mailbox = mailbox;
	string_to_phone_number(number, &req->number);
	req->msg = dbus_message_ref(msg);

	sim_adn_build(efmbdn, req->mw->efmbdn_length, &req->number);

	if (ofono_sim_write(modem, SIM_EFMBDN_FILEID, mbdn_set_cb,
			OFONO_SIM_FILE_STRUCTURE_FIXED,
			req->mw->efmbdn_record_id[mailbox],
			efmbdn, req->mw->efmbdn_length, req) == -1) {
		g_free(req);

		if (msg)
			return __ofono_error_failed(msg);
	}

	return NULL;
}

static DBusMessage *mw_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_modem *modem = data;
	struct message_waiting_data *mw = modem->message_waiting;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name, *value;
	int i;

	if (mw->efmbdn_length == 0)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);

	for (i = 0; i < 5; i++)
		if (mw_mailbox_property_name[i] &&
				!strcmp(name, mw_mailbox_property_name[i]))
			break;

	if (i < 5) {
		const char *cur_number;

		dbus_message_iter_next(&iter);

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_recurse(&iter, &var);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (!valid_phone_number_format(value))
			return __ofono_error_invalid_format(msg);

		cur_number = phone_number_to_string(&mw->mailbox_number[i]);

		if (g_str_equal(cur_number, value))
			return dbus_message_new_method_return(msg);

		return set_mbdn(modem, i, value, msg);
	}

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable message_waiting_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	mw_get_properties	},
	{ "SetProperty",	"sv",	"",		mw_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable message_waiting_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static void mw_mwis_read_cb(struct ofono_modem *modem, int ok,
		enum ofono_sim_file_structure structure, int total_length,
		int record, const unsigned char *data, int record_length,
		void *userdata)
{
	int i, status;
	struct mailbox_state info;
	dbus_bool_t indication;
	unsigned char count;
	DBusConnection *conn = ofono_dbus_get_connection();
	struct message_waiting_data *mw = modem->message_waiting;

	if (!ok ||
		structure != OFONO_SIM_FILE_STRUCTURE_FIXED ||
			record_length < 5) {
		ofono_error("Unable to read waiting messages numbers "
			"from SIM");

		mw->efmwis_length = 0;

		return;
	}

	/* Handle only current identity (TODO: currently assumes first) */
	if (record != 1)
		return;

	status = data[0];
	data++;

	for (i = 0; i < 5 && i < record_length - 1; i++) {
		info.indication = (status >> i) & 1;
		info.message_count = info.indication ? data[0] : 0;

		if (mw->messages[i].indication != info.indication ||
				mw->messages[i].message_count !=
				info.message_count) {
			memcpy(&mw->messages[i], &info, sizeof(info));

			indication = info.indication;
			count = info.message_count;

			if (!mw_message_waiting_property_name[i])
				continue;

			ofono_dbus_signal_property_changed(conn, modem->path,
					MESSAGE_WAITING_INTERFACE,
					mw_message_waiting_property_name[i],
					DBUS_TYPE_BOOLEAN, &indication);

			ofono_dbus_signal_property_changed(conn, modem->path,
					MESSAGE_WAITING_INTERFACE,
					mw_message_count_property_name[i],
					DBUS_TYPE_BYTE, &count);
		}
	}

	mw->efmwis_length = record_length;
}

static void mw_mbdn_read_cb(struct ofono_modem *modem, int ok,
		enum ofono_sim_file_structure structure, int total_length,
		int record, const unsigned char *data, int record_length,
		void *userdata)
{
	int i;
	DBusConnection *conn = ofono_dbus_get_connection();
	struct message_waiting_data *mw = modem->message_waiting;
	const char *value;

	if (!ok ||
		structure != OFONO_SIM_FILE_STRUCTURE_FIXED ||
			record_length < 14 || total_length < record_length) {
		ofono_error("Unable to read mailbox dialling numbers "
			"from SIM");

		mw->efmbdn_length = -1;
		return;
	}

	for (i = 0; i < 5; i++)
		if (record == mw->efmbdn_record_id[i])
			break;

	if (i == 5)
		return;

	if (sim_adn_parse(data, record_length, &mw->mailbox_number[i]) == FALSE)
		mw->mailbox_number[i].number[0] = '\0';

	if (mw_mailbox_property_name[i]) {
		value = phone_number_to_string(&mw->mailbox_number[i]);

		ofono_dbus_signal_property_changed(conn, modem->path,
				MESSAGE_WAITING_INTERFACE,
				mw_mailbox_property_name[i],
				DBUS_TYPE_STRING, &value);
	}

	mw->efmbdn_length = record_length;
}

static void mw_mbi_read_cb(struct ofono_modem *modem, int ok,
		enum ofono_sim_file_structure structure, int total_length,
		int record, const unsigned char *data, int record_length,
		void *userdata)
{
	int i, err;
	struct message_waiting_data *mw = modem->message_waiting;

	if (!ok ||
		structure != OFONO_SIM_FILE_STRUCTURE_FIXED ||
			record_length < 4) {
		ofono_error("Unable to read mailbox identifies "
			"from SIM");

		mw->efmbdn_length = -1;
		return;
	}

	/* Handle only current identity (TODO: currently assumes first) */
	if (record != 1)
		return;

	for (i = 0; i < 5 && i < record_length; i++)
		mw->efmbdn_record_id[i] = data[i];

	err = ofono_sim_read(modem, SIM_EFMBDN_FILEID, mw_mbdn_read_cb, NULL);

	if (err != 0)
		ofono_error("Unable to read EF-MBDN from SIM");
}

static void mw_mwis_write_cb(struct ofono_modem *modem, int ok, void *userdata)
{
	if (!ok)
		ofono_error("Writing new EF-MBDN failed");
}

/* Loads MWI states and MBDN from SIM */
static gboolean mw_mwis_load(struct ofono_modem *modem)
{
	int err;

	err = ofono_sim_read(modem, SIM_EFMWIS_FILEID, mw_mwis_read_cb, NULL);

	if (err != 0)
		return FALSE;

	err = ofono_sim_read(modem, SIM_EFMBI_FILEID, mw_mbi_read_cb, NULL);

	if (err != 0)
		return FALSE;

	return TRUE;
}

static void mw_set_indicator(struct ofono_modem *modem, int profile,
				enum sms_mwi_type type,
				gboolean present, unsigned char messages)
{
	struct message_waiting_data *mw = modem->message_waiting;
	DBusConnection *conn = ofono_dbus_get_connection();
	unsigned char efmwis[255];  /* Max record size */
	int i;

	if (mw == NULL)
		return;

	/* Handle only current identity (TODO: currently assumes first) */
	if (profile != 1)
		return;

	if (mw->messages[type].indication == present &&
			mw->messages[type].message_count == messages)
		return;

	if (mw->messages[type].indication != present) {
		dbus_bool_t indication;

		indication = present;
		mw->messages[type].indication = present;

		if (!mw_message_waiting_property_name[type])
			ofono_dbus_signal_property_changed(conn, modem->path,
					MESSAGE_WAITING_INTERFACE,
					mw_message_waiting_property_name[type],
					DBUS_TYPE_BOOLEAN, &indication);
	}

	if (mw->messages[type].message_count != messages) {
		mw->messages[type].message_count = messages;

		if (!mw_message_waiting_property_name[type])
			ofono_dbus_signal_property_changed(conn, modem->path,
					MESSAGE_WAITING_INTERFACE,
					mw_message_count_property_name[type],
					DBUS_TYPE_BYTE, &messages);
	}

	/* Writes MWI states and/or MBDN back to SIM */
	if ((mw->efmwis_length < 5)) {
		ofono_error("Unable to update MWIS indicator");
		return;
	}

	/* Fill in numbers of messages in bytes 1 to X of EF-MWIS */
	for (i = 0; i < 5 && i < mw->efmwis_length - 1; i++)
		efmwis[i + 1] = mw->messages[i].message_count;

	/* Fill in indicator state bits in byte 0 */
	for (i = 0; i < 5 && i < mw->efmwis_length - 1; i++)
		if (mw->messages[i].indication)
			efmwis[0] |= 1 << i;

	if (ofono_sim_write(modem, SIM_EFMWIS_FILEID, mw_mwis_write_cb,
				OFONO_SIM_FILE_STRUCTURE_FIXED, 1,
				efmwis, mw->efmwis_length, NULL) != 0) {
		ofono_error("Queuing a EF-MWI write to SIM failed");
	}
}

static void initialize_message_waiting(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (!mw_mwis_load(modem)) {
		ofono_error("Could not register MessageWaiting interface");
		message_waiting_destroy(modem);

		return;
	}

	if (!g_dbus_register_interface(conn, modem->path,
					MESSAGE_WAITING_INTERFACE,
					message_waiting_methods,
					message_waiting_signals,
					NULL, modem,
					message_waiting_destroy)) {
		ofono_error("Could not register MessageWaiting interface");
		message_waiting_destroy(modem);

		return;
	}

	ofono_debug("MessageWaiting interface for modem: %s created",
			modem->path);

	ofono_modem_add_interface(modem, MESSAGE_WAITING_INTERFACE);
}

int ofono_message_waiting_register(struct ofono_modem *modem)
{
	if (modem == NULL)
		return -1;

	modem->message_waiting = message_waiting_create();

	ofono_sim_ready_notify_register(modem, initialize_message_waiting);
	if (ofono_sim_get_ready(modem))
		initialize_message_waiting(modem);

	return 0;
}

void ofono_message_waiting_unregister(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	g_dbus_unregister_interface(conn, modem->path,
					MESSAGE_WAITING_INTERFACE);
	ofono_modem_remove_interface(modem, MESSAGE_WAITING_INTERFACE);
}

static void handle_special_sms_iei(struct ofono_modem *modem,
		const guint8 *iei, gboolean *discard)
{
	enum sms_mwi_type type;
	int profile;
	gboolean set;

	/* Parse type & storage byte */
	if (discard)
		*discard = (iei[0] & (1 << 7)) ? FALSE : TRUE;

	type = iei[0] & 0x1f;
	if (type > SMS_MWI_TYPE_OTHER) {
		if (type == (SMS_MWI_TYPE_OTHER | 4))
			type = SMS_MWI_TYPE_VIDEO;
		else
			/* 23.040 9.2.3.24.2: "Terminals should be capable of
			 * receiving any values in octet 1, even including
			 * those marked as Reserved."  Treat Reserved as
			 * "Other".  */
			type = SMS_MWI_TYPE_OTHER;
	}

	set = iei[1] > 0 ? TRUE : FALSE;
	profile = ((iei[0] >> 5) & 3) + 1;

	mw_set_indicator(modem, profile, type, set, iei[1]);
}

static void handle_enhanced_voicemail_iei(struct ofono_modem *modem,
		const guint8 *iei, gboolean *discard, int length)
{
	int profile, n;
	gboolean set;
	struct sms_address mailbox_address;

	if (length < 3)
		return;

	/* ENHANCED_VOICE_MAIL_PDU_TYPE */
	if (!(iei[0] & 1)) {
		/* 9.2.3.24.13.1 Enhanced Voice Mail Notification */

		/* MULTIPLE_SUBSCRIBER_PROFILE */
		profile = (iei[0] >> 2) & 3;

		/* SM_STORAGE */
		if (discard)
			*discard = (iei[0] & (1 << 4)) ? FALSE : TRUE;

		/* VM_MAILBOX_ACCESS_ADDRESS */
		n = 0;
		if (!sms_decode_address_field(iei + 1, length - 1, &n,
					FALSE, &mailbox_address))
			return;

		/* TODO: VM_MESSAGE_PRIORITY_INDICATION */

		/* Other parameters currently not supported */

		set = iei[n + 2] > 0 ? TRUE : FALSE;
		mw_set_indicator(modem, profile, SMS_MWI_TYPE_VOICE,
					set, iei[n + 2]);
	} else {
		/* 9.2.3.24.13.2 Enhanced Voice Delete Confirmation */

		/* MULTIPLE_SUBSCRIBER_PROFILE */
		profile = (iei[0] >> 2) & 3;

		/* SM_STORAGE */
		if (discard)
			*discard = (iei[0] & (1 << 4)) ? FALSE : TRUE;

		/* VM_MAILBOX_ACCESS_ADDRESS */
		n = 0;
		if (!sms_decode_address_field(iei + 1, length - 1, &n,
					FALSE, &mailbox_address))
			return;

		/* Other parameters currently not supported */

		set = iei[n + 2] > 0 ? TRUE : FALSE;
		mw_set_indicator(modem, profile, SMS_MWI_TYPE_VOICE,
					set, iei[n + 2]);
	}

	if (mailbox_address.address[0] != '\0')
		set_mbdn(modem, SMS_MWI_TYPE_VOICE,
				sms_address_to_string(&mailbox_address), NULL);
}

void ofono_handle_sms_mwi(struct ofono_modem *modem,
		struct sms *sms, gboolean *out_discard)
{
	gboolean active, discard;
	enum sms_mwi_type type;
	int profile = 1, iei_found = 0;

	if (out_discard)
		*out_discard = FALSE;

	/* Check MWI types in the order from highest priority to lowest
	 * because they must override one another.
	 */

	if (sms->deliver.udhi) {
		guint8 evm_iei[140];
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(sms, &iter))
			return;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			switch (iei) {
			case SMS_IEI_ENHANCED_VOICE_MAIL_INFORMATION:
				sms_udh_iter_get_ie_data(&iter, evm_iei);

				handle_enhanced_voicemail_iei(modem, evm_iei,
						out_discard,
						sms_udh_iter_get_ie_length(
							&iter));
				return;
			}

			sms_udh_iter_next(&iter);
		}
	}

	if (sms->deliver.udhi) {
		guint8 special_iei[4];
		struct sms_udh_iter iter;
		enum sms_iei iei;

		if (!sms_udh_iter_init(sms, &iter))
			return;

		while ((iei = sms_udh_iter_get_ie_type(&iter)) !=
				SMS_IEI_INVALID) {
			switch (iei) {
			case SMS_IEI_SPECIAL_MESSAGE_INDICATION:
				if (sms_udh_iter_get_ie_length(&iter) != 2)
					break;
				sms_udh_iter_get_ie_data(&iter, special_iei);

				handle_special_sms_iei(modem, special_iei,
						&discard);
				if (out_discard)
					*out_discard = *out_discard || discard;
				iei_found = 1;
			}

			sms_udh_iter_next(&iter);
		}

		if (iei_found) {
			/* 23.040 9.2.3.24.2 says "In the event of a
			 * conflict between this setting and the setting
			 * of the Data Coding Scheme (see 3GPP TS 23.038 [9])
			 * then the message shall be stored if either the DCS
			 * indicates this, or Octet 1 above indicates this."
			 */
			if (sms_mwi_dcs_decode(sms->deliver.dcs, NULL,
						NULL, NULL, &discard)) {
				if (out_discard)
					*out_discard = *out_discard || discard;
			}

			return;
		}
	}

	if (sms_mwi_dcs_decode(sms->deliver.dcs, &type,
				NULL, &active, out_discard)) {
		mw_set_indicator(modem, profile, type, active, 0);

		return;
	}

	if (sms->deliver.pid == SMS_PID_TYPE_RETURN_CALL)
		return;
}
