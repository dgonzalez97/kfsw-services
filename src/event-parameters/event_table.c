#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/event.h>
#include <kfsw/services/parameter.h>

/* Everything the event record already counts, published so it can be read from
 * the ground without pulling the records themselves. `overwritten` is the one
 * worth watching: a record that quietly discards is not a record, and the only
 * way to know it wrapped is to count what it lost.
 */
static uint32_t event_recorded;
static uint32_t event_rejected;
static uint32_t event_overwritten;
static uint16_t event_held;
static uint16_t event_capacity;

static void sample_stats(void)
{
	struct kfsw_event_stats stats;

	kfsw_event_get_stats(&stats);
	event_recorded = stats.recorded;
	event_rejected = stats.rejected;
	event_overwritten = stats.overwritten;
	event_held = stats.held;
	event_capacity = stats.capacity;
}

static void sample_recorded(void *value)
{
	sample_stats();
	*(uint32_t *)value = event_recorded;
}

static void sample_rejected(void *value)
{
	sample_stats();
	*(uint32_t *)value = event_rejected;
}

static void sample_overwritten(void *value)
{
	sample_stats();
	*(uint32_t *)value = event_overwritten;
}

static void sample_held(void *value)
{
	sample_stats();
	*(uint16_t *)value = event_held;
}

static void sample_capacity(void *value)
{
	sample_stats();
	*(uint16_t *)value = event_capacity;
}

static const struct kfsw_param_definition event_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "events_recorded",
		.description = "Events accepted since start",
		.value = &event_recorded,
		.sample = sample_recorded,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "events_overwritten",
		.description = "Events lost to the ring wrapping before they were read",
		.value = &event_overwritten,
		.sample = sample_overwritten,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "events_rejected",
		.description = "Events refused for an invalid payload size or source",
		.value = &event_rejected,
		.sample = sample_rejected,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "events_held",
		.description = "Records currently in the ring",
		.value = &event_held,
		.sample = sample_held,
	},
	{
		.offset = 0x0eU,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "events_capacity",
		.description = "Ring capacity",
		.value = &event_capacity,
		.sample = sample_capacity,
	},
};

const struct kfsw_param_definition_set kfsw_event_param_definitions = {
	.table = KFSW_EVENT_PARAM_TABLE_ID,
	.name = KFSW_EVENT_PARAM_TABLE_NAME,
	.definitions = event_param_definitions,
	.count = ARRAY_SIZE(event_param_definitions),
};
