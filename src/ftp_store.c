#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#include "ftp_internal.h"

/*
 * Storage below this node's FTP root: name resolution to real paths, whole-file
 * CRC, the temporary-file rule that keeps a commit atomic, and the directory
 * operations. The server runs these for a decoded request; the client runs them
 * directly when a request is addressed to this node, so a node can inspect its
 * own storage without a connection.
 */

int kfsw_ftp_file_crc(const char *path, struct kfsw_ftp_workspace *workspace, uint32_t *file_size,
		      uint32_t *crc32)
{
	struct fs_file_t file;
	uint32_t size = 0U;
	uint32_t crc = 0U;
	ssize_t bytes_read;
	int close_result;
	int result;

	if ((path == NULL) || (workspace == NULL) || (file_size == NULL) || (crc32 == NULL)) {
		return -EINVAL;
	}
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result != 0) {
		return result;
	}
	for (;;) {
		bytes_read = fs_read(&file, workspace->chunk, sizeof(workspace->chunk));
		if (bytes_read < 0) {
			result = (int)bytes_read;
			break;
		}
		if (bytes_read == 0) {
			break;
		}
		if ((uint32_t)bytes_read > UINT32_MAX - size) {
			result = -EFBIG;
			break;
		}
		size += (uint32_t)bytes_read;
		crc = crc32_ieee_update(crc, workspace->chunk, (size_t)bytes_read);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		*file_size = size;
		*crc32 = crc;
	}
	return result;
}

int kfsw_ftp_make_temporary_path(const char *path, char *temporary_path, size_t temporary_path_size)
{
	static const char suffix[] = ".part";
	size_t path_size;

	if ((path == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	path_size = strnlen(path, temporary_path_size);
	if ((path_size == temporary_path_size) ||
	    (path_size + sizeof(suffix) > temporary_path_size)) {
		return -ENAMETOOLONG;
	}
	memcpy(temporary_path, path, path_size);
	memcpy(&temporary_path[path_size], suffix, sizeof(suffix));
	return 0;
}

int kfsw_ftp_commit_temporary(const char *path, const char *temporary_path, uint32_t actual_size,
			      uint32_t actual_crc32, uint32_t expected_size,
			      uint32_t expected_crc32)
{
	int result;

	if ((path == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	if ((actual_size != expected_size) || (actual_crc32 != expected_crc32)) {
		result = -EILSEQ;
	} else {
		result = fs_rename(temporary_path, path);
	}
	if (result != 0) {
		(void)fs_unlink(temporary_path);
	}
	return result;
}

int kfsw_ftp_local_mkdir(const char *virtual_path, struct kfsw_ftp_workspace *workspace)
{
	int result;

	if (workspace == NULL) {
		return -EINVAL;
	}
	result = kfsw_ftp_resolve_path(virtual_path, false, workspace->path,
				       sizeof(workspace->path));
	if (result != 0) {
		return result;
	}
	return fs_mkdir(workspace->path);
}

int kfsw_ftp_local_stat(const char *virtual_path, struct kfsw_ftp_workspace *workspace,
			struct kfsw_ftp_stat *info)
{
	struct fs_dirent entry;
	int result;

	if ((workspace == NULL) || (info == NULL)) {
		return -EINVAL;
	}
	result =
		kfsw_ftp_resolve_path(virtual_path, true, workspace->path, sizeof(workspace->path));
	if (result != 0) {
		return result;
	}
	result = fs_stat(workspace->path, &entry);
	if (result != 0) {
		return result;
	}
	if (entry.type == FS_DIR_ENTRY_DIR) {
		info->type = KFSW_FTP_ENTRY_DIRECTORY;
		info->size = 0U;
		info->crc32 = 0U;
		return 0;
	}
	if (entry.type != FS_DIR_ENTRY_FILE) {
		return -ENOTSUP;
	}
	info->type = KFSW_FTP_ENTRY_FILE;
	return kfsw_ftp_file_crc(workspace->path, workspace, &info->size, &info->crc32);
}

int kfsw_ftp_local_list(const char *virtual_path, struct kfsw_ftp_workspace *workspace,
			kfsw_ftp_list_visitor_t visitor, void *context)
{
	struct fs_dir_t directory;
	struct fs_dirent entry;
	int close_result;
	int result;

	if ((workspace == NULL) || (visitor == NULL)) {
		return -EINVAL;
	}
	result =
		kfsw_ftp_resolve_path(virtual_path, true, workspace->path, sizeof(workspace->path));
	if (result != 0) {
		return result;
	}
	fs_dir_t_init(&directory);
	result = fs_opendir(&directory, workspace->path);
	if (result != 0) {
		return result;
	}
	while (result == 0) {
		struct kfsw_ftp_entry visited;

		result = fs_readdir(&directory, &entry);
		if ((result != 0) || (entry.name[0] == '\0')) {
			break;
		}
		if (strnlen(entry.name, KFSW_FTP_MAX_PATH_SIZE + 1U) > KFSW_FTP_MAX_PATH_SIZE) {
			result = -ENAMETOOLONG;
			break;
		}
		if ((entry.type == FS_DIR_ENTRY_FILE) && (entry.size > UINT32_MAX)) {
			result = -EFBIG;
			break;
		}
		visited.name = entry.name;
		visited.type = (entry.type == FS_DIR_ENTRY_DIR) ? KFSW_FTP_ENTRY_DIRECTORY
								: KFSW_FTP_ENTRY_FILE;
		visited.size = (entry.type == FS_DIR_ENTRY_FILE) ? (uint32_t)entry.size : 0U;
		if (!visitor(&visited, context)) {
			break;
		}
	}
	close_result = fs_closedir(&directory);
	return (result != 0) ? result : close_result;
}
