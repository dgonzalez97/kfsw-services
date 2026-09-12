#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <bootutil/image.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/fwu.h>
#include "fwu_internal.h"

#define BOOT_ROOT "/kfsw/boot"

BUILD_ASSERT(MAX_FILE_NAME >= sizeof("firmware_1.bin") - 1U);

static const char *const slot_names[] = {"firmware_1.bin", "firmware_2.bin"};
static const uint8_t slot_ids[] = {
	DT_FIXED_PARTITION_ID(DT_NODELABEL(slot0_partition)),
	DT_FIXED_PARTITION_ID(DT_CHOSEN(kfsw_fwu_partition)),
};

struct slot_file {
	const struct flash_area *area;
	uint32_t start;
	uint32_t size;
	uint32_t position;
	bool secondary;
};

K_MUTEX_DEFINE(slot_files_lock);
static struct slot_file files[CONFIG_KFSW_FWU_FILE_HANDLES];

static int slot_number(const char *path)
{
	for (size_t slot = 0; slot < ARRAY_SIZE(slot_names); slot++) {
		if ((strncmp(path, BOOT_ROOT "/", sizeof(BOOT_ROOT)) == 0) &&
		    (strcmp(path + sizeof(BOOT_ROOT), slot_names[slot]) == 0)) {
			return (int)slot;
		}
	}
	return -ENOENT;
}

/* Bounds cover the TLV records as well as their outer length. */
static int tlv_extent(const struct flash_area *area, uint32_t offset, uint32_t limit,
		      uint16_t magic, uint32_t *size)
{
	struct image_tlv_info info;
	uint32_t position;
	uint32_t end;
	int result;

	if ((offset > limit) || (limit - offset < sizeof(info))) {
		return -EBADMSG;
	}
	result = flash_area_read(area, offset, &info, sizeof(info));
	if (result != 0) {
		return result;
	}
	*size = sys_le16_to_cpu(info.it_tlv_tot);
	if ((sys_le16_to_cpu(info.it_magic) != magic) || (*size < sizeof(info)) ||
	    (*size > limit - offset)) {
		return -EBADMSG;
	}
	position = offset + sizeof(info);
	end = offset + *size;
	while (position < end) {
		struct image_tlv tlv;
		uint32_t length;

		if (end - position < sizeof(tlv)) {
			return -EBADMSG;
		}
		result = flash_area_read(area, position, &tlv, sizeof(tlv));
		if (result != 0) {
			return result;
		}
		position += sizeof(tlv);
		length = sys_le16_to_cpu(tlv.it_len);
		if (length > end - position) {
			return -EBADMSG;
		}
		position += length;
	}
	return 0;
}

static int image_extent(struct slot_file *file, uint32_t start)
{
	struct image_header header;
	struct flash_pages_info last_page;
	uint32_t limit;
	uint32_t header_size;
	uint32_t body_size;
	uint32_t protected_size;
	uint32_t position;
	uint32_t tlv_size;
	int result;

	result = flash_get_page_info_by_offs(flash_area_get_device(file->area),
					     file->area->fa_off + file->area->fa_size - 1U,
					     &last_page);
	if (result != 0) {
		return result;
	}
	if (last_page.size >= file->area->fa_size) {
		return -EBADMSG;
	}
	limit = file->area->fa_size - last_page.size;
	if ((start > limit) || (limit - start < sizeof(header))) {
		return -EBADMSG;
	}
	result = flash_area_read(file->area, start, &header, sizeof(header));
	if (result != 0) {
		return result;
	}
	if (sys_le32_to_cpu(header.ih_magic) != IMAGE_MAGIC) {
		return -ENOENT;
	}
	header_size = sys_le16_to_cpu(header.ih_hdr_size);
	body_size = sys_le32_to_cpu(header.ih_img_size);
	protected_size = sys_le16_to_cpu(header.ih_protect_tlv_size);
	if ((header_size < sizeof(header)) || (header_size > limit - start) || (body_size == 0U) ||
	    (body_size > limit - start - header_size)) {
		return -EBADMSG;
	}
	position = start + header_size + body_size;
	if (protected_size != 0U) {
		result = tlv_extent(file->area, position, limit, IMAGE_TLV_PROT_INFO_MAGIC,
				    &tlv_size);
		if (result != 0) {
			return result;
		}
		if (tlv_size != protected_size) {
			return -EBADMSG;
		}
		position += tlv_size;
	}
	result = tlv_extent(file->area, position, limit, IMAGE_TLV_INFO_MAGIC, &tlv_size);
	if (result != 0) {
		return result;
	}
	file->start = start;
	file->size = position + tlv_size - start;
	file->position = 0U;
	return 0;
}

static void release_image(struct slot_file *file)
{
	flash_area_close(file->area);
	file->area = NULL;
	if (file->secondary) {
		kfsw_fwu_secondary_release();
	}
}

static int open_image(int slot, struct slot_file *file)
{
	int result;

	file->secondary = slot == 1;
	if (file->secondary) {
		result = kfsw_fwu_secondary_acquire();
		if (result != 0) {
			return result;
		}
	}
	result = flash_area_open(slot_ids[slot], &file->area);
	if (result != 0) {
		if (file->secondary) {
			kfsw_fwu_secondary_release();
		}
		return result;
	}
	result = image_extent(file, 0U);
	/* Swap leaves the previous image at the slot base. Incoming images use
	 * the configured offset. Confirmation can change the swap status without
	 * moving those bytes, so locate the header instead of inferring its place.
	 */
	if ((result == -ENOENT) && file->secondary && (CONFIG_KFSW_FWU_SLOT_OFFSET_SECTORS != 0)) {
		uint32_t offset = kfsw_fwu_slot_write_offset();

		result = offset == 0U ? -EIO : image_extent(file, offset);
	}
	if (result != 0) {
		release_image(file);
	}
	return result;
}

static int slot_open(struct fs_file_t *fp, const char *path, fs_mode_t flags)
{
	int slot = slot_number(path);
	int result = -EMFILE;

	if (flags != FS_O_READ) {
		return -EROFS;
	}
	if (slot < 0) {
		return slot;
	}
	k_mutex_lock(&slot_files_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		if (files[i].area == NULL) {
			result = open_image(slot, &files[i]);
			if (result == 0) {
				fp->filep = &files[i];
			}
			break;
		}
	}
	k_mutex_unlock(&slot_files_lock);
	return result;
}

static int slot_close(struct fs_file_t *fp)
{
	k_mutex_lock(&slot_files_lock, K_FOREVER);
	release_image(fp->filep);
	fp->filep = NULL;
	k_mutex_unlock(&slot_files_lock);
	return 0;
}

static ssize_t slot_read(struct fs_file_t *fp, void *buffer, size_t size)
{
	struct slot_file *file = fp->filep;
	int result = 0;

	k_mutex_lock(&slot_files_lock, K_FOREVER);
	size = MIN(size, file->size - file->position);
	if (size != 0U) {
		result = flash_area_read(file->area, file->start + file->position, buffer, size);
		if (result == 0) {
			file->position += size;
		}
	}
	k_mutex_unlock(&slot_files_lock);
	return result == 0 ? (ssize_t)size : result;
}

static int slot_seek(struct fs_file_t *fp, off_t offset, int whence)
{
	struct slot_file *file = fp->filep;
	int64_t base = 0;
	int result = 0;

	k_mutex_lock(&slot_files_lock, K_FOREVER);
	if (whence == FS_SEEK_CUR) {
		base = file->position;
	} else if (whence == FS_SEEK_END) {
		base = file->size;
	} else if (whence != FS_SEEK_SET) {
		result = -EINVAL;
	}
	if ((offset < -base) || (offset > (int64_t)file->size - base)) {
		result = -EINVAL;
	}
	if (result == 0) {
		file->position = (uint32_t)(base + offset);
	}
	k_mutex_unlock(&slot_files_lock);
	return result;
}

static off_t slot_tell(struct fs_file_t *fp)
{
	struct slot_file *file = fp->filep;
	off_t position;

	k_mutex_lock(&slot_files_lock, K_FOREVER);
	position = file->position;
	k_mutex_unlock(&slot_files_lock);
	return position;
}

static int slot_stat(struct fs_mount_t *mount, const char *path, struct fs_dirent *entry)
{
	struct slot_file file = {0};
	int slot;
	int result;

	ARG_UNUSED(mount);
	memset(entry, 0, sizeof(*entry));
	if (strcmp(path, BOOT_ROOT) == 0) {
		entry->type = FS_DIR_ENTRY_DIR;
		strcpy(entry->name, "boot");
		return 0;
	}
	slot = slot_number(path);
	if (slot < 0) {
		return slot;
	}
	result = open_image(slot, &file);
	if (result != 0) {
		return result;
	}
	entry->type = FS_DIR_ENTRY_FILE;
	entry->size = file.size;
	strcpy(entry->name, slot_names[slot]);
	release_image(&file);
	return 0;
}

static int slot_opendir(struct fs_dir_t *dir, const char *path)
{
	if (strcmp(path, BOOT_ROOT) != 0) {
		return -ENOENT;
	}
	dir->dirp = (void *)(uintptr_t)1U;
	return 0;
}

static int slot_readdir(struct fs_dir_t *dir, struct fs_dirent *entry)
{
	while ((uintptr_t)dir->dirp <= ARRAY_SIZE(slot_names)) {
		struct slot_file file = {0};
		int slot = (uintptr_t)dir->dirp - 1U;
		int result = open_image(slot, &file);

		dir->dirp = (void *)((uintptr_t)dir->dirp + 1U);
		if ((result == -ENOENT) || (result == -EBUSY) || (result == -EBADMSG)) {
			continue;
		}
		if (result != 0) {
			return result;
		}
		memset(entry, 0, sizeof(*entry));
		entry->type = FS_DIR_ENTRY_FILE;
		entry->size = file.size;
		strcpy(entry->name, slot_names[slot]);
		release_image(&file);
		return 0;
	}
	memset(entry, 0, sizeof(*entry));
	return 0;
}

static int slot_closedir(struct fs_dir_t *dir)
{
	dir->dirp = NULL;
	return 0;
}

static int slot_mount(struct fs_mount_t *mount)
{
	ARG_UNUSED(mount);
	return 0;
}

static const struct fs_file_system_t slot_fs = {
	.open = slot_open,
	.close = slot_close,
	.read = slot_read,
	.lseek = slot_seek,
	.tell = slot_tell,
	.stat = slot_stat,
	.opendir = slot_opendir,
	.readdir = slot_readdir,
	.closedir = slot_closedir,
	.mount = slot_mount,
};

static struct fs_mount_t slot_mountpoint = {
	.type = FS_TYPE_EXTERNAL_BASE,
	.mnt_point = BOOT_ROOT,
	.flags = FS_MOUNT_FLAG_READ_ONLY,
};

int kfsw_fwu_files_mount(void)
{
	int result = fs_register(slot_mountpoint.type, &slot_fs);

	if (result != 0) {
		return result;
	}
	result = fs_mount(&slot_mountpoint);
	if (result != 0) {
		(void)fs_unregister(slot_mountpoint.type, &slot_fs);
	}
	return result;
}
