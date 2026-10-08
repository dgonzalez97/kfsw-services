#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <csp/csp.h>

#include <kfsw/comms/csp.h>
#include <kfsw/services/event.h>
#include <kfsw/services/log_history.h>
#include <kfsw/services/log_remote.h>

/*
 * Wire format, version 2. Every message starts with the version, a type and
 * the requester's nonce, so a stray reply from an older read is ignored.
 *
 *   request  version, stream, min level, count u16, nonce[8]
 *   start    header, format, first u64, end u64, overwritten u64
 *   message  header, sequence u64, uptime ms u64, module, severity, flags, length, data
 *   event    header, sequence u64, boot u64, utc u64, utc valid, uptime us u64,
 *            source u16, id u16, severity, length, payload
 *   end      header, status, records sent u16
 *
 * Integers are big-endian. A message carries text, or with FLAG_PACKAGE the
 * record's cbprintf package followed by an IEEE CRC32 of the node-rendered
 * text (without NUL). FLAG_TEXT_CRC marks the four-byte trailer; length still
 * counts only package bytes, so older readers reject it rather than misdecode.
 */
#define WIRE_VERSION 2U
#define TYPE_START 0U
#define TYPE_MESSAGE 1U
#define TYPE_END 2U
#define TYPE_EVENT 3U
#define NONCE_SIZE 8U
#define REQUEST_SIZE (5U + NONCE_SIZE)
#define HEADER_SIZE (2U + NONCE_SIZE)
#define START_SIZE (HEADER_SIZE + 25U)
#define MESSAGE_HEADER_SIZE (HEADER_SIZE + 20U)
#define EVENT_HEADER_SIZE (HEADER_SIZE + 39U)
#define END_SIZE (HEADER_SIZE + 3U)
#define FLAG_TRUNCATED BIT(0)
#define FLAG_PACKAGE BIT(1)
#define FLAG_TEXT_CRC BIT(2)
#define TEXT_CRC_SIZE 4U
/* A round number under what the encrypted link takes, leaving some margin. */
#define WIRE_DATA_MAX 190U

#define STATUS_COMPLETE 0U
#define STATUS_CHANGED 1U
#define STATUS_NO_BUFFER 2U
#define STATUS_NOT_SERVED 3U

BUILD_ASSERT(MESSAGE_HEADER_SIZE + WIRE_DATA_MAX <= KFSW_CSP_PAYLOAD_MAX,
	     "a log record must fit the tightest transport, not just a CSP buffer");
BUILD_ASSERT(KFSW_LOG_ENCODED_SIZE + TEXT_CRC_SIZE <= WIRE_DATA_MAX, "a package travels whole");
BUILD_ASSERT(EVENT_HEADER_SIZE + KFSW_EVENT_MAX_PAYLOAD_SIZE <= KFSW_CSP_PAYLOAD_MAX,
	     "a journal record must fit the tightest transport");

static csp_socket_t log_socket;
static bool running;
static K_MUTEX_DEFINE(start_lock);

static csp_packet_t *new_reply(uint8_t type, const uint8_t *nonce, size_t size)
{
	csp_packet_t *packet = csp_buffer_get(size);

	if (packet != NULL) {
		packet->data[0] = WIRE_VERSION;
		packet->data[1] = type;
		memcpy(&packet->data[2], nonce, NONCE_SIZE);
		packet->length = size;
	}
	return packet;
}

static bool send_start(csp_conn_t *connection, const uint8_t *nonce,
		       const struct kfsw_log_remote_start *start)
{
	csp_packet_t *reply = new_reply(TYPE_START, nonce, START_SIZE);

	if (reply == NULL) {
		return false;
	}
	reply->data[10] = start->format;
	sys_put_be64(start->first, &reply->data[11]);
	sys_put_be64(start->end, &reply->data[19]);
	sys_put_be64(start->overwritten, &reply->data[27]);
	csp_send(connection, reply);
	return true;
}

static void send_end(csp_conn_t *connection, const uint8_t *nonce, uint8_t status, uint16_t sent)
{
	csp_packet_t *reply = new_reply(TYPE_END, nonce, END_SIZE);

	if (reply != NULL) {
		reply->data[10] = status;
		sys_put_be16(sent, &reply->data[11]);
		csp_send(connection, reply);
	}
}

static bool send_message(csp_conn_t *connection, const uint8_t *nonce, uint8_t format,
			 const struct kfsw_log_encoded *encoded)
{
	struct kfsw_log_record record;
	const uint8_t *data = encoded->data;
	size_t length = encoded->size;
	uint8_t flags = encoded->truncated ? FLAG_TRUNCATED : 0U;
	csp_packet_t *reply;

	if ((format == KFSW_LOG_REMOTE_DICTIONARY) && encoded->package) {
		kfsw_log_history_format(encoded, &record);
		flags = (record.truncated ? FLAG_TRUNCATED : 0U) | FLAG_PACKAGE | FLAG_TEXT_CRC;
	} else {
		kfsw_log_history_format(encoded, &record);
		length = strlen(record.text);
		if (length > WIRE_DATA_MAX) {
			length = WIRE_DATA_MAX;
			record.truncated = true;
		}
		flags = record.truncated ? FLAG_TRUNCATED : 0U;
		data = (const uint8_t *)record.text;
	}
	reply = new_reply(TYPE_MESSAGE, nonce,
			  MESSAGE_HEADER_SIZE + length +
				  ((flags & FLAG_TEXT_CRC) ? TEXT_CRC_SIZE : 0U));
	if (reply == NULL) {
		return false;
	}
	sys_put_be64(encoded->sequence, &reply->data[10]);
	sys_put_be64(encoded->uptime_ms, &reply->data[18]);
	reply->data[26] = encoded->module;
	reply->data[27] = encoded->severity;
	reply->data[28] = flags;
	reply->data[29] = (uint8_t)length;
	memcpy(&reply->data[30], data, length);
	if ((flags & FLAG_TEXT_CRC) != 0U) {
		sys_put_be32(crc32_ieee((const uint8_t *)record.text, strlen(record.text)),
			     &reply->data[MESSAGE_HEADER_SIZE + length]);
	}
	csp_send(connection, reply);
	return true;
}

static void serve_log(csp_conn_t *connection, const uint8_t *nonce, uint8_t minimum, uint16_t count)
{
	struct kfsw_log_history_window window;
	struct kfsw_log_encoded encoded;
	struct kfsw_log_remote_start start = {.format = kfsw_log_remote_format()};
	uint8_t status = STATUS_COMPLETE;
	uint16_t sent = 0U;

	if (kfsw_log_history_window(count, &window) != 0) {
		return;
	}
	start.first = window.first;
	start.end = window.end;
	start.overwritten = window.overwritten;
	if (!send_start(connection, nonce, &start)) {
		return;
	}
	for (uint64_t sequence = window.first; sequence < window.end; sequence++) {
		if (kfsw_log_history_get_encoded(sequence, &encoded) != 0) {
			status = STATUS_CHANGED;
			break;
		}
		if (encoded.severity < minimum) {
			continue;
		}
		if (!send_message(connection, nonce, start.format, &encoded)) {
			status = STATUS_NO_BUFFER;
			break;
		}
		sent++;
	}
	send_end(connection, nonce, status, sent);
}

#if CONFIG_KFSW_JOURNAL
static bool send_event(csp_conn_t *connection, const uint8_t *nonce,
		       const struct kfsw_journal_record *record)
{
	const struct kfsw_event_record *event = &record->event;
	const size_t length = MIN(event->payload_size, KFSW_EVENT_MAX_PAYLOAD_SIZE);
	csp_packet_t *reply = new_reply(TYPE_EVENT, nonce, EVENT_HEADER_SIZE + length);

	if (reply == NULL) {
		return false;
	}
	sys_put_be64(record->sequence, &reply->data[10]);
	sys_put_be64(record->boot, &reply->data[18]);
	sys_put_be64((uint64_t)record->utc_seconds, &reply->data[26]);
	reply->data[34] = record->utc_valid ? 1U : 0U;
	sys_put_be64(event->monotonic_us, &reply->data[35]);
	sys_put_be16(event->source, &reply->data[43]);
	sys_put_be16(event->id, &reply->data[45]);
	reply->data[47] = event->severity;
	reply->data[48] = (uint8_t)length;
	memcpy(&reply->data[49], event->payload, length);
	csp_send(connection, reply);
	return true;
}
#endif

static void serve_journal(csp_conn_t *connection, const uint8_t *nonce, uint16_t count)
{
	const struct kfsw_log_remote_start start = {.format = KFSW_LOG_REMOTE_TEXT};

	if (!send_start(connection, nonce, &start)) {
		return;
	}
#if CONFIG_KFSW_JOURNAL
	struct kfsw_journal_stats stats;
	struct kfsw_journal_record record;
	uint8_t status = STATUS_COMPLETE;
	uint16_t sent = 0U;
	uint16_t age;

	kfsw_journal_get_stats(&stats);
	/* Oldest first, the order the log uses. */
	for (age = MIN(count, stats.held); age > 0U; age--) {
		if (kfsw_journal_get(age - 1U, &record) != 0) {
			status = STATUS_CHANGED;
			break;
		}
		if (!send_event(connection, nonce, &record)) {
			status = STATUS_NO_BUFFER;
			break;
		}
		sent++;
	}
	send_end(connection, nonce, status, sent);
#else
	ARG_UNUSED(count);
	send_end(connection, nonce, STATUS_NOT_SERVED, 0U);
#endif
}

static void serve(csp_conn_t *connection, csp_packet_t *request)
{
	uint8_t nonce[NONCE_SIZE];
	uint8_t stream;
	uint8_t minimum;
	uint16_t count;

	/* A malformed request gets no reply; the requester times out. */
	if ((request->length != REQUEST_SIZE) || (request->data[0] != WIRE_VERSION) ||
	    (request->data[1] > KFSW_LOG_REMOTE_JOURNAL) || (request->data[2] > 3U)) {
		csp_buffer_free(request);
		return;
	}
	stream = request->data[1];
	minimum = request->data[2];
	count = sys_get_be16(&request->data[3]);
	memcpy(nonce, &request->data[5], sizeof(nonce));
	csp_buffer_free(request);
	if ((count == 0U) || (count > KFSW_LOG_HISTORY_MAX_READ)) {
		return;
	}
	if (stream == KFSW_LOG_REMOTE_JOURNAL) {
		serve_journal(connection, nonce, count);
	} else {
		serve_log(connection, nonce, minimum, count);
	}
}

static void log_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection = csp_accept(&log_socket, 100U);
		csp_packet_t *request;

		if (connection == NULL) {
			continue;
		}
		request = csp_read(connection, 0U);
		if (request != NULL) {
			serve(connection, request);
		}
		(void)csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_log_remote_thread, CONFIG_KFSW_LOG_REMOTE_STACK_SIZE, log_server, NULL, NULL,
		NULL, 8, 0, SYS_FOREVER_MS);

int kfsw_log_remote_server_start(void)
{
	struct kfsw_csp_info info;
	int result = 0;

	k_mutex_lock(&start_lock, K_FOREVER);
	if (running) {
		goto done;
	}
	kfsw_csp_get_info(&info);
	if (!info.initialized || !info.router_running) {
		result = -ENETDOWN;
		goto done;
	}
	memset(&log_socket, 0, sizeof(log_socket));
	log_socket.opts = CSP_SO_CRC32REQ;
	if (csp_listen(&log_socket, 1U) != CSP_ERR_NONE) {
		(void)csp_socket_close(&log_socket);
		result = -EIO;
		goto done;
	}
	if (csp_bind(&log_socket, CONFIG_KFSW_LOG_REMOTE_PORT) != CSP_ERR_NONE) {
		(void)csp_socket_close(&log_socket);
		result = -EADDRINUSE;
		goto done;
	}
	running = true;
	k_thread_start(kfsw_log_remote_thread);
done:
	k_mutex_unlock(&start_lock);
	return result;
}

static bool parse_start(const csp_packet_t *packet, struct kfsw_log_remote_start *start)
{
	if (packet->length != START_SIZE) {
		return false;
	}
	start->format = packet->data[10];
	start->first = sys_get_be64(&packet->data[11]);
	start->end = sys_get_be64(&packet->data[19]);
	start->overwritten = sys_get_be64(&packet->data[27]);
	return true;
}

static bool parse_message(const csp_packet_t *packet, struct kfsw_log_remote_message *message)
{
	const size_t length = (packet->length >= MESSAGE_HEADER_SIZE) ? packet->data[29] : 0U;
	const bool text_crc = (packet->length >= MESSAGE_HEADER_SIZE) &&
			      ((packet->data[28] & FLAG_TEXT_CRC) != 0U);

	if ((packet->length < MESSAGE_HEADER_SIZE) ||
	    (packet->length != MESSAGE_HEADER_SIZE + length + (text_crc ? TEXT_CRC_SIZE : 0U)) ||
	    (length + (text_crc ? TEXT_CRC_SIZE : 0U) > WIRE_DATA_MAX) ||
	    (text_crc && ((packet->data[28] & FLAG_PACKAGE) == 0U))) {
		return false;
	}
	*message = (struct kfsw_log_remote_message){
		.sequence = sys_get_be64(&packet->data[10]),
		.uptime_ms = sys_get_be64(&packet->data[18]),
		.module = packet->data[26],
		.severity = packet->data[27],
		.truncated = (packet->data[28] & FLAG_TRUNCATED) != 0U,
		.package = (packet->data[28] & FLAG_PACKAGE) != 0U,
		.size = (uint8_t)length,
		.text_crc_present = text_crc,
	};
	memcpy(message->data, &packet->data[30], length);
	if (text_crc) {
		message->text_crc = sys_get_be32(&packet->data[MESSAGE_HEADER_SIZE + length]);
	}
	/* Text is terminated for the caller; a package is used by its size. */
	if (!message->package) {
		message->data[length] = '\0';
	}
	return true;
}

static bool parse_event(const csp_packet_t *packet, struct kfsw_journal_record *record)
{
	const size_t length = (packet->length > EVENT_HEADER_SIZE) ? packet->data[48] : 0U;

	if ((packet->length < EVENT_HEADER_SIZE) ||
	    (packet->length != EVENT_HEADER_SIZE + length) ||
	    (length > KFSW_EVENT_MAX_PAYLOAD_SIZE)) {
		return false;
	}
	*record = (struct kfsw_journal_record){
		.sequence = sys_get_be64(&packet->data[10]),
		.boot = sys_get_be64(&packet->data[18]),
		.utc_seconds = (int64_t)sys_get_be64(&packet->data[26]),
		.utc_valid = packet->data[34] != 0U,
		.event =
			{
				.monotonic_us = sys_get_be64(&packet->data[35]),
				.source = sys_get_be16(&packet->data[43]),
				.id = sys_get_be16(&packet->data[45]),
				.severity = packet->data[47],
				.payload_size = (uint8_t)length,
			},
	};
	memcpy(record->event.payload, &packet->data[49], length);
	return true;
}

/* 0 to keep reading, 1 when the read is over, or a negative errno. */
static int handle_reply(const csp_packet_t *packet, const struct kfsw_log_remote_visitor *visitor,
			void *context)
{
	struct kfsw_log_remote_start start;
	struct kfsw_log_remote_message message;
	struct kfsw_journal_record record;

	switch (packet->data[1]) {
	case TYPE_START:
		if (!parse_start(packet, &start)) {
			return -EBADMSG;
		}
		if (visitor->start != NULL) {
			visitor->start(&start, context);
		}
		return 0;
	case TYPE_MESSAGE:
		if (!parse_message(packet, &message)) {
			return -EBADMSG;
		}
		return ((visitor->message == NULL) || visitor->message(&message, context)) ? 0 : 1;
	case TYPE_EVENT:
		if (!parse_event(packet, &record)) {
			return -EBADMSG;
		}
		return ((visitor->event == NULL) || visitor->event(&record, context)) ? 0 : 1;
	case TYPE_END:
		if (packet->length != END_SIZE) {
			return -EBADMSG;
		}
		if (packet->data[10] == STATUS_COMPLETE) {
			return 1;
		}
		return (packet->data[10] == STATUS_NOT_SERVED) ? -ENOTSUP : -EIO;
	default:
		return -EBADMSG;
	}
}

int kfsw_log_remote_read(uint16_t node, enum kfsw_log_remote_stream stream, uint16_t count,
			 uint8_t min_level, const struct kfsw_log_remote_visitor *visitor,
			 void *context)
{
	uint8_t nonce[NONCE_SIZE];
	csp_packet_t *request;
	csp_conn_t *connection;
	bool answered = false;
	int result = 0;

	if ((visitor == NULL) || (node == 0U) || (node >= KFSW_CSP_BROADCAST_ADDRESS) ||
	    (stream > KFSW_LOG_REMOTE_JOURNAL) || (count == 0U) ||
	    (count > KFSW_LOG_HISTORY_MAX_READ) || (min_level > 3U)) {
		return -EINVAL;
	}
	sys_put_be64(k_cycle_get_64() ^ ((uint64_t)node << 48), nonce);
	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_LOG_REMOTE_PORT,
				 CONFIG_KFSW_LOG_REMOTE_TIMEOUT_MS, CSP_O_CRC32);
	if (connection == NULL) {
		return -ENOTCONN;
	}
	request = csp_buffer_get(REQUEST_SIZE);
	if (request == NULL) {
		(void)csp_close(connection);
		return -ENOBUFS;
	}
	request->data[0] = WIRE_VERSION;
	request->data[1] = (uint8_t)stream;
	request->data[2] = min_level;
	sys_put_be16(count, &request->data[3]);
	memcpy(&request->data[5], nonce, sizeof(nonce));
	request->length = REQUEST_SIZE;
	csp_send(connection, request);

	while (result == 0) {
		csp_packet_t *reply = csp_read(connection, CONFIG_KFSW_LOG_REMOTE_TIMEOUT_MS);

		if (reply == NULL) {
			result = answered ? -EIO : -ETIMEDOUT;
			break;
		}
		/* A reply to another read, or from another version, is not ours. */
		if ((reply->length >= HEADER_SIZE) && (reply->data[0] == WIRE_VERSION) &&
		    (memcmp(&reply->data[2], nonce, sizeof(nonce)) == 0)) {
			answered = true;
			result = handle_reply(reply, visitor, context);
		}
		csp_buffer_free(reply);
	}
	(void)csp_close(connection);
	return (result == 1) ? 0 : result;
}
