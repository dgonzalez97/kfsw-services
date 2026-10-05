#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#include "ftp_internal.h"

/*
 * Storage under the FTP root: path resolution, file CRC, the temporary file and
 * rename, and directory operations. Used by the server and by local requests.
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

int kfsw_ftp_check_space(const char *path, uint32_t bytes)
{
	struct fs_statvfs volume;
	int result = fs_statvfs(path, &volume);

	if (result != 0) {
		return result;
	}
	if ((uint64_t)volume.f_bfree * volume.f_frsize <
	    (uint64_t)bytes + CONFIG_KFSW_FTP_SPACE_MARGIN_BYTES) {
		return -ENOSPC;
	}
	return 0;
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

/*
 * A partial upload keeps a note beside it naming the file it will become, so a
 * partial from a different version is not continued into this one. Chunks
 * arrive in order, so the partial's own size is the resume point.
 */
#define KFSW_FTP_PARTIAL_MAGIC 0x4B465450UL /* "KFTP" */
#define KFSW_FTP_PARTIAL_VERSION 1U

struct partial_note {
	uint32_t magic;
	uint32_t version;
	uint32_t total_size;
	uint32_t crc32;
	uint32_t note_crc32;
};

static uint32_t note_crc(const struct partial_note *note)
{
	return crc32_ieee((const uint8_t *)note, offsetof(struct partial_note, note_crc32));
}

static int make_note_path(const char *path, char *note_path, size_t note_path_size)
{
	static const char suffix[] = ".map";
	size_t path_size;
	int result = kfsw_ftp_make_temporary_path(path, note_path, note_path_size);

	if (result != 0) {
		return result;
	}
	path_size = strnlen(note_path, note_path_size);
	if (path_size + sizeof(suffix) > note_path_size) {
		return -ENAMETOOLONG;
	}
	memcpy(&note_path[path_size], suffix, sizeof(suffix));
	return 0;
}

static int read_note(const char *note_path, struct partial_note *note)
{
	struct fs_file_t file;
	ssize_t bytes_read;
	int close_result;
	int result;

	fs_file_t_init(&file);
	result = fs_open(&file, note_path, FS_O_READ);
	if (result != 0) {
		return result;
	}
	bytes_read = fs_read(&file, note, sizeof(*note));
	close_result = fs_close(&file);
	if (bytes_read < 0) {
		return (int)bytes_read;
	}
	if (close_result != 0) {
		return close_result;
	}
	if ((size_t)bytes_read != sizeof(*note)) {
		return -EILSEQ;
	}
	if ((note->magic != KFSW_FTP_PARTIAL_MAGIC) ||
	    (note->version != KFSW_FTP_PARTIAL_VERSION) || (note->note_crc32 != note_crc(note))) {
		return -EILSEQ;
	}
	return 0;
}

int kfsw_ftp_partial_resume_point(const char *path, struct kfsw_ftp_workspace *workspace,
				  uint32_t total_size, uint32_t crc32, uint32_t *offset,
				  uint32_t *partial_crc32)
{
	char scratch[KFSW_FTP_FULL_PATH_SIZE];
	struct partial_note note;
	uint32_t partial_size;

	if ((path == NULL) || (workspace == NULL) || (offset == NULL) || (partial_crc32 == NULL)) {
		return -EINVAL;
	}
	*offset = 0U;
	*partial_crc32 = 0U;

	if (make_note_path(path, scratch, sizeof(scratch)) != 0) {
		return 0;
	}
	if (read_note(scratch, &note) != 0) {
		return 0;
	}
	/* A note for a different file is no use, however far that upload got. */
	if ((note.total_size != total_size) || (note.crc32 != crc32)) {
		return 0;
	}
	if (kfsw_ftp_make_temporary_path(path, scratch, sizeof(scratch)) != 0) {
		return 0;
	}
	if (kfsw_ftp_file_crc(scratch, workspace, &partial_size, partial_crc32) != 0) {
		return 0;
	}
	/* Nothing to continue from an empty or already complete partial. */
	if ((partial_size == 0U) || (partial_size >= total_size)) {
		*partial_crc32 = 0U;
		return 0;
	}
	*offset = partial_size;
	return 0;
}

int kfsw_ftp_partial_note_write(const char *path, uint32_t total_size, uint32_t crc32)
{
	char note_path[KFSW_FTP_FULL_PATH_SIZE];
	struct partial_note note = {
		.magic = KFSW_FTP_PARTIAL_MAGIC,
		.version = KFSW_FTP_PARTIAL_VERSION,
		.total_size = total_size,
		.crc32 = crc32,
	};
	struct fs_file_t file;
	ssize_t written;
	int close_result;
	int result = make_note_path(path, note_path, sizeof(note_path));

	if (result != 0) {
		return result;
	}
	note.note_crc32 = note_crc(&note);
	fs_file_t_init(&file);
	result = fs_open(&file, note_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		return result;
	}
	written = fs_write(&file, &note, sizeof(note));
	if (written < 0) {
		result = (int)written;
	} else if ((size_t)written != sizeof(note)) {
		result = -EIO;
	}
	if (result == 0) {
		result = fs_sync(&file);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result != 0) {
		(void)fs_unlink(note_path);
	}
	return result;
}

void kfsw_ftp_partial_note_remove(const char *path)
{
	char note_path[KFSW_FTP_FULL_PATH_SIZE];

	if (make_note_path(path, note_path, sizeof(note_path)) == 0) {
		(void)fs_unlink(note_path);
	}
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
	/* The partial is gone either way, so its note describes nothing. */
	kfsw_ftp_partial_note_remove(path);
	return result;
}

int kfsw_ftp_local_mkdir(const char *virtual_path, struct kfsw_ftp_workspace *workspace)
{
	int result;

	if (workspace == NULL) {
		return -EINVAL;
	}
	result =
		kfsw_ftp_resolve_write_path(virtual_path, workspace->path, sizeof(workspace->path));
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
	bool listing_root;
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
	listing_root = strcmp(workspace->path, KFSW_FTP_ROOT_PATH) == 0;
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
			listing_root = false;
			break;
		}
	}
	close_result = fs_closedir(&directory);
	if (result != 0) {
		return result;
	}
	if ((close_result == 0) && listing_root) {
		(void)kfsw_ftp_list_roots(visitor, context);
	}
	return close_result;
}
