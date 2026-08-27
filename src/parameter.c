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
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

/* Upstream's version 3 list wire structure; kept private to this adapter. */
#include "param_list.h"

#define KFSW_PARAM_PROTOCOL_VERSION 2
#define KFSW_PARAM_LIST_VERSION 3
#define KFSW_PARAM_DEFAULT_TEST_U32 42U
#define KFSW_PARAM_DEFAULT_TEST_I32 -7
#define KFSW_PARAM_DEFAULT_TEST_FLOAT 1.5F

static uint16_t node_id_value = CONFIG_KFSW_CSP_ADDRESS;
static uint8_t log_level_value = CONFIG_KFSW_LOG_MIN_LEVEL;
static uint32_t test_u32_value = KFSW_PARAM_DEFAULT_TEST_U32;
static int32_t test_i32_value = KFSW_PARAM_DEFAULT_TEST_I32;
static float test_float_value = KFSW_PARAM_DEFAULT_TEST_FLOAT;

static void log_level_changed(const param_t *param, int offset)
{
	ARG_UNUSED(param);
	ARG_UNUSED(offset);

	if (kfsw_log_set_level(log_level_value) != 0) {
		log_level_value = CONFIG_KFSW_LOG_MIN_LEVEL;
		(void)kfsw_log_set_level(log_level_value);
	}
}

PARAM_DEFINE_STATIC_RAM(0, node_id, PARAM_TYPE_UINT16, 1, sizeof(node_id_value),
			PM_READONLY | PM_SYSINFO, NULL, NULL, &node_id_value,
			"Build-time CSP node address");
PARAM_DEFINE_STATIC_RAM(1, log_level, PARAM_TYPE_UINT8, 1, sizeof(log_level_value),
			PM_CONF | KFSW_PARAM_FLAG_PERSISTENT, log_level_changed, NULL,
			&log_level_value, "Runtime logging policy value");
PARAM_DEFINE_STATIC_RAM(2, test_u32, PARAM_TYPE_UINT32, 1, sizeof(test_u32_value),
			PM_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NULL, NULL, &test_u32_value,
			"Writable unsigned integration value");
PARAM_DEFINE_STATIC_RAM(3, test_i32, PARAM_TYPE_INT32, 1, sizeof(test_i32_value),
			PM_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NULL, NULL, &test_i32_value,
			"Writable signed integration value");
PARAM_DEFINE_STATIC_RAM(4, test_float, PARAM_TYPE_FLOAT, 1, sizeof(test_float_value),
			PM_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NULL, NULL, &test_float_value,
			"Writable floating-point integration value");

_Static_assert(sizeof(param_transfer3_t) <= CSP_BUFFER_SIZE,
	       "CSP buffers must fit an upstream parameter-list entry");

K_MUTEX_DEFINE(kfsw_param_lock);
K_MUTEX_DEFINE(kfsw_param_remote_lock);

static bool initialized;
static bool server_started;
static csp_socket_t list_socket;

void kfsw_param_table_lock(void)
{
	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
}

void kfsw_param_table_unlock(void)
{
	k_mutex_unlock(&kfsw_param_lock);
}

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

static int visit_node(uint16_t node, kfsw_param_visitor_t visitor, void *context)
{
	param_list_iterator iterator = {0};
	const param_t *param;

	if (visitor == NULL) {
		return -EINVAL;
	}

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

	return 0;
}

int kfsw_param_init(void)
{
	param_list_iterator outer = {0};
	const param_t *param;
	size_t local_count = 0U;

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	if (initialized) {
		k_mutex_unlock(&kfsw_param_lock);
		return 0;
	}

	while ((param = param_list_iterate(&outer)) != NULL) {
		param_list_iterator inner = {0};
		const param_t *candidate;

		if (*param->node != 0U) {
			continue;
		}
		if ((param->name == NULL) || (param->addr == NULL) || (param->array_size == 0U) ||
		    (from_libparam_type((param_type_e)param->type) == KFSW_PARAM_INVALID)) {
			k_mutex_unlock(&kfsw_param_lock);
			return -EINVAL;
		}

		while ((candidate = param_list_iterate(&inner)) != NULL) {
			if (candidate == param) {
				break;
			}
			if ((*candidate->node == 0U) &&
			    ((candidate->id == param->id) ||
			     (strcmp(candidate->name, param->name) == 0))) {
				k_mutex_unlock(&kfsw_param_lock);
				return -EEXIST;
			}
		}

		local_count++;
	}

	if (local_count == 0U) {
		k_mutex_unlock(&kfsw_param_lock);
		return -ENOENT;
	}

	initialized = true;
	k_mutex_unlock(&kfsw_param_lock);
	return 0;
}

bool kfsw_param_is_initialized(void)
{
	return initialized;
}

int kfsw_param_get(const char *name, struct kfsw_param_value *value)
{
	const param_t *param;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	param = param_list_find_name(0, name);
	result = (param == NULL) ? -ENOENT : read_scalar(param, value);
	k_mutex_unlock(&kfsw_param_lock);
	return result;
}

int kfsw_param_set(const char *name, const struct kfsw_param_value *value)
{
	const param_t *param;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	param = param_list_find_name(0, name);
	if (param == NULL) {
		result = -ENOENT;
	} else if ((param->mask & PM_READONLY) != 0U) {
		result = -EACCES;
	} else {
		result = validate_scalar(param, value);
		if ((result == 0) && (param == &log_level) && (value->scalar.u8 > 4U)) {
			result = -ERANGE;
		}
		if (result == 0) {
			param_set(param, 0U, (void *)&value->scalar);
		}
	}
	k_mutex_unlock(&kfsw_param_lock);
	return result;
}

#if CONFIG_KFSW_PARAM_PERSISTENCE
int kfsw_param_restore_defaults(void)
{
	uint8_t default_log_level = CONFIG_KFSW_LOG_MIN_LEVEL;
	uint32_t default_test_u32 = KFSW_PARAM_DEFAULT_TEST_U32;
	int32_t default_test_i32 = KFSW_PARAM_DEFAULT_TEST_I32;
	float default_test_float = KFSW_PARAM_DEFAULT_TEST_FLOAT;

	if (!initialized) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	param_set(&log_level, 0U, &default_log_level);
	param_set(&test_u32, 0U, &default_test_u32);
	param_set(&test_i32, 0U, &default_test_i32);
	param_set(&test_float, 0U, &default_test_float);
	k_mutex_unlock(&kfsw_param_lock);
	return 0;
}
#endif

int kfsw_param_visit(kfsw_param_visitor_t visitor, void *context)
{
	int result;

	if (!initialized) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	result = visit_node(0U, visitor, context);
	k_mutex_unlock(&kfsw_param_lock);
	return result;
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
	if (!initialized) {
		csp_buffer_free(packet);
		return;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	if (push_allowed(packet)) {
		param_serve(packet);
	} else {
		csp_buffer_free(packet);
	}
	k_mutex_unlock(&kfsw_param_lock);
}

static void serve_list(csp_conn_t *connection)
{
	param_list_iterator iterator = {0};
	const param_t *param;

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
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
	k_mutex_unlock(&kfsw_param_lock);
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

	if (!initialized) {
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
	if (!initialized || !csp_info.initialized || !csp_info.router_running) {
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
	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	if (node_is_cached(node)) {
		k_mutex_unlock(&kfsw_param_lock);
		k_mutex_unlock(&kfsw_param_remote_lock);
		return 0;
	}
	k_mutex_unlock(&kfsw_param_lock);

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

		k_mutex_lock(&kfsw_param_lock, K_FOREVER);
		unpack_result = param_list_unpack(node, packet->data, packet->length,
						  KFSW_PARAM_LIST_VERSION, 0);
		k_mutex_unlock(&kfsw_param_lock);
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
		k_mutex_lock(&kfsw_param_lock, K_FOREVER);
		result = (param_queue_apply(&queue, node, 0) == 0) ? 0 : -EBADMSG;
		k_mutex_unlock(&kfsw_param_lock);
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

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	param = param_list_find_name(node, name);
	k_mutex_unlock(&kfsw_param_lock);
	if (param == NULL) {
		return -ENOENT;
	}

	result = pull_remote(param, node);
	if (result != 0) {
		return result;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	result = read_scalar(param, value);
	k_mutex_unlock(&kfsw_param_lock);
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

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	param = param_list_find_name(node, name);
	if (param == NULL) {
		result = -ENOENT;
	} else if ((param->mask & PM_READONLY) != 0U) {
		result = -EACCES;
	} else {
		result = validate_scalar(param, value);
	}
	k_mutex_unlock(&kfsw_param_lock);
	if (result != 0) {
		return result;
	}

	result = push_remote(param, node, value);
	if (result == 0) {
		k_mutex_lock(&kfsw_param_lock, K_FOREVER);
		param_set(param, 0U, (void *)&value->scalar);
		k_mutex_unlock(&kfsw_param_lock);
	}
	return result;
}

int kfsw_param_remote_visit(uint16_t node, kfsw_param_visitor_t visitor, void *context)
{
	int result = kfsw_param_remote_refresh(node);

	if (result != 0) {
		return result;
	}

	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
	result = visit_node(node, visitor, context);
	k_mutex_unlock(&kfsw_param_lock);
	return result;
}

const char *kfsw_param_type_name(enum kfsw_param_type type)
{
	switch (type) {
	case KFSW_PARAM_U8:
		return "u8";
	case KFSW_PARAM_U16:
		return "u16";
	case KFSW_PARAM_U32:
		return "u32";
	case KFSW_PARAM_U64:
		return "u64";
	case KFSW_PARAM_I8:
		return "i8";
	case KFSW_PARAM_I16:
		return "i16";
	case KFSW_PARAM_I32:
		return "i32";
	case KFSW_PARAM_I64:
		return "i64";
	case KFSW_PARAM_X8:
		return "x8";
	case KFSW_PARAM_X16:
		return "x16";
	case KFSW_PARAM_X32:
		return "x32";
	case KFSW_PARAM_X64:
		return "x64";
	case KFSW_PARAM_FLOAT:
		return "float";
	case KFSW_PARAM_DOUBLE:
		return "double";
	case KFSW_PARAM_STRING:
		return "string";
	case KFSW_PARAM_DATA:
		return "data";
	case KFSW_PARAM_INVALID:
	default:
		return "invalid";
	}
}
