#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <kfsw/services/ftp.h>

#include "ftp_internal.h"

/*
 * Counters and the handful of settings an operator may want to change from the
 * ground. They live with the service rather than with the parameter table that
 * publishes them: the service is what applies them, and a value nothing applies
 * is worse than one nobody can see.
 *
 * Outcomes were recorded as events and counted nowhere, which answers "what
 * happened" but not "how often". A pass is short, and reading a ring to find
 * out whether transfers are succeeding costs more of it than reading a number.
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
	/* The bound matches the Kconfig range, so a value accepted here is one
	 * the composition could have been built with.
	 *
	 * Separate from applying it because a change callback cannot refuse: by
	 * the time one runs the value is already stored, and undoing it
	 * afterwards still reports success for something rejected.
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
	/* The workspace buffer is sized at build time and the protocol codec
	 * refuses anything larger, so a bigger value would be accepted here and
	 * rejected at the first transfer. Refuse it where it can be explained.
	 */
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
