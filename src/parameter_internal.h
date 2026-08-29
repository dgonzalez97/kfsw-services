#ifndef KFSW_SERVICES_PARAMETER_INTERNAL_H
#define KFSW_SERVICES_PARAMETER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/parameter.h>

#define KFSW_PARAM_FLAG_READ_ONLY 0x00000001UL
#define KFSW_PARAM_FLAG_CONFIGURATION 0x00000004UL
#define KFSW_PARAM_FLAG_SYSTEM_INFO 0x00000040UL
#define KFSW_PARAM_FLAG_DEBUG 0x00000200UL

#if CONFIG_KFSW_CSP
#define KFSW_PARAM_NODE_ID_DEFAULT CONFIG_KFSW_CSP_ADDRESS
#else
#define KFSW_PARAM_NODE_ID_DEFAULT 0
#endif

#define KFSW_PARAM_TABLE(ENTRY)                                                                    \
	ENTRY(0, node_id, KFSW_PARAM_U16, u16, uint16_t, KFSW_PARAM_NODE_ID_DEFAULT,               \
	      KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_SYSTEM_INFO, NONE,                       \
	      "Build-time CSP node address")                                                       \
	ENTRY(1, log_level, KFSW_PARAM_U8, u8, uint8_t, CONFIG_KFSW_LOG_MIN_LEVEL,                 \
	      KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT, LOG_LEVEL,               \
	      "Runtime logging policy value")                                                      \
	ENTRY(2, test_u32, KFSW_PARAM_U32, u32, uint32_t, 42U,                                     \
	      KFSW_PARAM_FLAG_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NONE,                            \
	      "Writable unsigned integration value")                                               \
	ENTRY(3, test_i32, KFSW_PARAM_I32, i32, int32_t, -7,                                       \
	      KFSW_PARAM_FLAG_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NONE,                            \
	      "Writable signed integration value")                                                 \
	ENTRY(4, test_float, KFSW_PARAM_FLOAT, f32, float, 1.5F,                                   \
	      KFSW_PARAM_FLAG_DEBUG | KFSW_PARAM_FLAG_PERSISTENT, NONE,                            \
	      "Writable floating-point integration value")

struct kfsw_param_entry {
	struct kfsw_param_info info;
	void *value;
	union kfsw_param_scalar default_value;
	void (*changed)(void);
};

#define KFSW_PARAM_DECLARE_VALUE(id, name, type, member, c_type, default_value, flags, callback,   \
				 description)                                                      \
	extern c_type kfsw_param_value_##name;
KFSW_PARAM_TABLE(KFSW_PARAM_DECLARE_VALUE)
#undef KFSW_PARAM_DECLARE_VALUE

void kfsw_param_table_lock(void);
void kfsw_param_table_unlock(void);

size_t kfsw_param_entry_count(void);
const struct kfsw_param_entry *kfsw_param_entry_at(size_t index);
const struct kfsw_param_entry *kfsw_param_find_id(uint16_t id);
const struct kfsw_param_entry *kfsw_param_find_name(const char *name);
int kfsw_param_read_entry(const struct kfsw_param_entry *entry, struct kfsw_param_value *value);
void kfsw_param_write_entry(const struct kfsw_param_entry *entry,
			    const union kfsw_param_scalar *value);
void kfsw_param_value_changed(uint16_t id);

#endif
