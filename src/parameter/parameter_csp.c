#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <csp/csp.h>
#include <csp/csp_id.h>

#include <mpack/mpack.h>
#include <param/param.h>
#include <param/param_list.h>
#include <param/param_queue.h>
#include <param/param_serializer.h>
#include <param/param_server.h>

#include <kfsw/comms/csp.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

/* Upstream's version 3 list wire structure; kept private to this adapter. */
#include "param_list.h"

#define KFSW_PARAM_PROTOCOL_VERSION 2
#define KFSW_PARAM_LIST_VERSION 3

static param_t local_parameters[KFSW_PARAM_MAX_DEFINITIONS];
static bool local_parameters_registered;

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
		descriptor->array_step = scalar_size(entry->info.type);
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

_Static_assert(sizeof(param_transfer3_t) <= CSP_BUFFER_SIZE,
	       "CSP buffers must fit an upstream parameter-list entry");

K_MUTEX_DEFINE(kfsw_param_remote_lock);

static bool server_started;
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
		if ((param == NULL) || ((param->mask & PM_READONLY) != 0U) ||
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

static void param_server_callback(csp_packet_t *packet)
{
	if (!kfsw_param_is_initialized()) {
		csp_buffer_free(packet);
		return;
	}

	kfsw_param_table_lock();
	if (push_allowed(packet)) {
		param_serve(packet);
	} else {
		csp_buffer_free(packet);
	}
	kfsw_param_table_unlock();
}

static void serve_list(csp_conn_t *connection)
{
	param_list_iterator iterator = {0};
	const param_t *param;

	kfsw_param_table_lock();
	while ((param = param_list_iterate(&iterator)) != NULL) {
		param_transfer3_t *wire;
		csp_packet_t *packet;
		size_t help_length = 0U;

		if ((*param->node != 0U) || ((param->mask & PM_HIDDEN) != 0U)) {
			continue;
		}

		packet = csp_buffer_get(CSP_BUFFER_SIZE);
		if (packet == NULL) {
			break;
		}
		memset(packet->data, 0, CSP_BUFFER_SIZE);
		wire = (param_transfer3_t *)packet->data;
		wire->id = htobe16(param->id);
		wire->node = 0U;
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
			help_length = strnlen(param->docstr, sizeof(wire->help) - 1U);
		}

		packet->length = offsetof(param_transfer3_t, help) + help_length + 1U;
		csp_send(connection, packet);
	}
	kfsw_param_table_unlock();
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
		serve_list(connection);
		csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_param_list_thread, CONFIG_KFSW_PARAM_LIST_STACK_SIZE, list_server, NULL, NULL,
		NULL, CONFIG_KFSW_PARAM_LIST_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_param_server_start(void)
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

	result = csp_bind(&list_socket, CONFIG_KFSW_PARAM_LIST_PORT);
	if (result != CSP_ERR_NONE) {
		return -EADDRINUSE;
	}
	result = csp_listen(&list_socket, 4U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&list_socket);
		return -EIO;
	}
	result = csp_bind_callback(param_server_callback, CONFIG_KFSW_PARAM_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&list_socket);
		return -EADDRINUSE;
	}

	k_thread_start(kfsw_param_list_thread);
	server_started = true;
	return 0;
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

static bool node_is_cached(uint16_t node)
{
	param_list_iterator iterator = {0};
	const param_t *param;

	while ((param = param_list_iterate(&iterator)) != NULL) {
		if (*param->node == node) {
			return true;
		}
	}
	return false;
}

int kfsw_param_remote_refresh(uint16_t node)
{
	csp_conn_t *connection;
	csp_packet_t *packet;
	int downloaded = 0;
	int result;

	result = validate_remote_node(node);
	if (result != 0) {
		return result;
	}

	k_mutex_lock(&kfsw_param_remote_lock, K_FOREVER);
	kfsw_param_table_lock();
	if (node_is_cached(node)) {
		kfsw_param_table_unlock();
		k_mutex_unlock(&kfsw_param_remote_lock);
		return 0;
	}
	kfsw_param_table_unlock();

	connection = csp_connect(CSP_PRIO_HIGH, node, CONFIG_KFSW_PARAM_LIST_PORT, 0, CSP_O_CRC32);
	if (connection == NULL) {
		k_mutex_unlock(&kfsw_param_remote_lock);
		return -ECONNREFUSED;
	}

	/*
	 * Upstream's list client uses an RDP handshake to make the server accept
	 * the connection. K-FSW keeps libcsp RDP disabled, so carry the requested
	 * upstream list version in a small connectionless trigger instead.
	 */
	packet = csp_buffer_get(1U);
	if (packet == NULL) {
		(void)csp_close(connection);
		k_mutex_unlock(&kfsw_param_remote_lock);
		return -ENOMEM;
	}
	packet->data[0] = KFSW_PARAM_LIST_VERSION;
	packet->length = 1U;
	csp_send(connection, packet);

	while ((packet = csp_read(connection, CONFIG_KFSW_PARAM_TIMEOUT_MS)) != NULL) {
		int unpack_result;
		const size_t minimum_length = offsetof(param_transfer3_t, help) + 1U;

		if ((packet->length < minimum_length) ||
		    (packet->length > sizeof(param_transfer3_t))) {
			csp_buffer_free(packet);
			result = -EBADMSG;
			goto close_connection;
		}

		kfsw_param_table_lock();
		unpack_result = param_list_unpack(node, packet->data, packet->length,
						  KFSW_PARAM_LIST_VERSION, 0);
		kfsw_param_table_unlock();
		csp_buffer_free(packet);
		if (unpack_result < 0) {
			result = -ENOSPC;
			goto close_connection;
		}
		if (unpack_result == 0) {
			downloaded++;
		}
	}
	result = (downloaded > 0) ? 0 : -ETIMEDOUT;

close_connection:
	(void)csp_close(connection);
	k_mutex_unlock(&kfsw_param_remote_lock);
	return result;
}

static int pull_remote(const param_t *param, uint16_t node)
{
	csp_conn_t *connection;
	csp_packet_t *packet;
	param_queue_t queue;
	int result = -ETIMEDOUT;

	packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL) {
		return -ENOMEM;
	}
	packet->data[0] = PARAM_PULL_REQUEST_V2;
	packet->data[1] = 0U;
	param_queue_init(&queue, &packet->data[2], PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_GET,
			 KFSW_PARAM_PROTOCOL_VERSION);
	if (param_queue_add(&queue, param, -1, NULL) != 0) {
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

	while ((packet = csp_read(connection, CONFIG_KFSW_PARAM_TIMEOUT_MS)) != NULL) {
		bool end;

		if ((packet->length < 2U) || (packet->data[0] != PARAM_PULL_RESPONSE_V2)) {
			csp_buffer_free(packet);
			result = -EBADMSG;
			break;
		}

		end = (packet->data[1] & PARAM_FLAG_END) != 0U;
		param_queue_init(&queue, &packet->data[2], packet->length - 2U, packet->length - 2U,
				 PARAM_QUEUE_TYPE_SET, KFSW_PARAM_PROTOCOL_VERSION);
		kfsw_param_table_lock();
		result = (param_queue_apply(&queue, node, 0) == 0) ? 0 : -EBADMSG;
		kfsw_param_table_unlock();
		csp_buffer_free(packet);
		if ((result != 0) || end) {
			break;
		}
	}
	(void)csp_close(connection);
	return result;
}

static int push_remote(const param_t *param, uint16_t node, const struct kfsw_param_value *value)
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
	if (param_queue_add(&queue, param, -1, (void *)&value->scalar) != 0) {
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
	packet = csp_read(connection, CONFIG_KFSW_PARAM_TIMEOUT_MS);
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

int kfsw_param_remote_get(uint16_t node, const char *name, struct kfsw_param_value *value)
{
	const param_t *param;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	result = kfsw_param_remote_refresh(node);
	if (result != 0) {
		return result;
	}

	kfsw_param_table_lock();
	param = param_list_find_name(node, name);
	kfsw_param_table_unlock();
	if (param == NULL) {
		return -ENOENT;
	}

	result = pull_remote(param, node);
	if (result != 0) {
		return result;
	}

	kfsw_param_table_lock();
	result = read_scalar(param, value);
	kfsw_param_table_unlock();
	return result;
}

int kfsw_param_remote_set(uint16_t node, const char *name, const struct kfsw_param_value *value)
{
	const param_t *param;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	result = kfsw_param_remote_refresh(node);
	if (result != 0) {
		return result;
	}

	kfsw_param_table_lock();
	param = param_list_find_name(node, name);
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

	result = push_remote(param, node, value);
	if (result == 0) {
		kfsw_param_table_lock();
		param_set(param, 0U, (void *)&value->scalar);
		kfsw_param_table_unlock();
	}
	return result;
}

int kfsw_param_remote_visit(uint16_t node, kfsw_param_visitor_t visitor, void *context)
{
	param_list_iterator iterator = {0};
	const param_t *param;
	int result;

	if (visitor == NULL) {
		return -EINVAL;
	}
	result = kfsw_param_remote_refresh(node);
	if (result != 0) {
		return result;
	}

	kfsw_param_table_lock();
	while ((param = param_list_iterate(&iterator)) != NULL) {
		struct kfsw_param_info info;

		if (*param->node != node) {
			continue;
		}
		fill_info(param, &info);
		if (!visitor(&info, context)) {
			break;
		}
	}
	kfsw_param_table_unlock();
	return 0;
}
