#ifndef KFSW_SERVICES_PARAMETER_INTERNAL_H
#define KFSW_SERVICES_PARAMETER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/parameter.h>

#define KFSW_PARAM_MAX_DEFINITIONS 16U

struct kfsw_param_entry {
	struct kfsw_param_info info;
	const struct kfsw_param_definition *definition;
};

void kfsw_param_table_lock(void);
void kfsw_param_table_unlock(void);

size_t kfsw_param_entry_count(void);
const struct kfsw_param_entry *kfsw_param_entry_at(size_t index);
const struct kfsw_param_entry *kfsw_param_find_id(uint16_t id);
const struct kfsw_param_entry *kfsw_param_find_name(const char *name);
int kfsw_param_read_entry(const struct kfsw_param_entry *entry, struct kfsw_param_value *value);
int kfsw_param_validate_entry(const struct kfsw_param_entry *entry,
			      const struct kfsw_param_value *value);
void kfsw_param_write_entry(const struct kfsw_param_entry *entry,
			    const union kfsw_param_scalar *value);
void kfsw_param_value_changed(uint16_t id);

#endif
