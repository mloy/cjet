/*
 *The MIT License (MIT)
 *
 * Copyright (c) <2016> <Stephan Gatzka>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#define BOOST_TEST_MODULE websocket_frame_tests

#include <boost/test/unit_test.hpp>
#include <endian.h>
#include <stdint.h>

#include "buffered_reader.h"
#include "compiler.h"
#include "http_connection.h"
#include "jet_random.h"
#include "websocket.h"

#ifndef ARRAY_SIZE
 #define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const uint8_t WS_HEADER_FIN = 0x80;
static const uint8_t WS_HEADER_RSV = 0x70;
static const uint8_t WS_HEADER_MASK = 0x80;

static const uint8_t WS_OPCODE_CONTINUATION = 0x00;
static const uint8_t WS_OPCODE_TEXT = 0x01;
static const uint8_t WS_OPCODE_BINARY = 0x02;
static const uint8_t WS_OPCODE_CLOSE = 0x08;
static const uint8_t WS_OPCODE_PING = 0x09;
static const uint8_t WS_OPCODE_PONG = 0x0a;

static uint8_t write_buffer[70000];
static uint8_t *write_buffer_ptr;

static uint8_t *read_buffer;
static uint8_t *read_buffer_ptr;
static size_t read_buffer_length;
static uint8_t readback_buffer[70000];
static size_t readback_buffer_length;

static bool br_close_called;
static bool got_error;
static bool text_message_received_called;
static int text_frame_received_called;
static bool binary_message_received_called;
static int binary_frame_received_called;
static bool ping_received_called;
static bool pong_received_called;
static bool close_received_called;
static enum ws_status_code status_code_received;

static void mask_payload(uint8_t *ptr, size_t length, uint8_t mask[4])
{
	for (unsigned int i = 0; i < length; i++) {
		uint8_t byte = ptr[i] ^ mask[i % 4];
		ptr[i] = byte;
	}
}

static void ws_on_error(struct websocket *ws)
{
	(void)ws;
	got_error = true;
}

_Pragma ("GCC diagnostic ignored \"-Wunused-parameter\"")
static int writev(void *this_ptr, struct socket_io_vector *io_vec, unsigned int count, int more)
{
	(void)this_ptr;
	size_t complete_length = 0;

	for (unsigned int i = 0; i < count; i++) {
		::memcpy(write_buffer_ptr, io_vec[i].iov_base, io_vec[i].iov_len);
		write_buffer_ptr += io_vec[i].iov_len;
		complete_length += io_vec[i].iov_len;
	}
	return complete_length;
}

static int read_exactly(void *this_ptr, size_t num, read_handler handler, void *handler_context)
{
	(void)this_ptr;
	uint8_t *ptr = read_buffer_ptr;
	read_buffer_ptr += num;
	if ((ptr - read_buffer) < (cjet_ssize_t)read_buffer_length) {
		handler(handler_context, ptr, num);
	}
	return 0;
}

static bool is_pong_frame(const char *payload)
{
	size_t payload_length = ::strlen(payload);

	uint8_t *ptr = write_buffer;
	uint8_t header;
	::memcpy(&header, ptr, sizeof(header));
	if ((header & WS_HEADER_FIN) != WS_HEADER_FIN) {
		return false;
	}
	if ((header & WS_OPCODE_PONG) != WS_OPCODE_PONG) {
		return false;
	}
	ptr += sizeof(header);

	uint8_t length;
	::memcpy(&length, ptr, sizeof(length));
	bool is_masked = false;
	if ((length & WS_HEADER_MASK) == WS_HEADER_MASK) {
		is_masked = true;
	}
	length = length & ~WS_HEADER_MASK;
	if (length != payload_length) {
		return false;
	}
	ptr += sizeof(length);

	if (is_masked) {
		uint8_t mask[4];
		memcpy(mask, ptr, sizeof(mask));
		ptr += sizeof(mask);

		mask_payload(ptr, length, mask);
	}
	

	if (::memcmp(ptr, payload, payload_length) != 0) {
		return false;
	}
	return true;
}

static bool is_close_frame(enum ws_status_code code)
{
	const uint8_t *ptr = write_buffer;
	uint8_t header;
	::memcpy(&header, ptr, sizeof(header));
	if ((header & WS_HEADER_FIN) != WS_HEADER_FIN) {
		return false;
	}
	if ((header & WS_OPCODE_CLOSE) != WS_OPCODE_CLOSE) {
		return false;
	}
	ptr += sizeof(header);

	uint8_t length;
	::memcpy(&length, ptr, sizeof(length));
	if ((length & WS_HEADER_MASK) == WS_HEADER_MASK) {
		return false;
	}
	if (length != 2) {
		return false;
	}
	ptr += sizeof(length);

	uint16_t status_code;
	::memcpy(&status_code, ptr, sizeof(status_code));
	status_code = be16toh(status_code);
	if (status_code != code) {
		return false;
	}
	return true;
}

static bool check_frame(uint8_t opcode, const char *payload, size_t payload_length)
{
	uint8_t *ptr = write_buffer;
	uint8_t header;
	::memcpy(&header, ptr, sizeof(header));
	if ((header & WS_HEADER_FIN) != WS_HEADER_FIN) {
		return false;
	}
	if ((header & WS_HEADER_RSV) != 0) {
		return false;
	}
	if ((header & opcode) != opcode) {
		return false;
	}
	ptr += sizeof(header);

	uint8_t first_length;
	::memcpy(&first_length, ptr, sizeof(first_length));

	bool is_masked = ((first_length & WS_HEADER_MASK) == WS_HEADER_MASK);
	first_length = first_length & ~WS_HEADER_MASK;
	ptr += sizeof(first_length);

	uint64_t length;
	if (first_length == 126) {
		uint16_t len;
		::memcpy(&len, ptr, sizeof(len));
		ptr += sizeof(len);
		len = be16toh(len);
		length = len;
	} else if (first_length == 127) {
		uint64_t len;
		::memcpy(&len, ptr, sizeof(len));
		ptr += sizeof(len);
		len = be64toh(len);
		length = len;
	} else {
		length = first_length;
	}

	if (length != payload_length) {
		return false;
	}

	if (is_masked) {
		uint8_t mask[4];
		::memcpy(mask, ptr, sizeof(mask));
		ptr += sizeof(mask);
		mask_payload(ptr, length, mask);
	}
	if (::memcmp(ptr, payload, length) != 0) {
		return false;
	}

	return true;
}

static int close(void *this_ptr)
{
	(void)this_ptr;
	br_close_called = true;
	return 0;
}

static enum websocket_callback_return text_message_received(struct websocket *s, char *msg, size_t length)
{
	(void)s;
	::memcpy(readback_buffer, msg, length);
	readback_buffer_length += length;
	text_message_received_called = true;
	return WS_OK;
}

static enum websocket_callback_return text_frame_received(struct websocket *s, char *msg, size_t length, bool last_frame)
{
	(void)s;
	::memcpy(readback_buffer + readback_buffer_length, msg, length);
	readback_buffer_length += length;
	if (last_frame) {
		//do nothing
	}
	text_frame_received_called++;
	return WS_OK;
}

static enum websocket_callback_return binary_message_received(struct websocket *s, uint8_t *msg, size_t length)
{
	(void)s;
	::memcpy(readback_buffer, msg, length);
	readback_buffer_length += length;
	binary_message_received_called = true;
	return WS_OK;
}

static enum websocket_callback_return binary_frame_received(struct websocket *s, uint8_t *msg, size_t length, bool last_frame)
{
	(void)s;
	::memcpy(readback_buffer + readback_buffer_length, msg, length);
	readback_buffer_length += length;
	if (last_frame) {
		//do nothing
	}
	binary_frame_received_called++;
	return WS_OK;
}

static enum websocket_callback_return ping_received(struct websocket *s, uint8_t *msg, size_t length)
{
	(void)s;
	(void)msg;
	(void)length;
	::memcpy(readback_buffer + readback_buffer_length, msg, length);
	readback_buffer_length += length;
	ping_received_called = true;
	return WS_OK;
}

static enum websocket_callback_return close_received(struct websocket *s, enum ws_status_code code)
{
	(void)s;
	close_received_called = true;
	status_code_received = code;
	return WS_OK;
}

static enum websocket_callback_return pong_received(struct websocket *s, uint8_t *msg, size_t length)
{
	(void)s;
	::memcpy(readback_buffer, msg, length);
	readback_buffer_length += length;
	pong_received_called = true;
	return WS_OK;
}

static void fill_payload(uint8_t *ptr, const uint8_t *payload, uint64_t length, bool shall_mask, uint8_t mask[4])
{
	if (length > 0) {
		std::memcpy(ptr, payload, length);
		if (shall_mask) {
			mask_payload(ptr, length, mask);
		}
	}
}

static void prepare_message(uint8_t type, uint8_t *buffer, uint64_t length, bool shall_mask, uint8_t mask[4], bool set_fin)
{
	uint8_t *ptr = read_buffer;
	read_buffer_ptr = read_buffer;
	read_buffer_length = 0;
	uint8_t header = 0x00;
	if (set_fin) {
		header |= WS_HEADER_FIN;
	}
	header |= type;
	::memcpy(ptr, &header, sizeof(header));
	ptr += sizeof(header);
	read_buffer_length += sizeof(header);

	uint8_t first_length = 0x00;
	if (shall_mask) {
		first_length |= WS_HEADER_MASK;
	}
	if (length < 126) {
		first_length = first_length | (uint8_t)length;
		::memcpy(ptr, &first_length, sizeof(first_length));
		ptr += sizeof(first_length);
		read_buffer_length += sizeof(first_length);
	} else if (length <= 65535) {
		first_length = first_length | 126;
		::memcpy(ptr, &first_length, sizeof(first_length));
		ptr += sizeof(first_length);
		read_buffer_length += sizeof(first_length);
		uint16_t len = (uint16_t)length;
		len = htobe16(len);
		::memcpy(ptr, &len, sizeof(len));
		ptr += sizeof(len);
		read_buffer_length += sizeof(len);
	} else {
		first_length = first_length | 127;
		::memcpy(ptr, &first_length, sizeof(first_length));
		ptr += sizeof(first_length);
		read_buffer_length += sizeof(first_length);
		uint64_t len = htobe64(length);
		::memcpy(ptr, &len, sizeof(length));
		ptr += sizeof(len);
		read_buffer_length += sizeof(len);
	}

	if (shall_mask) {
		::memcpy(ptr, mask, 4);
		ptr += 4;
		read_buffer_length += 4;
	}

	fill_payload(ptr, buffer, length, shall_mask, mask);
	read_buffer_length += length;
}

static void prepare_message_string(uint8_t type, const char *message, bool shall_mask, uint8_t mask[4])
{
	prepare_message(type, (uint8_t *)message, ::strlen(message), shall_mask, mask, true);
}

static void prepare_message_string_frag(uint8_t type, const char *message, bool shall_mask, uint8_t mask[4], bool set_fin)
{
	prepare_message(type, (uint8_t *)message, ::strlen(message), shall_mask, mask, set_fin);
}

struct F {

	F(bool is_server, uint32_t buffer_length)
	{
		init_random();

		br_close_called = false;
		got_error = false;
		text_message_received_called = false;
		text_frame_received_called = 0;
		binary_message_received_called = false;
		binary_frame_received_called = 0;
		ping_received_called = false;
		close_received_called = false;
		pong_received_called = false;
		status_code_received = (enum ws_status_code)0;
		write_buffer_ptr = write_buffer;
		read_buffer = (uint8_t *)::malloc(buffer_length);
		read_buffer_ptr = read_buffer;
		readback_buffer_length = 0;

		struct http_connection *connection = alloc_http_connection();
		connection->br.writev = writev;
		connection->br.read_exactly = read_exactly;
		connection->br.close = close;
		connection->compression_level = 0;
		ws.protocol_requested = false;
		ws.binary_frame_received = NULL;
		ws.text_frame_received = NULL;
		ws.on_error = NULL;
		ws.connection = NULL;
		ws.length = 0;
		ws.ws_flags.fin = 0;
		ws.ws_flags.rsv = 0;
		ws.ws_flags.mask = 0;
		ws.ws_flags.opcode = 0;
		websocket_init(&ws, connection, is_server, ws_on_error, "jet");
		ws.upgrade_complete = true;
		ws.text_message_received = text_message_received;
		ws.binary_message_received = binary_message_received;
		ws.text_frame_received = text_frame_received;
		ws.binary_frame_received = binary_frame_received;
		ws.ping_received = ping_received;
		ws.pong_received = pong_received;
		ws.close_received = close_received;
	}

	~F()
	{
		free(read_buffer);
		close_random();
	}

	struct websocket ws;
};

BOOST_AUTO_TEST_CASE(test_close_frame_on_websocket_free)
{
	bool is_server = true;
	F f(is_server, 5000);

	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_GOING_AWAY), "No close frame sent when freeing the websocket!");
}

BOOST_AUTO_TEST_CASE(test_receive_text_message)
{
	const char *messages[2] = {"Hello World!", ""};
	bool is_server[2] = {true, false};

	for (unsigned int j = 0; j < ARRAY_SIZE(is_server); j++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
			F f(is_server[j], 5000);

			uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
			prepare_message_string(WS_OPCODE_TEXT, messages[i], is_server[j], mask);
			ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
			websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
			BOOST_CHECK_MESSAGE(text_message_received_called, "Callback for text messages was not called!");
			BOOST_CHECK_MESSAGE(::strncmp(messages[i], (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
		}
	}
}

BOOST_AUTO_TEST_CASE(test_receive_binary_message)
{
	const char *messages[2] = {"Hello World!", ""};
	bool is_server[2] = {true, false};

	for (unsigned int j = 0; j < ARRAY_SIZE(is_server); j++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
			F f(is_server[j], 5000);

			uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
			prepare_message_string(WS_OPCODE_BINARY, messages[i], is_server[j], mask);
			ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
			websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
			BOOST_CHECK_MESSAGE(binary_message_received_called, "Callback for binary messages was not called!");
			BOOST_CHECK_MESSAGE(::strncmp(messages[i], (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
		}
	}
}

BOOST_AUTO_TEST_CASE(test_receive_text_frames)
{
	const char *messages[3] = {"frag1", "frag2", "frag3"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_TEXT, messages[0], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[1], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[2], is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(text_frame_received_called == 3, "Callback for text frames was not or wrong times called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(::strncmp("frag1frag2frag3", (char *)readback_buffer, 15) == 0, "Did not received the same message as sent!");
}

BOOST_AUTO_TEST_CASE(test_recceive_binary_frames)
{
	const char *messages[3] = {"frag1", "frag2", "frag3"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_BINARY, messages[0], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[1], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[2], is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(binary_frame_received_called == 3, "Callback for binary frames was not or wrong times called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(::strncmp("frag1frag2frag3", (char *)readback_buffer, 15) == 0, "Did not received the same message as sent!");
}

BOOST_AUTO_TEST_CASE(test_receive_text_frames_with_ping_in_between)
{
	const char *messages[4] = {"frag1", "frag2", "frag3", "ping"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_TEXT, messages[0], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[1], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_PING, messages[3], is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[2], is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(ping_received_called, "Callback for ping messages was not called!");
	BOOST_CHECK_MESSAGE(is_pong_frame(messages[3]), "No pong frame sent when ping received!");
	BOOST_CHECK_MESSAGE(text_frame_received_called == 3, "Callback for text frames was not or wrong times called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(::strncmp("frag1frag2pingfrag3", (char *)readback_buffer, 19) == 0, "Did not received the same message as sent!: " << readback_buffer);
}

BOOST_AUTO_TEST_CASE(test_receive_text_frames_without_start_frame)
{
	const char *messages[3] = {"frag1", "frag2", "frag3"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[1], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	BOOST_CHECK_MESSAGE(text_frame_received_called == 0, "Callback for text frames was called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a message without start frame!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	BOOST_CHECK_MESSAGE(readback_buffer_length == 0, "Received a message!");
}

BOOST_AUTO_TEST_CASE(test_receive_text_frame_only_end_frame)
{
	const char *messages[3] = {"frag1", "frag2", "frag3"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_CONTINUATION, messages[2], is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	BOOST_CHECK_MESSAGE(text_frame_received_called == 0, "Callback for text frames was called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a message without start frame!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	BOOST_CHECK_MESSAGE(readback_buffer_length == 0, "Received a message!");
}

BOOST_AUTO_TEST_CASE(test_receive_text_frames_with_opcode)
{
	const char *messages[3] = {"frag1", "frag2", "frag3"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_TEXT, messages[0], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	prepare_message_string_frag(WS_OPCODE_TEXT, messages[1], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	BOOST_CHECK_MESSAGE(text_frame_received_called == 1, "Callback for text frames was called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a message without start frame!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	BOOST_CHECK_MESSAGE(::strncmp("frag1", (char *)readback_buffer, 5) == 0, "Did not received the same message as sent!: " << readback_buffer);
}

BOOST_AUTO_TEST_CASE(test_receive_fragmented_ping)
{
	const char *messages[3] = {"controll frames", "must not", "be fragmented"};
	bool is_server = true;
	F f(is_server, 5000);
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};

	prepare_message_string_frag(WS_OPCODE_PING, messages[0], is_server, mask, false);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);

	BOOST_CHECK_MESSAGE(text_frame_received_called == 0, "Callback for text frames was called: " << text_frame_received_called);
	BOOST_CHECK_MESSAGE(!ping_received_called, "Callback for ping frame was called!");
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a message without start frame!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	BOOST_CHECK_MESSAGE(readback_buffer_length == 0, "Received a message!");
}

BOOST_AUTO_TEST_CASE(test_receive_ping_frame_on_server)
{
	bool is_server = true;
	F f(is_server, 5000);

	char message[] = "Playing ping pong!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_PING, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(ping_received_called, "Callback for ping messages was not called!");
	BOOST_CHECK_MESSAGE(is_pong_frame(message), "No pong frame sent when ping received!");
}

BOOST_AUTO_TEST_CASE(test_recieve_to_large_ping_payload_on_server)
{
	bool is_server = true;
	F f(is_server, 5000);

	char message[127];
	::memset(message, 'A', sizeof(message));
	message[sizeof(message) - 1] = 0x00;

	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_PING, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	BOOST_CHECK_MESSAGE(!ping_received_called, "Callback for ping messages was not called!");
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a to large ping payload!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	BOOST_CHECK_MESSAGE(!is_pong_frame(message), "No pong frame sent when ping received!");
}

BOOST_AUTO_TEST_CASE(test_receive_ping_frame_on_client)
{
	bool is_server = false;
	F f(is_server, 5000);

	char message[] = "Playing ping pong!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_PING, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(ping_received_called, "Callback for ping messages was not called!");
	BOOST_CHECK_MESSAGE(is_pong_frame(message), "No pong frame sent when ping received!");
}

BOOST_AUTO_TEST_CASE(test_receive_pong_on_server)
{
	bool is_server = true;
	F f(is_server, 5000);

	char message[] = "Playing pong!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_PONG, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(pong_received_called, "Callback for pong messages was not called!");
	BOOST_CHECK_MESSAGE(::strncmp(message, (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
}

BOOST_AUTO_TEST_CASE(test_receive_pong_on_client)
{
	bool is_server = false;
	F f(is_server, 5000);

	char message[] = "Playing pong!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_PONG, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(pong_received_called, "Callback for pong messages was not called!");
	BOOST_CHECK_MESSAGE(::strncmp(message, (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
}

/*
 * tests if the connection is failed immediately, when any rsv-bit is set.
 * It doesn't matter if it is a text, binary or ping, because it is
 * shut down before the type is determined.
 */
BOOST_AUTO_TEST_CASE(test_receive_text_message_with_rsv_bit_set)
{
	for (uint8_t rsv = 0x10; rsv <= 0x70; rsv += 0x10){
		bool is_server = true;
		char message [] = "Hello World!";
		uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
		F f(is_server, 5000);
		const uint8_t type = WS_OPCODE_TEXT | rsv;
		prepare_message_string(type, message, is_server, mask);
		ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
		BOOST_CHECK_MESSAGE(!text_message_received_called, "Callback for message must not be called!");
		BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a text message with rsv set!");
		BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_PROTOCOL_ERROR), "No close frame sent after error!");
		BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
	}
}

BOOST_AUTO_TEST_CASE(test_receive_text_message_on_server_without_callback)
{
	bool is_server = true;
	F f(is_server, 5000);
	f.ws.text_message_received = NULL;

	char message[] = "Hello World!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_TEXT, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a text message without a callback registered!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_UNSUPPORTED), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
}

BOOST_AUTO_TEST_CASE(test_receive_binary_message_on_server_without_callback)
{
	bool is_server = true;
	F f(is_server, 5000);
	f.ws.binary_message_received = NULL;

	char message[] = "Hello World!";
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_BINARY, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	BOOST_CHECK_MESSAGE(got_error, "Did not got an error when receiving a binary message without a callback registered!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_UNSUPPORTED), "No close frame sent after error!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
}

BOOST_AUTO_TEST_CASE(test_receive_close_message_on_server)
{
	bool is_server = true;
	F f(is_server, 5000);
	uint16_t code = WS_CLOSE_GOING_AWAY;
	uint16_t code_be = htobe16(code);
	char message[3];
	memcpy(message, &code_be, sizeof(code_be));
	message[2] = '\0';

	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_CLOSE, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	BOOST_CHECK_MESSAGE(close_received_called, "Callback for close message was not called!");
	BOOST_CHECK_MESSAGE(status_code_received == code, "Incorrect status code received!");
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_NORMAL), "No close frame sent receiving a close!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
}

BOOST_AUTO_TEST_CASE(test_receive_medium_binary_message)
{
	bool is_server = true;
	F f(is_server, 5000);

	char message[127];
	::memset(message, 'A', sizeof(message));
	message[sizeof(message) - 1] = 0x00;
	
	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_BINARY, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(binary_message_received_called, "Callback for binary messages was not called!");
	BOOST_CHECK_MESSAGE(::strncmp(message, (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
}

BOOST_AUTO_TEST_CASE(test_receive_large_binary_message)
{
	bool is_server = true;
	F f(is_server, 70000);

	static const size_t buffer_size = 65537;
	char *message = (char *)::malloc(buffer_size);
	::memset(message, 'A', buffer_size);
	message[buffer_size - 1] = 0x00;

	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message_string(WS_OPCODE_BINARY, message, is_server, mask);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(binary_message_received_called, "Callback for binary messages was not called!");
	BOOST_CHECK_MESSAGE(::strncmp(message, (char *)readback_buffer, readback_buffer_length) == 0, "Did not received the same message as sent!");
	free(message);
}

BOOST_AUTO_TEST_CASE(test_sending_text_message)
{
	const char *messages[2] = {"Hello World!", ""};
	bool is_server[2] = {true, false};

	for (unsigned int j = 0; j < ARRAY_SIZE(is_server); j++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
			F f(is_server[j], 5000);
			char *message = (char *)::malloc(::strlen(messages[i]) + 1);
			::strcpy(message, messages[i]);
			websocket_send_text_frame(&f.ws, message, ::strlen(message));
			websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
			BOOST_CHECK_MESSAGE(check_frame(WS_OPCODE_TEXT, messages[i], ::strlen(messages[i])), "Not a valid text message sent!");
			free(message);
		}
	}
}

BOOST_AUTO_TEST_CASE(test_sending_binary_message)
{
	const char *messages[2] = {"Hello World!", ""};
	bool is_server[2] = {true, false};

	for (unsigned int j = 0; j < ARRAY_SIZE(is_server); j++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
			F f(is_server[j], 5000);
			char *message = (char *)::malloc(::strlen(messages[i]) + 1);
			::strcpy(message, messages[i]);
			websocket_send_binary_frame(&f.ws, (uint8_t *)message, ::strlen(message));
			websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
			BOOST_CHECK_MESSAGE(check_frame(WS_OPCODE_BINARY, messages[i], ::strlen(messages[i])), "Not a valid binary message sent!");
			free(message);
		}
	}
}

BOOST_AUTO_TEST_CASE(test_send_ping_message)
{
	const char *messages[2] = {"Hello World!", ""};
	bool is_server[2] = {true, false};

	for (unsigned int j = 0; j < ARRAY_SIZE(is_server); j++) {
		for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
			F f(is_server[j], 5000);
			char *message = (char *)::malloc(::strlen(messages[i]) + 1);
			::strcpy(message, messages[i]);
			websocket_send_ping_frame(&f.ws, (uint8_t *)message, ::strlen(message));
			websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
			BOOST_CHECK_MESSAGE(check_frame(WS_OPCODE_PING, messages[i], ::strlen(messages[i])), "Not a valid ping message sent!");
			free(message);
		}
	}
}

BOOST_AUTO_TEST_CASE(test_send_medium_binary_message)
{
	bool is_server = true;
	F f(is_server, 5000);

	char message[127];
	::memset(message, 'A', sizeof(message));
	message[sizeof(message) - 1] = 0x00;

	websocket_send_binary_frame(&f.ws, (uint8_t *)message, ::strlen(message));
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(check_frame(WS_OPCODE_BINARY, message, ::strlen(message)), "Not a valid medium size binary message sent!");
}

BOOST_AUTO_TEST_CASE(test_send_large_binary_message)
{
	bool is_server = true;
	F f(is_server, 5000);

	static const size_t buffer_size = 65537;
	char *message = (char *)::malloc(buffer_size);
	::memset(message, 'A', buffer_size);
	message[buffer_size - 1] = 0x00;

	websocket_send_binary_frame(&f.ws, (uint8_t *)message, ::strlen(message));
	websocket_close(&f.ws, WS_CLOSE_GOING_AWAY);
	BOOST_CHECK_MESSAGE(check_frame(WS_OPCODE_BINARY, message, ::strlen(message)), "Not a valid large size binary message sent!");
	free(message);
}

BOOST_AUTO_TEST_CASE(test_receive_fin_on_server)
{
	bool is_server = true;
	F f(is_server, 5000);

	uint8_t mask[4] = {0xaa, 0x55, 0xcc, 0x11};
	prepare_message(WS_OPCODE_CLOSE, NULL, 0, is_server, mask, true);
	ws_get_header(&f.ws, read_buffer_ptr++, read_buffer_length);
	BOOST_CHECK_MESSAGE(is_close_frame(WS_CLOSE_NORMAL), "No close frame sent after receiving a close frame!");
	BOOST_CHECK_MESSAGE(close_received_called, "Callback for close message was not called after receiving a close frame!");
	BOOST_CHECK_MESSAGE(br_close_called, "buffered_reader not closed after websocket close!");
}
