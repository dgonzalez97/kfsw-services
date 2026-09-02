#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

static struct kfsw_param_entry parameter_table[KFSW_PARAM_MAX_DEFINITIONS];
static size_t parameter_count;

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
	return parameter_count;
}

const struct kfsw_param_entry *kfsw_param_entry_at(size_t index)
{
	return (index < parameter_count) ? &parameter_table[index] : NULL;
}

const struct kfsw_param_entry *kfsw_param_find_id(uint16_t id)
{
	for (size_t index = 0U; index < parameter_count; index++) {
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

	for (size_t index = 0U; index < parameter_count; index++) {
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
	memcpy(&value->scalar, entry->definition->value, size);
	return 0;
}

int kfsw_param_validate_entry(const struct kfsw_param_entry *entry,
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
	if (entry->definition->validate != NULL) {
		return entry->definition->validate(&value->scalar);
	}
	return 0;
}

void kfsw_param_write_entry(const struct kfsw_param_entry *entry,
			    const union kfsw_param_scalar *value)
{
	const size_t size = scalar_size(entry->info.type);

	memcpy(entry->definition->value, value, size);
	if (entry->definition->changed != NULL) {
		entry->definition->changed(value);
	}
}

void kfsw_param_value_changed(uint16_t id)
{
	const struct kfsw_param_entry *entry = kfsw_param_find_id(id);
	struct kfsw_param_value value;

	if ((entry == NULL) || (kfsw_param_read_entry(entry, &value) != 0)) {
		return;
	}
	if (kfsw_param_validate_entry(entry, &value) != 0) {
		kfsw_param_write_entry(entry, &entry->definition->default_value);
		return;
	}
	if (entry->definition->changed != NULL) {
		entry->definition->changed(&value.scalar);
	}
}

static int add_definition(const struct kfsw_param_definition *definition)
{
	struct kfsw_param_value default_value;
	size_t insert_at = parameter_count;
	const size_t size = (definition == NULL) ? 0U : scalar_size(definition->type);

	if ((definition == NULL) || (definition->name == NULL) || (definition->name[0] == '\0') ||
	    (definition->value == NULL) || (size == 0U)) {
		return -EINVAL;
	}
	if (parameter_count >= KFSW_PARAM_MAX_DEFINITIONS) {
		return -ENOSPC;
	}

	default_value.type = definition->type;
	default_value.size = size;
	default_value.scalar = definition->default_value;
	if ((definition->validate != NULL) && (definition->validate(&default_value.scalar) != 0)) {
		return -ERANGE;
	}

	for (size_t index = 0U; index < parameter_count; index++) {
		const struct kfsw_param_entry *entry = &parameter_table[index];

		if ((entry->info.id == definition->id) ||
		    (strcmp(entry->info.name, definition->name) == 0)) {
			return -EEXIST;
		}
		if ((insert_at == parameter_count) && (entry->info.id > definition->id)) {
			insert_at = index;
		}
	}

	if (insert_at < parameter_count) {
		memmove(&parameter_table[insert_at + 1U], &parameter_table[insert_at],
			(parameter_count - insert_at) * sizeof(parameter_table[0]));
	}
	parameter_table[insert_at] = (struct kfsw_param_entry){
		.info =
			{
				.node = 0U,
				.id = definition->id,
				.array_size = 1U,
				.type = definition->type,
				.flags = definition->flags,
				.name = definition->name,
				.unit = definition->unit,
				.description = definition->description,
				.read_only = (definition->flags & KFSW_PARAM_FLAG_READ_ONLY) != 0U,
			},
		.definition = definition,
	};
	parameter_count++;
	return 0;
}

int kfsw_param_init(const struct kfsw_param_definition_set *const *sets, size_t set_count)
{
	int result = 0;

	if ((sets == NULL) || (set_count == 0U)) {
		return -EINVAL;
	}

	kfsw_param_table_lock();
	if (initialized) {
		kfsw_param_table_unlock();
		return 0;
	}

	parameter_count = 0U;
	for (size_t set_index = 0U; set_index < set_count; set_index++) {
		const struct kfsw_param_definition_set *set = sets[set_index];

		if ((set == NULL) || (set->definitions == NULL) || (set->count == 0U)) {
			result = -EINVAL;
			break;
		}
		for (size_t definition_index = 0U; definition_index < set->count;
		     definition_index++) {
			result = add_definition(&set->definitions[definition_index]);
			if (result != 0) {
				break;
			}
		}
		if (result != 0) {
			break;
		}
	}

	if ((result == 0) && (parameter_count == 0U)) {
		result = -ENOENT;
	}
	if (result == 0) {
		for (size_t index = 0U; index < parameter_count; index++) {
			const struct kfsw_param_entry *entry = &parameter_table[index];

			kfsw_param_write_entry(entry, &entry->definition->default_value);
		}
		initialized = true;
	} else {
		parameter_count = 0U;
	}
	kfsw_param_table_unlock();
	return result;
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
		result = kfsw_param_validate_entry(entry, value);
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
	for (size_t index = 0U; index < parameter_count; index++) {
		const struct kfsw_param_entry *entry = &parameter_table[index];

		if ((entry->info.flags & KFSW_PARAM_FLAG_PERSISTENT) != 0U) {
			kfsw_param_write_entry(entry, &entry->definition->default_value);
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
	for (size_t index = 0U; index < parameter_count; index++) {
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
