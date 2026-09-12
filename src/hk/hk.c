#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#if CONFIG_KFSW_CSP
#include <kfsw/comms/csp.h>
#endif
#include <kfsw/services/hk.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "hk_internal.h"

static struct kfsw_hk_report reports[CONFIG_KFSW_HK_REPORTS];
static struct kfsw_hk_stats stats;
static bool initialized;
static bool enabled = true;
static bool restoring;
static uint64_t config_revision;
#if CONFIG_KFSW_HK_PERSISTENCE
static bool save_blocked;
#endif
static K_MUTEX_DEFINE(config_lock);
static K_MUTEX_DEFINE(init_lock);

K_MUTEX_DEFINE(hk_lock);
static K_MUTEX_DEFINE(collection_lock);
static K_MUTEX_DEFINE(storage_lock);
static uint64_t generations[CONFIG_KFSW_HK_REPORTS];
static uint64_t schedule_revisions[CONFIG_KFSW_HK_REPORTS];

void kfsw_hk_storage_lock(void)
{
	k_mutex_lock(&storage_lock, K_FOREVER);
}

void kfsw_hk_storage_unlock(void)
{
	k_mutex_unlock(&storage_lock);
}

void kfsw_hk_lock(void)
{
	k_mutex_lock(&hk_lock, K_FOREVER);
}

void kfsw_hk_unlock(void)
{
	k_mutex_unlock(&hk_lock);
}

void kfsw_hk_count_overwritten(void)
{
	stats.overwritten++;
}

struct kfsw_hk_report *kfsw_hk_report_at(uint8_t report)
{
	if (report >= ARRAY_SIZE(reports)) {
		return NULL;
	}
	return &reports[report];
}

/*
 * A value's width on the wire.
 *
 * Fixed by the declaration rather than by what a particular sample happens to
 * hold, so every sample of a report has the same layout and ground can read
 * the tenth value without parsing the nine before it. A string that is shorter
 * than its capacity is padded, which costs bytes and buys a frame that can be
 * indexed.
 */
static size_t entry_width(enum kfsw_param_type type, uint16_t array_size)
{
	switch (type) {
	case KFSW_PARAM_U8:
	case KFSW_PARAM_I8:
	case KFSW_PARAM_X8:
		return 1U;
	case KFSW_PARAM_U16:
	case KFSW_PARAM_I16:
	case KFSW_PARAM_X16:
		return 2U;
	case KFSW_PARAM_U32:
	case KFSW_PARAM_I32:
	case KFSW_PARAM_X32:
	case KFSW_PARAM_FLOAT:
		return 4U;
	case KFSW_PARAM_U64:
	case KFSW_PARAM_I64:
	case KFSW_PARAM_X64:
	case KFSW_PARAM_DOUBLE:
		return 8U;
	case KFSW_PARAM_STRING:
	case KFSW_PARAM_DATA:
		return array_size;
	default:
		return 0U;
	}
}

/* Big-endian, like every other K-FSW protocol. */
void kfsw_hk_write_value(uint8_t *out, size_t width, const struct kfsw_param_value *value)
{
	memset(out, 0, width);

	switch (value->type) {
	case KFSW_PARAM_U8:
	case KFSW_PARAM_I8:
	case KFSW_PARAM_X8:
		out[0] = value->scalar.u8;
		break;
	case KFSW_PARAM_U16:
	case KFSW_PARAM_I16:
	case KFSW_PARAM_X16:
		sys_put_be16(value->scalar.u16, out);
		break;
	case KFSW_PARAM_U32:
	case KFSW_PARAM_I32:
	case KFSW_PARAM_X32:
		sys_put_be32(value->scalar.u32, out);
		break;
	case KFSW_PARAM_FLOAT: {
		uint32_t raw;

		memcpy(&raw, &value->scalar.f32, sizeof(raw));
		sys_put_be32(raw, out);
		break;
	}
	case KFSW_PARAM_U64:
	case KFSW_PARAM_I64:
	case KFSW_PARAM_X64:
		sys_put_be64(value->scalar.u64, out);
		break;
	case KFSW_PARAM_DOUBLE: {
		uint64_t raw;

		memcpy(&raw, &value->scalar.f64, sizeof(raw));
		sys_put_be64(raw, out);
		break;
	}
	case KFSW_PARAM_STRING:
	case KFSW_PARAM_DATA:
		memcpy(out, value->bytes, MIN(width, value->size));
		break;
	default:
		break;
	}
}

/*
 * The declared width of one entry, without reading it.
 *
 * Sizing a definition must not depend on a value being readable right now: a
 * remote node that is briefly quiet should not make a report undefinable, and
 * a report whose size depends on what a string happens to hold would change
 * shape between collections.
 */
struct width_search {
	uint16_t wanted;
	size_t width;
	bool found;
};

static bool match_width(const struct kfsw_param_info *info, void *context)
{
	struct width_search *search = context;

	if (info->id != search->wanted) {
		return true;
	}
	search->width = entry_width(info->type, info->array_size);
	search->found = true;
	return false;
}

static int entry_declared_width(const struct kfsw_hk_entry *entry, size_t *width)
{
	struct width_search search = {.wanted = entry->param_id};
	int result;

	if (entry->node == KFSW_HK_NODE_LOCAL) {
		result = kfsw_param_visit(match_width, &search);
	} else {
#if CONFIG_KFSW_PARAM_CSP
		result = kfsw_param_remote_visit_until(entry->node, match_width, &search,
						       k_uptime_get() +
							       CONFIG_KFSW_PARAM_LIST_TIMEOUT_MS);
#else
		return -ENOTSUP;
#endif
	}
	if (result != 0) {
		return result;
	}
	if (!search.found) {
		return -ENOENT;
	}
	if (search.width == 0U) {
		return -ENOTSUP;
	}
	*width = search.width;
	return 0;
}

int kfsw_hk_prepare_definition(uint8_t report, const struct kfsw_hk_entry *entries, size_t count,
			       struct kfsw_hk_definition *definition)
{
	size_t payload = 0U;
	int result;

	if (report >= CONFIG_KFSW_HK_REPORTS || entries == NULL || definition == NULL ||
	    count == 0U || count > CONFIG_KFSW_HK_ENTRIES) {
		return -EINVAL;
	}
	/* Every entry is priced before anything is stored, so a definition that
	 * cannot fit leaves the report that was working exactly as it was.
	 * The widths are kept, because resolving them again on every collection
	 * would walk the parameter list once per value.
	 */

	for (size_t index = 0U; index < count; index++) {
		size_t width = 0U;

		result = entry_declared_width(&entries[index], &width);
		if (result != 0) {
			kfsw_log_warning("HK: report %u entry %u (node %u id 0x%04x): %d", report,
					 (unsigned int)index, entries[index].node,
					 entries[index].param_id, result);
			return result;
		}
		definition->offsets[index] = (uint16_t)(KFSW_HK_HEADER_SIZE + payload);
		definition->widths[index] = (uint16_t)width;
		payload += width;
	}

	if ((payload + KFSW_HK_HEADER_SIZE) > CONFIG_KFSW_HK_SAMPLE_BYTES) {
		kfsw_log_warning("HK: report %u needs %u bytes, one sample holds %u", report,
				 (unsigned int)(payload + KFSW_HK_HEADER_SIZE),
				 (unsigned int)CONFIG_KFSW_HK_SAMPLE_BYTES);
		return -EMSGSIZE;
	}

	memcpy(definition->entries, entries, count * sizeof(entries[0]));
	definition->entry_count = count;
	definition->payload_bytes = payload;
	return 0;
}

static int clear_impl(uint8_t report);

static int define_impl(uint8_t report, const struct kfsw_hk_entry *entries, size_t count)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	struct kfsw_hk_definition definition;
	int result;

	if (target == NULL || entries == NULL) {
		return -EINVAL;
	}
	if (count == 0U) {
		return clear_impl(report);
	}
	if (count > CONFIG_KFSW_HK_ENTRIES) {
		return -E2BIG;
	}
	result = kfsw_hk_prepare_definition(report, entries, count, &definition);
	if (result != 0) {
		return result;
	}

	kfsw_hk_storage_lock();
	kfsw_hk_lock();
	generations[report]++;
	schedule_revisions[report]++;
	memcpy(target->entries, entries, count * sizeof(entries[0]));
	memcpy(target->widths, definition.widths, count * sizeof(definition.widths[0]));
	memcpy(target->offsets, definition.offsets, count * sizeof(definition.offsets[0]));
	target->entry_count = (uint8_t)count;
	target->payload_bytes = definition.payload_bytes;
	target->defined = true;
	target->next_uptime_ms = k_uptime_get() + target->period_ms;
	/* A redefinition invalidates what was collected: the same bytes would
	 * mean different things under the new layout.
	 */
	target->held = 0U;
	target->next_slot = 0U;
	stats.reports = 0U;
	for (size_t index = 0U; index < ARRAY_SIZE(reports); index++) {
		if (reports[index].defined) {
			stats.reports++;
		}
	}
	kfsw_hk_unlock();

#if CONFIG_KFSW_HK_STORE
	/* Same rule as the ring above: a redefinition invalidates what was
	 * collected, and a file of the old layout would decode into the wrong
	 * parameters. Storage has to be asked for again.
	 */
	kfsw_hk_store_forget(report);
#endif
	kfsw_hk_storage_unlock();
	kfsw_hk_wake();
	kfsw_log_info("HK: report %u defined, %u entries, %u bytes", report, (unsigned int)count,
		      (unsigned int)definition.payload_bytes);

	return 0;
}

static int clear_impl(uint8_t report)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);

	if (target == NULL) {
		return -EINVAL;
	}
	kfsw_hk_storage_lock();
#if CONFIG_KFSW_HK_STORE
	kfsw_hk_store_forget(report);
#endif

	kfsw_hk_lock();
	generations[report]++;
	schedule_revisions[report]++;
	memset(target, 0, sizeof(*target));
	stats.reports = 0U;
	for (size_t index = 0U; index < ARRAY_SIZE(reports); index++) {
		if (reports[index].defined) {
			stats.reports++;
		}
	}
	kfsw_hk_unlock();
	kfsw_hk_storage_unlock();
	kfsw_hk_wake();

	return 0;
}

int kfsw_hk_get_definition(uint8_t report, struct kfsw_hk_entry *entries, size_t *count)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	int result = 0;

	if ((target == NULL) || (entries == NULL) || (count == NULL)) {
		return -EINVAL;
	}

	kfsw_hk_lock();
	if (!target->defined) {
		result = -ENOENT;
	} else if (*count < target->entry_count) {
		result = -ENOSPC;
	} else {
		memcpy(entries, target->entries, target->entry_count * sizeof(entries[0]));
		*count = target->entry_count;
	}
	kfsw_hk_unlock();
	return result;
}

void kfsw_hk_set_enabled(bool value)
{
	bool changed;

	kfsw_hk_lock();
	changed = (enabled != value);
	enabled = value;
	if (changed) {
		for (size_t index = 0; index < ARRAY_SIZE(reports); index++) {
			schedule_revisions[index]++;
		}
	}
	kfsw_hk_unlock();

	/* Logged outside the lock, and only on a change, so setting the
	 * parameter to what it already is does not fill a pass with lines.
	 */
	if (changed) {
		kfsw_hk_wake();
		kfsw_log_info("HK: periodic collection %s", value ? "enabled" : "disabled");
	}
}

bool kfsw_hk_enabled(void)
{
	bool value;

	kfsw_hk_lock();
	value = enabled;
	kfsw_hk_unlock();
	return value;
}

/*
 * Whether the node knows what time it is.
 *
 * A composition without CSP has nowhere for a wall clock to come from, and one
 * with CSP has none until the ground sets it. Both answer false, and the
 * collector treats them the same.
 */
bool kfsw_hk_clock_valid(void)
{
#if CONFIG_KFSW_CSP
	struct kfsw_csp_clock clock = {0};

	kfsw_csp_clock_get(&clock);
	return kfsw_csp_clock_is_set(&clock);
#else
	return false;
#endif
}

/*
 * One collection.
 *
 * The timestamp is taken once, at the start, which is what the field is called:
 * a remote value arrives over a radio and cannot be simultaneous with anything.
 *
 * An unreadable entry is zero-filled and flagged, not dropped, so the layout
 * still matches the definition ground holds.
 */
int kfsw_hk_collect_report(const struct kfsw_hk_definition *entry, struct kfsw_hk_sample *sample,
			   uint32_t *failures)
{
	uint8_t flags = 0U;
#if CONFIG_KFSW_PARAM_CSP
	int64_t remote_deadline = k_uptime_get() + CONFIG_KFSW_HK_REMOTE_BUDGET_MS;
#endif

	if ((entry == NULL) || (sample == NULL)) {
		return -EINVAL;
	}

	memset(sample, 0, sizeof(*sample));

	/* Zero means the clock was never set, which is what a composition
	 * without CSP has: there is nowhere for a wall clock to come from. A
	 * sample still says what the values were, it just cannot say when, and
	 * the flag says so rather than leaving a reader to infer it from a
	 * timestamp that happens to be zero.
	 */
	sample->seconds = 0U;
	if (kfsw_hk_clock_valid()) {
#if CONFIG_KFSW_CSP
		struct kfsw_csp_clock clock = {0};

		kfsw_csp_clock_get(&clock);
		sample->seconds = (uint32_t)clock.seconds;
#endif
	} else {
		flags |= KFSW_HK_FLAG_CLOCK_UNSET;
	}
	sample->entry_count = entry->entry_count;
	sample->length = (uint16_t)(KFSW_HK_HEADER_SIZE + entry->payload_bytes);

	/* Local first, and each one straight into its reserved slot. Widths and
	 * offsets were settled when the report was defined, so nothing here
	 * looks a parameter up twice.
	 */
	for (size_t index = 0U; index < entry->entry_count; index++) {
		const struct kfsw_hk_entry *definition = &entry->entries[index];
		struct kfsw_param_value value;

		if (definition->node != KFSW_HK_NODE_LOCAL) {
			continue;
		}
		if (kfsw_param_get_by_id(definition->param_id, &value) == 0) {
			kfsw_hk_write_value(&sample->data[entry->offsets[index]],
					    entry->widths[index], &value);
		} else {
			flags |= KFSW_HK_FLAG_INCOMPLETE;
			(*failures)++;
		}
	}

#if CONFIG_KFSW_PARAM_CSP
	/* Then one pass per remote node. Alternating between two nodes would
	 * make the descriptor cache re-download a list over the radio, so every
	 * entry of a node is taken before moving on.
	 */
	for (size_t index = 0U; index < entry->entry_count; index++) {
		uint16_t node = entry->entries[index].node;
		bool seen = false;

		if (node == KFSW_HK_NODE_LOCAL) {
			continue;
		}
		for (size_t earlier = 0U; earlier < index; earlier++) {
			if (entry->entries[earlier].node == node) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		if (kfsw_hk_collect_remote(entry, node, sample, failures, remote_deadline) != 0) {
			flags |= KFSW_HK_FLAG_INCOMPLETE;
		}
	}
#else
	for (size_t index = 0U; index < entry->entry_count; index++) {
		if (entry->entries[index].node != KFSW_HK_NODE_LOCAL) {
			flags |= KFSW_HK_FLAG_INCOMPLETE;
			(*failures)++;
		}
	}
#endif

	sample->flags = flags;
	sample->sequence = entry->sequence;

	sample->data[0] = KFSW_HK_PROTOCOL_VERSION;
	sys_put_be16(sample->sequence, &sample->data[2]);
	sys_put_be32(sample->seconds, &sample->data[4]);
	sample->data[8] = sample->entry_count;
	sample->data[9] = sample->flags;
	return 0;
}

int kfsw_hk_collect(uint8_t report)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	static struct kfsw_hk_sample scratch;
	static struct kfsw_hk_definition definition;
	uint64_t generation;
	uint32_t failures = 0U;
	uint16_t next_sequence;
	int result;

	if (target == NULL) {
		return -EINVAL;
	}
	if (!kfsw_hk_is_ready()) {
		return -EACCES;
	}
	k_mutex_lock(&collection_lock, K_FOREVER);
	kfsw_hk_lock();
	if (!target->defined) {
		kfsw_hk_unlock();
		k_mutex_unlock(&collection_lock);
		return -ENOENT;
	}
	memcpy(definition.entries, target->entries, sizeof(definition.entries));
	memcpy(definition.widths, target->widths, sizeof(definition.widths));
	memcpy(definition.offsets, target->offsets, sizeof(definition.offsets));
	definition.entry_count = target->entry_count;
	definition.payload_bytes = target->payload_bytes;
	definition.sequence = target->sequence;
	generation = generations[report];
	kfsw_hk_unlock();

	result = kfsw_hk_collect_report(&definition, &scratch, &failures);
	kfsw_hk_storage_lock();
	kfsw_hk_lock();
	if (!target->defined || (generation != generations[report])) {
		result = -EAGAIN;
	}
	if (result != 0) {
		stats.failures++;
		kfsw_hk_unlock();
		kfsw_hk_storage_unlock();
		k_mutex_unlock(&collection_lock);
		return result;
	}
	scratch.data[1] = report;
	if (target->held == ARRAY_SIZE(target->ring)) {
		kfsw_hk_count_overwritten();
	} else {
		target->held++;
	}
	target->ring[target->next_slot] = scratch;
	target->next_slot = (uint16_t)((target->next_slot + 1U) % ARRAY_SIZE(target->ring));
	target->sequence++;
	next_sequence = target->sequence;
	stats.collections++;
	stats.entries_failed += failures;
	stats.last_seconds = scratch.seconds;
	kfsw_hk_unlock();
#if CONFIG_KFSW_HK_STORE
	/* Collection owns the ring writes; storage excludes definition changes. */
	if (kfsw_hk_store_interval(report) != 0U) {
		int stored = kfsw_hk_store_flush(report, next_sequence);

		if (stored != 0) {
			kfsw_log_warning("HK: report %u could not be stored (%d)", report, stored);
		}
	}
#else
	ARG_UNUSED(next_sequence);
#endif
	kfsw_hk_storage_unlock();
	k_mutex_unlock(&collection_lock);
	return 0;
}

#if CONFIG_KFSW_HK_STORE
static int set_store_impl(uint8_t report, uint32_t interval_ms)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	uint32_t period;
	uint16_t record_size;
	int result;

	if (target == NULL) {
		return -EINVAL;
	}
	kfsw_hk_storage_lock();
	kfsw_hk_lock();
	if (!target->defined) {
		kfsw_hk_unlock();
		kfsw_hk_storage_unlock();
		return -ENOENT;
	}
	period = target->period_ms;
	record_size = (uint16_t)(KFSW_HK_HEADER_SIZE + target->payload_bytes);
	kfsw_hk_unlock();

	/* Stopping is not discarding. Turning a store off used to unlink the
	 * file in the same call, so the command that reads as "stop writing"
	 * also destroyed the pass it had already captured. Removing it is now
	 * its own request.
	 */
	result = kfsw_hk_store_configure(report, interval_ms, period, record_size);
	kfsw_hk_storage_unlock();
	return result;
}

static int clear_store_impl(uint8_t report)
{
	if (report >= CONFIG_KFSW_HK_REPORTS) {
		return -EINVAL;
	}
	kfsw_hk_storage_lock();
	kfsw_hk_store_forget(report);
	kfsw_hk_storage_unlock();
	return 0;
}

int kfsw_hk_get_store(uint8_t report, uint32_t *interval_ms)
{
	if ((report >= CONFIG_KFSW_HK_REPORTS) || (interval_ms == NULL)) {
		return -EINVAL;
	}
	kfsw_hk_storage_lock();
	*interval_ms = kfsw_hk_store_interval(report);
	kfsw_hk_storage_unlock();
	return 0;
}
#endif

int kfsw_hk_get(uint8_t report, uint16_t age, struct kfsw_hk_sample *sample)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	int result = 0;

	if ((target == NULL) || (sample == NULL)) {
		return -EINVAL;
	}

	kfsw_hk_lock();
	if (age >= target->held) {
		result = -ENOENT;
	} else {
		/* next_slot is where the following sample goes, so the newest
		 * is one behind it, and age counts further back from there.
		 */
		uint16_t slot =
			(uint16_t)((target->next_slot + ARRAY_SIZE(target->ring) - 1U - age) %
				   ARRAY_SIZE(target->ring));

		*sample = target->ring[slot];
	}
	kfsw_hk_unlock();
	return result;
}

int kfsw_hk_depth(uint8_t report, uint16_t *depth)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);

	if ((target == NULL) || (depth == NULL)) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	*depth = target->held;
	kfsw_hk_unlock();
	return 0;
}

static int set_period_impl(uint8_t report, uint32_t period_ms)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);

	if (target == NULL) {
		return -EINVAL;
	}
	if ((period_ms != 0U) && (period_ms < CONFIG_KFSW_HK_PERIOD_FLOOR_MS)) {
		return -ERANGE;
	}

	kfsw_hk_lock();
	if (!target->defined) {
		kfsw_hk_unlock();
		return -ENOENT;
	}
	schedule_revisions[report]++;
	target->period_ms = period_ms;
	target->next_uptime_ms = (period_ms == 0U) ? 0 : (k_uptime_get() + (int64_t)period_ms);
	kfsw_hk_unlock();
	kfsw_hk_wake();

	return 0;
}

int kfsw_hk_get_period(uint8_t report, uint32_t *period_ms)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);

	if ((target == NULL) || (period_ms == NULL)) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	*period_ms = target->period_ms;
	kfsw_hk_unlock();
	return 0;
}

void kfsw_hk_get_stats(struct kfsw_hk_stats *out)
{
	if (out == NULL) {
		return;
	}
	kfsw_hk_lock();
	*out = stats;
	kfsw_hk_unlock();
	/* Read outside the lock: it asks the clock, not this service, and
	 * holding the housekeeping mutex across that buys nothing.
	 */
	out->clock_valid = kfsw_hk_clock_valid();
	out->enabled = kfsw_hk_enabled();
#if CONFIG_KFSW_HK_BEACON
	kfsw_hk_beacon_stats(&out->beacons_sent, &out->beacons_skipped);
#endif
}

int kfsw_hk_init(void)
{
	k_mutex_lock(&init_lock, K_FOREVER);
	if (!initialized) {
#if CONFIG_KFSW_HK_PERSISTENCE
		/* A rejected snapshot leaves diagnostics available with default settings. */
		(void)kfsw_hk_persist_load();
#endif
		kfsw_hk_lock();
		initialized = true;
		kfsw_hk_unlock();
	}
	k_mutex_unlock(&init_lock);
	return 0;
}

int64_t kfsw_hk_next_due(int64_t due, uint32_t period, int64_t now)
{
	/* Advance strictly past completion, without a catch-up burst. */
	return due + ((now - due) / period + 1) * period;
}

bool kfsw_hk_schedule_take(uint8_t index, int64_t now, struct kfsw_hk_due *due)
{
	struct kfsw_hk_report *report = kfsw_hk_report_at(index);
	bool ready = false;

	kfsw_hk_lock();
	if (initialized && !restoring && enabled && report != NULL && report->defined &&
	    report->period_ms != 0U && now >= report->next_uptime_ms) {
		*due = (struct kfsw_hk_due){schedule_revisions[index], report->next_uptime_ms,
					    report->period_ms};
		stats.scheduled_attempts++;
		ready = true;
	}
	kfsw_hk_unlock();
	return ready;
}

void kfsw_hk_schedule_finish(uint8_t index, const struct kfsw_hk_due *due, int64_t now)
{
	struct kfsw_hk_report *report = kfsw_hk_report_at(index);
	uint64_t missed = (uint64_t)(MAX(now, due->uptime_ms) - due->uptime_ms) / due->period_ms;

	kfsw_hk_lock();
	stats.missed_slots += (uint32_t)MIN(missed, UINT32_MAX - stats.missed_slots);
	if (enabled && report != NULL && report->defined && report->period_ms != 0U &&
	    schedule_revisions[index] == due->revision) {
		report->next_uptime_ms =
			kfsw_hk_next_due(due->uptime_ms, due->period_ms, MAX(now, due->uptime_ms));
	}
	kfsw_hk_unlock();
}

int64_t kfsw_hk_schedule_wait(int64_t now)
{
	int64_t wait = 200;

	kfsw_hk_lock();
	for (uint8_t index = 0; index < ARRAY_SIZE(reports); index++) {
		struct kfsw_hk_report *report = &reports[index];

		if (report->defined && report->period_ms != 0U) {
			wait = MIN(wait, MAX(0, report->next_uptime_ms - now));
		}
#if CONFIG_KFSW_HK_BEACON
		wait = MIN(wait, kfsw_hk_beacon_wait(index, now));
#endif
	}
	kfsw_hk_unlock();
	return wait;
}

bool kfsw_hk_is_ready(void)
{
	bool ready;

	kfsw_hk_lock();
	ready = initialized && !restoring;
	kfsw_hk_unlock();
	return ready;
}

int kfsw_hk_config_begin(void)
{
	if (!kfsw_hk_is_ready()) {
		return -EACCES;
	}
	k_mutex_lock(&config_lock, K_FOREVER);
	if (!kfsw_hk_is_ready()) {
		k_mutex_unlock(&config_lock);
		return -EACCES;
	}
	return 0;
}

int kfsw_hk_config_end(int result)
{
	if (result == 0) {
		kfsw_hk_lock();
		config_revision++;
		stats.settings_dirty = IS_ENABLED(CONFIG_KFSW_HK_PERSISTENCE);
		kfsw_hk_unlock();
	}
	k_mutex_unlock(&config_lock);
#if CONFIG_KFSW_HK_PERSISTENCE
	if (result == 0) {
		result = kfsw_hk_persist_save();
		if (result != 0) {
			return KFSW_HK_APPLIED_UNSAVED;
		}
	}
#endif
	return result;
}

int kfsw_hk_define(uint8_t report, const struct kfsw_hk_entry *entries, size_t count)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result : kfsw_hk_config_end(define_impl(report, entries, count));
}

int kfsw_hk_clear(uint8_t report)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result : kfsw_hk_config_end(clear_impl(report));
}

int kfsw_hk_set_period(uint8_t report, uint32_t period_ms)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result : kfsw_hk_config_end(set_period_impl(report, period_ms));
}

#if CONFIG_KFSW_HK_STORE
int kfsw_hk_set_store(uint8_t report, uint32_t interval_ms)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result : kfsw_hk_config_end(set_store_impl(report, interval_ms));
}

int kfsw_hk_clear_store(uint8_t report)
{
	int result = kfsw_hk_config_begin();

	return result != 0 ? result : kfsw_hk_config_end(clear_store_impl(report));
}
#endif

#if CONFIG_KFSW_HK_PERSISTENCE
/* Save caller owns HK state. The file mutex is always acquired first. */
uint64_t kfsw_hk_config_revision(void)
{
	return config_revision;
}

bool kfsw_hk_save_blocked(void)
{
	return save_blocked;
}

void kfsw_hk_save_result(uint64_t revision, int result)
{
	kfsw_hk_lock();
	stats.last_save_error = result;
	if (result == 0 && config_revision == revision) {
		stats.settings_dirty = false;
	}
	kfsw_hk_unlock();
}

int kfsw_hk_save(void)
{
	if (!kfsw_hk_is_ready()) {
		return -EACCES;
	}
	kfsw_hk_lock();
	save_blocked = false;
	kfsw_hk_unlock();
	return kfsw_hk_persist_save();
}

void kfsw_hk_restore_begin(void)
{
	k_mutex_lock(&config_lock, K_FOREVER);
	kfsw_hk_lock();
	restoring = true;
	kfsw_hk_unlock();
}

void kfsw_hk_restore_end(int result)
{
	kfsw_hk_lock();
	stats.last_load_error = result == -ENOENT ? 0 : result;
	save_blocked = result != 0 && result != -ENOENT;
	stats.settings_dirty = save_blocked;
	if (result == 0) {
		config_revision++;
		stats.settings_dirty = false;
	}
	restoring = false;
	kfsw_hk_unlock();
	k_mutex_unlock(&config_lock);
	kfsw_hk_wake();
}

/* The complete snapshot has already been checked. Caller holds config ownership. */
void kfsw_hk_restore_report(uint8_t index, const struct kfsw_hk_definition *definition,
			    uint32_t period_ms)
{
	struct kfsw_hk_report *report = &reports[index];

	kfsw_hk_lock();
	generations[index]++;
	schedule_revisions[index]++;
	memset(report, 0, sizeof(*report));
	if (definition != NULL) {
		memcpy(report->entries, definition->entries, sizeof(report->entries));
		memcpy(report->widths, definition->widths, sizeof(report->widths));
		memcpy(report->offsets, definition->offsets, sizeof(report->offsets));
		report->entry_count = definition->entry_count;
		report->payload_bytes = definition->payload_bytes;
		report->defined = true;
		report->period_ms = period_ms;
		report->next_uptime_ms = k_uptime_get() + period_ms;
#if CONFIG_KFSW_HK_STORE
		report->sequence = kfsw_hk_store_restore_sequence(index);
#endif
	}
	stats.reports = 0;
	for (size_t i = 0; i < ARRAY_SIZE(reports); i++) {
		stats.reports += reports[i].defined;
	}
	kfsw_hk_unlock();
}
#endif
