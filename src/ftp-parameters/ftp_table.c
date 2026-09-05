#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/ftp.h>
#include <kfsw/services/parameter.h>

static uint32_t ftp_transfers;
static uint32_t ftp_failures;
static uint32_t ftp_bytes;
static uint32_t ftp_timeout_ms = CONFIG_KFSW_FTP_TIMEOUT_MS;
static uint16_t ftp_chunk_size = KFSW_FTP_CHUNK_SIZE;
static uint8_t ftp_busy;
static char ftp_root[] = KFSW_FTP_STORAGE_ROOT;

static void sample_stats(void)
{
	struct kfsw_ftp_stats stats;

	if (kfsw_ftp_get_stats(&stats) != 0) {
		return;
	}
	ftp_transfers = stats.transfers;
	ftp_failures = stats.failures;
	ftp_bytes = stats.bytes;
	ftp_busy = stats.busy ? 1U : 0U;
}

#define FTP_SAMPLE(field, type)                                                                    \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_stats();                                                                    \
		*(type *)value = ftp_##field;                                                      \
	}

FTP_SAMPLE(transfers, uint32_t)
FTP_SAMPLE(failures, uint32_t)
FTP_SAMPLE(bytes, uint32_t)
FTP_SAMPLE(busy, uint8_t)

static void sample_timeout(void *value)
{
	*(uint32_t *)value = kfsw_ftp_get_timeout_ms();
}

static void sample_chunk_size(void *value)
{
	*(uint16_t *)value = kfsw_ftp_get_chunk_size();
}

/* The service owns both decisions. Refusing happens in the validator, before
 * the value is stored, so a rejected setting is reported as rejected rather
 * than written and quietly undone.
 */
static int validate_timeout(const union kfsw_param_scalar *value)
{
	return kfsw_ftp_check_timeout_ms(value->u32);
}

static void apply_timeout(const union kfsw_param_scalar *value)
{
	(void)kfsw_ftp_set_timeout_ms(value->u32);
}

static int validate_chunk_size(const union kfsw_param_scalar *value)
{
	return kfsw_ftp_check_chunk_size(value->u16);
}

static void apply_chunk_size(const union kfsw_param_scalar *value)
{
	(void)kfsw_ftp_set_chunk_size(value->u16);
}

static const struct kfsw_param_definition ftp_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "ftp_transfers",
		.description = "Transfers that committed, in either direction",
		.value = &ftp_transfers,
		.sample = sample_transfers,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "ftp_failures",
		.description = "Transfers that failed or were abandoned",
		.value = &ftp_failures,
		.sample = sample_failures,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "ftp_bytes",
		.unit = "B",
		.description = "Bytes moved by transfers that committed",
		.value = &ftp_bytes,
		.sample = sample_bytes,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "ftp_timeout_ms",
		.unit = "ms",
		.description = "Reply timeout for new transfers; a running one keeps its value",
		.value = &ftp_timeout_ms,
		.default_value = {.u32 = CONFIG_KFSW_FTP_TIMEOUT_MS},
		.validate = validate_timeout,
		.changed = apply_timeout,
		.sample = sample_timeout,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "ftp_chunk_size",
		.unit = "B",
		.description = "File data per message; can only be shortened",
		.value = &ftp_chunk_size,
		.default_value = {.u16 = KFSW_FTP_CHUNK_SIZE},
		.validate = validate_chunk_size,
		.changed = apply_chunk_size,
		.sample = sample_chunk_size,
	},
	{
		.offset = 0x12U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "ftp_busy",
		.description = "Whether the server is handling a transfer",
		.value = &ftp_busy,
		.sample = sample_busy,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_STRING,
		.capacity = sizeof(ftp_root),
		/* Read-only: the path resolver takes the root's length from a
		 * compile-time literal, and changing it under a running
		 * transfer is not a failure worth having. */
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "ftp_root",
		.description = "Sandbox the service resolves every path against",
		.value = ftp_root,
		.default_text = KFSW_FTP_STORAGE_ROOT,
	},
};

const struct kfsw_param_definition_set kfsw_ftp_param_definitions = {
	.table = KFSW_FTP_PARAM_TABLE_ID,
	.name = KFSW_FTP_PARAM_TABLE_NAME,
	.definitions = ftp_param_definitions,
	.count = ARRAY_SIZE(ftp_param_definitions),
};
