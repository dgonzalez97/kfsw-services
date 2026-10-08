#ifndef KFSW_SERVICES_TABLE_H
#define KFSW_SERVICES_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/parameter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_table K-FSW parameter tables as files
 * @ingroup kfsw_services
 *
 * A parameter table uploaded as one file, checked completely before any of it
 * takes effect, and undoable afterwards.
 *
 * The parameter snapshot is the other way of changing many values at once, and
 * it is a different job: the node writes it, it covers whatever is marked
 * persistent across every table, and it is adopted at boot. A table file is
 * written on the ground, addresses one table, and is adopted when an operator
 * says so during a pass. Both use the same value encoding, so one decoder reads
 * either.
 *
 * @{
 */

/** Entries one table file may carry. */
#define KFSW_TABLE_MAX_ENTRIES CONFIG_KFSW_TABLE_MAX_ENTRIES

/** Longest path kept for the file that was loaded, including the terminator. */
#define KFSW_TABLE_PATH_SIZE 64U

/** Where this node keeps table files it writes, and the usual place to upload to. */
#define KFSW_TABLE_DIRECTORY "/kfsw/tables"

/** Parameter table of this service. */
#define KFSW_TABLE_PARAM_TABLE_ID 37U
#define KFSW_TABLE_PARAM_TABLE_NAME "table"

/** Event IDs of this service. */
enum kfsw_table_event {
	/** A file was refused; nothing was applied. */
	KFSW_EVENT_TABLE_REJECTED = 1,
	/** A file was adopted. */
	KFSW_EVENT_TABLE_LOADED = 2,
	/** The last load was undone. */
	KFSW_EVENT_TABLE_REVERTED = 3,
};

/** Whether anything is loaded, and whether it can still be undone. */
enum kfsw_table_state {
	/** Nothing has been adopted since boot. */
	KFSW_TABLE_EMPTY = 0,
	/** A file is in effect and the values it replaced are still held. */
	KFSW_TABLE_LOADED = 1,
	/** The last load has been undone; there is nothing left to revert. */
	KFSW_TABLE_REVERTED = 2,
};

/** What a validate or a load found in a file. */
struct kfsw_table_report {
	/** Table the file addresses. */
	uint8_t table;
	/** Entries the file carries. */
	uint16_t entries;
	/** Entries that passed every check. */
	uint16_t accepted;
	/** Position of the first entry that failed, counting from one, or zero. */
	uint16_t failed_entry;
	/** That entry's offset within the table. */
	uint8_t failed_offset;
	/** Why it failed, as an errno, or zero. */
	int reason;
};

/** Service state and counters. */
struct kfsw_table_status {
	/** One of @ref kfsw_table_state. */
	uint8_t state;
	/** Table of the last adopted file. */
	uint8_t table;
	/** Entries it carried. */
	uint16_t entries;
	/** Files adopted since boot; saturates. */
	uint32_t loads;
	/** Files refused since boot; saturates. */
	uint32_t rejections;
	/** Loads undone since boot; saturates. */
	uint32_t reverts;
	/** File of the last adopted load, or empty. */
	char path[KFSW_TABLE_PATH_SIZE];
};

/**
 * @brief Check a table file without applying any of it.
 *
 * Reads the file, checks its header, its checksum and every entry against the
 * running parameter registry, and reports what it found.
 *
 * @param path File to read.
 * @param[out] report What the file holds and, on failure, which entry stopped
 *                    it. May be NULL.
 *
 * @retval 0 Every entry would be accepted.
 * @retval -EINVAL @p path is NULL or too long.
 * @retval -EACCES The parameter service is not initialized.
 * @retval -ENODEV Storage is not mounted.
 * @retval -ENOENT There is no such file.
 * @retval -EFBIG The file is larger than this image can read.
 * @retval -EBADMSG It is not a table file, its checksum is wrong, or an entry
 *                  was refused. @p report says which and why.
 * @retval -ENOTSUP Its format version is not the one this image writes.
 */
int kfsw_table_validate(const char *path, struct kfsw_table_report *report);

/**
 * @brief Adopt a table file.
 *
 * Validates the whole file first and applies nothing unless every entry passes,
 * so a file with one bad entry leaves the table as it was. The values it
 * replaces are held, so the load can be undone with @ref kfsw_table_revert.
 *
 * @param path File to read.
 * @param[out] report What was applied, or what stopped it. May be NULL.
 *
 * @return The codes of @ref kfsw_table_validate.
 */
int kfsw_table_load(const char *path, struct kfsw_table_report *report);

/**
 * @brief Put back the values the last load replaced.
 *
 * @retval 0 The previous values are back.
 * @retval -ENOENT Nothing has been loaded, or the last load was already undone.
 * @retval -EACCES The parameter service is not initialized.
 */
int kfsw_table_revert(void);

/**
 * @brief Write a table's current values out as a table file.
 *
 * So an operator can fetch what is running, change a value and upload it back
 * rather than composing a file from the manual.
 *
 * @param table Table identifier, 1..99.
 * @param path File to write, usually under @ref KFSW_TABLE_DIRECTORY, which is
 *             created if it is missing. Replaced atomically.
 * @param[out] entries Entries written. May be NULL.
 *
 * @retval 0 The file holds the table.
 * @retval -EINVAL @p table or @p path is not usable.
 * @retval -EACCES The parameter service is not initialized.
 * @retval -ENODEV Storage is not mounted.
 * @retval -ENOENT No such table is registered.
 * @retval -E2BIG The table has more entries than a file may carry.
 * @retval <0 An errno from the filesystem; any previous file is unchanged.
 */
int kfsw_table_dump(uint8_t table, const char *path, uint16_t *entries);

/**
 * @brief Read a snapshot of the service.
 *
 * @param[out] status Destination.
 * @retval 0 Written.
 * @retval -EINVAL @p status is NULL.
 */
int kfsw_table_get_status(struct kfsw_table_status *status);

/** Human-readable name for a state; "unknown" for an unrecognised value. */
const char *kfsw_table_state_name(enum kfsw_table_state state);

/** Definitions of this service's own parameters. */
extern const struct kfsw_param_definition_set kfsw_table_param_definitions;

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* KFSW_SERVICES_TABLE_H */
