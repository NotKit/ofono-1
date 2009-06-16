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

char *convert_gsm_to_utf8(const unsigned char *text, long len, long *items_read,
				long *items_written, unsigned char terminator);
unsigned char *convert_utf8_to_gsm(const char *text, long len, long *items_read,
				long *items_written, unsigned char terminator);

unsigned char *decode_hex_own_buf(const char *in, long len, long *items_written,
					unsigned char terminator,
					unsigned char *buf);

unsigned char *decode_hex(const char *in, long len, long *items_written,
				unsigned char terminator);

char *encode_hex_own_buf(const unsigned char *in, long len,
				unsigned char terminator, char *buf);

char *encode_hex(const unsigned char *in, long len,
			unsigned char terminator);

unsigned char *unpack_7bit_own_buf(const unsigned char *in, long len,
					int byte_offset, gboolean ussd,
					long max_to_unpack, long *items_written,
					unsigned char terminator,
					unsigned char *buf);

unsigned char *unpack_7bit(const unsigned char *in, long len, int byte_offset,
				gboolean ussd, long max_to_unpack,
				long *items_written, unsigned char terminator);

unsigned char *pack_7bit_own_buf(const unsigned char *in, long len,
					int byte_offset, gboolean ussd,
					long *items_written,
					unsigned char terminator,
					unsigned char *buf);

unsigned char *pack_7bit(const unsigned char *in, long len, int byte_offset,
				gboolean ussd,
				long *items_written, unsigned char terminator);
