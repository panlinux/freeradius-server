/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/tacacs/decode.c
 * @brief Low-Level TACACS+ decoding functions
 *
 * @copyright 2017 The FreeRADIUS server project
 * @copyright 2017 Network RADIUS SARL (legal@networkradius.com)
 */

#include <freeradius-devel/io/test_point.h>
#include <freeradius-devel/protocol/tacacs/dictionary.h>
#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/log.h>
#include <freeradius-devel/util/base.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/net.h>
#include <freeradius-devel/util/struct.h>

#include "tacacs.h"
#include "attrs.h"

#define DECODE_FIELD_UINT8(_da, _field) do { \
	vp = fr_pair_afrom_da(ctx, _da); \
	if (!vp) goto oom; \
	vp->vp_uint8 = _field; \
	fr_cursor_append(&cursor, vp); \
} while (0)

#define DECODE_FIELD_STRING8(_da, _field) do { \
	if (tacacs_decode_field(ctx, &cursor, _da, &p, \
	    _field, end) < 0) return -1; \
} while (0)

#define DECODE_FIELD_STRING16(_da, _field) do { \
	if (tacacs_decode_field(ctx, &cursor, _da, &p, \
	    ntohs(_field), end) < 0) return -1; \
} while (0)


/**
 *	Decode a TACACS+ 'arg_N' fields.
 */
static int tacacs_decode_args(TALLOC_CTX *ctx, fr_cursor_t *cursor, fr_dict_attr_t const *da,
			      uint8_t arg_cnt, uint8_t *arg_list, uint8_t **data, uint8_t const *end)
{
	uint8_t i;
	uint8_t *p = *data;
	VALUE_PAIR *vp;

	/*
	 *	No one? Just get out!
	 */
	if (!arg_cnt) return 0;

	if ((p + arg_cnt) > end) {
		fr_strerror_printf("Argument count %u overflows the remaining data in the packet", arg_cnt);
		return -1;
	}

	/*
	 *	Then, do the dirty job...
	 */
	for (i = 0; i < arg_cnt; i++) {
		if ((p + arg_list[i]) > end) {
			fr_strerror_printf("'%s' argument %u length %u overflows the remaining data in the packet",
					   da->name, i, arg_list[i]);
			return -1;
		}

		vp = fr_pair_afrom_da(ctx, da);
		if (!vp) {
			fr_strerror_printf("Out of Memory");
			return -1;
		}

		fr_pair_value_bstrndup(vp, (char *) p, arg_list[i], true);
		fr_cursor_append(cursor, vp);
		p += arg_list[i];
		*data  = (uint8_t *)p;
	}

	return 0;
}

/**
 *	Decode a TACACS+ field.
 */
static int tacacs_decode_field(TALLOC_CTX *ctx, fr_cursor_t *cursor, fr_dict_attr_t const *da,
				uint8_t **field_data, uint16_t field_len, uint8_t const *end)
{
	uint8_t *p = *field_data;
	VALUE_PAIR *vp;

	if ((p + field_len) > end) {
		fr_strerror_printf("'%s' length %u overflows the remaining data in the packet",
				   da->name, field_len);
		return -1;
	}

	vp = fr_pair_afrom_da(ctx, da);
	if (!vp) {
		fr_strerror_printf("Out of Memory");
		return -1;
	}

	if (field_len) {
		fr_pair_value_bstrndup(vp, (char const *)p, field_len, true);
		p += field_len;
		*field_data = p;
	}

	fr_cursor_append(cursor, vp);

	return 0;
}

/**
 *	Decode a TACACS+ packet
 */
ssize_t fr_tacacs_decode(TALLOC_CTX *ctx, uint8_t const *buffer, size_t buffer_len, UNUSED const uint8_t *original, char const * const secret, size_t secret_len, VALUE_PAIR **vps)
{
	fr_dict_attr_t const	*tlv;
	fr_tacacs_packet_t	*pkt;
	VALUE_PAIR		*vp;
	uint8_t			*p;
	uint8_t const		*end;
	uint8_t			*our_buffer;
	fr_cursor_t		cursor;

	/*
	 *	We need that to decrypt the body content.
	 */
	our_buffer = talloc_memdup(ctx, buffer, buffer_len);
	if (!our_buffer) {
	oom:
		fr_strerror_printf("Out of Memory");
		return -1;
	}

	/*
	 * 3.4. The TACACS+ Packet Header
	 *
	 * 1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
	 * +----------------+----------------+----------------+----------------+
	 * |major  | minor  |                |                |                |
	 * |version| version|      type      |     seq_no     |   flags        |
	 * +----------------+----------------+----------------+----------------+
	 * |                                                                   |
	 * |                            session_id                             |
	 * +----------------+----------------+----------------+----------------+
	 * |                                                                   |
	 * |                              length                               |
	 * +----------------+----------------+----------------+----------------+
	 */
	fr_cursor_init(&cursor, vps);

	/*
	 *	Call the struct encoder to do the actual work.
	 */
	if (fr_struct_from_network(ctx, &cursor, attr_tacacs_packet, our_buffer, buffer_len, &tlv, NULL, NULL) < 0) {
		fr_strerror_printf("Problems to decode %s using fr_struct_from_network()", attr_tacacs_packet->name);
		return -1;
	}

	pkt = (fr_tacacs_packet_t *)our_buffer;
	end = our_buffer + buffer_len;

	/*
	 *	3.6. Encryption
	 */
	if (pkt->hdr.flags == FR_TAC_PLUS_ENCRYPTED_MULTIPLE_CONNECTIONS_FLAG) {
		uint8_t *body = (our_buffer + sizeof(fr_tacacs_packet_hdr_t));

		fr_assert(secret != NULL);
		fr_assert(secret_len > 0);

		if (!secret || secret_len < 1) {
			fr_strerror_printf("Packet is encrypted, but no secret is set.");
			return -1;
		}

		if (fr_tacacs_body_xor(pkt, body, ntohl(pkt->hdr.length), secret, secret_len) < 0) return -1;
	}

	switch (pkt->hdr.type) {
	case FR_TAC_PLUS_AUTHEN:
		if (packet_is_authen_start_request(pkt)) {
			/**
			 * 4.1. The Authentication START Packet Body
			 *
			 *  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |    action      |    priv_lvl    |  authen_type   | authen_service |
			 * +----------------+----------------+----------------+----------------+
			 * |    user_len    |    port_len    |  rem_addr_len  |    data_len    |
			 * +----------------+----------------+----------------+----------------+
			 * |    user ...
			 * +----------------+----------------+----------------+----------------+
			 * |    port ...
			 * +----------------+----------------+----------------+----------------+
			 * |    rem_addr ...
			 * +----------------+----------------+----------------+----------------+
			 * |    data...
			 * +----------------+----------------+----------------+----------------+
			 */

//			PACKET_HEADER_CHECKER("Authentication START", 8)
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_START);

			/*
			 *	Decode 4 octets of various flags.
			 */
			DECODE_FIELD_UINT8(attr_tacacs_action, pkt->authen.start.action);
			DECODE_FIELD_UINT8(attr_tacacs_privilege_level, pkt->authen.start.priv_lvl);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_type, pkt->authen.start.authen_type);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_service, pkt->authen.start.authen_service);

			/*
			 *	Decode 4 fields, based on their "length"
			 */
			p = pkt->authen.start.body;
			DECODE_FIELD_STRING8(attr_tacacs_user_name, pkt->authen.start.user_len);
			DECODE_FIELD_STRING8(attr_tacacs_client_port, pkt->authen.start.port_len);
			DECODE_FIELD_STRING8(attr_tacacs_remote_address, pkt->authen.start.rem_addr_len);
			DECODE_FIELD_STRING8(attr_tacacs_data, pkt->authen.start.data_len);

		} else if (packet_is_authen_continue(pkt)) {
			/*
			 * 4.3. The Authentication CONTINUE Packet Body
			 *
			 * This packet is sent from the client to the server following the receipt of
			 * a REPLY packet.
			 *
			 *  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |          user_msg len           |            data_len             |
			 * +----------------+----------------+----------------+----------------+
			 * |     flags      |  user_msg ...
			 * +----------------+----------------+----------------+----------------+
			 * |    data ...
			 * +----------------+
			 */

//			PACKET_HEADER_CHECKER("Authentication CONTINUE", 5);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_CONTINUE);

			/*
			 *	Decode 2 fields, based on their "length"
			 */
			p = pkt->authen.cont.body;
			DECODE_FIELD_STRING16(attr_tacacs_user_message, pkt->authen.cont.user_msg_len);
			DECODE_FIELD_STRING16(attr_tacacs_data, pkt->authen.cont.data_len);

			/*
			 *	And finally the flags.
			 */
			DECODE_FIELD_UINT8(attr_tacacs_authentication_flags, pkt->authen.cont.flags);

		} else if (packet_is_authen_reply(pkt)) {
			/*
			 * 4.2. The Authentication REPLY Packet Body
			 *
			 * 1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |     status     |      flags     |        server_msg_len           |
			 * +----------------+----------------+----------------+----------------+
			 * |           data_len              |        server_msg ...
			 * +----------------+----------------+----------------+----------------+
			 * |           data ...
			 * +----------------+----------------+
			 */

//			PACKET_HEADER_CHECKER("Authentication REPLY", 6);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_REPLY);

			DECODE_FIELD_UINT8(attr_tacacs_authentication_status, pkt->authen.reply.status);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_flags, pkt->authen.reply.flags);

			/*
			 *	Decode 2 fields, based on their "length"
			 */
			p = pkt->authen.reply.body;
			DECODE_FIELD_STRING16(attr_tacacs_server_message, pkt->authen.reply.server_msg_len);
			DECODE_FIELD_STRING16(attr_tacacs_data, pkt->authen.reply.data_len);

		} else {
		unknown_packet:
			fr_strerror_printf("Unknown packet type");
			return -1;
		}
		break;

	case FR_TAC_PLUS_AUTHOR:
		if (packet_is_author_request(pkt)) {
			/*
			 * 5.1. The Authorization REQUEST Packet Body
			 *
			 *  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |  authen_method |    priv_lvl    |  authen_type   | authen_service |
			 * +----------------+----------------+----------------+----------------+
			 * |    user_len    |    port_len    |  rem_addr_len  |    arg_cnt     |
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_1_len    |   arg_2_len    |      ...       |   arg_N_len    |
			 * +----------------+----------------+----------------+----------------+
			 * |   user ...
			 * +----------------+----------------+----------------+----------------+
			 * |   port ...
			 * +----------------+----------------+----------------+----------------+
			 * |   rem_addr ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_1 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_2 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_N ...
			 * +----------------+----------------+----------------+----------------+
			 */

//			PACKET_HEADER_CHECKER("Authorization REQUEST", 8);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_REQUEST);

			/*
			 *	Decode 4 octets of various flags.
			 */
			DECODE_FIELD_UINT8(attr_tacacs_authentication_method, pkt->author.req.authen_method);
			DECODE_FIELD_UINT8(attr_tacacs_privilege_level, pkt->author.req.priv_lvl);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_type, pkt->author.req.authen_type);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_service, pkt->author.req.authen_service);

			/*
			 *	Decode 3 fields, based on their "length"
			 */
			p = (pkt->author.req.body + pkt->author.req.arg_cnt);
			DECODE_FIELD_STRING8(attr_tacacs_user_name, pkt->author.req.user_len);
			DECODE_FIELD_STRING8(attr_tacacs_client_port, pkt->author.req.port_len);
			DECODE_FIELD_STRING8(attr_tacacs_remote_address, pkt->author.req.rem_addr_len);

			/*
			 *	Decode 'arg_N' arguments (horrible format)
			 */
			if (tacacs_decode_args(ctx, &cursor, attr_tacacs_argument_list,
					       pkt->author.req.arg_cnt, pkt->author.req.body, &p, end) < 0) return -1;

		} else if (packet_is_author_response(pkt)) {
			/*
			 * 5.2. The Authorization RESPONSE Packet Body
			 *
			 *  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |    status      |     arg_cnt    |         server_msg len          |
			 * +----------------+----------------+----------------+----------------+
			 * +            data_len             |    arg_1_len   |    arg_2_len   |
			 * +----------------+----------------+----------------+----------------+
			 * |      ...       |   arg_N_len    |         server_msg ...
			 * +----------------+----------------+----------------+----------------+
			 * |   data ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_1 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_2 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_N ...
			 * +----------------+----------------+----------------+----------------+
			 */

//			PACKET_HEADER_CHECKER("Authorization RESPONSE", 6);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_RESPONSE);

			/*
			 *	Decode 1 octets
			 */
			DECODE_FIELD_UINT8(attr_tacacs_authorization_status, pkt->author.res.status);

			/*
			 *	Decode 2 fields, based on their "length"
			 */
			p = (pkt->author.res.body + pkt->author.res.arg_cnt);
			DECODE_FIELD_STRING16(attr_tacacs_server_message, pkt->author.res.server_msg_len);
			DECODE_FIELD_STRING16(attr_tacacs_data, pkt->author.res.data_len);

			/*
			 *	Decode 'arg_N' arguments (horrible format)
			 */
			if (tacacs_decode_args(ctx, &cursor, attr_tacacs_argument_list,
					pkt->author.res.arg_cnt, pkt->author.res.body, &p, end) < 0) return -1;

		} else {
			goto unknown_packet;
		}
		break;

	case FR_TAC_PLUS_ACCT:
		if (packet_is_acct_request(pkt)) {
			/**
			 * 6.1. The Account REQUEST Packet Body
			 *
			 1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |      flags     |  authen_method |    priv_lvl    |  authen_type   |
			 * +----------------+----------------+----------------+----------------+
			 * | authen_service |    user_len    |    port_len    |  rem_addr_len  |
			 * +----------------+----------------+----------------+----------------+
			 * |    arg_cnt     |   arg_1_len    |   arg_2_len    |      ...       |
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_N_len    |    user ...
			 * +----------------+----------------+----------------+----------------+
			 * |   port ...
			 * +----------------+----------------+----------------+----------------+
			 * |   rem_addr ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_1 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_2 ...
			 * +----------------+----------------+----------------+----------------+
			 * |   ...
			 * +----------------+----------------+----------------+----------------+
			 * |   arg_N ...
			 * +----------------+----------------+----------------+----------------+
			 */

//			PACKET_HEADER_CHECKER("Accounting REQUEST", 9);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_REQUEST);

			/*
			 *	Decode 5 octets of various flags.
			 */
			DECODE_FIELD_UINT8(attr_tacacs_accounting_flags, pkt->acct.req.flags);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_method, pkt->acct.req.authen_method);
			DECODE_FIELD_UINT8(attr_tacacs_privilege_level, pkt->acct.req.priv_lvl);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_type, pkt->acct.req.authen_type);
			DECODE_FIELD_UINT8(attr_tacacs_authentication_service, pkt->acct.req.authen_service);

			/*
			 *	Decode 3 fields, based on their "length"
			 */
			p = (pkt->acct.req.body + pkt->acct.req.arg_cnt);
			DECODE_FIELD_STRING8(attr_tacacs_user_name, pkt->acct.req.user_len);
			DECODE_FIELD_STRING8(attr_tacacs_client_port, pkt->acct.req.port_len);
			DECODE_FIELD_STRING8(attr_tacacs_remote_address, pkt->acct.req.rem_addr_len);

			/*
			 *	Decode 'arg_N' arguments (horrible format)
			 */
			if (tacacs_decode_args(ctx, &cursor, attr_tacacs_argument_list,
					pkt->acct.req.arg_cnt, pkt->acct.req.body, &p, end) < 0) return -1;

		} else if (packet_is_acct_reply(pkt)) {
			/**
			 * 6.2. The Accounting REPLY Packet Body
			 *
			 * 1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8  1 2 3 4 5 6 7 8
			 * +----------------+----------------+----------------+----------------+
			 * |         server_msg len          |            data_len             |
			 * +----------------+----------------+----------------+----------------+
			 * |     status     |         server_msg ...
			 * +----------------+----------------+----------------+----------------+
			 * |     data ...
			 * +----------------+
			 */

//			PACKET_HEADER_CHECKER("Accounting REPLY", 5);
			DECODE_FIELD_UINT8(attr_tacacs_packet_body_type, FR_TACACS_PACKET_BODY_TYPE_REPLY);

			/*
			 *	Decode 2 fields, based on their "length"
			 */
			p = pkt->acct.reply.body;
			DECODE_FIELD_STRING16(attr_tacacs_server_message, pkt->acct.reply.server_msg_len);
			DECODE_FIELD_STRING16(attr_tacacs_data, pkt->acct.reply.data_len);

			/* Decode 1 octet */
			DECODE_FIELD_UINT8(attr_tacacs_accounting_status, pkt->acct.reply.status);
		} else {
			goto unknown_packet;
		}
		break;
	default:
		fr_strerror_printf("decode: Unsupported TACACS+ type %u", pkt->hdr.type);
		return -1;
	}

	return buffer_len;
}

/*
 *	Test points for protocol decode
 */
static ssize_t fr_tacacs_decode_proto(TALLOC_CTX *ctx, VALUE_PAIR **vps, uint8_t const *data, size_t data_len, void *proto_ctx)
{
	fr_tacacs_ctx_t	*test_ctx = talloc_get_type_abort(proto_ctx, fr_tacacs_ctx_t);

	return fr_tacacs_decode(ctx, data, data_len, NULL, test_ctx->secret, (talloc_array_length(test_ctx->secret)-1), vps);
}

static int _encode_test_ctx(fr_tacacs_ctx_t *proto_ctx)
{
	talloc_const_free(proto_ctx->secret);

	fr_tacacs_free();

	return 0;
}

static int decode_test_ctx(void **out, TALLOC_CTX *ctx)
{
	fr_tacacs_ctx_t *test_ctx;

	if (fr_tacacs_init() < 0) return -1;

	test_ctx = talloc_zero(ctx, fr_tacacs_ctx_t);
	if (!test_ctx) return -1;

	test_ctx->secret = talloc_strdup(test_ctx, "testing123");
	test_ctx->root = fr_dict_root(dict_tacacs);
	talloc_set_destructor(test_ctx, _encode_test_ctx);

	*out = test_ctx;

	return 0;
}

extern fr_test_point_proto_decode_t tacacs_tp_decode_proto;
fr_test_point_proto_decode_t tacacs_tp_decode_proto = {
	.test_ctx	= decode_test_ctx,
	.func		= fr_tacacs_decode_proto
};
