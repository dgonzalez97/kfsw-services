#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/health.h>
#include <kfsw/services/parameter.h>

#if CONFIG_KFSW_HEALTH_SLOTS
#include CONFIG_KFSW_HEALTH_SLOTS_HEADER
#endif

static char health_faulted_by[KFSW_HEALTH_NAME_SIZE];
static uint32_t health_faults;
static uint32_t health_feeds;
static uint16_t health_interval;
static uint8_t health_state;
static uint8_t health_feeding;
static uint8_t health_count;

static void sample_status(void)
{
	struct kfsw_health_status status;

	if (kfsw_health_get_status(&status) != 0) {
		return;
	}
	health_faults = status.faults;
	health_feeds = status.feeds;
	health_state = status.state;
	health_feeding = status.feeding ? 1U : 0U;
	health_count = status.count;
	for (size_t index = 0U; index < sizeof(health_faulted_by); index++) {
		health_faulted_by[index] = status.faulted_by[index];
	}
	health_faulted_by[sizeof(health_faulted_by) - 1U] = '\0';
}

#define HEALTH_SAMPLE(field, type)                                                                 \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_status();                                                                   \
		*(type *)value = health_##field;                                                   \
	}

HEALTH_SAMPLE(faults, uint32_t)
HEALTH_SAMPLE(feeds, uint32_t)
HEALTH_SAMPLE(state, uint8_t)
HEALTH_SAMPLE(feeding, uint8_t)
HEALTH_SAMPLE(count, uint8_t)

static void sample_faulted_by(void *value)
{
	sample_status();
	((char *)value)[0] = health_faulted_by[0];
	for (size_t index = 0U; index < KFSW_HEALTH_NAME_SIZE; index++) {
		((char *)value)[index] = health_faulted_by[index];
	}
}

static void sample_interval(void *value)
{
	*(uint16_t *)value = (uint16_t)kfsw_health_get_interval_ms();
}

/* Refuse an interval the watchdog can't tolerate before it is stored. */
static int validate_interval(const union kfsw_param_scalar *value)
{
	return kfsw_health_check_interval_ms(value->u16);
}

static void apply_interval(const union kfsw_param_scalar *value)
{
	(void)kfsw_health_set_interval_ms(value->u16);
}

#if CONFIG_KFSW_HEALTH_SLOTS
/* The parameters the table carried before the slots, and two per slot. */
#define HEALTH_AGGREGATES 7U
#define HEALTH_PER_SLOT 2U
#define HEALTH_PARAMETERS_MAX (KFSW_PARAM_TABLE_WIRE_MAX / KFSW_PARAM_WIRE_BYTES)
#define HEALTH_SLOT_MAX ((HEALTH_PARAMETERS_MAX - HEALTH_AGGREGATES) / HEALTH_PER_SLOT)

/* A slot's offsets follow from its index alone, so adding or removing another
 * slot never moves it.
 */
#define HEALTH_SLOT_BASE 0x20U
#define HEALTH_SLOT_STRIDE 0x08U
#define HEALTH_SLOT_SINCE_OFFSET 0x04U
#define HEALTH_SLOT_OFFSET(index) (HEALTH_SLOT_BASE + ((index) * HEALTH_SLOT_STRIDE))

#define HEALTH_SLOT_COUNT_ONE(slot, index) +1U
#define HEALTH_SLOT_COUNT (0U KFSW_HEALTH_SLOTS(HEALTH_SLOT_COUNT_ONE))
#define HEALTH_SLOT_PARAMETERS (HEALTH_PER_SLOT * HEALTH_SLOT_COUNT)

BUILD_ASSERT(HEALTH_SLOT_COUNT <= HEALTH_SLOT_MAX,
	     "table 31 carries at most 14 health slots on a 4800 baud link");

#define HEALTH_SLOT_CHECKS(slot, index)                                                            \
	BUILD_ASSERT(sizeof(#slot) <= KFSW_HEALTH_NAME_SIZE, "a health slot name is too long");    \
	BUILD_ASSERT((index) < HEALTH_SLOT_MAX, "a health slot index is 0 to 13");

KFSW_HEALTH_SLOTS(HEALTH_SLOT_CHECKS)

/* Each index and each name appears once here, so a repeat is a build error. */
#define HEALTH_SLOT_IDS(slot, index) HEALTH_SLOT_INDEX_##index, HEALTH_SLOT_NAME_##slot,

enum health_slot_ids { KFSW_HEALTH_SLOTS(HEALTH_SLOT_IDS) };

struct health_slot {
	const char *name;
	uint32_t since_ms;
	uint8_t state;
};

#define HEALTH_SLOT_STORAGE(slot, index) [index] = {.name = #slot},

static struct health_slot health_slots[HEALTH_SLOT_MAX] = {KFSW_HEALTH_SLOTS(HEALTH_SLOT_STORAGE)};

/* By name, so the order components registered in never decides the slot. */
static bool find_component(const char *name, struct kfsw_health_component *component)
{
	for (uint8_t index = 0U; index < KFSW_HEALTH_MAX_COMPONENTS; index++) {
		if (kfsw_health_get_component(index, component) != 0) {
			continue;
		}
		if (strncmp(component->name, name, KFSW_HEALTH_NAME_SIZE) == 0) {
			return true;
		}
	}
	return false;
}

static void sample_slot(struct health_slot *slot)
{
	struct kfsw_health_component component;

	if (!find_component(slot->name, &component)) {
		slot->state = (uint8_t)KFSW_HEALTH_SLOT_ABSENT;
		slot->since_ms = KFSW_HEALTH_SLOT_SINCE_ABSENT;
		return;
	}
	slot->since_ms = component.since_report_ms;
	if (component.overdue) {
		slot->state = (uint8_t)KFSW_HEALTH_SLOT_OVERDUE;
	} else {
		slot->state = (uint8_t)KFSW_HEALTH_SLOT_OK;
	}
}

/* The table samples into the value it publishes; the slot is the struct holding it. */
static void sample_slot_state(void *value)
{
	sample_slot(CONTAINER_OF(value, struct health_slot, state));
}

static void sample_slot_since(void *value)
{
	sample_slot(CONTAINER_OF(value, struct health_slot, since_ms));
}

#define HEALTH_SLOT_STATE(slot, index)                                                             \
	{                                                                                          \
		.offset = HEALTH_SLOT_OFFSET(index),                                               \
		.type = KFSW_PARAM_U8,                                                             \
		.flags = KFSW_PARAM_FLAG_READ_ONLY,                                                \
		.name = "health_" #slot "_state",                                                  \
		.description = "0 ok, 1 overdue, 2 absent",                                        \
		.value = &health_slots[index].state,                                               \
		.sample = sample_slot_state,                                                       \
	}

#define HEALTH_SLOT_SINCE(slot, index)                                                             \
	{                                                                                          \
		.offset = HEALTH_SLOT_OFFSET(index) + HEALTH_SLOT_SINCE_OFFSET,                    \
		.type = KFSW_PARAM_U32,                                                            \
		.flags = KFSW_PARAM_FLAG_READ_ONLY,                                                \
		.name = "health_" #slot "_since_ms",                                               \
		.unit = "ms",                                                                      \
		.description = "Since it last reported; 4294967295 when absent",                   \
		.value = &health_slots[index].since_ms,                                            \
		.sample = sample_slot_since,                                                       \
	}

#define HEALTH_SLOT_ROWS(slot, index)                                                              \
	HEALTH_SLOT_STATE(slot, index), HEALTH_SLOT_SINCE(slot, index),
#endif

static const struct kfsw_param_definition health_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_HEALTH_NAME_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_faulted_by",
		.description = "Component holding the system faulted, or empty",
		.value = health_faulted_by,
		.sample = sample_faulted_by,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_faults",
		.description = "Times the system was judged unwell since boot",
		.value = &health_faults,
		.sample = sample_faults,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_feeds",
		.description = "Watchdog feeds this service has issued",
		.value = &health_feeds,
		.sample = sample_feeds,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "health_interval_ms",
		.unit = "ms",
		.description = "How often deadlines are checked; refused above the feed interval",
		.value = &health_interval,
		.default_value = {.u16 = CONFIG_KFSW_HEALTH_INTERVAL_MS},
		.validate = validate_interval,
		.changed = apply_interval,
		.sample = sample_interval,
	},
	{
		.offset = 0x1aU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_state",
		.description = "ok, faulted or stopped",
		.value = &health_state,
		.sample = sample_state,
	},
	{
		.offset = 0x1bU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_feeding",
		.description = "Whether health is the one feeding the watchdog",
		.value = &health_feeding,
		.sample = sample_feeding,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_components",
		.description = "Components currently watched",
		.value = &health_count,
		.sample = sample_count,
	},
#if CONFIG_KFSW_HEALTH_SLOTS
	KFSW_HEALTH_SLOTS(HEALTH_SLOT_ROWS)
#endif
};

KFSW_PARAM_TABLE_FITS_THE_LINK(health_param_definitions);

#if CONFIG_KFSW_HEALTH_SLOTS
BUILD_ASSERT(ARRAY_SIZE(health_param_definitions) == HEALTH_AGGREGATES + HEALTH_SLOT_PARAMETERS,
	     "HEALTH_AGGREGATES must count the parameters ahead of the slots");
#endif

const struct kfsw_param_definition_set kfsw_health_param_definitions = {
	.table = KFSW_HEALTH_PARAM_TABLE_ID,
	.name = KFSW_HEALTH_PARAM_TABLE_NAME,
	.description = "Health monitor and watchdog feeds",
	.definitions = health_param_definitions,
	.count = ARRAY_SIZE(health_param_definitions),
};
