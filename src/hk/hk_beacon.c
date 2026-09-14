#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <csp/csp.h>

#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_BEACON

/* One unacknowledged sample per interval, with a CSP buffer reserve. */
struct beacon_state {
	uint16_t node;
	uint32_t interval_ms;
	int64_t next_uptime_ms;
};

static struct beacon_state beacons[CONFIG_KFSW_HK_REPORTS];
static uint32_t sent_count;
static uint32_t skipped_count;

static int set_beacon_impl(uint8_t report, uint16_t node, uint32_t interval_ms)
{
	if (report >= ARRAY_SIZE(beacons)) {
		return -EINVAL;
	}
	if (interval_ms != 0U && interval_ms < CONFIG_KFSW_HK_BEACON_FLOOR_MS) {
		return -ERANGE;
	}
	if (interval_ms != 0U && (node == 0U || node > 16383U)) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	beacons[report].node = node;
	beacons[report].interval_ms = interval_ms;
	beacons[report].next_uptime_ms = 0;
	kfsw_hk_unlock();
	kfsw_hk_wake();
	return 0;
}

int kfsw_hk_set_beacon(uint8_t report, uint16_t node, uint32_t interval_ms)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result
			   : kfsw_hk_config_end(set_beacon_impl(report, node, interval_ms));
}

void kfsw_hk_beacon_restore(uint8_t report, uint16_t node, uint32_t interval_ms)
{
	(void)set_beacon_impl(report, node, interval_ms);
}

int kfsw_hk_get_beacon(uint8_t report, uint16_t *node, uint32_t *interval_ms)
{
	if ((report >= ARRAY_SIZE(beacons)) || (node == NULL) || (interval_ms == NULL)) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	*node = beacons[report].node;
	*interval_ms = beacons[report].interval_ms;
	kfsw_hk_unlock();
	return 0;
}

void kfsw_hk_beacon_stats(uint32_t *sent, uint32_t *skipped)
{
	kfsw_hk_lock();
	if (sent != NULL) {
		*sent = sent_count;
	}
	if (skipped != NULL) {
		*skipped = skipped_count;
	}
	kfsw_hk_unlock();
}

void kfsw_hk_beacon_tick(uint8_t report, int64_t now)
{
	struct kfsw_hk_sample sample;
	csp_packet_t *packet;
	uint16_t node;

	if (report >= ARRAY_SIZE(beacons)) {
		return;
	}
	kfsw_hk_lock();
	struct beacon_state *beacon = &beacons[report];

	if (beacon->interval_ms == 0U || now < beacon->next_uptime_ms) {
		kfsw_hk_unlock();
		return;
	}
	node = beacon->node;
	beacon->next_uptime_ms = kfsw_hk_next_due(beacon->next_uptime_ms, beacon->interval_ms, now);
	kfsw_hk_unlock();

	if (kfsw_hk_get(report, 0U, &sample) != 0) {
		return;
	}

	/* Skip the beacon when too few buffers are free, and count it. */
	if (csp_buffer_remaining() <= CONFIG_KFSW_HK_BEACON_BUFFER_RESERVE) {
		kfsw_hk_lock();
		skipped_count++;
		kfsw_hk_unlock();
		return;
	}

	packet = csp_buffer_get(sample.length);
	if (packet == NULL) {
		kfsw_hk_lock();
		skipped_count++;
		kfsw_hk_unlock();
		return;
	}
	memcpy(packet->data, sample.data, sample.length);
	packet->length = sample.length;

	/* Use a separate destination port so a beacon cannot trigger a request. */
	csp_sendto(CSP_PRIO_LOW, node, CONFIG_KFSW_HK_BEACON_PORT, CONFIG_KFSW_HK_CSP_PORT,
		   CSP_O_CRC32, packet);
	kfsw_hk_lock();
	sent_count++;
	kfsw_hk_unlock();
}

/* Caller holds the HK state lock. */
int64_t kfsw_hk_beacon_wait(uint8_t report, int64_t now)
{
	return beacons[report].interval_ms == 0U ? 200
						 : MAX(0, beacons[report].next_uptime_ms - now);
}

#endif /* CONFIG_KFSW_HK_BEACON */
