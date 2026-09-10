#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/hk.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "hk_internal.h"

#if CONFIG_KFSW_PARAM_CSP

/* One window's worth is asked for at a time, and the window is what
 * kfsw_param_remote_get_many() will pack into a request anyway.
 */
#define KFSW_HK_REMOTE_WINDOW 8U

struct name_search {
	const struct kfsw_hk_report *report;
	uint16_t node;
	/* Indices into the report, and the name each one resolved to. */
	uint8_t indices[CONFIG_KFSW_HK_ENTRIES];
	const char *names[CONFIG_KFSW_HK_ENTRIES];
	size_t count;
};

/*
 * The remote read is by name, and a definition holds identifiers, so the
 * node's cached descriptors are what bridges the two. The cache is local, so
 * this costs no round trip -- it is walked once per node per collection rather
 * than once per value.
 */
static bool match_names(const struct kfsw_param_info *info, void *context)
{
	struct name_search *search = context;

	for (size_t index = 0U; index < search->report->entry_count; index++) {
		const struct kfsw_hk_entry *entry = &search->report->entries[index];

		if ((entry->node != search->node) || (entry->param_id != info->id)) {
			continue;
		}
		if (search->count >= ARRAY_SIZE(search->indices)) {
			return false;
		}
		search->indices[search->count] = (uint8_t)index;
		search->names[search->count] = info->name;
		search->count++;
	}
	return true;
}

int kfsw_hk_collect_remote(struct kfsw_hk_report *report, uint16_t node,
			   struct kfsw_hk_sample *sample)
{
	static struct name_search search;
	static struct kfsw_param_value values[KFSW_HK_REMOTE_WINDOW];
	size_t expected = 0U;
	int result;

	if ((report == NULL) || (sample == NULL)) {
		return -EINVAL;
	}

	for (size_t index = 0U; index < report->entry_count; index++) {
		if (report->entries[index].node == node) {
			expected++;
		}
	}

	memset(&search, 0, sizeof(search));
	search.report = report;
	search.node = node;

	result = kfsw_param_remote_visit(node, match_names, &search);
	if (result != 0) {
		/* The node did not answer, so every value it owed is absent.
		 * Their bytes are already zero and the caller marks the sample
		 * incomplete.
		 */
		kfsw_log_warning("HK: node %u did not list its parameters (%d)", node, result);
		for (size_t index = 0U; index < expected; index++) {
			kfsw_hk_count_entry_failure();
		}
		return result;
	}

	if (search.count != expected) {
		kfsw_log_warning("HK: node %u carries %u of the %u values asked of it", node,
				 (unsigned int)search.count, (unsigned int)expected);
	}

	for (size_t base = 0U; base < search.count; base += KFSW_HK_REMOTE_WINDOW) {
		size_t span = MIN(search.count - base, (size_t)KFSW_HK_REMOTE_WINDOW);

		result = kfsw_param_remote_get_many(node, &search.names[base], span, values);
		if (result != 0) {
			/* A window that failed leaves its values zero rather
			 * than half-written, so the frame still reads.
			 */
			for (size_t offset = 0U; offset < span; offset++) {
				kfsw_hk_count_entry_failure();
			}
			continue;
		}
		for (size_t offset = 0U; offset < span; offset++) {
			uint8_t index = search.indices[base + offset];

			kfsw_hk_write_value(&sample->data[report->offsets[index]],
					    report->widths[index], &values[offset]);
		}
	}

	return (search.count == expected) ? 0 : -ENOENT;
}

#endif /* CONFIG_KFSW_PARAM_CSP */
