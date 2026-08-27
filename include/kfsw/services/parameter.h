#ifndef KFSW_SERVICES_PARAMETER_H
#define KFSW_SERVICES_PARAMETER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum kfsw_param_type {
	KFSW_PARAM_U8,
	KFSW_PARAM_U16,
	KFSW_PARAM_U32,
	KFSW_PARAM_U64,
	KFSW_PARAM_I8,
	KFSW_PARAM_I16,
	KFSW_PARAM_I32,
	KFSW_PARAM_I64,
	KFSW_PARAM_X8,
	KFSW_PARAM_X16,
	KFSW_PARAM_X32,
	KFSW_PARAM_X64,
	KFSW_PARAM_FLOAT,
	KFSW_PARAM_DOUBLE,
	KFSW_PARAM_STRING,
	KFSW_PARAM_DATA,
	KFSW_PARAM_INVALID,
};

union kfsw_param_scalar {
	uint8_t u8;
	uint16_t u16;
	uint32_t u32;
	uint64_t u64;
	int8_t i8;
	int16_t i16;
	int32_t i32;
	int64_t i64;
	float f32;
	double f64;
};

struct kfsw_param_value {
	enum kfsw_param_type type;
	size_t size;
	union kfsw_param_scalar scalar;
};

struct kfsw_param_info {
	uint16_t node;
	uint16_t id;
	uint16_t array_size;
	enum kfsw_param_type type;
	uint32_t flags;
	const char *name;
	const char *unit;
	const char *description;
	bool read_only;
};

/** K-FSW-owned libparam user flag selecting values for persistence. */
#define KFSW_PARAM_FLAG_PERSISTENT 0x00010000UL

typedef bool (*kfsw_param_visitor_t)(const struct kfsw_param_info *info, void *context);

/** Validate and enable the statically linked local parameter table. */
int kfsw_param_init(void);

/** Return whether the local parameter table was initialized successfully. */
bool kfsw_param_is_initialized(void);

/** Read a scalar local parameter by name. */
int kfsw_param_get(const char *name, struct kfsw_param_value *value);

/** Write a scalar local parameter after exact type/size validation. */
int kfsw_param_set(const char *name, const struct kfsw_param_value *value);

/** Visit local parameter descriptions. */
int kfsw_param_visit(kfsw_param_visitor_t visitor, void *context);

#if CONFIG_KFSW_PARAM_PERSISTENCE
/** Save all explicitly persistent local parameters as one atomic snapshot. */
int kfsw_param_persist_save(void);

/** Load a valid snapshot, ignoring unknown names and incompatible entries. */
int kfsw_param_persist_load(void);

/** Delete the active persistent snapshot and any abandoned temporary file. */
int kfsw_param_persist_clear(void);

/** Restore persistent parameters to their compiled defaults in RAM only. */
int kfsw_param_restore_defaults(void);
#endif

/** Register the CSP parameter and parameter-list endpoints once. */
int kfsw_param_server_start(void);

/** Download and cache a node's upstream version 3 parameter list. */
int kfsw_param_remote_refresh(uint16_t node);

/** Read a scalar parameter from a selected CSP node. */
int kfsw_param_remote_get(uint16_t node, const char *name, struct kfsw_param_value *value);

/** Write a scalar parameter on a selected CSP node. */
int kfsw_param_remote_set(uint16_t node, const char *name, const struct kfsw_param_value *value);

/** Visit the cached parameter descriptions for a selected CSP node. */
int kfsw_param_remote_visit(uint16_t node, kfsw_param_visitor_t visitor, void *context);

/** Return a stable printable name for a K-FSW parameter type. */
const char *kfsw_param_type_name(enum kfsw_param_type type);

#ifdef __cplusplus
}
#endif

#endif
