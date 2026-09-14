#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <kfsw/services/ftp.h>

#include "ftp_internal.h"

/*
 * Counters and settings that can be changed from the ground.
 */

static atomic_t ftp_transfers;
static atomic_t ftp_failures;
static atomic_t ftp_bytes;

static uint32_t ftp_timeout_ms = CONFIG_KFSW_FTP_TIMEOUT_MS;
static uint16_t ftp_chunk_size = KFSW_FTP_CHUNK_SIZE;

void kfsw_ftp_count_transfer(uint32_t bytes, bool failed)
{
	if (failed) {
		(void)atomic_inc(&ftp_failures);
		return;
	}
	(void)atomic_inc(&ftp_transfers);
	(void)atomic_add(&ftp_bytes, (atomic_val_t)bytes);
}

int kfsw_ftp_get_stats(struct kfsw_ftp_stats *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}

	stats->transfers = (uint32_t)atomic_get(&ftp_transfers);
	stats->failures = (uint32_t)atomic_get(&ftp_failures);
	stats->bytes = (uint32_t)atomic_get(&ftp_bytes);
	stats->busy = kfsw_ftp_server_is_busy();
	return 0;
}

uint32_t kfsw_ftp_get_timeout_ms(void)
{
	return ftp_timeout_ms;
}

int kfsw_ftp_check_timeout_ms(uint32_t timeout_ms)
{
	/* Same range as Kconfig. Checked separately because a change callback
	 * can't refuse a value.
	 */
	if ((timeout_ms < KFSW_FTP_TIMEOUT_MIN_MS) || (timeout_ms > KFSW_FTP_TIMEOUT_MAX_MS)) {
		return -ERANGE;
	}
	return 0;
}

int kfsw_ftp_set_timeout_ms(uint32_t timeout_ms)
{
	int result = kfsw_ftp_check_timeout_ms(timeout_ms);

	if (result != 0) {
		return result;
	}
	ftp_timeout_ms = timeout_ms;
	return 0;
}

uint16_t kfsw_ftp_get_chunk_size(void)
{
	return ftp_chunk_size;
}

int kfsw_ftp_check_chunk_size(uint16_t chunk_size)
{
	/* The buffer is sized at build time, so a larger chunk is refused here. */
	if ((chunk_size == 0U) || (chunk_size > KFSW_FTP_CHUNK_SIZE)) {
		return -ERANGE;
	}
	return 0;
}

int kfsw_ftp_set_chunk_size(uint16_t chunk_size)
{
	int result = kfsw_ftp_check_chunk_size(chunk_size);

	if (result != 0) {
		return result;
	}
	ftp_chunk_size = chunk_size;
	return 0;
}
