#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>

#include "ftp_link.h"

/*
 * The one send loop and the one receive loop. Client PUT and server GET send;
 * client GET and server PUT receive. The roles differ only in which data
 * opcode they carry, so both directions run the same code and the same
 * validation.
 */

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

/*
 * Every field of an inbound data message is checked against what the transfer
 * already agreed, so a stray or replayed packet cannot advance the write.
 */
static bool data_message_is_valid(const struct kfsw_ftp_transfer *transfer,
				  const struct kfsw_ftp_message *message)
{
	return (message->opcode == transfer->data_opcode) &&
	       (message->request_id == transfer->request_id) && (message->status == 0U) &&
	       (message->flags == 0U) && (message->path_size == 0U) && (message->data_size != 0U) &&
	       (message->offset == transfer->offset) &&
	       (message->total_size == transfer->total_size) &&
	       (message->crc32 == transfer->crc32) &&
	       (message->data_size <= transfer->total_size - transfer->offset);
}

int kfsw_ftp_transfer_open_source(struct kfsw_ftp_transfer *transfer, const char *source_path)
{
	if ((transfer == NULL) || (source_path == NULL)) {
		return -EINVAL;
	}
	fs_file_t_init(&transfer->file);
	return fs_open(&transfer->file, source_path, FS_O_READ);
}

int kfsw_ftp_transfer_send(struct kfsw_ftp_transfer *transfer)
{
	int close_result;
	int result = 0;

	if ((transfer == NULL) || (transfer->workspace == NULL)) {
		return -EINVAL;
	}
	while ((result == 0) && (transfer->offset < transfer->total_size)) {
		ssize_t bytes_read = fs_read(&transfer->file, transfer->workspace->chunk,
					     sizeof(transfer->workspace->chunk));
		struct kfsw_ftp_message data_message = {
			.opcode = transfer->data_opcode,
			.request_id = transfer->request_id,
			.offset = transfer->offset,
			.total_size = transfer->total_size,
			.crc32 = transfer->crc32,
			.data = transfer->workspace->chunk,
		};

		if (bytes_read <= 0) {
			result = (bytes_read < 0) ? (int)bytes_read : -EIO;
			break;
		}
		data_message.data_size = (uint16_t)bytes_read;
		result = kfsw_ftp_link_send(transfer->link, &data_message);
		transfer->offset += (result == 0) ? (uint32_t)bytes_read : 0U;
	}
	close_result = fs_close(&transfer->file);
	return (result != 0) ? result : close_result;
}

int kfsw_ftp_transfer_open_sink(struct kfsw_ftp_transfer *transfer, const char *temporary_path)
{
	if ((transfer == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	(void)fs_unlink(temporary_path);
	fs_file_t_init(&transfer->file);
	return fs_open(&transfer->file, temporary_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
}

int kfsw_ftp_transfer_receive(struct kfsw_ftp_transfer *transfer)
{
	int result = 0;

	if (transfer == NULL) {
		return -EINVAL;
	}
	while ((result == 0) && (transfer->offset < transfer->total_size)) {
		struct kfsw_ftp_link_frame frame;

		result = kfsw_ftp_link_receive(transfer->link, &frame);
		if (result != 0) {
			break;
		}
		if (!data_message_is_valid(transfer, &frame.message)) {
			result = -EBADMSG;
		} else {
			result = write_all(&transfer->file, frame.message.data,
					   frame.message.data_size);
		}
		if (result == 0) {
			transfer->actual_crc32 =
				crc32_ieee_update(transfer->actual_crc32, frame.message.data,
						  frame.message.data_size);
			transfer->offset += frame.message.data_size;
		}
		kfsw_ftp_link_release(&frame);
	}
	return result;
}

int kfsw_ftp_transfer_finish(struct kfsw_ftp_transfer *transfer, const char *target_path,
			     const char *temporary_path, int result)
{
	int close_result;

	if ((transfer == NULL) || (target_path == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	if (result == 0) {
		result = fs_sync(&transfer->file);
	}
	close_result = fs_close(&transfer->file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		result = kfsw_ftp_commit_temporary(target_path, temporary_path, transfer->offset,
						   transfer->actual_crc32, transfer->total_size,
						   transfer->crc32);
	}
	if (result != 0) {
		(void)fs_unlink(temporary_path);
	}
	return result;
}
