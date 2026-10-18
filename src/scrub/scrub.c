#define KFSW_LOG_MODULE KFSW_LOG_MODULE_SCRUB

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <kfsw/services/event.h>
#include <kfsw/services/fwu.h>
#include <kfsw/services/log.h>
#include <kfsw/services/scrub.h>
#if CONFIG_KFSW_PARAM_PERSISTENCE
#include <kfsw/services/parameter.h>
#endif

#define SLOT0_PRESENT DT_NODE_EXISTS(DT_NODELABEL(slot0_partition))
#define SLOT1_PRESENT DT_NODE_EXISTS(DT_NODELABEL(slot1_partition))
#define GOLDEN_NODE DT_CHOSEN(kfsw_golden_partition)
#define GOLDEN_PRESENT DT_NODE_EXISTS(GOLDEN_NODE)
#define PRESENT_REGIONS (SLOT0_PRESENT + SLOT1_PRESENT + GOLDEN_PRESENT)

/* Read in pieces: a slot is larger than anything worth putting on a stack. */
#define SCRUB_CHUNK 256U

/*
 * The bootloader's image header as it sits on flash, little endian: the magic
 * at 0, the header size at 8 and the image size at 12. Read from the bytes
 * rather than through bootutil/image.h, which brings MCUboot's own
 * configuration headers into a build that is not the bootloader.
 */
#define IMAGE_HEADER_MAGIC 0x96f3b83dUL
#define IMAGE_HEADER_SIZE_AT 8U
#define IMAGE_SIZE_AT 12U
#define IMAGE_HEADER_READ 16U

struct region {
	const char *name;
	/* Flash area id, or -1 when the build has no such partition. */
	int area;
	/* Where the image starts inside the partition. */
	uint32_t (*image_offset)(void);
};

static uint32_t at_partition_start(void)
{
	return 0U;
}

/*
 * MCUboot's swap-using-offset mode expects the secondary image one sector in,
 * which is where FWU writes it. Reading slot1 from the start would find erased
 * flash and report the region empty.
 */
static uint32_t at_fwu_write_offset(void)
{
	return kfsw_fwu_slot_write_offset();
}

static const struct region regions[KFSW_SCRUB_REGIONS] = {
	[KFSW_SCRUB_SLOT0] =
		{
			.name = "slot0",
#if SLOT0_PRESENT
			.area = DT_FIXED_PARTITION_ID(DT_NODELABEL(slot0_partition)),
#else
			.area = -1,
#endif
			.image_offset = at_partition_start,
		},
	[KFSW_SCRUB_SLOT1] =
		{
			.name = "slot1",
#if SLOT1_PRESENT
			.area = DT_FIXED_PARTITION_ID(DT_NODELABEL(slot1_partition)),
#else
			.area = -1,
#endif
			.image_offset = at_fwu_write_offset,
		},
	[KFSW_SCRUB_GOLDEN] =
		{
			.name = "golden",
#if GOLDEN_PRESENT
			.area = DT_FIXED_PARTITION_ID(GOLDEN_NODE),
#else
			.area = -1,
#endif
			.image_offset = at_partition_start,
		},
};

static K_MUTEX_DEFINE(scrub_lock);
static struct kfsw_scrub_status scrub_state = {
	.regions = PRESENT_REGIONS,
};
/* A region absent from this build says so, instead of reporting a state it
 * never measured.
 */
static struct kfsw_scrub_result results[KFSW_SCRUB_REGIONS] = {
	[KFSW_SCRUB_SLOT0] = {.id = KFSW_SCRUB_SLOT0,
			      .state = SLOT0_PRESENT ? KFSW_SCRUB_UNKNOWN : KFSW_SCRUB_ABSENT},
	[KFSW_SCRUB_SLOT1] = {.id = KFSW_SCRUB_SLOT1,
			      .state = SLOT1_PRESENT ? KFSW_SCRUB_UNKNOWN : KFSW_SCRUB_ABSENT},
	[KFSW_SCRUB_GOLDEN] = {.id = KFSW_SCRUB_GOLDEN,
			       .state = GOLDEN_PRESENT ? KFSW_SCRUB_UNKNOWN : KFSW_SCRUB_ABSENT},
};
/* Persisted through the parameter table; the service reads and writes them. */
uint32_t kfsw_scrub_references[KFSW_SCRUB_REGIONS];
/* Latch a mismatch so one decayed image is counted once, not every sweep. */
static bool mismatching[KFSW_SCRUB_REGIONS];

static void scrub_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(scrub_work, scrub_work_handler);

/*
 * The length to sweep comes from the image header, so an erased tail does not
 * change the result and a larger partition does not slow the sweep.
 */
static int image_length(const struct flash_area *area, uint32_t offset, uint32_t *length)
{
	uint8_t header[IMAGE_HEADER_READ];
	uint32_t header_size;
	uint32_t image_size;

	if (flash_area_read(area, offset, header, sizeof(header)) != 0) {
		return -EIO;
	}
	if (sys_get_le32(header) != IMAGE_HEADER_MAGIC) {
		return -ENOENT;
	}
	header_size = sys_get_le16(&header[IMAGE_HEADER_SIZE_AT]);
	image_size = sys_get_le32(&header[IMAGE_SIZE_AT]);
	if (header_size == 0U || image_size == 0U) {
		return -ENOENT;
	}
	*length = header_size + image_size;
	if (*length > (area->fa_size - offset)) {
		return -ENOENT;
	}
	return 0;
}

static int measure(const struct region *region, uint32_t *bytes, uint32_t *crc)
{
	const struct flash_area *area;
	uint8_t chunk[SCRUB_CHUNK];
	uint32_t offset = region->image_offset();
	uint32_t length;
	uint32_t done = 0U;
	int result;

	if (flash_area_open((uint8_t)region->area, &area) != 0) {
		return -EIO;
	}
	result = image_length(area, offset, &length);
	if (result != 0) {
		flash_area_close(area);
		return result;
	}

	*crc = 0U;
	while (done < length) {
		uint32_t span = MIN(sizeof(chunk), length - done);

		if (flash_area_read(area, offset + done, chunk, span) != 0) {
			flash_area_close(area);
			return -EIO;
		}
		*crc = crc32_ieee_update(*crc, chunk, span);
		done += span;
		/* A sweep of a few hundred kilobytes must not hold the CPU
		 * against the watchdog feed.
		 */
		k_yield();
	}
	flash_area_close(area);
	*bytes = length;
	return 0;
}

static void record(enum kfsw_scrub_region id, enum kfsw_scrub_state state, uint32_t bytes,
		   uint32_t measured)
{
	results[id].state = (uint8_t)state;
	results[id].bytes = bytes;
	results[id].measured = measured;
	results[id].reference = kfsw_scrub_references[id];
}

static int sweep_one(enum kfsw_scrub_region id)
{
	const struct region *region = &regions[id];
	uint32_t bytes = 0U;
	uint32_t measured = 0U;
	enum kfsw_scrub_state state;
	int result;

	if (region->area < 0) {
		record(id, KFSW_SCRUB_ABSENT, 0U, 0U);
		return -ENODEV;
	}

	result = measure(region, &bytes, &measured);
	if (result == -ENOENT) {
		state = KFSW_SCRUB_EMPTY;
	} else if (result != 0) {
		state = KFSW_SCRUB_UNREADABLE;
	} else if (kfsw_scrub_references[id] == 0U) {
		state = KFSW_SCRUB_UNKNOWN;
	} else if (kfsw_scrub_references[id] == measured) {
		state = KFSW_SCRUB_OK;
	} else {
		state = KFSW_SCRUB_MISMATCH;
	}

	k_mutex_lock(&scrub_lock, K_FOREVER);
	record(id, state, bytes, measured);
	if (state == KFSW_SCRUB_MISMATCH) {
		if (!mismatching[id]) {
			mismatching[id] = true;
			if (scrub_state.mismatches < UINT32_MAX) {
				scrub_state.mismatches++;
			}
			kfsw_log_error("Scrub: %s no longer matches its reference (%08x)",
				       region->name, measured);
#if CONFIG_KFSW_EVENT
			{
				uint8_t payload[5];

				payload[0] = (uint8_t)id;
				sys_put_be32(measured, &payload[1]);
				kfsw_event_emit(KFSW_EVENT_SOURCE_SCRUB, KFSW_EVENT_SCRUB_MISMATCH,
						KFSW_EVENT_ERROR, payload, sizeof(payload));
			}
#endif
		}
	} else {
		mismatching[id] = false;
	}
	k_mutex_unlock(&scrub_lock);
	return 0;
}

int kfsw_scrub_sweep_region(enum kfsw_scrub_region region)
{
	if (region >= KFSW_SCRUB_REGIONS) {
		return -EINVAL;
	}
	return sweep_one(region);
}

int kfsw_scrub_sweep(void)
{
	int swept = 0;

	for (unsigned int id = 0U; id < KFSW_SCRUB_REGIONS; id++) {
		if (sweep_one((enum kfsw_scrub_region)id) == 0) {
			swept++;
		}
	}

	k_mutex_lock(&scrub_lock, K_FOREVER);
	if (scrub_state.sweeps < UINT32_MAX) {
		scrub_state.sweeps++;
	}
	scrub_state.last_sweep_ms = (uint32_t)k_uptime_get_32();
	k_mutex_unlock(&scrub_lock);
	return swept;
}

int kfsw_scrub_learn(enum kfsw_scrub_region region)
{
	int result;

	if (region >= KFSW_SCRUB_REGIONS) {
		return -EINVAL;
	}
	result = sweep_one(region);
	if (result != 0) {
		return result;
	}
	if (results[region].state == KFSW_SCRUB_EMPTY) {
		return -ENOENT;
	}
	if (results[region].state == KFSW_SCRUB_UNREADABLE) {
		return -EIO;
	}

	k_mutex_lock(&scrub_lock, K_FOREVER);
	kfsw_scrub_references[region] = results[region].measured;
	results[region].reference = results[region].measured;
	results[region].state = (uint8_t)KFSW_SCRUB_OK;
	mismatching[region] = false;
	k_mutex_unlock(&scrub_lock);

#if CONFIG_KFSW_PARAM_PERSISTENCE
	/* A reference held only in RAM is relearned at every boot, and a
	 * reference that is always fresh can never report a mismatch.
	 */
	result = kfsw_param_persist_save();
	if (result != 0) {
		kfsw_log_error("Scrub: could not persist the %s reference (%d)",
			       regions[region].name, result);
		return -EIO;
	}
#endif
	kfsw_log_info("Scrub: %s reference is %08x over %u bytes", regions[region].name,
		      results[region].measured, results[region].bytes);
	return 0;
}

static void scrub_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)kfsw_scrub_sweep();

	k_mutex_lock(&scrub_lock, K_FOREVER);
	if (scrub_state.running) {
		(void)k_work_reschedule(&scrub_work, K_MSEC(CONFIG_KFSW_SCRUB_PERIOD_MS));
	}
	k_mutex_unlock(&scrub_lock);
}

int kfsw_scrub_start(void)
{
	k_mutex_lock(&scrub_lock, K_FOREVER);
	if (scrub_state.running) {
		k_mutex_unlock(&scrub_lock);
		return -EALREADY;
	}
	scrub_state.running = true;
	k_mutex_unlock(&scrub_lock);

	/* Sweep once now, so the regions are readable without waiting a period. */
	(void)kfsw_scrub_sweep();
	(void)k_work_reschedule(&scrub_work, K_MSEC(CONFIG_KFSW_SCRUB_PERIOD_MS));
	kfsw_log_info("Image scrub started, every %u ms, %u region(s)", CONFIG_KFSW_SCRUB_PERIOD_MS,
		      scrub_state.regions);
	return 0;
}

int kfsw_scrub_stop(void)
{
	k_mutex_lock(&scrub_lock, K_FOREVER);
	if (!scrub_state.running) {
		k_mutex_unlock(&scrub_lock);
		return -EALREADY;
	}
	scrub_state.running = false;
	k_mutex_unlock(&scrub_lock);

	(void)k_work_cancel_delayable(&scrub_work);
	return 0;
}

void kfsw_scrub_get_status(struct kfsw_scrub_status *status)
{
	if (status == NULL) {
		return;
	}
	k_mutex_lock(&scrub_lock, K_FOREVER);
	*status = scrub_state;
	k_mutex_unlock(&scrub_lock);
}

int kfsw_scrub_get_result(enum kfsw_scrub_region region, struct kfsw_scrub_result *result)
{
	if (region >= KFSW_SCRUB_REGIONS || result == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&scrub_lock, K_FOREVER);
	*result = results[region];
	k_mutex_unlock(&scrub_lock);
	result->name = regions[region].name;
	return 0;
}

void kfsw_scrub_visit_regions(kfsw_scrub_visitor_t visitor, void *context)
{
	if (visitor == NULL) {
		return;
	}
	for (unsigned int id = 0U; id < KFSW_SCRUB_REGIONS; id++) {
		struct kfsw_scrub_result copy;

		k_mutex_lock(&scrub_lock, K_FOREVER);
		copy = results[id];
		k_mutex_unlock(&scrub_lock);
		copy.name = regions[id].name;
		if (!visitor(&copy, context)) {
			return;
		}
	}
}
