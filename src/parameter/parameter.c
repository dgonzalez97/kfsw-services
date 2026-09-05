#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

static struct kfsw_param_entry parameter_table[KFSW_PARAM_MAX_DEFINITIONS];
static size_t parameter_count;

static struct kfsw_param_table_info parameter_tables[KFSW_PARAM_MAX_TABLES];
static size_t table_count;

K_MUTEX_DEFINE(kfsw_param_lock);

static bool initialized;

/* strnlen is not visible under the C17 profile these builds use, and a plain
 * strlen on a name that is not terminated would read past it.
 */
static bool name_is_within_limit(const char *name)
{
	for (size_t index = 0U; index <= KFSW_PARAM_NAME_MAX; index++) {
		if (name[index] == '\0') {
			return true;
		}
	}
	return false;
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

/* A scalar's width comes from its type; a string's comes from the capacity its
 * owner declared, because that is the storage a write has to fit inside.
 */
static size_t entry_capacity(const struct kfsw_param_entry *entry)
{
	if ((entry->info.type == KFSW_PARAM_STRING) || (entry->info.type == KFSW_PARAM_DATA)) {
		return entry->info.array_size;
	}
	return scalar_size(entry->info.type);
}

/* Bounded copy that always terminates. The source is owner storage which a
 * previous write kept terminated, but a corrupted snapshot or a bad pointer
 * must not be able to run off the end of it.
 */
static size_t copy_text(char *destination, size_t destination_size, const char *source,
			size_t source_size)
{
	size_t length = 0U;

	while ((length < source_size) && (length + 1U < destination_size) &&
	       (source[length] != '\0')) {
		destination[length] = source[length];
		length++;
	}
	destination[length] = '\0';
	return length;
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

static int read_entry(const struct kfsw_param_entry *entry, struct kfsw_param_value *value,
		      bool sample)
{
	size_t size;

	if ((entry == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	size = entry_capacity(entry);
	if (size == 0U) {
		return -ENOTSUP;
	}

	/* Sampled parameters hold live state that nothing else writes, so the
	 * backing store is refreshed here rather than on a timer: a value read
	 * over a link is worth having only if it is current at the moment it
	 * was asked for.
	 */
	if (sample && (entry->definition->sample != NULL)) {
		entry->definition->sample(entry->definition->value);
	}

	memset(value, 0, sizeof(*value));
	value->type = entry->info.type;

	if (entry->info.type == KFSW_PARAM_STRING) {
		value->size = copy_text(value->text, sizeof(value->text), entry->definition->value,
					size) +
			      1U;
		return 0;
	}

	if (entry->info.type == KFSW_PARAM_DATA) {
		/* Always the whole array: the elements of one are only sensible
		 * together, so a partial read would be a different value. */
		memcpy(value->bytes, entry->definition->value, size);
		value->size = size;
		return 0;
	}

	value->size = size;
	memcpy(&value->scalar, entry->definition->value, size);
	return 0;
}

int kfsw_param_read_entry(const struct kfsw_param_entry *entry, struct kfsw_param_value *value)
{
	return read_entry(entry, value, true);
}

/*
 * Reads what is in the backing store without refreshing it first.
 *
 * The distinction matters on exactly one path. A write that arrives over CSP
 * lands in the store and is then handed back here to be applied; sampling at
 * that moment would overwrite the value that just arrived with the one the
 * owner still holds, and the change callback would be handed the old value.
 * The write would report success and change nothing.
 */
int kfsw_param_read_stored_entry(const struct kfsw_param_entry *entry,
				 struct kfsw_param_value *value)
{
	return read_entry(entry, value, false);
}

int kfsw_param_validate_entry(const struct kfsw_param_entry *entry,
			      const struct kfsw_param_value *value)
{
	size_t size;

	if ((entry == NULL) || (value == NULL)) {
		return -EINVAL;
	}
	size = entry_capacity(entry);
	if (size == 0U) {
		return -ENOTSUP;
	}
	if (value->type != entry->info.type) {
		return -EMSGSIZE;
	}

	if (entry->info.type == KFSW_PARAM_STRING) {
		/* size carries the terminator, so a value that exactly fills the
		 * declared capacity is accepted and one byte more is not. */
		if ((value->size == 0U) || (value->size > size)) {
			return -EMSGSIZE;
		}
		if (entry->definition->validate_text != NULL) {
			return entry->definition->validate_text(value->text);
		}
		return 0;
	}

	if (entry->info.type == KFSW_PARAM_DATA) {
		/* Exact length: an array is written whole or not at all, because
		 * a short write would leave some elements at their old values
		 * and the caller could not tell which. */
		if (value->size != size) {
			return -EMSGSIZE;
		}
		if (entry->definition->validate_data != NULL) {
			return entry->definition->validate_data(value->bytes, value->size);
		}
		return 0;
	}

	if (value->size != size) {
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

void kfsw_param_write_data_entry(const struct kfsw_param_entry *entry, const uint8_t *data,
				 size_t size)
{
	memcpy(entry->definition->value, data, size);
	if (entry->definition->changed_data != NULL) {
		entry->definition->changed_data(entry->definition->value, size);
	}
}

void kfsw_param_write_text_entry(const struct kfsw_param_entry *entry, const char *text)
{
	(void)copy_text(entry->definition->value, entry->info.array_size, text,
			entry->info.array_size);
	if (entry->definition->changed_text != NULL) {
		entry->definition->changed_text(entry->definition->value);
	}
}

/* Writing a default is the one place both kinds meet, so it is worth having
 * once rather than repeated at every caller that resets a table.
 */
void kfsw_param_write_default(const struct kfsw_param_entry *entry)
{
	if (entry->info.type == KFSW_PARAM_DATA) {
		const size_t size = entry->info.array_size;

		if (entry->definition->default_data != NULL) {
			kfsw_param_write_data_entry(entry, entry->definition->default_data, size);
		} else {
			(void)memset(entry->definition->value, 0, size);
			if (entry->definition->changed_data != NULL) {
				entry->definition->changed_data(entry->definition->value, size);
			}
		}
		return;
	}
	if (entry->info.type == KFSW_PARAM_STRING) {
		kfsw_param_write_text_entry(entry, (entry->definition->default_text != NULL)
							   ? entry->definition->default_text
							   : "");
		return;
	}
	kfsw_param_write_entry(entry, &entry->definition->default_value);
}

void kfsw_param_value_changed(uint16_t id)
{
	const struct kfsw_param_entry *entry = kfsw_param_find_id(id);
	struct kfsw_param_value value;

	if ((entry == NULL) || (kfsw_param_read_stored_entry(entry, &value) != 0)) {
		return;
	}
	if (kfsw_param_validate_entry(entry, &value) != 0) {
		/* The write already reached owner storage, so the only way back
		 * to a value the owner would accept is the compiled default. */
		kfsw_param_write_default(entry);
		return;
	}
	if (entry->info.type == KFSW_PARAM_STRING) {
		if (entry->definition->changed_text != NULL) {
			entry->definition->changed_text(value.text);
		}
		return;
	}
	if (entry->info.type == KFSW_PARAM_DATA) {
		if (entry->definition->changed_data != NULL) {
			entry->definition->changed_data(value.bytes, value.size);
		}
		return;
	}
	if (entry->definition->changed != NULL) {
		entry->definition->changed(&value.scalar);
	}
}

static struct kfsw_param_table_info *find_table(uint8_t id)
{
	for (size_t index = 0U; index < table_count; index++) {
		if (parameter_tables[index].id == id) {
			return &parameter_tables[index];
		}
	}
	return NULL;
}

/* A table identifier must fall inside an allocated band. Zero is reserved so
 * that an uninitialised field cannot address a real table, and anything above
 * the module band is left for mission payloads that this build does not own.
 */
static bool table_is_allocated(uint8_t table)
{
	return (table >= KFSW_PARAM_TABLE_CORE_FIRST) && (table <= KFSW_PARAM_TABLE_MODULE_LAST);
}

static int add_table(const struct kfsw_param_definition_set *set)
{
	size_t insert_at;

	if ((set->name == NULL) || (set->name[0] == '\0') || !name_is_within_limit(set->name)) {
		kfsw_log_error("PARAM: table %u has no usable name", set->table);
		return -EINVAL;
	}
	if (!table_is_allocated(set->table)) {
		kfsw_log_error("PARAM: table %u is outside every allocated band", set->table);
		return -EINVAL;
	}
	if (find_table(set->table) != NULL) {
		kfsw_log_error("PARAM: table %u (%s) is already registered", set->table, set->name);
		return -EEXIST;
	}
	if (table_count >= KFSW_PARAM_MAX_TABLES) {
		kfsw_log_error("PARAM: no room for table %u (%s)", set->table, set->name);
		return -ENOSPC;
	}

	/* Kept in ascending identifier order so a listing reads as one table
	 * regardless of the order the composition happens to register in.
	 */
	insert_at = table_count;
	for (size_t index = 0U; index < table_count; index++) {
		if (parameter_tables[index].id > set->table) {
			insert_at = index;
			break;
		}
	}
	if (insert_at < table_count) {
		memmove(&parameter_tables[insert_at + 1U], &parameter_tables[insert_at],
			(table_count - insert_at) * sizeof(parameter_tables[0]));
	}
	parameter_tables[insert_at] = (struct kfsw_param_table_info){
		.id = set->table,
		.name = set->name,
		.count = 0U,
	};
	table_count++;
	return 0;
}

static int add_definition(const struct kfsw_param_definition_set *set,
			  const struct kfsw_param_definition *definition)
{
	struct kfsw_param_value default_value;
	struct kfsw_param_table_info *table;
	size_t insert_at = parameter_count;
	size_t size;
	uint16_t wire_id;
	uint32_t flags;

	if ((definition == NULL) || (definition->name == NULL) || (definition->name[0] == '\0') ||
	    (definition->value == NULL)) {
		return -EINVAL;
	}

	/* A string or an array declares the storage it owns; a scalar's width
	 * comes from its type. One without a capacity, or larger than a value
	 * can carry, is refused here rather than overrunning owner storage
	 * later.
	 */
	if (definition->type == KFSW_PARAM_DATA) {
		if ((definition->capacity == 0U) ||
		    (definition->capacity > KFSW_PARAM_STRING_MAX)) {
			kfsw_log_error("PARAM: %s declares %u elements", definition->name,
				       definition->capacity);
			return -EINVAL;
		}
		size = definition->capacity;
	} else if (definition->type == KFSW_PARAM_STRING) {
		if ((definition->capacity < 2U) || (definition->capacity > KFSW_PARAM_STRING_MAX)) {
			kfsw_log_error("PARAM: %s declares a capacity of %u", definition->name,
				       definition->capacity);
			return -EINVAL;
		}
		size = definition->capacity;
	} else {
		size = scalar_size(definition->type);
		if (size == 0U) {
			return -EINVAL;
		}
	}
	if (!name_is_within_limit(definition->name)) {
		kfsw_log_error("PARAM: name '%s' is longer than %u characters", definition->name,
			       KFSW_PARAM_NAME_MAX);
		return -ENAMETOOLONG;
	}
	if (parameter_count >= KFSW_PARAM_MAX_DEFINITIONS) {
		kfsw_log_error("PARAM: no room for %s; the table holds %u", definition->name,
			       (unsigned int)KFSW_PARAM_MAX_DEFINITIONS);
		return -ENOSPC;
	}

	table = find_table(set->table);
	if (table == NULL) {
		return -ENOENT;
	}
	wire_id = KFSW_PARAM_WIRE_ID(set->table, definition->offset);

	/* A definition that refuses its own default would leave the table in a
	 * state its owner never sanctioned, so it is caught at registration.
	 */
	if (definition->type == KFSW_PARAM_DATA) {
		if (definition->validate_data != NULL) {
			static const uint8_t zero[KFSW_PARAM_STRING_MAX];
			const uint8_t *data = (definition->default_data != NULL)
						      ? definition->default_data
						      : zero;

			if (definition->validate_data(data, definition->capacity) != 0) {
				kfsw_log_error("PARAM: %s refuses its own default",
					       definition->name);
				return -ERANGE;
			}
		}
	} else if (definition->type == KFSW_PARAM_STRING) {
		const char *text =
			(definition->default_text != NULL) ? definition->default_text : "";

		if (copy_text(default_value.text, sizeof(default_value.text), text,
			      sizeof(default_value.text) - 1U) >= definition->capacity) {
			kfsw_log_error("PARAM: the default for %s does not fit its capacity",
				       definition->name);
			return -ERANGE;
		}
		if ((definition->validate_text != NULL) &&
		    (definition->validate_text(default_value.text) != 0)) {
			kfsw_log_error("PARAM: %s refuses its own default", definition->name);
			return -ERANGE;
		}
	} else {
		default_value.type = definition->type;
		default_value.size = size;
		default_value.scalar = definition->default_value;
		if ((definition->validate != NULL) &&
		    (definition->validate(&default_value.scalar) != 0)) {
			kfsw_log_error("PARAM: %s refuses its own default", definition->name);
			return -ERANGE;
		}
	}

	/* Offsets are unique inside a table and names across the whole node:
	 * the wire identifier is what a remote list is keyed by, and the name
	 * is what an operator types.
	 */
	for (size_t index = 0U; index < parameter_count; index++) {
		const struct kfsw_param_entry *entry = &parameter_table[index];

		if (entry->info.id == wire_id) {
			kfsw_log_error("PARAM: table %u offset 0x%02x is taken by %s", set->table,
				       definition->offset, entry->info.name);
			return -EEXIST;
		}
		if (strcmp(entry->info.name, definition->name) == 0) {
			kfsw_log_error("PARAM: name '%s' is already registered", definition->name);
			return -EEXIST;
		}
		if ((insert_at == parameter_count) && (entry->info.id > wire_id)) {
			insert_at = index;
		}
	}

	/* The service sets LIVE, never the definition: a parameter cannot claim
	 * a write takes effect immediately without the callback that applies it.
	 */
	flags = definition->flags;
	if ((definition->changed != NULL) || (definition->changed_text != NULL) ||
	    (definition->changed_data != NULL)) {
		flags |= KFSW_PARAM_FLAG_LIVE;
	}

	if (insert_at < parameter_count) {
		memmove(&parameter_table[insert_at + 1U], &parameter_table[insert_at],
			(parameter_count - insert_at) * sizeof(parameter_table[0]));
	}
	parameter_table[insert_at] = (struct kfsw_param_entry){
		.info =
			{
				.node = 0U,
				.id = wire_id,
				.table = set->table,
				.offset = definition->offset,
				.array_size = ((definition->type == KFSW_PARAM_STRING) ||
					       (definition->type == KFSW_PARAM_DATA))
						      ? definition->capacity
						      : 1U,
				.type = definition->type,
				.flags = flags,
				.table_name = set->name,
				.name = definition->name,
				.unit = definition->unit,
				.description = definition->description,
				.read_only = (flags & KFSW_PARAM_FLAG_READ_ONLY) != 0U,
			},
		.definition = definition,
	};
	parameter_count++;
	table->count++;
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
	table_count = 0U;
	for (size_t set_index = 0U; set_index < set_count; set_index++) {
		const struct kfsw_param_definition_set *set = sets[set_index];

		if ((set == NULL) || (set->definitions == NULL) || (set->count == 0U)) {
			result = -EINVAL;
			break;
		}
		result = add_table(set);
		if (result != 0) {
			break;
		}
		for (size_t definition_index = 0U; definition_index < set->count;
		     definition_index++) {
			result = add_definition(set, &set->definitions[definition_index]);
			if (result != 0) {
				kfsw_log_error("PARAM: table %u (%s) rejected %s: %d", set->table,
					       set->name, set->definitions[definition_index].name,
					       result);
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

			kfsw_param_write_default(entry);
		}
		initialized = true;
		kfsw_log_info("PARAM: %u parameters in %u tables", (unsigned int)parameter_count,
			      (unsigned int)table_count);
	} else {
		parameter_count = 0U;
		table_count = 0U;
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
			if (entry->info.type == KFSW_PARAM_STRING) {
				kfsw_param_write_text_entry(entry, value->text);
			} else if (entry->info.type == KFSW_PARAM_DATA) {
				kfsw_param_write_data_entry(entry, value->bytes, value->size);
			} else {
				kfsw_param_write_entry(entry, &value->scalar);
			}
		}
	}
	kfsw_param_table_unlock();

	/* A parameter change is an operator action on a spacecraft, so both the
	 * acceptance and the refusal have to be reconstructible afterwards from
	 * the log alone.
	 */
	if (result == 0) {
		kfsw_log_info("PARAM: %s set (%s)", name, kfsw_param_mode_name(entry->info.flags));
#if CONFIG_KFSW_PARAM_PERSISTENCE
		/* Written after the lock is released, because saving reads every
		 * entry and would otherwise re-enter the table lock. Only
		 * persistent values are in a snapshot, so saving after a
		 * volatile write costs a flash cycle for nothing. */
		if (kfsw_param_autosave_enabled() &&
		    ((entry->info.flags & KFSW_PARAM_FLAG_PERSISTENT) != 0U)) {
			(void)kfsw_param_persist_save();
		}
#endif
	} else {
		kfsw_log_warning("PARAM: %s refused: %d", name, result);
	}
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
			kfsw_param_write_default(entry);
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

static atomic_t param_saves;
static atomic_t param_load_failures;

void kfsw_param_count_save(void)
{
	(void)atomic_inc(&param_saves);
}

void kfsw_param_count_load_failure(void)
{
	(void)atomic_inc(&param_load_failures);
}

int kfsw_param_get_stats(struct kfsw_param_stats *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}

	kfsw_param_table_lock();
	stats->count = (uint16_t)parameter_count;
	stats->tables = (uint16_t)table_count;
	stats->persistent = 0U;
	for (size_t index = 0U; index < parameter_count; index++) {
		if ((parameter_table[index].info.flags & KFSW_PARAM_FLAG_PERSISTENT) != 0U) {
			stats->persistent++;
		}
	}
	kfsw_param_table_unlock();

	stats->saves = (uint32_t)atomic_get(&param_saves);
	stats->load_failures = (uint32_t)atomic_get(&param_load_failures);
	return 0;
}

void kfsw_param_sample_all(void)
{
	/* The CSP server hands libparam the backing storage directly, so it
	 * never goes through the read path that refreshes a sampled value. A
	 * remote read would otherwise report whatever was last written locally,
	 * which for a value nothing writes is its compiled default forever: an
	 * uptime that is always zero, an identity that is always empty. Caller
	 * holds the table lock.
	 */
	for (size_t index = 0U; index < parameter_count; index++) {
		const struct kfsw_param_entry *entry = &parameter_table[index];

		if (entry->definition->sample != NULL) {
			entry->definition->sample(entry->definition->value);
		}
	}
}

int kfsw_param_get_info(const char *name, struct kfsw_param_info *info)
{
	const struct kfsw_param_entry *entry;
	int result;

	if ((name == NULL) || (info == NULL)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	entry = kfsw_param_find_name(name);
	if (entry == NULL) {
		result = -ENOENT;
	} else {
		*info = entry->info;
		result = 0;
	}
	kfsw_param_table_unlock();
	return result;
}

int kfsw_param_visit_tables(kfsw_param_table_visitor_t visitor, void *context)
{
	if (visitor == NULL) {
		return -EINVAL;
	}
	if (!initialized) {
		return -EACCES;
	}

	kfsw_param_table_lock();
	for (size_t index = 0U; index < table_count; index++) {
		if (!visitor(&parameter_tables[index], context)) {
			break;
		}
	}
	kfsw_param_table_unlock();
	return 0;
}

size_t kfsw_param_table_count(void)
{
	return table_count;
}

const char *kfsw_param_band_name(uint8_t table)
{
	if ((table >= KFSW_PARAM_TABLE_CORE_FIRST) && (table <= KFSW_PARAM_TABLE_CORE_LAST)) {
		return "core";
	}
	if ((table >= KFSW_PARAM_TABLE_SERVICE_FIRST) && (table <= KFSW_PARAM_TABLE_SERVICE_LAST)) {
		return "service";
	}
	if ((table >= KFSW_PARAM_TABLE_MODULE_FIRST) && (table <= KFSW_PARAM_TABLE_MODULE_LAST)) {
		return "module";
	}
	return "invalid";
}

const char *kfsw_param_mode_name(uint32_t flags)
{
	const bool live = (flags & KFSW_PARAM_FLAG_LIVE) != 0U;
	const bool stored = (flags & KFSW_PARAM_FLAG_PERSISTENT) != 0U;

	if ((flags & KFSW_PARAM_FLAG_READ_ONLY) != 0U) {
		return "r";
	}
	if (live && stored) {
		return "wb";
	}
	if (stored) {
		return "b";
	}
	return "w";
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
