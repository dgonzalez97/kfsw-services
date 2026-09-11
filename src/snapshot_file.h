#ifndef KFSW_SNAPSHOT_FILE_H
#define KFSW_SNAPSHOT_FILE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Replace a file's contents, or leave the previous contents alone.
 *
 * Two services keep a snapshot on the filesystem — the parameter set and the
 * housekeeping definitions — and both want the same guarantee: a reset landing
 * mid-write leaves the previous snapshot intact rather than half of two. Both
 * had their own copy of the sequence, and the copies had drifted.
 *
 * The sequence is: write a temporary file beside the target, flush it, close
 * it, and only then rename it over the target. Rename is the atomic step;
 * littlefs replaces an existing destination of the same type, so the target is
 * never unlinked first. Unlinking it would open a window in which the previous
 * snapshot is already gone and the new one is not yet in place, which is the
 * one outcome this function exists to prevent.
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
