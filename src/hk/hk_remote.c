#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "hk_internal.h"

#if CONFIG_KFSW_PARAM_CSP

/* Values are requested one window at a time. */
#define KFSW_HK_REMOTE_WINDOW 8U

struct name_search {
	const struct kfsw_hk_definition *report;
	uint16_t node;
	/* Indices into the report, and the name each one resolved to. */
	uint8_t indices[CONFIG_KFSW_HK_ENTRIES];
	const char *names[CONFIG_KFSW_HK_ENTRIES];
	char name_storage[CONFIG_KFSW_HK_ENTRIES][KFSW_PARAM_NAME_MAX + 1];
	size_t count;
};

/*
 * Map IDs to names with the cached descriptors; no round trip is needed.
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
		strncpy(search->name_storage[search->count], info->name, KFSW_PARAM_NAME_MAX);
		search->names[search->count] = search->name_storage[search->count];
		search->count++;
	}
	return true;
}

int kfsw_hk_collect_remote(const struct kfsw_hk_definition *report, uint16_t node,
			   struct kfsw_hk_sample *sample, uint32_t *failures, int64_t deadline)
{
	static struct name_search search;
	static struct kfsw_param_value values[KFSW_HK_REMOTE_WINDOW];
	size_t expected = 0U;
	int first_read_error = 0;
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

	result = kfsw_param_remote_visit_until(node, match_names, &search, deadline);
	if (result != 0) {
		/* No answer: the values stay zero and the sample is marked incomplete. */
		kfsw_log_warning("HK: node %u did not list its parameters (%d)", node, result);
		for (size_t index = 0U; index < expected; index++) {
			(*failures)++;
		}
		return result;
	}

	if (search.count != expected) {
		kfsw_log_warning("HK: node %u carries %u of the %u values asked of it", node,
				 (unsigned int)search.count, (unsigned int)expected);
	}

	for (size_t base = 0U; base < search.count; base += KFSW_HK_REMOTE_WINDOW) {
		size_t span = MIN(search.count - base, (size_t)KFSW_HK_REMOTE_WINDOW);

		result = kfsw_param_remote_get_many_until(node, &search.names[base], span, values,
							  deadline);
		if (result != 0) {
			if (first_read_error == 0) {
				first_read_error = result;
			}
			/* A failed window leaves its values zero. */
			for (size_t offset = 0U; offset < span; offset++) {
				(*failures)++;
			}
			continue;
		}
		for (size_t offset = 0U; offset < span; offset++) {
			uint8_t index = search.indices[base + offset];

			kfsw_hk_write_value(&sample->data[report->offsets[index]],
					    report->widths[index], &values[offset]);
		}
	}

	if (first_read_error != 0) {
		return first_read_error;
	}
	return (search.count == expected) ? 0 : -ENOENT;
}

#endif /* CONFIG_KFSW_PARAM_CSP */
