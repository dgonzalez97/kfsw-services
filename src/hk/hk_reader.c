#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_STORE

/*
 * Reading back what the store wrote. The ring holds the frames themselves, so a
 * record needs no decoding to be served again: the same sample structure, the
 * same printer, the same remote reply as a live one.
 *
 * An extract is a file of selected records for downlink, which is the only way
 * a pass produces a dataset rather than whatever the newest sample happened to
 * be when the ground asked.
 *
 *   0  magic "KHKD"
 *   4  format version
 *   5  report
 *   6  record size
 *   8  record count
 *  12  first sequence
 *  14  last sequence
 *  16  CRC32 over the whole file with these four bytes zero
 */
#define KFSW_HK_DATASET_MAGIC "KHKD"
#define KFSW_HK_DATASET_MAGIC_SIZE 4U
#define KFSW_HK_DATASET_VERSION 1U
#define KFSW_HK_DATASET_HEADER_SIZE 20U
#define KFSW_HK_DATASET_CRC_OFFSET 16U

#define KFSW_HK_EXTRACT_PATH_MAX 96U
#define KFSW_HK_EXTRACT_SUFFIX ".part"

/* Frame header, as hk.c writes it into every sample. */
#define FRAME_VERSION_OFFSET 0U
#define FRAME_REPORT_OFFSET 1U
#define FRAME_SEQUENCE_OFFSET 2U
#define FRAME_SECONDS_OFFSET 4U
#define FRAME_ENTRY_COUNT_OFFSET 8U
#define FRAME_FLAGS_OFFSET 9U

/** An open store file and what its slots were found to hold. */
struct store_reader {
	struct fs_file_t file;
	uint16_t record_size;
	uint32_t capacity;
	/** Sequence of the newest record found. */
	uint16_t newest;
	/** Slots that hold a record, so also how far back the ring goes. */
	uint16_t written;
	bool open;
};

static void reader_close(struct store_reader *reader)
{
	if (reader->open) {
		(void)fs_close(&reader->file);
		reader->open = false;
	}
}

/* A slot is this report's when its own header says so and its sequence lands on
 * the slot it was read from.
 */
static bool slot_belongs(const uint8_t *record, uint8_t report, uint32_t capacity, uint32_t slot)
{
	uint16_t sequence;

	if (record[FRAME_VERSION_OFFSET] != KFSW_HK_PROTOCOL_VERSION) {
		return false;
	}
	if (record[FRAME_REPORT_OFFSET] != report) {
		return false;
	}
	sequence = sys_get_be16(&record[FRAME_SEQUENCE_OFFSET]);
	return ((uint32_t)sequence % capacity) == slot;
}

static int read_at(struct fs_file_t *file, off_t offset, uint8_t *data, size_t size)
{
	int result = fs_seek(file, offset, FS_SEEK_SET);

	if (result != 0) {
		return result;
	}
	while (size > 0U) {
		ssize_t read = fs_read(file, data, size);

		if (read < 0) {
			return (int)read;
		}
		if (read == 0) {
			return -EBADMSG;
		}
		data += read;
		size -= (size_t)read;
	}
	return 0;
}

/*
 * Opens the file and walks every slot once, which is what tells the window
 * apart: the newest sequence and how many slots hold anything.
 */
static int reader_open(uint8_t report, struct store_reader *reader)
{
	uint8_t header[KFSW_HK_STORE_HEADER_SIZE];
	uint8_t record[CONFIG_KFSW_HK_SAMPLE_BYTES];
	char path[64];
	bool have_newest = false;
	int result;

	*reader = (struct store_reader){0};

	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	kfsw_hk_store_path(report, path, sizeof(path));
	fs_file_t_init(&reader->file);
	result = fs_open(&reader->file, path, FS_O_READ);
	if (result != 0) {
		return result;
	}
	reader->open = true;

	result = read_at(&reader->file, 0, header, sizeof(header));
	if (result != 0) {
		reader_close(reader);
		return result;
	}
	if ((memcmp(header, KFSW_HK_STORE_MAGIC, KFSW_HK_STORE_MAGIC_SIZE) != 0) ||
	    (header[4] != KFSW_HK_STORE_VERSION) || (header[5] != report)) {
		reader_close(reader);
		return -EBADMSG;
	}
	reader->record_size = sys_get_be16(&header[6]);
	reader->capacity = sys_get_be32(&header[8]);
	if ((reader->record_size == 0U) ||
	    (reader->record_size > (uint16_t)CONFIG_KFSW_HK_SAMPLE_BYTES) ||
	    (reader->capacity == 0U)) {
		reader_close(reader);
		return -EBADMSG;
	}

	for (uint32_t slot = 0U; slot < reader->capacity; slot++) {
		off_t offset = (off_t)KFSW_HK_STORE_HEADER_SIZE +
			       ((off_t)slot * (off_t)reader->record_size);
		uint16_t sequence;

		result = read_at(&reader->file, offset, record, reader->record_size);
		if (result != 0) {
			/* A file shorter than its capacity is the normal case while
			 * the ring is still filling. */
			result = 0;
			break;
		}
		if (record[FRAME_VERSION_OFFSET] == 0U) {
			continue; /* Never written. */
		}
		if (!slot_belongs(record, report, reader->capacity, slot)) {
			continue; /* Left by a file with a different shape. */
		}

		sequence = sys_get_be16(&record[FRAME_SEQUENCE_OFFSET]);
		reader->written++;
		if (!have_newest || ((int16_t)(sequence - reader->newest) > 0)) {
			reader->newest = sequence;
			have_newest = true;
		}
	}

	if (!have_newest) {
		reader->written = 0U;
	}
	return 0;
}

/** Sequence of the oldest record still readable. */
static uint16_t reader_oldest(const struct store_reader *reader)
{
	return (uint16_t)(reader->newest - (uint16_t)(reader->written - 1U));
}

/* Reads the slot a sequence number selects, and refuses one holding something
 * else, which is how a gap in a partly filled ring is skipped.
 */
static int reader_record(struct store_reader *reader, uint8_t report, uint16_t sequence,
			 struct kfsw_hk_sample *sample)
{
	off_t offset =
		(off_t)KFSW_HK_STORE_HEADER_SIZE +
		((off_t)((uint32_t)sequence % reader->capacity) * (off_t)reader->record_size);
	int result;

	(void)memset(sample, 0, sizeof(*sample));
	result = read_at(&reader->file, offset, sample->data, reader->record_size);
	if (result != 0) {
		return result;
	}
	if ((sample->data[FRAME_VERSION_OFFSET] == 0U) ||
	    !slot_belongs(sample->data, report, reader->capacity,
			  (uint32_t)sequence % reader->capacity) ||
	    (sys_get_be16(&sample->data[FRAME_SEQUENCE_OFFSET]) != sequence)) {
		return -ENOENT;
	}

	sample->length = reader->record_size;
	sample->sequence = sequence;
	sample->seconds = sys_get_be32(&sample->data[FRAME_SECONDS_OFFSET]);
	sample->entry_count = sample->data[FRAME_ENTRY_COUNT_OFFSET];
	sample->flags = sample->data[FRAME_FLAGS_OFFSET];
	return 0;
}

/* Sequence numbers wrap, so a window is a signed difference rather than a
 * comparison, and a range across the wrap still selects what it should.
 */
static bool sequence_within(const struct kfsw_hk_store_filter *filter, uint16_t sequence)
{
	if ((filter->from_sequence != 0U) && ((int16_t)(sequence - filter->from_sequence) < 0)) {
		return false;
	}
	if ((filter->to_sequence != 0U) && ((int16_t)(sequence - filter->to_sequence) > 0)) {
		return false;
	}
	return true;
}

static bool sample_matches(const struct kfsw_hk_store_filter *filter,
			   const struct kfsw_hk_sample *sample)
{
	if (!sequence_within(filter, sample->sequence)) {
		return false;
	}
	if ((filter->from_seconds != 0U) && (sample->seconds < filter->from_seconds)) {
		return false;
	}
	if ((filter->to_seconds != 0U) && (sample->seconds > filter->to_seconds)) {
		return false;
	}
	if ((filter->without_flags != 0U) && ((sample->flags & filter->without_flags) != 0U)) {
		return false;
	}
	return true;
}

/*
 * Walks the matching records oldest first. Stops early when the visitor says so,
 * which is how reading one record avoids reading the rest.
 */
typedef bool (*record_visitor_t)(const struct kfsw_hk_sample *sample, uint16_t position,
				 void *context);

static int walk(const struct kfsw_hk_store_filter *filter, record_visitor_t visitor, void *context,
		uint16_t *matched)
{
	struct store_reader reader;
	uint16_t position = 0U;
	int result;

	if ((filter == NULL) || (filter->report >= CONFIG_KFSW_HK_REPORTS)) {
		return -EINVAL;
	}

	result = reader_open(filter->report, &reader);
	if (result != 0) {
		return result;
	}

	for (uint16_t index = 0U; index < reader.written; index++) {
		struct kfsw_hk_sample sample;
		uint16_t sequence = (uint16_t)(reader_oldest(&reader) + index);

		if (reader_record(&reader, filter->report, sequence, &sample) != 0) {
			continue;
		}
		if (!sample_matches(filter, &sample)) {
			continue;
		}
		position++;
		if ((visitor != NULL) && !visitor(&sample, (uint16_t)(position - 1U), context)) {
			break;
		}
	}

	reader_close(&reader);
	*matched = position;
	return 0;
}

int kfsw_hk_store_query(uint8_t report, struct kfsw_hk_store_info *info)
{
	struct store_reader reader;
	int result;

	if ((info == NULL) || (report >= CONFIG_KFSW_HK_REPORTS)) {
		return -EINVAL;
	}

	result = reader_open(report, &reader);
	if (result != 0) {
		return result;
	}

	*info = (struct kfsw_hk_store_info){
		.record_size = reader.record_size,
		.capacity = reader.capacity,
		.records = reader.written,
		.oldest = (reader.written != 0U) ? reader_oldest(&reader) : 0U,
		.newest = (reader.written != 0U) ? reader.newest : 0U,
		.interval_ms = kfsw_hk_store_interval(report),
	};
	reader_close(&reader);
	return 0;
}

int kfsw_hk_store_count(const struct kfsw_hk_store_filter *filter, uint16_t *records)
{
	uint16_t matched = 0U;
	int result;

	if (records == NULL) {
		return -EINVAL;
	}
	result = walk(filter, NULL, NULL, &matched);
	if (result == 0) {
		*records = matched;
	}
	return result;
}

struct read_one {
	struct kfsw_hk_sample *sample;
	uint16_t wanted;
	bool found;
};

static bool take_one(const struct kfsw_hk_sample *sample, uint16_t position, void *context)
{
	struct read_one *state = context;

	if (position != state->wanted) {
		return true;
	}
	*state->sample = *sample;
	state->found = true;
	return false;
}

int kfsw_hk_store_read(const struct kfsw_hk_store_filter *filter, uint16_t index,
		       struct kfsw_hk_sample *sample)
{
	struct read_one state = {.sample = sample, .wanted = index};
	uint16_t matched = 0U;
	int result;

	if (sample == NULL) {
		return -EINVAL;
	}
	result = walk(filter, take_one, &state, &matched);
	if (result != 0) {
		return result;
	}
	return state.found ? 0 : -ENOENT;
}

/** The window a filter selects, so the header is complete before a record is written. */
struct window {
	uint16_t first_sequence;
	uint16_t last_sequence;
};

static bool note_window(const struct kfsw_hk_sample *sample, uint16_t position, void *context)
{
	struct window *state = context;

	if (position == 0U) {
		state->first_sequence = sample->sequence;
	}
	state->last_sequence = sample->sequence;
	return true;
}

struct extract_state {
	struct fs_file_t file;
	uint32_t crc;
	uint16_t record_size;
	int error;
};

static bool write_one(const struct kfsw_hk_sample *sample, uint16_t position, void *context)
{
	struct extract_state *state = context;
	size_t remaining = state->record_size;
	const uint8_t *data = sample->data;

	ARG_UNUSED(position);

	while (remaining > 0U) {
		ssize_t written = fs_write(&state->file, data, remaining);

		if (written <= 0) {
			state->error = (written < 0) ? (int)written : -EIO;
			return false;
		}
		data += written;
		remaining -= (size_t)written;
	}
	state->crc = crc32_ieee_update(state->crc, sample->data, state->record_size);
	return true;
}

int kfsw_hk_store_extract(const struct kfsw_hk_store_filter *filter, const char *path,
			  uint16_t *records)
{
	char temporary_path[KFSW_HK_EXTRACT_PATH_MAX + sizeof(KFSW_HK_EXTRACT_SUFFIX)];
	uint8_t header[KFSW_HK_DATASET_HEADER_SIZE] = {0};
	uint8_t checksum[sizeof(uint32_t)];
	struct kfsw_hk_store_info info;
	struct extract_state state = {0};
	struct window selection = {0};
	uint16_t selected = 0U;
	uint16_t written = 0U;
	int result;
	int close_result;

	if ((filter == NULL) || (path == NULL) || (path[0] == '\0') ||
	    (strlen(path) >= (size_t)KFSW_HK_EXTRACT_PATH_MAX)) {
		return -EINVAL;
	}

	result = kfsw_hk_store_query(filter->report, &info);
	if (result != 0) {
		return result;
	}

	/* One pass over the selection first, so every header field but the
	 * checksum is known before a record is written. */
	result = walk(filter, note_window, &selection, &selected);
	if (result != 0) {
		return result;
	}
	if (selected == 0U) {
		return -ENOENT;
	}

	memcpy(header, KFSW_HK_DATASET_MAGIC, KFSW_HK_DATASET_MAGIC_SIZE);
	header[4] = KFSW_HK_DATASET_VERSION;
	header[5] = filter->report;
	sys_put_be16(info.record_size, &header[6]);
	sys_put_be32(selected, &header[8]);
	sys_put_be16(selection.first_sequence, &header[12]);
	sys_put_be16(selection.last_sequence, &header[14]);
	sys_put_be32(0U, &header[KFSW_HK_DATASET_CRC_OFFSET]);

	(void)snprintf(temporary_path, sizeof(temporary_path), "%s%s", path,
		       KFSW_HK_EXTRACT_SUFFIX);
	/* A temporary left by an interrupted extract would be appended to. */
	(void)fs_unlink(temporary_path);

	state.record_size = info.record_size;
	state.crc = crc32_ieee_update(0U, header, sizeof(header));
	fs_file_t_init(&state.file);
	result = fs_open(&state.file, temporary_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		return result;
	}

	if (fs_write(&state.file, header, sizeof(header)) != (ssize_t)sizeof(header)) {
		result = -EIO;
	}
	if (result == 0) {
		result = walk(filter, write_one, &state, &written);
		if (result == 0) {
			result = state.error;
		}
	}
	if ((result == 0) && (written != selected)) {
		/* The ring moved between the two passes. */
		result = -EAGAIN;
	}

	/* The checksum covers what follows it, so it is the one field written
	 * after the records, over the zeros the header was opened with. */
	if (result == 0) {
		sys_put_be32(state.crc, checksum);
		result = fs_seek(&state.file, (off_t)KFSW_HK_DATASET_CRC_OFFSET, FS_SEEK_SET);
		if ((result == 0) && (fs_write(&state.file, checksum, sizeof(checksum)) !=
				      (ssize_t)sizeof(checksum))) {
			result = -EIO;
		}
	}
	if (result == 0) {
		result = fs_sync(&state.file);
	}
	close_result = fs_close(&state.file);
	if (result == 0) {
		result = close_result;
	}
	if (result != 0) {
		(void)fs_unlink(temporary_path);
		return result;
	}

	result = fs_rename(temporary_path, path);
	if (result != 0) {
		(void)fs_unlink(temporary_path);
		return result;
	}

	kfsw_log_info("HK: extracted %u records of report %u to '%s'", written, filter->report,
		      path);
	if (records != NULL) {
		*records = written;
	}
	return 0;
}

#endif /* CONFIG_KFSW_HK_STORE */
