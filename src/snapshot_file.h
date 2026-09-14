#ifndef KFSW_SNAPSHOT_FILE_H
#define KFSW_SNAPSHOT_FILE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Replace a file's contents atomically.
 *
 * Writes a temporary file beside the target, syncs and closes it, then renames
 * it over the target, so a reset during the write leaves the old contents.
 *
 * @param path Target file.
 * @param temporary_path Scratch file beside it, on the same mount.
 * @param data Bytes to write.
 * @param size How many.
 * @retval 0 The target now holds @p data.
 * @retval -ENODEV Storage is not mounted.
 * @retval <0 An errno from the filesystem; the target is unchanged.
 */
int kfsw_snapshot_write(const char *path, const char *temporary_path, const uint8_t *data,
			size_t size);

#endif /* KFSW_SNAPSHOT_FILE_H */
