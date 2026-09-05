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
	/**
	 * A fixed-length array of bytes. Useful where one setting is really a
	 * value per something -- a log level per module, say -- and publishing
	 * one parameter per element would make the table impossible to read
	 * and its offsets impossible to keep stable.
	 */
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

/**
 * Longest string parameter, including the terminator.
 *
 * Bounded rather than allocated: a value travels on the caller's stack, and a
 * parameter service that allocated would have to fail at the worst moment.
 */
#define KFSW_PARAM_STRING_MAX CONFIG_KFSW_PARAM_STRING_MAX

struct kfsw_param_value {
	enum kfsw_param_type type;
	/** Bytes carried: the scalar width, or the string length with its terminator. */
	size_t size;
	union kfsw_param_scalar scalar;
	union {
		/** Value for KFSW_PARAM_STRING; always terminated. */
		char text[KFSW_PARAM_STRING_MAX];
		/** Value for KFSW_PARAM_DATA; size gives the element count. */
		uint8_t bytes[KFSW_PARAM_STRING_MAX];
	};
};

/**
 * Longest parameter name kept, excluding the terminator.
 *
 * The table already says which component owns the parameter, so a name does
 * not need to repeat it. Thirty-two characters is what the listing column is
 * sized for; a longer name is refused at registration rather than truncated,
 * because a truncated name is a name nobody can address.
 */
#define KFSW_PARAM_NAME_MAX 32U

/**
 * Table identifier bands.
 *
 * A parameter is addressed by table and offset, not by a flat identifier. The
 * band a table sits in says who owns it, so two independently developed
 * components cannot be given the same table by accident.
 *
 * Zero is reserved and never valid, so an uninitialised field cannot address a
 * real table.
 */
#define KFSW_PARAM_TABLE_INVALID 0U
/** First and last table owned by the composition, platform or comms layers. */
#define KFSW_PARAM_TABLE_CORE_FIRST 1U
#define KFSW_PARAM_TABLE_CORE_LAST 24U
/** First and last table owned by a service. */
#define KFSW_PARAM_TABLE_SERVICE_FIRST 25U
#define KFSW_PARAM_TABLE_SERVICE_LAST 49U
/** First and last table owned by a module. */
#define KFSW_PARAM_TABLE_MODULE_FIRST 50U
#define KFSW_PARAM_TABLE_MODULE_LAST 99U

/** Largest offset addressable within one table. */
#define KFSW_PARAM_OFFSET_MAX 255U

struct kfsw_param_info {
	uint16_t node;
	/**
	 * Wire identifier: the table in the high byte and the offset in the
	 * low byte. Unique across the node, which is what the libcsp parameter
	 * list requires, while remaining decodable back to table and offset.
	 */
	uint16_t id;
	/** Owning table identifier. */
	uint8_t table;
	/** Byte offset of this parameter within its table. */
	uint8_t offset;
	uint16_t array_size;
	enum kfsw_param_type type;
	uint32_t flags;
	/** Table name, or NULL for a remote parameter whose table is unnamed. */
	const char *table_name;
	const char *name;
	const char *unit;
	const char *description;
	bool read_only;
};

/** One table as a whole. */
struct kfsw_param_table_info {
	/** Table identifier, 1..99. */
	uint8_t id;
	/** Stable lowercase table name. */
	const char *name;
	/** Parameters registered in this table. */
	uint16_t count;
};

/** Parameter cannot be changed through local or remote set operations. */
#define KFSW_PARAM_FLAG_READ_ONLY 0x00000001UL
/** Parameter controls operator-selected configuration. */
#define KFSW_PARAM_FLAG_CONFIGURATION 0x00000004UL
/** Parameter reports build or runtime system identity. */
#define KFSW_PARAM_FLAG_SYSTEM_INFO 0x00000040UL
/** Parameter exists for diagnostic or test behavior. */
#define KFSW_PARAM_FLAG_DEBUG 0x00000200UL
/** K-FSW-owned user flag selecting local values for persistence. */
#define KFSW_PARAM_FLAG_PERSISTENT 0x00010000UL
/**
 * K-FSW-owned user flag: a write takes effect immediately.
 *
 * Always set by the service for a definition that supplies a change callback,
 * so a parameter that applies its value cannot be reported as needing a reboot.
 * An owner that applies the value simply by reading it on every cycle, and so
 * needs no callback, may set the flag itself.
 *
 * It travels in the wire mask, which is what lets a remote listing report the
 * same write behaviour as a local one.
 */
#define KFSW_PARAM_FLAG_LIVE 0x00020000UL

/** Validate a proposed scalar value; return zero to accept it. */
typedef int (*kfsw_param_validator_t)(const union kfsw_param_scalar *value);
/**
 * Validate a proposed string value; return zero to accept it.
 *
 * Separate from the scalar validator because a string cannot be passed through
 * the scalar union, and an owner that could not refuse a malformed string --
 * a route table, say -- would store one that the next boot cannot parse.
 */
typedef int (*kfsw_param_text_validator_t)(const char *text);
/**
 * Validate a proposed byte array; return zero to accept it.
 *
 * Given the whole array rather than one element, because the values in an
 * array are usually only sensible together.
 */
typedef int (*kfsw_param_data_validator_t)(const uint8_t *data, size_t size);
/** Apply owner behavior after the backing scalar changes. */
typedef void (*kfsw_param_changed_t)(const union kfsw_param_scalar *value);
/** Apply owner behavior after the backing string changes. */
typedef void (*kfsw_param_text_changed_t)(const char *text);
/** Apply owner behavior after the backing byte array changes. */
typedef void (*kfsw_param_data_changed_t)(const uint8_t *data, size_t size);
/**
 * Refresh backing storage from live state immediately before it is read.
 *
 * Needed because a reported value is only worth reading if it is current: an
 * uptime refreshed on a timer is wrong by up to one period every time it is
 * asked for. Runs while PARAM serializes access, so it must not call back into
 * the parameter API.
 */
typedef void (*kfsw_param_sample_t)(void *value);

/**
 * One scalar parameter declaration owned by a component.
 *
 * The definition, its strings, and its writable value storage must remain
 * valid for the service lifetime. Defaults are copied into value storage at
 * successful initialization. Validator and change callbacks run while PARAM
 * serializes access, so they must not call back into the parameter API.
 */
struct kfsw_param_definition {
	/** Byte offset within the owning table; unique inside that table. */
	uint8_t offset;
	enum kfsw_param_type type;
	/**
	 * Storage in bytes: for a KFSW_PARAM_STRING the capacity with its
	 * terminator, for a KFSW_PARAM_DATA the element count. Left zero for a
	 * scalar, whose width comes from its type.
	 */
	uint16_t capacity;
	uint32_t flags;
	const char *name;
	const char *unit;
	const char *description;
	void *value;
	union kfsw_param_scalar default_value;
	/** Compiled default for a KFSW_PARAM_STRING; NULL means empty. */
	const char *default_text;
	kfsw_param_validator_t validate;
	kfsw_param_changed_t changed;
	kfsw_param_text_validator_t validate_text;
	kfsw_param_text_changed_t changed_text;
	kfsw_param_data_validator_t validate_data;
	kfsw_param_data_changed_t changed_data;
	/** Compiled default for a KFSW_PARAM_DATA; NULL means all zero. */
	const uint8_t *default_data;
	kfsw_param_sample_t sample;
};

/**
 * One parameter table: a compile-time group of definitions with one owner.
 *
 * The set and the table are the same thing on purpose. A table is owned by
 * exactly one component -- the one that can validate and apply its values --
 * so there is nothing for a second grouping to express.
 */
struct kfsw_param_definition_set {
	/** Table identifier; must fall in the band belonging to the owner. */
	uint8_t table;
	/** Stable lowercase table name, at most KFSW_PARAM_NAME_MAX. */
	const char *name;
	const struct kfsw_param_definition *definitions;
	size_t count;
};

typedef bool (*kfsw_param_visitor_t)(const struct kfsw_param_info *info, void *context);
typedef bool (*kfsw_param_table_visitor_t)(const struct kfsw_param_table_info *info, void *context);

/** Aggregate, validate, and enable the supplied component definition sets. */
int kfsw_param_init(const struct kfsw_param_definition_set *const *sets, size_t set_count);

/** Return whether the local parameter table was initialized successfully. */
bool kfsw_param_is_initialized(void);

/** Read a scalar local parameter by name. */
int kfsw_param_get(const char *name, struct kfsw_param_value *value);

/** Write a scalar local parameter after exact type/size validation. */
int kfsw_param_set(const char *name, const struct kfsw_param_value *value);

/**
 * @brief Read one local parameter's description by name.
 *
 * @param name Parameter name.
 * @param[out] info Destination description.
 *
 * @retval 0 The description was written.
 * @retval -EINVAL @p name or @p info is NULL.
 * @retval -EACCES The local table is not initialized.
 * @retval -ENOENT No parameter of that name is registered.
 */
int kfsw_param_get_info(const char *name, struct kfsw_param_info *info);

/** Visit local parameter descriptions, ordered by table then offset. */
int kfsw_param_visit(kfsw_param_visitor_t visitor, void *context);

/** Visit each registered local table once, in ascending identifier order. */
int kfsw_param_visit_tables(kfsw_param_table_visitor_t visitor, void *context);

/** Registered local tables. */
size_t kfsw_param_table_count(void);

/** What the parameter service knows about itself. */
struct kfsw_param_stats {
	/** Parameters registered across every table. */
	uint16_t count;
	/** Tables registered. */
	uint16_t tables;
	/** Of those parameters, the ones a snapshot carries. */
	uint16_t persistent;
	/** Snapshots written since boot. */
	uint32_t saves;
	/** Snapshot loads that failed for a reason other than absence. */
	uint32_t load_failures;
};

/** Read what the service knows about itself. -EINVAL for a NULL destination. */
int kfsw_param_get_stats(struct kfsw_param_stats *stats);

/** Parameter table owned by the service itself, in the service band. */
#define KFSW_PARAM_PARAM_TABLE_ID 26U
/** Stable logical name paired with KFSW_PARAM_PARAM_TABLE_ID. */
#define KFSW_PARAM_PARAM_TABLE_NAME "param"

/** The parameter service describing itself. */
extern const struct kfsw_param_definition_set kfsw_param_param_definitions;

/** Whether an accepted change to a persistent value writes a snapshot. */
bool kfsw_param_autosave_enabled(void);

/**
 * @brief Name of the ownership band a table identifier falls in.
 *
 * @return "core", "service", "module", or "invalid" for an unallocated value.
 */
const char *kfsw_param_band_name(uint8_t table);

/**
 * @brief Write behaviour implied by a parameter's flags.
 *
 * @return "r" read-only, "w" applied immediately, "b" stored until reboot,
 *         "wb" both.
 */
const char *kfsw_param_mode_name(uint32_t flags);

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

#if CONFIG_KFSW_PARAM_CSP
/** Register the optional CSP parameter and parameter-list endpoints once. */
int kfsw_param_server_start(void);

/** Download and cache a node's upstream version 3 parameter list. */
int kfsw_param_remote_refresh(uint16_t node);

/** Read a scalar parameter from a selected CSP node. */
int kfsw_param_remote_get(uint16_t node, const char *name, struct kfsw_param_value *value);

/** Write a scalar parameter on a selected CSP node. */
int kfsw_param_remote_set(uint16_t node, const char *name, const struct kfsw_param_value *value);

/** Visit the cached parameter descriptions for a selected CSP node. */
int kfsw_param_remote_visit(uint16_t node, kfsw_param_visitor_t visitor, void *context);
#endif

/** Return a stable printable name for a K-FSW parameter type. */
const char *kfsw_param_type_name(enum kfsw_param_type type);

#ifdef __cplusplus
}
#endif

#endif
