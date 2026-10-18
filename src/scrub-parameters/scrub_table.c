#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>
#include <kfsw/services/scrub.h>

/* The references are the service's own, persisted through this table: a
 * reference held only in RAM would be relearned at every boot.
 */
extern uint32_t kfsw_scrub_references[KFSW_SCRUB_REGIONS];

static uint32_t scrub_measured[KFSW_SCRUB_REGIONS];
static uint32_t scrub_bytes[KFSW_SCRUB_REGIONS];
static uint8_t scrub_state[KFSW_SCRUB_REGIONS];
static uint32_t scrub_sweeps;
static uint32_t scrub_mismatches;
static uint32_t scrub_last_ms;
static uint8_t scrub_regions;
static uint8_t scrub_running;

static void sample_state(void)
{
	struct kfsw_scrub_status status;

	kfsw_scrub_get_status(&status);
	scrub_sweeps = status.sweeps;
	scrub_mismatches = status.mismatches;
	scrub_last_ms = status.last_sweep_ms;
	scrub_regions = status.regions;
	scrub_running = status.running ? 1U : 0U;

	for (unsigned int id = 0U; id < KFSW_SCRUB_REGIONS; id++) {
		struct kfsw_scrub_result result;

		if (kfsw_scrub_get_result((enum kfsw_scrub_region)id, &result) != 0) {
			continue;
		}
		scrub_measured[id] = result.measured;
		scrub_bytes[id] = result.bytes;
		scrub_state[id] = result.state;
	}
}

static void sample_measured_slot0(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_measured[KFSW_SCRUB_SLOT0];
}

static void sample_measured_slot1(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_measured[KFSW_SCRUB_SLOT1];
}

static void sample_measured_golden(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_measured[KFSW_SCRUB_GOLDEN];
}

static void sample_bytes_slot0(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_bytes[KFSW_SCRUB_SLOT0];
}

static void sample_bytes_slot1(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_bytes[KFSW_SCRUB_SLOT1];
}

static void sample_bytes_golden(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_bytes[KFSW_SCRUB_GOLDEN];
}

static void sample_state_slot0(void *value)
{
	sample_state();
	*(uint8_t *)value = scrub_state[KFSW_SCRUB_SLOT0];
}

static void sample_state_slot1(void *value)
{
	sample_state();
	*(uint8_t *)value = scrub_state[KFSW_SCRUB_SLOT1];
}

static void sample_state_golden(void *value)
{
	sample_state();
	*(uint8_t *)value = scrub_state[KFSW_SCRUB_GOLDEN];
}

static void sample_sweeps(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_sweeps;
}

static void sample_mismatches(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_mismatches;
}

static void sample_last_ms(void *value)
{
	sample_state();
	*(uint32_t *)value = scrub_last_ms;
}

static void sample_regions(void *value)
{
	sample_state();
	*(uint8_t *)value = scrub_regions;
}

static void sample_running(void *value)
{
	sample_state();
	*(uint8_t *)value = scrub_running;
}

/* Offsets are fixed per region, so a build without one does not move the rest. */
static const struct kfsw_param_definition scrub_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_PERSISTENT,
		.name = "slot0_reference",
		.description = "Learned CRC32 of the primary slot image; 0 unlearned",
		.value = &kfsw_scrub_references[KFSW_SCRUB_SLOT0],
		.default_value = {.u32 = 0U},
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot0_measured",
		.description = "CRC32 the last sweep read from the primary slot",
		.value = &scrub_measured[KFSW_SCRUB_SLOT0],
		.default_value = {.u32 = 0U},
		.sample = sample_measured_slot0,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot0_bytes",
		.description = "Image bytes swept in the primary slot",
		.value = &scrub_bytes[KFSW_SCRUB_SLOT0],
		.default_value = {.u32 = 0U},
		.sample = sample_bytes_slot0,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot0_state",
		.description = "0 empty, 1 unknown, 2 ok, 3 mismatch, 4 unreadable",
		.value = &scrub_state[KFSW_SCRUB_SLOT0],
		.default_value = {.u8 = 0U},
		.sample = sample_state_slot0,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_PERSISTENT,
		.name = "slot1_reference",
		.description = "Learned CRC32 of the secondary slot image; 0 unlearned",
		.value = &kfsw_scrub_references[KFSW_SCRUB_SLOT1],
		.default_value = {.u32 = 0U},
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot1_measured",
		.description = "CRC32 the last sweep read from the secondary slot",
		.value = &scrub_measured[KFSW_SCRUB_SLOT1],
		.default_value = {.u32 = 0U},
		.sample = sample_measured_slot1,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot1_bytes",
		.description = "Image bytes swept in the secondary slot",
		.value = &scrub_bytes[KFSW_SCRUB_SLOT1],
		.default_value = {.u32 = 0U},
		.sample = sample_bytes_slot1,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "slot1_state",
		.description = "0 empty, 1 unknown, 2 ok, 3 mismatch, 4 unreadable",
		.value = &scrub_state[KFSW_SCRUB_SLOT1],
		.default_value = {.u8 = 0U},
		.sample = sample_state_slot1,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_PERSISTENT,
		.name = "golden_reference",
		.description = "Learned CRC32 of the recovery image; 0 unlearned",
		.value = &kfsw_scrub_references[KFSW_SCRUB_GOLDEN],
		.default_value = {.u32 = 0U},
	},
	{
		.offset = 0x24U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "golden_measured",
		.description = "CRC32 the last sweep read from the recovery image",
		.value = &scrub_measured[KFSW_SCRUB_GOLDEN],
		.default_value = {.u32 = 0U},
		.sample = sample_measured_golden,
	},
	{
		.offset = 0x28U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "golden_bytes",
		.description = "Image bytes swept in the recovery partition",
		.value = &scrub_bytes[KFSW_SCRUB_GOLDEN],
		.default_value = {.u32 = 0U},
		.sample = sample_bytes_golden,
	},
	{
		.offset = 0x2cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "golden_state",
		.description = "0 empty, 1 unknown, 2 ok, 3 mismatch, 4 unreadable",
		.value = &scrub_state[KFSW_SCRUB_GOLDEN],
		.default_value = {.u8 = 0U},
		.sample = sample_state_golden,
	},
	{
		.offset = 0x30U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "scrub_sweeps",
		.description = "Sweeps completed since start; saturates",
		.value = &scrub_sweeps,
		.default_value = {.u32 = 0U},
		.sample = sample_sweeps,
	},
	{
		.offset = 0x34U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "scrub_mismatches",
		.description = "Regions found changed, counted once per crossing",
		.value = &scrub_mismatches,
		.default_value = {.u32 = 0U},
		.sample = sample_mismatches,
	},
	{
		.offset = 0x38U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "scrub_last_ms",
		.description = "Monotonic ms at the end of the last sweep",
		.value = &scrub_last_ms,
		.default_value = {.u32 = 0U},
		.sample = sample_last_ms,
	},
	{
		.offset = 0x3cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "scrub_regions",
		.description = "Regions this build can sweep",
		.value = &scrub_regions,
		.default_value = {.u8 = 0U},
		.sample = sample_regions,
	},
	{
		.offset = 0x3dU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "scrub_running",
		.description = "Whether the periodic sweep is running",
		.value = &scrub_running,
		.default_value = {.u8 = 0U},
		.sample = sample_running,
	},
};

KFSW_PARAM_TABLE_FITS_THE_LINK(scrub_param_definitions);

const struct kfsw_param_definition_set kfsw_scrub_param_definitions = {
	.table = KFSW_SCRUB_PARAM_TABLE_ID,
	.name = KFSW_SCRUB_PARAM_TABLE_NAME,
	.description = "Resident image checksums and their references",
	.definitions = scrub_param_definitions,
	.count = ARRAY_SIZE(scrub_param_definitions),
};
