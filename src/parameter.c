#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

#define KFSW_PARAM_CALLBACK_NONE NULL
#define KFSW_PARAM_CALLBACK_LOG_LEVEL log_level_changed
#define KFSW_PARAM_CALLBACK(name) KFSW_PARAM_CALLBACK_(name)
#define KFSW_PARAM_CALLBACK_(name) KFSW_PARAM_CALLBACK_##name

#define KFSW_PARAM_DEFINE_VALUE(id, name, type, member, c_type, default_value, flags, callback,    \
				description)                                                       \
	c_type kfsw_param_value_##name = default_value;
KFSW_PARAM_TABLE(KFSW_PARAM_DEFINE_VALUE)
#undef KFSW_PARAM_DEFINE_VALUE

static void log_level_changed(void)
{
	if (kfsw_log_set_level(kfsw_param_value_log_level) != 0) {
		kfsw_param_value_log_level = CONFIG_KFSW_LOG_MIN_LEVEL;
		(void)kfsw_log_set_level(kfsw_param_value_log_level);
	}
}

#define KFSW_PARAM_DEFINE_ENTRY(entry_id, entry_name, entry_type, member, c_type, entry_default,   \
				entry_flags, callback, entry_description)                          \
	{                                                                                          \
		.info =                                                                            \
			{                                                                          \
				.node = 0U,                                                        \
				.id = entry_id,                                                    \
				.array_size = 1U,                                                  \
				.type = entry_type,                                                \
				.flags = entry_flags,                                              \
				.name = #entry_name,                                               \
				.unit = NULL,                                                      \
				.description = entry_description,                                  \
				.read_only = ((entry_flags) & KFSW_PARAM_FLAG_READ_ONLY) != 0U,    \
			},                                                                         \
		.value = &kfsw_param_value_##entry_name,                                           \
		.default_value = {.member = entry_default},                                        \
		.changed = KFSW_PARAM_CALLBACK(callback),                                          \
	},

static const struct kfsw_param_entry parameter_table[] = {
	KFSW_PARAM_TABLE(KFSW_PARAM_DEFINE_ENTRY)};
#undef KFSW_PARAM_DEFINE_ENTRY

K_MUTEX_DEFINE(kfsw_param_lock);

static bool initialized;

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

void kfsw_param_table_lock(void)
{
	k_mutex_lock(&kfsw_param_lock, K_FOREVER);
}

void kfsw_param_table_unlock(void)
{
	k_mutex_unlock(&kfsw_param_lock);
}

size_t kfsw_param_entry_count(void)
{
	return ARRAY_SIZE(parameter_table);
}

const struct kfsw_param_entry *kfsw_param_entry_at(size_t index)
{
	return (index < ARRAY_SIZE(parameter_table)) ? &parameter_table[index] : NULL;
}

const struct kfsw_param_entry *kfsw_param_find_id(uint16_t id)
{
	for (size_t index = 0U; index < ARRAY_SIZE(parameter_table); index++) {
		if (parameter_table[index].info.id == id) {
			return &parameter_table[index];
		}
	}
	return NULL;
}

const struct kfsw_param_entry *kfsw_param_find_name(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	for (size_t index = 0U; index < ARRAY_SIZE(parameter_table); index++) {
		if (strcmp(parameter_table[index].info.name, name) == 0) {
			return &parameter_table[index];
		}
	}
	return NULL;
}

int kfsw_param_read_entry(const struct kfsw_param_entry *entry, struct kfsw_param_value *value)
{
	size_t size;

	if ((entry == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	size = scalar_size(entry->info.type);
	if ((size == 0U) || (entry->info.array_size != 1U)) {
		return -ENOTSUP;
	}

	memset(value, 0, sizeof(*value));
	value->type = entry->info.type;
	value->size = size;
	memcpy(&value->scalar, entry->value, size);
	return 0;
}

void kfsw_param_write_entry(const struct kfsw_param_entry *entry,
			    const union kfsw_param_scalar *value)
{
	const size_t size = scalar_size(entry->info.type);

	memcpy(entry->value, value, size);
	if (entry->changed != NULL) {
		entry->changed();
	}
}

void kfsw_param_value_changed(uint16_t id)
{
	const struct kfsw_param_entry *entry = kfsw_param_find_id(id);

	if ((entry != NULL) && (entry->changed != NULL)) {
		entry->changed();
	}
}

static int validate_scalar(const struct kfsw_param_entry *entry,
			   const struct kfsw_param_value *value)
{
	const size_t size = (entry == NULL) ? 0U : scalar_size(entry->info.type);

	if ((entry == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	if ((size == 0U) || (entry->info.array_size != 1U)) {
		return -ENOTSUP;
	}
	if ((value->type != entry->info.type) || (value->size != size)) {
		return -EMSGSIZE;
	}
	return 0;
}

int kfsw_param_init(void)
{
	size_t local_count = 0U;

	kfsw_param_table_lock();
	if (initialized) {
		kfsw_param_table_unlock();
		return 0;
	}

	for (size_t outer = 0U; outer < ARRAY_SIZE(parameter_table); outer++) {
		const struct kfsw_param_entry *entry = &parameter_table[outer];

		if ((entry->info.node != 0U) || (entry->info.name == NULL) ||
		    (entry->value == NULL) || (entry->info.array_size == 0U) ||
		    (scalar_size(entry->info.type) == 0U)) {
			kfsw_param_table_unlock();
			return -EINVAL;
		}

		for (size_t inner = 0U; inner < outer; inner++) {
			const struct kfsw_param_entry *candidate = &parameter_table[inner];

			if ((candidate->info.id == entry->info.id) ||
			    (strcmp(candidate->info.name, entry->info.name) == 0)) {
				kfsw_param_table_unlock();
				return -EEXIST;
			}
		}
		local_count++;
	}

	if (local_count == 0U) {
		kfsw_param_table_unlock();
		return -ENOENT;
	}

	initialized = true;
	kfsw_param_table_unlock();
	return 0;
}

bool kfsw_param_is_initialized(void)
{
	return initialized;
}

int kfsw_param_get(const char *name, struct kfsw_param_value *value)
{
	const struct kfsw_param_entry *entry;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	entry = kfsw_param_find_name(name);
	result = (entry == NULL) ? -ENOENT : kfsw_param_read_entry(entry, value);
	kfsw_param_table_unlock();
	return result;
}

int kfsw_param_set(const char *name, const struct kfsw_param_value *value)
{
	const struct kfsw_param_entry *entry;
	int result;

	if ((name == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	entry = kfsw_param_find_name(name);
	if (entry == NULL) {
		result = -ENOENT;
	} else if (entry->info.read_only) {
		result = -EACCES;
	} else {
		result = validate_scalar(entry, value);
		if ((result == 0) && (entry->info.id == 1U) && (value->scalar.u8 > 4U)) {
			result = -ERANGE;
		}
		if (result == 0) {
			kfsw_param_write_entry(entry, &value->scalar);
		}
	}
	kfsw_param_table_unlock();
	return result;
}

#if CONFIG_KFSW_PARAM_PERSISTENCE
int kfsw_param_restore_defaults(void)
{
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	for (size_t index = 0U; index < ARRAY_SIZE(parameter_table); index++) {
		const struct kfsw_param_entry *entry = &parameter_table[index];

		if ((entry->info.flags & KFSW_PARAM_FLAG_PERSISTENT) != 0U) {
			kfsw_param_write_entry(entry, &entry->default_value);
		}
	}
	kfsw_param_table_unlock();
	return 0;
}
#endif

int kfsw_param_visit(kfsw_param_visitor_t visitor, void *context)
{
	if (visitor == NULL) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	for (size_t index = 0U; index < ARRAY_SIZE(parameter_table); index++) {
		if (!visitor(&parameter_table[index].info, context)) {
			break;
		}
	}
	kfsw_param_table_unlock();
	return 0;
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
