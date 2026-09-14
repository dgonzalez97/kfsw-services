#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#include <kfsw/platform/storage.h>

#include "snapshot_file.h"

/* fs_write is allowed to write less than it was given. */
static int write_all(struct fs_file_t *file, const uint8_t *data, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t written = fs_write(file, &data[offset], size - offset);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}
		offset += (size_t)written;
	}
	return 0;
}

int kfsw_snapshot_write(const char *path, const char *temporary_path, const uint8_t *data,
			size_t size)
{
	struct fs_file_t file;
	int result;
	int close_result;

	if ((path == NULL) || (temporary_path == NULL) || (data == NULL)) {
		return -EINVAL;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	/* A temporary left by an interrupted write would otherwise be appended
	 * to or reopened at its old length.
	 */
	(void)fs_unlink(temporary_path);

	fs_file_t_init(&file);
	result = fs_open(&file, temporary_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		return result;
	}

	result = write_all(&file, data, size);
	if (result == 0) {
		/* Sync before the rename. */
		result = fs_sync(&file);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result != 0) {
		(void)fs_unlink(temporary_path);
		return result;
	}

	/* Rename is the atomic step; the target is not unlinked first. */
	result = fs_rename(temporary_path, path);
	if (result != 0) {
		(void)fs_unlink(temporary_path);
	}
	return result;
}
