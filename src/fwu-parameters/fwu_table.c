#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/fwu.h>
#include <kfsw/services/parameter.h>

/* Read-only throughout. The update state belongs to the update service: if an
 * operator could set it, an unverified image could be marked ready, which is
 * the one thing this service exists to prevent.
 */
static uint32_t fwu_total_size;
static uint32_t fwu_received;
static uint32_t fwu_expected_crc32;
static uint32_t fwu_actual_crc32;
static uint32_t fwu_started;
static uint32_t fwu_completed;
static uint32_t fwu_failed;
static uint8_t fwu_state;
static uint8_t fwu_swap_scheduled;
static uint8_t fwu_target_bound;

static void sample_status(void)
{
	struct kfsw_fwu_status status;

	if (kfsw_fwu_get_status(&status) != 0) {
		return;
	}
	fwu_total_size = status.total_size;
	fwu_received = status.received;
	fwu_expected_crc32 = status.expected_crc32;
	fwu_actual_crc32 = status.actual_crc32;
	fwu_started = status.started;
	fwu_completed = status.completed;
	fwu_failed = status.failed;
	fwu_state = status.state;
	fwu_swap_scheduled = status.swap_scheduled ? 1U : 0U;
	fwu_target_bound = status.target_bound ? 1U : 0U;
}

#define FWU_SAMPLE(field, type)                                                                    \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_status();                                                                   \
		*(type *)value = fwu_##field;                                                      \
	}

FWU_SAMPLE(total_size, uint32_t)
FWU_SAMPLE(received, uint32_t)
FWU_SAMPLE(expected_crc32, uint32_t)
FWU_SAMPLE(actual_crc32, uint32_t)
FWU_SAMPLE(started, uint32_t)
FWU_SAMPLE(completed, uint32_t)
FWU_SAMPLE(failed, uint32_t)
FWU_SAMPLE(state, uint8_t)
FWU_SAMPLE(swap_scheduled, uint8_t)
FWU_SAMPLE(target_bound, uint8_t)

static const struct kfsw_param_definition fwu_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_total_size",
		.unit = "B",
		.description = "Image size the sender declared",
		.value = &fwu_total_size,
		.sample = sample_total_size,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_received",
		.unit = "B",
		.description = "Bytes accepted so far",
		.value = &fwu_received,
		.sample = sample_received,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_expected_crc",
		.description = "CRC32 the sender declared; IEEE, not the CSP variant",
		.value = &fwu_expected_crc32,
		.sample = sample_expected_crc32,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_actual_crc",
		.description = "CRC32 accumulated over what was accepted",
		.value = &fwu_actual_crc32,
		.sample = sample_actual_crc32,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_started",
		.description = "Transfers begun since boot",
		.value = &fwu_started,
		.sample = sample_started,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_completed",
		.description = "Transfers that reached ready since boot",
		.value = &fwu_completed,
		.sample = sample_completed,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_failed",
		.description = "Transfers that failed or were aborted since boot",
		.value = &fwu_failed,
		.sample = sample_failed,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_state",
		.description = "idle, receiving, ready or failed",
		.value = &fwu_state,
		.sample = sample_state,
	},
	{
		.offset = 0x1dU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_swap_scheduled",
		.description = "Whether the bootloader accepted the image",
		.value = &fwu_swap_scheduled,
		.sample = sample_swap_scheduled,
	},
	{
		.offset = 0x1eU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fwu_target_bound",
		.description = "Whether a target partition was bound at build time",
		.value = &fwu_target_bound,
		.sample = sample_target_bound,
	},
};

const struct kfsw_param_definition_set kfsw_fwu_param_definitions = {
	.table = KFSW_FWU_PARAM_TABLE_ID,
	.name = KFSW_FWU_PARAM_TABLE_NAME,
	.definitions = fwu_param_definitions,
	.count = ARRAY_SIZE(fwu_param_definitions),
};
