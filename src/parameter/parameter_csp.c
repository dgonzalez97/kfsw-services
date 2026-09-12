#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <csp/csp.h>
#include <csp/csp_id.h>

#include <mpack/mpack.h>
#include <param/param.h>
#include <param/param_list.h>
#include <param/param_queue.h>
#include <param/param_serializer.h>
#include <param/param_server.h>

#include <kfsw/comms/csp.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

/* Upstream's version 3 list wire structure; kept private to this adapter. */
#include "param_list.h"

#define KFSW_PARAM_PROTOCOL_VERSION 2
#define KFSW_PARAM_LIST_VERSION 3

/* v4: one indexed descriptor per CRC-protected request/reply. v3 remains
 * available to legacy clients, including their RDP handshake-only requests. */
#define KFSW_PARAM_LIST_INDEXED_VERSION 4U
#define KFSW_PARAM_LIST_REPLY_HEADER 10U
#define KFSW_PARAM_LIST_REQUEST_SIZE 7U
#define KFSW_PARAM_LIST_ITEM 0U
#define KFSW_PARAM_LIST_END 1U
#define KFSW_PARAM_LIST_CHANGED 2U
#define KFSW_PARAM_REMOTE_BATCH_MAX 16U
#define KFSW_PARAM_REPLY_MAX_PACKETS 64U
#define KFSW_PARAM_LIST_SOCKET_OPTIONS CSP_SO_CRC32REQ

static param_t local_parameters[KFSW_PARAM_MAX_DEFINITIONS];
static bool local_parameters_registered;
static uint32_t local_list_crc;

static size_t scalar_size(enum kfsw_param_type type);

static void parameter_changed_from_csp(const param_t *param, int offset)
{
	ARG_UNUSED(offset);

	kfsw_param_value_changed(param->id);
}

static param_type_e to_libparam_type(enum kfsw_param_type type)
{
	switch (type) {
	case KFSW_PARAM_U8:
		return PARAM_TYPE_UINT8;
	case KFSW_PARAM_U16:
		return PARAM_TYPE_UINT16;
	case KFSW_PARAM_U32:
		return PARAM_TYPE_UINT32;
	case KFSW_PARAM_U64:
		return PARAM_TYPE_UINT64;
	case KFSW_PARAM_I8:
		return PARAM_TYPE_INT8;
	case KFSW_PARAM_I16:
		return PARAM_TYPE_INT16;
	case KFSW_PARAM_I32:
		return PARAM_TYPE_INT32;
	case KFSW_PARAM_I64:
		return PARAM_TYPE_INT64;
	case KFSW_PARAM_X8:
		return PARAM_TYPE_XINT8;
	case KFSW_PARAM_X16:
		return PARAM_TYPE_XINT16;
	case KFSW_PARAM_X32:
		return PARAM_TYPE_XINT32;
	case KFSW_PARAM_X64:
		return PARAM_TYPE_XINT64;
	case KFSW_PARAM_FLOAT:
		return PARAM_TYPE_FLOAT;
	case KFSW_PARAM_DOUBLE:
		return PARAM_TYPE_DOUBLE;
	case KFSW_PARAM_STRING:
		return PARAM_TYPE_STRING;
	case KFSW_PARAM_DATA:
		return PARAM_TYPE_DATA;
	case KFSW_PARAM_INVALID:
	default:
		return PARAM_TYPE_INVALID;
	}
}

static int register_local_parameters(void)
{
	if (local_parameters_registered) {
		return 0;
	}

	memset(local_parameters, 0, sizeof(local_parameters));
	for (size_t index = kfsw_param_entry_count(); index > 0U; index--) {
		const struct kfsw_param_entry *entry = kfsw_param_entry_at(index - 1U);
		param_t *descriptor = &local_parameters[index - 1U];

		descriptor->node = (uint16_t *)&node_self;
		descriptor->id = entry->info.id;
		descriptor->type = to_libparam_type(entry->info.type);
		descriptor->name = (char *)entry->info.name;
		descriptor->array_size = entry->info.array_size;
		/* libparam's own width for the type, not the scalar table's: a
		 * string has no scalar width, and a step of zero makes the
		 * serializer walk nowhere and send an empty value.
		 */
		descriptor->array_step = param_typesize(descriptor->type);
		descriptor->mask = entry->info.flags;
		descriptor->unit = (char *)entry->info.unit;
		descriptor->callback = parameter_changed_from_csp;
		descriptor->addr = entry->definition->value;
		descriptor->docstr = (char *)entry->info.description;

		if ((descriptor->type == PARAM_TYPE_INVALID) || (param_list_add(descriptor) != 0)) {
			return -EEXIST;
		}
	}
	local_parameters_registered = true;
	return 0;
}

_Static_assert(sizeof(param_transfer3_t) + KFSW_PARAM_LIST_REPLY_HEADER <= CSP_BUFFER_SIZE,
	       "CSP buffers must fit an upstream parameter-list entry");

K_MUTEX_DEFINE(kfsw_param_remote_lock);

static bool server_started;
static K_MUTEX_DEFINE(server_start_lock);
static csp_socket_t list_socket;

static enum kfsw_param_type from_libparam_type(param_type_e type)
{
	switch (type) {
	case PARAM_TYPE_UINT8:
		return KFSW_PARAM_U8;
	case PARAM_TYPE_UINT16:
		return KFSW_PARAM_U16;
	case PARAM_TYPE_UINT32:
		return KFSW_PARAM_U32;
	case PARAM_TYPE_UINT64:
		return KFSW_PARAM_U64;
	case PARAM_TYPE_INT8:
		return KFSW_PARAM_I8;
	case PARAM_TYPE_INT16:
		return KFSW_PARAM_I16;
	case PARAM_TYPE_INT32:
		return KFSW_PARAM_I32;
	case PARAM_TYPE_INT64:
		return KFSW_PARAM_I64;
	case PARAM_TYPE_XINT8:
		return KFSW_PARAM_X8;
	case PARAM_TYPE_XINT16:
		return KFSW_PARAM_X16;
	case PARAM_TYPE_XINT32:
		return KFSW_PARAM_X32;
	case PARAM_TYPE_XINT64:
		return KFSW_PARAM_X64;
	case PARAM_TYPE_FLOAT:
		return KFSW_PARAM_FLOAT;
	case PARAM_TYPE_DOUBLE:
		return KFSW_PARAM_DOUBLE;
	case PARAM_TYPE_STRING:
		return KFSW_PARAM_STRING;
	case PARAM_TYPE_DATA:
		return KFSW_PARAM_DATA;
	case PARAM_TYPE_INVALID:
	default:
		return KFSW_PARAM_INVALID;
	}
}

static size_t scalar_size(enum kfsw_param_type type)
{
	switch (type) {
	case KFSW_PARAM_U8:
	case KFSW_PARAM_I8:
	case KFSW_PARAM_X8:
		return sizeof(uint8_t);
	case KFSW_PARAM_U16:
	case KFSW_PARAM_I16:
	case KFSW_PARAM_X16:
		return sizeof(uint16_t);
	case KFSW_PARAM_U32:
	case KFSW_PARAM_I32:
	case KFSW_PARAM_X32:
		return sizeof(uint32_t);
	case KFSW_PARAM_U64:
	case KFSW_PARAM_I64:
	case KFSW_PARAM_X64:
		return sizeof(uint64_t);
	case KFSW_PARAM_FLOAT:
		return sizeof(float);
	case KFSW_PARAM_DOUBLE:
		return sizeof(double);
	case KFSW_PARAM_STRING:
	case KFSW_PARAM_DATA:
	case KFSW_PARAM_INVALID:
	default:
		return 0U;
	}
}

static int validate_scalar(const param_t *param, const struct kfsw_param_value *value)
{
	enum kfsw_param_type type;
	size_t size;

	if ((param == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	type = from_libparam_type((param_type_e)param->type);
	if (type == KFSW_PARAM_DATA) {
		/* Written whole or not at all: a short write would leave some
		 * elements at their old values with no way to tell which. */
		if ((value->type != type) || (value->size != param->array_size)) {
			return -EMSGSIZE;
		}
		return 0;
	}
	if (type == KFSW_PARAM_STRING) {
		if (value->type != type) {
			return -EMSGSIZE;
		}
		/* size carries the terminator, so a value that exactly fills the
		 * remote capacity is accepted and one byte more is not. */
		if ((value->size == 0U) || (value->size > param->array_size)) {
			return -EMSGSIZE;
		}
		return 0;
	}

	size = scalar_size(type);
	if ((size == 0U) || (param->array_size != 1U)) {
		return -ENOTSUP;
	}
	if ((value->type != type) || (value->size != size)) {
		return -EMSGSIZE;
	}
	return 0;
}

static int read_scalar(const param_t *param, struct kfsw_param_value *value)
{
	enum kfsw_param_type type;
	size_t size;

	if ((param == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	type = from_libparam_type((param_type_e)param->type);

	if (type == KFSW_PARAM_DATA) {
		if ((param->array_size == 0U) || (param->array_size > sizeof(value->bytes))) {
			return -ENOTSUP;
		}
		memset(value, 0, sizeof(*value));
		value->type = type;
		param_get_data(param, value->bytes, (int)param->array_size);
		value->size = param->array_size;
		return 0;
	}

	if (type == KFSW_PARAM_STRING) {
		size_t length;

		if ((param->array_size == 0U) || (param->array_size > sizeof(value->text))) {
			return -ENOTSUP;
		}
		memset(value, 0, sizeof(*value));
		value->type = type;
		param_get_data(param, value->text, (int)param->array_size);
		/* The remote store may not have terminated it, so the copy is
		 * terminated here rather than trusted. */
		value->text[param->array_size - 1U] = '\0';
		length = 0U;
		while (value->text[length] != '\0') {
			length++;
		}
		value->size = length + 1U;
		return 0;
	}

	size = scalar_size(type);
	if ((size == 0U) || (param->array_size != 1U)) {
		return -ENOTSUP;
	}

	memset(value, 0, sizeof(*value));
	value->type = type;
	value->size = size;
	param_get(param, 0U, &value->scalar);
	return 0;
}

static void fill_info(const param_t *param, struct kfsw_param_info *info)
{
	info->node = *param->node;
	info->id = param->id;
	/* The wire identifier carries the table in its high byte and the offset
	 * in its low byte, so a remote parameter is addressable the same way a
	 * local one is. The table's name is not on the wire, which is why the
	 * listing shows the number for a remote node.
	 */
	info->table = (uint8_t)(param->id >> 8);
	info->offset = (uint8_t)(param->id & 0xFFU);
	info->table_name = NULL;
	info->array_size = param->array_size;
	info->type = from_libparam_type((param_type_e)param->type);
	info->flags = param->mask;
	info->name = param->name;
	info->unit = param->unit;
	info->description = param->docstr;
	info->read_only = (param->mask & PM_READONLY) != 0U;
}

static bool push_allowed(csp_packet_t *packet)
{
	param_queue_t queue;
	mpack_reader_t reader;
	size_t payload_length;
	int version;

	if (packet->length < 2U) {
		return false;
	}

	switch (packet->data[0]) {
	case PARAM_PULL_RESPONSE:
	case PARAM_PUSH_REQUEST:
		version = 1;
		break;
	case PARAM_PULL_RESPONSE_V2:
	case PARAM_PUSH_REQUEST_V2:
		version = 2;
		break;
	case PARAM_PUSH_REQUEST_V2_HWID:
		if (packet->length < (2U + sizeof(uint32_t))) {
			return false;
		}
		version = 2;
		break;
	default:
		return true;
	}

	payload_length = packet->length - 2U;
	if (packet->data[0] == PARAM_PUSH_REQUEST_V2_HWID) {
		payload_length -= sizeof(uint32_t);
	}
	param_queue_init(&queue, &packet->data[2], payload_length, payload_length,
			 PARAM_QUEUE_TYPE_SET, version);
	mpack_reader_init_data(&reader, queue.buffer, queue.used);
	while (reader.data < reader.end) {
		csp_timestamp_t timestamp = {0};
		const param_t *param;
		int offset = -1;
		int node = 0;
		int id = 0;

		param_deserialize_id(&reader, &id, &node, &timestamp, &offset, &queue);
		if (mpack_reader_error(&reader) != mpack_ok) {
			return false;
		}

		param = param_list_find_id(node, id);
		if ((param == NULL) ||
		    ((param->mask & (PM_READONLY | KFSW_PARAM_FLAG_LOCAL_ONLY)) != 0U) ||
		    ((offset >= 0) && (offset >= param->array_size))) {
			return false;
		}

		mpack_discard(&reader);
		if (mpack_reader_error(&reader) != mpack_ok) {
			return false;
		}
	}

	return true;
}

BUILD_ASSERT(CONFIG_KFSW_PARAM_VALUE_PRIORITY > CONFIG_KFSW_CSP_ROUTER_PRIORITY,
	     "PARAM value worker must run below the CSP router");
BUILD_ASSERT(CONFIG_KFSW_PARAM_VALUE_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
	     "PARAM value worker priority is outside the preemptible range");
BUILD_ASSERT(CONFIG_KFSW_PARAM_VALUE_QUEUE_DEPTH + 4U <= CSP_BUFFER_COUNT,
	     "Reserve CSP buffers for the worker, its reply, routing and list traffic");

K_MSGQ_DEFINE(value_requests, sizeof(csp_packet_t *), CONFIG_KFSW_PARAM_VALUE_QUEUE_DEPTH,
	      sizeof(void *));
static atomic_t value_requests_dropped;

uint32_t kfsw_param_csp_dropped_requests(void)
{
	return (uint32_t)atomic_get(&value_requests_dropped);
}

static void serve_values(csp_packet_t *packet)
{
	if (!kfsw_param_is_initialized()) {
		csp_buffer_free(packet);
		return;
	}

	kfsw_param_table_lock();
	if (push_allowed(packet)) {
		/* libparam serves from the backing storage without going through
		 * the read path, so anything sampled has to be refreshed first
		 * or the answer is the value the storage happened to hold. */
		kfsw_param_sample_all();
		param_serve(packet);
	} else {
		csp_buffer_free(packet);
	}
	kfsw_param_table_unlock();
}

static void param_server_callback(csp_packet_t *packet)
{
	if (!kfsw_param_is_initialized() ||
	    (k_msgq_put(&value_requests, &packet, K_NO_WAIT) != 0)) {
		atomic_inc(&value_requests_dropped);
		csp_buffer_free(packet);
	}
}

static void value_server(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		csp_packet_t *packet;

		k_msgq_get(&value_requests, &packet, K_FOREVER);
		serve_values(packet);
	}
}

K_THREAD_DEFINE(kfsw_param_value_thread, CONFIG_KFSW_PARAM_VALUE_STACK_SIZE, value_server, NULL,
		NULL, NULL, CONFIG_KFSW_PARAM_VALUE_PRIORITY, 0, SYS_FOREVER_MS);

static uint32_t remaining_ms(int64_t deadline)
{
	return (uint32_t)CLAMP(deadline - k_uptime_get(), 0, UINT32_MAX);
}

static const param_t *list_entry(uint16_t wanted, uint16_t *total)
{
	const param_t *found = NULL;

	*total = 0;
	for (size_t i = 0; i < kfsw_param_entry_count(); i++) {
		const param_t *param = &local_parameters[i];

		if ((param->mask & PM_HIDDEN) != 0U) {
			continue;
		}
		if ((*total)++ == wanted) {
			found = param;
		}
	}
	return found;
}

static size_t encode_descriptor(const param_t *param, uint8_t *data)
{
	param_transfer3_t *wire = (param_transfer3_t *)data;

	memset(wire, 0, sizeof(*wire));
	wire->id = htobe16(param->id);
	wire->type = param->type;
	wire->size = param->array_size;
	wire->mask = htobe32(param->mask);
	strncpy(wire->name, param->name, sizeof(wire->name) - 1U);
	if (param->vmem != NULL) {
		wire->storage_type = param->vmem->type;
	}
	if (param->unit != NULL) {
		strncpy(wire->unit, param->unit, sizeof(wire->unit) - 1U);
	}
	if (param->docstr != NULL) {
		strncpy(wire->help, param->docstr, sizeof(wire->help) - 1U);
	}
	return offsetof(param_transfer3_t, help) + strlen(wire->help) + 1U;
}

static uint32_t list_fingerprint(void)
{
	param_transfer3_t wire;
	uint32_t crc = 0;

	for (size_t i = 0; i < kfsw_param_entry_count(); i++) {
		if ((local_parameters[i].mask & PM_HIDDEN) == 0U) {
			size_t size = encode_descriptor(&local_parameters[i], (uint8_t *)&wire);

			crc = crc32_ieee_update(crc, (uint8_t *)&wire, size);
		}
	}
	return crc;
}

static void serve_list(csp_conn_t *connection)
{
	int64_t deadline = k_uptime_get() + CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS;
	uint16_t total;

	for (uint16_t index = 0;; index++) {
		const param_t *param = list_entry(index, &total);
		csp_packet_t *packet = NULL;

		if (param == NULL) {
			return;
		}
		/* Retry this descriptor; allocation failure must not advance the index. */
		while (remaining_ms(deadline) != 0U && packet == NULL) {
			packet = csp_buffer_get(CSP_BUFFER_SIZE);
			if (packet == NULL) {
				k_sleep(K_MSEC(5));
			}
		}
		if (packet == NULL) {
			return;
		}
		packet->length = encode_descriptor(param, packet->data);
		/* Metadata is fixed after registration. No PARAM lock across a send. */
		csp_send(connection, packet);
		k_yield();
	}
}

static void serve_indexed_list(csp_conn_t *connection, const csp_packet_t *request)
{
	uint16_t index = sys_get_be16(&request->data[1]);
	uint16_t total;
	uint32_t expected = sys_get_be32(&request->data[3]);
	const param_t *param = list_entry(index, &total);
	uint32_t crc = local_list_crc;
	csp_packet_t *reply = csp_buffer_get(CSP_BUFFER_SIZE);

	if (reply == NULL) {
		return;
	}
	reply->data[0] = KFSW_PARAM_LIST_INDEXED_VERSION;
	reply->data[1] = (index != 0U && expected != crc) || index > total
				 ? KFSW_PARAM_LIST_CHANGED
				 : (param == NULL ? KFSW_PARAM_LIST_END : KFSW_PARAM_LIST_ITEM);
	sys_put_be16(index, &reply->data[2]);
	sys_put_be16(total, &reply->data[4]);
	sys_put_be32(crc, &reply->data[6]);
	reply->length = KFSW_PARAM_LIST_REPLY_HEADER;
	if (reply->data[1] == KFSW_PARAM_LIST_ITEM) {
		reply->length +=
			encode_descriptor(param, &reply->data[KFSW_PARAM_LIST_REPLY_HEADER]);
	}
	csp_send(connection, reply);
}

static void list_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection = csp_accept(&list_socket, CSP_MAX_DELAY);

		if (connection == NULL) {
			continue;
		}
		if ((csp_conn_flags(connection) & CSP_FRDP) != 0 &&
		    !IS_ENABLED(CONFIG_KFSW_PARAM_LIST_RDP)) {
			csp_close(connection);
			continue;
		}
		csp_packet_t *request = csp_read(connection, 0);

		if (request != NULL && request->length == KFSW_PARAM_LIST_REQUEST_SIZE &&
		    request->data[0] == KFSW_PARAM_LIST_INDEXED_VERSION) {
			serve_indexed_list(connection, request);
		} else if (request == NULL ||
			   (request->length == 1U && request->data[0] == KFSW_PARAM_LIST_VERSION)) {
			serve_list(connection);
		}
		if (request != NULL) {
			csp_buffer_free(request);
		}
		csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_param_list_thread, CONFIG_KFSW_PARAM_LIST_STACK_SIZE, list_server, NULL, NULL,
		NULL, CONFIG_KFSW_PARAM_LIST_PRIORITY, 0, SYS_FOREVER_MS);

static int server_start(void)
{
	struct kfsw_csp_info csp_info;
	int result;

	if (!kfsw_param_is_initialized()) {
		return -EACCES;
	}
	if (server_started) {
		return 0;
	}
	if (CONFIG_KFSW_PARAM_PORT == CONFIG_KFSW_PARAM_LIST_PORT) {
		return -EINVAL;
	}

	kfsw_csp_get_info(&csp_info);
	if (!csp_info.initialized) {
		return -EACCES;
	}
	result = register_local_parameters();
	if (result != 0) {
		return result;
	}

	local_list_crc = list_fingerprint();
	memset(&list_socket, 0, sizeof(list_socket));
	list_socket.opts = KFSW_PARAM_LIST_SOCKET_OPTIONS;
	result = csp_listen(&list_socket, 4U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&list_socket);
		return -EIO;
	}
	result = csp_bind(&list_socket, CONFIG_KFSW_PARAM_LIST_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&list_socket);
		return -EADDRINUSE;
	}
	result = csp_bind_callback(param_server_callback, CONFIG_KFSW_PARAM_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&list_socket);
		return -EADDRINUSE;
	}

	k_thread_start(kfsw_param_list_thread);
	k_thread_start(kfsw_param_value_thread);
	server_started = true;
	return 0;
}

int kfsw_param_server_start(void)
{
	int result;

	k_mutex_lock(&server_start_lock, K_FOREVER);
	result = server_start();
	k_mutex_unlock(&server_start_lock);
	return result;
}

static int validate_remote_node(uint16_t node)
{
	struct kfsw_csp_info csp_info;
	const unsigned int host_bits = csp_id_get_host_bits();

	kfsw_csp_get_info(&csp_info);
	if (!kfsw_param_is_initialized() || !csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	if ((node == csp_info.address) || (node >= (1UL << host_bits))) {
		return -EINVAL;
	}
	return 0;
}

/* One remote node owns the upstream static pool at a time. Its destructor
 * resets the whole pool, so every descriptor is unlinked before reusing it. */
static uint16_t cached_node;
static bool cache_complete;
static const param_t *cached[CONFIG_KFSW_PARAM_REMOTE_POOL_SIZE];
static size_t cached_count;

static void clear_remote_cache(void)
{
	if (cache_complete) {
		for (size_t i = 0; i < cached_count; i++) {
			param_list_remove_specific(cached[i], 0, 0);
		}
	}
	if (cached_count != 0U) {
		param_list_destroy(cached[0]);
	}
	cached_count = 0;
	cache_complete = false;
	cached_node = 0;
}

static int validate_descriptor(const uint8_t *data, size_t size)
{
	const param_transfer3_t *wire = (const param_transfer3_t *)data;

	if (size < offsetof(param_transfer3_t, help) + 1U || size > sizeof(*wire) ||
	    wire->node != 0U || wire->size == 0U || wire->size == 255U ||
	    from_libparam_type(wire->type) == KFSW_PARAM_INVALID || wire->name[0] == '\0' ||
	    memchr(wire->name, '\0', sizeof(wire->name)) == NULL ||
	    strnlen(wire->name, sizeof(wire->name)) > KFSW_PARAM_NAME_MAX ||
	    memchr(wire->unit, '\0', sizeof(wire->unit)) == NULL || data[size - 1] != 0U) {
		return -EBADMSG;
	}
	return 0;
}

static int refresh_remote(uint16_t node, int64_t deadline, bool force)
{
	uint32_t crc = 0;
	uint32_t received_crc = 0;
	uint16_t total = 0;
	int result = validate_remote_node(node);

	if (result != 0 || node == 0U) {
		return result != 0 ? result : -EINVAL;
	}
	if (!force && cache_complete && cached_node == node) {
		return 0;
	}
	if (kfsw_param_table_lock_until(deadline) != 0) {
		return -ETIMEDOUT;
	}
	clear_remote_cache();
	cached_node = node;
	kfsw_param_table_unlock();

	for (uint16_t index = 0; index <= CONFIG_KFSW_PARAM_REMOTE_POOL_SIZE; index++) {
		uint32_t remaining = remaining_ms(deadline);
		csp_conn_t *connection;
		csp_packet_t *packet;

		if (remaining == 0U) {
			result = -ETIMEDOUT;
			break;
		}
		connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_PARAM_LIST_PORT, 0,
					 CSP_O_CRC32);
		if (connection == NULL) {
			result = -ECONNREFUSED;
			break;
		}
		packet = csp_buffer_get(KFSW_PARAM_LIST_REQUEST_SIZE);
		if (packet == NULL) {
			csp_close(connection);
			result = -ENOMEM;
			break;
		}
		packet->data[0] = KFSW_PARAM_LIST_INDEXED_VERSION;
		sys_put_be16(index, &packet->data[1]);
		sys_put_be32(crc, &packet->data[3]);
		packet->length = KFSW_PARAM_LIST_REQUEST_SIZE;
		csp_send(connection, packet);
		remaining = remaining_ms(deadline);
		packet = remaining == 0U ? NULL : csp_read(connection, remaining);
		csp_close(connection);
		if (packet == NULL) {
			result = -ETIMEDOUT;
			break;
		}
		result = -EBADMSG;
		if (packet->length < KFSW_PARAM_LIST_REPLY_HEADER ||
		    packet->data[0] != KFSW_PARAM_LIST_INDEXED_VERSION) {
			csp_buffer_free(packet);
			break;
		}
		uint16_t offered = sys_get_be16(&packet->data[4]);
		uint32_t fingerprint = sys_get_be32(&packet->data[6]);
		uint8_t status = packet->data[1];

		if (index == 0U) {
			total = offered;
			crc = fingerprint;
		}
		if (sys_get_be16(&packet->data[2]) != index || offered != total ||
		    fingerprint != crc || total > CONFIG_KFSW_PARAM_REMOTE_POOL_SIZE) {
			csp_buffer_free(packet);
			break;
		}
		if (status == KFSW_PARAM_LIST_END && index == total &&
		    packet->length == KFSW_PARAM_LIST_REPLY_HEADER && received_crc == crc) {
			csp_buffer_free(packet);
			if (kfsw_param_table_lock_until(deadline) != 0) {
				result = -ETIMEDOUT;
				break;
			}
			for (size_t i = 0; i < cached_count; i++) {
				/* Node and ID/name uniqueness were checked before allocation. */
				(void)param_list_add((param_t *)cached[i]);
			}
			cache_complete = true;
			kfsw_param_table_unlock();
			return 0;
		}
		uint8_t *data = &packet->data[KFSW_PARAM_LIST_REPLY_HEADER];
		size_t size = packet->length - KFSW_PARAM_LIST_REPLY_HEADER;

		if (status != KFSW_PARAM_LIST_ITEM || index >= total ||
		    validate_descriptor(data, size) != 0) {
			csp_buffer_free(packet);
			break;
		}
		received_crc = crc32_ieee_update(received_crc, data, size);
		param_transfer3_t *wire = (param_transfer3_t *)data;
		bool duplicate = false;

		for (size_t i = 0; i < cached_count; i++) {
			duplicate |= cached[i]->id == sys_get_be16(data) ||
				     strcmp(cached[i]->name, wire->name) == 0;
		}
		if (duplicate) {
			result = -EBADMSG;
		} else {
			memset(data + size, 0, sizeof(*wire) - size);
			const param_t *param = param_list_create_remote(
				sys_get_be16(data), node, wire->type,
				sys_get_be32((uint8_t *)&wire->mask) | PM_REMOTE, wire->size,
				wire->name, wire->unit, wire->help, wire->storage_type);

			result = param == NULL ? -ENOSPC : 0;
			if (param != NULL) {
				cached[cached_count++] = param;
			}
		}
		csp_buffer_free(packet);
		if (result != 0) {
			break;
		}
	}
	/* Staged descriptors were never linked into the shared table. */
	clear_remote_cache();
	return result == 0 ? -EBADMSG : result;
}

int kfsw_param_remote_refresh(uint16_t node)
{
	int64_t deadline = k_uptime_get() + CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS;
	int result = k_mutex_lock(&kfsw_param_remote_lock, K_MSEC(remaining_ms(deadline)));

	if (result != 0) {
		return -ETIMEDOUT;
	}
	result = refresh_remote(node, deadline, true);
	k_mutex_unlock(&kfsw_param_remote_lock);
	return result;
}

static int decode_value(mpack_reader_t *reader, const param_t *param,
			struct kfsw_param_value *value)
{
	memset(value, 0, sizeof(*value));
	value->type = from_libparam_type(param->type);
	value->size = scalar_size(value->type);
	if (value->size != 0U && param->array_size != 1U) {
		return -ENOTSUP;
	}
	switch (value->type) {
	case KFSW_PARAM_U8:
	case KFSW_PARAM_X8:
		value->scalar.u8 = mpack_expect_u8(reader);
		break;
	case KFSW_PARAM_U16:
	case KFSW_PARAM_X16:
		value->scalar.u16 = mpack_expect_u16(reader);
		break;
	case KFSW_PARAM_U32:
	case KFSW_PARAM_X32:
		value->scalar.u32 = mpack_expect_u32(reader);
		break;
	case KFSW_PARAM_U64:
	case KFSW_PARAM_X64:
		value->scalar.u64 = mpack_expect_u64(reader);
		break;
	case KFSW_PARAM_I8:
		value->scalar.i8 = mpack_expect_i8(reader);
		break;
	case KFSW_PARAM_I16:
		value->scalar.i16 = mpack_expect_i16(reader);
		break;
	case KFSW_PARAM_I32:
		value->scalar.i32 = mpack_expect_i32(reader);
		break;
	case KFSW_PARAM_I64:
		value->scalar.i64 = mpack_expect_i64(reader);
		break;
	case KFSW_PARAM_FLOAT:
		if (mpack_peek_tag(reader).type != mpack_type_float) {
			return -EBADMSG;
		}
		value->scalar.f32 = mpack_expect_float(reader);
		break;
	case KFSW_PARAM_DOUBLE:
		if (mpack_peek_tag(reader).type != mpack_type_double) {
			return -EBADMSG;
		}
		value->scalar.f64 = mpack_expect_double(reader);
		break;
	case KFSW_PARAM_STRING: {
		size_t size = mpack_expect_str(reader);

		if (size >= param->array_size || size >= sizeof(value->text)) {
			return -EBADMSG;
		}
		mpack_read_bytes(reader, value->text, size);
		mpack_done_str(reader);
		if (memchr(value->text, 0, size) != NULL) {
			return -EBADMSG;
		}
		value->size = size + 1;
		break;
	}
	case KFSW_PARAM_DATA: {
		size_t size = mpack_expect_bin(reader);

		if (size != param->array_size || size > sizeof(value->bytes)) {
			return -EBADMSG;
		}
		mpack_read_bytes(reader, (char *)value->bytes, size);
		mpack_done_bin(reader);
		value->size = size;
		break;
	}
	default:
		return -ENOTSUP;
	}
	return mpack_reader_error(reader) == mpack_ok ? 0 : -EBADMSG;
}

static void store_remote_value(const param_t *param, const struct kfsw_param_value *value)
{
	if (value->type == KFSW_PARAM_STRING) {
		param_set_string(param, value->text, value->size);
	} else if (value->type == KFSW_PARAM_DATA) {
		param_set_data(param, value->bytes, value->size);
	} else {
		param_set(param, 0, (void *)&value->scalar);
	}
}

static int decode_reply(uint16_t node, csp_packet_t *packet, const param_t *const *params,
			size_t count, struct kfsw_param_value *values, bool *seen)
{
	param_queue_t queue = {0};
	mpack_reader_t reader;

	param_queue_init(&queue, &packet->data[2], packet->length - 2, packet->length - 2,
			 PARAM_QUEUE_TYPE_SET, KFSW_PARAM_PROTOCOL_VERSION);
	mpack_reader_init_data(&reader, queue.buffer, queue.used);
	while (reader.data < reader.end) {
		struct kfsw_param_value value;
		csp_timestamp_t timestamp = {0};
		int offset = -1, id = 0, source = 0;
		/* Bit 10 is reserved. Check it before using the pinned ID decoder. */
		mpack_reader_t peek = reader;
		uint16_t header = be16toh(mpack_expect_u16(&peek));

		if (mpack_reader_error(&peek) != mpack_ok || (header & BIT(10)) != 0U ||
		    ((header & BIT(11)) != 0U && (header & BIT(13)) == 0U)) {
			return -EBADMSG;
		}
		param_deserialize_id(&reader, &id, &source, &timestamp, &offset, &queue);
		if (mpack_reader_error(&reader) != mpack_ok || offset > 0 || offset < -1 ||
		    (source != 0 && source != node)) {
			return -EBADMSG;
		}
		size_t index;

		for (index = 0; index < count && params[index]->id != id; index++) {
		}
		if (index == count || decode_value(&reader, params[index], &value) != 0) {
			return -EBADMSG;
		}
		for (; index < count; index++) {
			if (params[index]->id != id) {
				continue;
			}
			if (seen[index] && memcmp(&values[index], &value, sizeof(value)) != 0) {
				return -EBADMSG;
			}
			values[index] = value;
			seen[index] = true;
		}
	}
	return mpack_reader_error(&reader) == mpack_ok ? 0 : -EBADMSG;
}

static int pull_remote_batch(uint16_t node, const param_t *const *params, size_t count,
			     size_t *consumed, int64_t deadline)
{
	static struct kfsw_param_value staged[KFSW_PARAM_REMOTE_BATCH_MAX];
	bool seen[KFSW_PARAM_REMOTE_BATCH_MAX] = {0};
	bool end_seen = false;
	csp_conn_t *connection;
	csp_packet_t *packet;
	param_queue_t queue = {0};
	size_t added = 0;
	int result = -ETIMEDOUT;

	*consumed = 0;
	packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL) {
		return -ENOMEM;
	}
	packet->data[0] = PARAM_PULL_REQUEST_V2;
	packet->data[1] = 0;
	param_queue_init(&queue, &packet->data[2], PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_GET,
			 KFSW_PARAM_PROTOCOL_VERSION);
	while (added < count && added < ARRAY_SIZE(staged)) {
		if (param_queue_add(&queue, params[added], -1, NULL) != 0) {
			break;
		}
		added++;
	}
	if (added == 0 || remaining_ms(deadline) == 0) {
		csp_buffer_free(packet);
		return added == 0 ? -EMSGSIZE : -ETIMEDOUT;
	}
	packet->length = queue.used + 2;
	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_PARAM_PORT, 0, CSP_O_CRC32);
	if (connection == NULL) {
		csp_buffer_free(packet);
		return -ECONNREFUSED;
	}
	csp_send(connection, packet);
	for (size_t packets = 0; packets < KFSW_PARAM_REPLY_MAX_PACKETS; packets++) {
		uint32_t remaining = MIN(remaining_ms(deadline), CONFIG_KFSW_PARAM_TIMEOUT_MS);

		packet = remaining == 0 ? NULL : csp_read(connection, remaining);
		if (packet == NULL) {
			result = -ETIMEDOUT;
			break;
		}
		if (packet->length < 2U || packet->data[0] != PARAM_PULL_RESPONSE_V2 ||
		    (packet->data[1] & ~PARAM_FLAG_END) != 0U) {
			csp_buffer_free(packet);
			result = -EBADMSG;
			break;
		}
		result = decode_reply(node, packet, params, added, staged, seen);
		end_seen = (packet->data[1] & PARAM_FLAG_END) != 0U;
		csp_buffer_free(packet);
		if (result != 0 || end_seen) {
			break;
		}
	}
	csp_close(connection);
	if (result == 0 && !end_seen) {
		result = -ETIMEDOUT;
	}
	for (size_t i = 0; result == 0 && i < added; i++) {
		if (!seen[i]) {
			result = -EBADMSG;
		}
	}
	if (result != 0) {
		return result;
	}
	if (kfsw_param_table_lock_until(deadline) != 0) {
		return -ETIMEDOUT;
	}
	for (size_t i = 0; i < added; i++) {
		store_remote_value(params[i], &staged[i]);
	}
	kfsw_param_table_unlock();
	*consumed = added;
	return 0;
}

static int push_remote(const param_t *param, uint16_t node, const struct kfsw_param_value *value,
		       int64_t deadline)
{
	csp_conn_t *connection;
	csp_packet_t *packet;
	param_queue_t queue;
	int result = -ETIMEDOUT;

	packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL) {
		return -ENOMEM;
	}
	packet->data[0] = PARAM_PUSH_REQUEST_V2;
	packet->data[1] = 0U;
	param_queue_init(&queue, &packet->data[2], PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_SET,
			 KFSW_PARAM_PROTOCOL_VERSION);
	if (param_queue_add(&queue, param, -1,
			    (value->type == KFSW_PARAM_STRING || value->type == KFSW_PARAM_DATA)
				    ? (void *)value->bytes
				    : (void *)&value->scalar) != 0) {
		csp_buffer_free(packet);
		return -EMSGSIZE;
	}
	packet->length = queue.used + 2U;

	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_PARAM_PORT, 0, CSP_O_CRC32);
	if (connection == NULL) {
		csp_buffer_free(packet);
		return -ECONNREFUSED;
	}
	csp_send(connection, packet);
	uint32_t remaining = MIN(remaining_ms(deadline), CONFIG_KFSW_PARAM_TIMEOUT_MS);

	packet = remaining == 0 ? NULL : csp_read(connection, remaining);
	if (packet != NULL) {
		if ((packet->length >= 2U) && (packet->data[0] == PARAM_PUSH_RESPONSE) &&
		    ((packet->data[1] & PARAM_FLAG_END) != 0U)) {
			result = 0;
		} else {
			result = -EBADMSG;
		}
		csp_buffer_free(packet);
	}
	(void)csp_close(connection);
	return result;
}

/* All callers hold remote ownership until names, reads and callbacks finish. */
static int find_remote(uint16_t node, const char *name, const param_t **found, int64_t deadline)
{
	int result = refresh_remote(node, deadline, false);

	if (result != 0) {
		return result;
	}
	for (size_t i = 0; i < cached_count; i++) {
		if (strcmp(cached[i]->name, name) == 0) {
			*found = cached[i];
			return 0;
		}
	}
	return -ENOENT;
}

static int get_many(uint16_t node, const char *const *names, size_t count,
		    struct kfsw_param_value *values, int64_t deadline)
{
	size_t done = 0U;

	if ((names == NULL) || (values == NULL)) {
		return -EINVAL;
	}

	while (done < count) {
		const param_t *window[KFSW_PARAM_REMOTE_BATCH_MAX];
		size_t window_count = MIN(count - done, ARRAY_SIZE(window));
		size_t pulled = 0U;
		size_t index;
		int result = 0;

		/* Every name is resolved before anything is asked for, so a
		 * request that names something this node does not have fails
		 * without spending a round trip on the ones that are fine.
		 */
		for (index = 0U; index < window_count; index++) {
			result = names[done + index] == NULL
					 ? -EINVAL
					 : find_remote(node, names[done + index], &window[index],
						       deadline);
			if (result != 0) {
				return result;
			}
		}

		while (pulled < window_count) {
			size_t consumed = 0U;

			result = pull_remote_batch(node, &window[pulled], window_count - pulled,
						   &consumed, deadline);
			if (result != 0) {
				return result;
			}
			pulled += consumed;
		}

		if (kfsw_param_table_lock_until(deadline) != 0) {
			return -ETIMEDOUT;
		}
		for (index = 0U; index < window_count; index++) {
			result = read_scalar(window[index], &values[done + index]);
			if (result != 0) {
				break;
			}
		}
		kfsw_param_table_unlock();
		if (result != 0) {
			return result;
		}
		done += window_count;
	}
	return 0;
}

static int set_remote(uint16_t node, const char *name, const struct kfsw_param_value *value,
		      int64_t deadline)
{
	const param_t *param;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	result = find_remote(node, name, &param, deadline);
	if (result != 0) {
		return result;
	}

	if (kfsw_param_table_lock_until(deadline) != 0) {
		return -ETIMEDOUT;
	}
	if (param == NULL) {
		result = -ENOENT;
	} else if ((param->mask & PM_READONLY) != 0U) {
		result = -EACCES;
	} else {
		result = validate_scalar(param, value);
	}
	kfsw_param_table_unlock();
	if (result != 0) {
		return result;
	}

	result = push_remote(param, node, value, deadline);
	if (result == 0) {
		if (kfsw_param_table_lock_until(deadline) != 0) {
			return -ETIMEDOUT;
		}
		if (value->type == KFSW_PARAM_STRING) {
			param_set_string(param, value->text, (int)value->size);
		} else if (value->type == KFSW_PARAM_DATA) {
			param_set_data(param, value->bytes, (int)value->size);
		} else {
			param_set(param, 0U, (void *)&value->scalar);
		}
		kfsw_param_table_unlock();
	}
	return result;
}

static int visit_remote(uint16_t node, kfsw_param_visitor_t visitor, void *context,
			int64_t deadline)
{
	struct kfsw_param_info info;
	uint16_t previous_id = 0U;
	bool emitted = false;
	int result;

	if (visitor == NULL) {
		return -EINVAL;
	}
	result = refresh_remote(node, deadline, false);
	if (result != 0) {
		return result;
	}

	/* Emitted in ascending identifier order, which is table then offset.
	 * The cache iterates in registration order, so a listing taken straight
	 * from it comes out backwards and does not read as a table. Selecting
	 * the next smallest each pass keeps that ordering without a second copy
	 * of the descriptors: the tables are bounded and this is an operator
	 * command, so the extra passes cost nothing worth saving.
	 */

	for (;;) {
		if (remaining_ms(deadline) == 0) {
			return -ETIMEDOUT;
		}
		const param_t *next = NULL;

		for (size_t i = 0; i < cached_count; i++) {
			const param_t *param = cached[i];

			if (emitted && param->id <= previous_id) {
				continue;
			}
			if (next == NULL || param->id < next->id) {
				next = param;
			}
		}
		if (next == NULL) {
			break;
		}

		previous_id = next->id;
		emitted = true;
		fill_info(next, &info);
		if (!visitor(&info, context)) {
			break;
		}
	}

	return 0;
}

int kfsw_param_remote_get_many_until(uint16_t node, const char *const *names, size_t count,
				     struct kfsw_param_value *values, int64_t deadline)
{
	if (remaining_ms(deadline) == 0 ||
	    k_mutex_lock(&kfsw_param_remote_lock, K_MSEC(remaining_ms(deadline))) != 0) {
		return -ETIMEDOUT;
	}
	int result = get_many(node, names, count, values, deadline);

	k_mutex_unlock(&kfsw_param_remote_lock);
	return result;
}

int kfsw_param_remote_get_many(uint16_t node, const char *const *names, size_t count,
			       struct kfsw_param_value *values)
{
	return kfsw_param_remote_get_many_until(node, names, count, values,
						k_uptime_get() + CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS +
							CONFIG_KFSW_PARAM_TIMEOUT_MS);
}

int kfsw_param_remote_get(uint16_t node, const char *name, struct kfsw_param_value *value)
{
	return kfsw_param_remote_get_many(node, &name, 1, value);
}

int kfsw_param_remote_set(uint16_t node, const char *name, const struct kfsw_param_value *value)
{
	int64_t deadline =
		k_uptime_get() + CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS + CONFIG_KFSW_PARAM_TIMEOUT_MS;
	if (k_mutex_lock(&kfsw_param_remote_lock, K_MSEC(remaining_ms(deadline))) != 0) {
		return -ETIMEDOUT;
	}
	int result = set_remote(node, name, value, deadline);

	k_mutex_unlock(&kfsw_param_remote_lock);
	return result;
}

int kfsw_param_remote_visit_until(uint16_t node, kfsw_param_visitor_t visitor, void *context,
				  int64_t deadline)
{
	if (remaining_ms(deadline) == 0 ||
	    k_mutex_lock(&kfsw_param_remote_lock, K_MSEC(remaining_ms(deadline))) != 0) {
		return -ETIMEDOUT;
	}
	int result = visit_remote(node, visitor, context, deadline);

	k_mutex_unlock(&kfsw_param_remote_lock);
	return result;
}

int kfsw_param_remote_visit(uint16_t node, kfsw_param_visitor_t visitor, void *context)
{
	return kfsw_param_remote_visit_until(node, visitor, context,
					     k_uptime_get() + CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS);
}
