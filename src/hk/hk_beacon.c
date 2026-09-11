#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <csp/csp.h>

#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_BEACON

/*
 * A node that speaks without being asked.
 *
 * Housekeeping answers requests, which is the right default and useless in the
 * first seconds of a pass: the ground has to find the node, ask, and wait a
 * round trip before it knows anything. A beacon puts the newest sample on the
 * link the moment the link exists.
 *
 * Deferred until now for a reason worth keeping in mind. A node transmitting
 * unprompted can flood a link and starve everything that shares it, so this
 * has a period floor it cannot go below, an enable an operator can take away,
 * and one rule that matters more than either: **a beacon never takes the last
 * buffer.** A reply somebody is waiting for outranks a broadcast nobody asked
 * for.
 *
 * Nothing on the wire changes. A beacon is the same frame the request path
 * sends, to the same port, so the ground decodes it with what it already has.
 */
struct beacon_state {
	uint16_t node;
	uint32_t interval_ms;
	int64_t next_uptime_ms;
};

static struct beacon_state beacons[CONFIG_KFSW_HK_REPORTS];
static uint32_t sent_count;
static uint32_t skipped_count;

int kfsw_hk_beacon_configure(uint8_t report, uint16_t node, uint32_t interval_ms)
{
	if (report >= ARRAY_SIZE(beacons)) {
		return -EINVAL;
	}
	if (interval_ms == 0U) {
		beacons[report].interval_ms = 0U;
		kfsw_log_info("HK: report %u stops beaconing", report);
		return 0;
	}
	if (interval_ms < CONFIG_KFSW_HK_BEACON_FLOOR_MS) {
		return -ERANGE;
	}
	if ((node == 0U) || (node > 16383U)) {
		return -EINVAL;
	}
	beacons[report].node = node;
	beacons[report].interval_ms = interval_ms;
	beacons[report].next_uptime_ms = 0;
	kfsw_log_info("HK: report %u beacons to node %u every %u ms", report, node, interval_ms);
	return 0;
}

int kfsw_hk_beacon_get(uint8_t report, uint16_t *node, uint32_t *interval_ms)
{
	if ((report >= ARRAY_SIZE(beacons)) || (node == NULL) || (interval_ms == NULL)) {
		return -EINVAL;
	}
	*node = beacons[report].node;
	*interval_ms = beacons[report].interval_ms;
	return 0;
}

void kfsw_hk_beacon_stats(uint32_t *sent, uint32_t *skipped)
{
	if (sent != NULL) {
		*sent = sent_count;
	}
	if (skipped != NULL) {
		*skipped = skipped_count;
	}
}

void kfsw_hk_beacon_tick(uint8_t report, int64_t now)
{
	struct kfsw_hk_sample sample;
	csp_packet_t *packet;

	if ((report >= ARRAY_SIZE(beacons)) || (beacons[report].interval_ms == 0U)) {
		return;
	}
	if (now < beacons[report].next_uptime_ms) {
		return;
	}
	/* Advanced before sending, so a slow link does not queue another. */
	beacons[report].next_uptime_ms = now + (int64_t)beacons[report].interval_ms;

	if (kfsw_hk_get(report, 0U, &sample) != 0) {
		return;
	}

	/* The rule that makes an unprompted transmitter safe to have: leave the
	 * pool with room for the traffic somebody is waiting on. Counted rather
	 * than logged, because a busy link would fill a pass with warnings.
	 */
	if (csp_buffer_remaining() <= CONFIG_KFSW_HK_BEACON_BUFFER_RESERVE) {
		skipped_count++;
		return;
	}

	packet = csp_buffer_get(sample.length);
	if (packet == NULL) {
		skipped_count++;
		return;
	}
	memcpy(packet->data, sample.data, sample.length);
	packet->length = sample.length;

	/* Connection-less: a beacon is one packet to an address that may not be
	 * listening, and holding a connection open for something nobody
	 * acknowledged is what a pass cannot afford.
	 *
	 * The two ports are chosen carefully and are not the same.
	 *
	 * The source is the housekeeping port, so a beacon looks exactly like
	 * the reply to a request. That is what makes "nothing on the ground
	 * changes" true: a listener recognises housekeeping by the port it
	 * came *from*, and a beacon sent from anywhere else would be ignored.
	 *
	 * The destination is a port nothing binds. Sending to the serving port
	 * would drop a beacon onto another node's request handler, where a
	 * frame whose first byte is also a version number could be read as a
	 * request — and two nodes beaconing at each other would then answer
	 * each other.
	 */
	csp_sendto(CSP_PRIO_LOW, beacons[report].node, CONFIG_KFSW_HK_BEACON_PORT,
		   CONFIG_KFSW_HK_CSP_PORT, CSP_O_CRC32, packet);
	sent_count++;
}

#endif /* CONFIG_KFSW_HK_BEACON */
