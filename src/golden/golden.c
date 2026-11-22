#define KFSW_LOG_MODULE KFSW_LOG_MODULE_BOOT

#include <errno.h>
#include <string.h>

#include <psa/crypto.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <bootutil/image.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <kfsw/services/boot.h>
#if CONFIG_KFSW_BOOT_BUDGET
#include <kfsw/services/boot_budget.h>
#endif
#include <kfsw/services/fwu.h>
#include <kfsw/services/golden.h>
#include <kfsw/services/log.h>

#if CONFIG_KFSW_CSP_HMAC_REQUIRE
#warning "A recovery image that requires authentication cannot be reached without the key"
#endif

#define GOLDEN_NODE DT_CHOSEN(kfsw_golden_partition)
#define GOLDEN_PRESENT (DT_NODE_EXISTS(GOLDEN_NODE) && !IS_ENABLED(CONFIG_KFSW_GOLDEN_IMAGE))

#if GOLDEN_PRESENT
#define GOLDEN_ID DT_FIXED_PARTITION_ID(GOLDEN_NODE)
#define GOLDEN_SIZE DT_REG_SIZE(GOLDEN_NODE)

/* Read in pieces: the partition is larger than anything worth putting on a stack. */
#define GOLDEN_CHUNK 256U
#define GOLDEN_SHA256_SIZE 32U

static K_MUTEX_DEFINE(install_lock);
static struct kfsw_fwu_handle pending_cleanup;
#endif

bool kfsw_golden_present(void)
{
#if GOLDEN_PRESENT
	const struct flash_area *area;
	uint8_t header[4];
	bool found = false;

	if (flash_area_open(GOLDEN_ID, &area) != 0) {
		return false;
	}
	if (flash_area_read(area, 0U, header, sizeof(header)) == 0) {
		found = sys_get_le32(header) == IMAGE_MAGIC;
	}
	flash_area_close(area);
	return found;
#else
	return false;
#endif
}

#if GOLDEN_PRESENT
static int abort_install(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status)
{
	int result = kfsw_fwu_session_abort(handle, status);

	if (result == 0 || result == -ESTALE || result == -EACCES) {
		if (pending_cleanup.generation == handle->generation) {
			pending_cleanup = (struct kfsw_fwu_handle){0};
		}
		return 0;
	}
	pending_cleanup = *handle;
	kfsw_log_error("Recovery image cleanup failed (%d)", result);
	return result;
}

/* Where the TLV area starts, after the header, the body and the protected TLVs
   that the image's SHA-256 covers. */
static int hashed_size(const struct flash_area *area, uint32_t *size)
{
	struct image_header header;
	uint32_t total;
	int result = flash_area_read(area, 0U, &header, sizeof(header));

	if (result != 0) {
		return result;
	}
	total = (uint32_t)sys_le16_to_cpu(header.ih_hdr_size) +
		sys_le16_to_cpu(header.ih_protect_tlv_size);
	if ((sys_le32_to_cpu(header.ih_magic) != IMAGE_MAGIC) || (total > GOLDEN_SIZE) ||
	    (sys_le32_to_cpu(header.ih_img_size) > GOLDEN_SIZE - total)) {
		return -EBADMSG;
	}
	*size = total + sys_le32_to_cpu(header.ih_img_size);
	return 0;
}

static int expected_digest(const struct flash_area *area, uint32_t offset, uint8_t *digest)
{
	struct image_tlv_info info;
	struct image_tlv tlv;
	uint32_t end;
	int result;

	if (GOLDEN_SIZE - offset < sizeof(info)) {
		return -EBADMSG;
	}
	result = flash_area_read(area, offset, &info, sizeof(info));
	if (result != 0) {
		return result;
	}
	if ((sys_le16_to_cpu(info.it_magic) != IMAGE_TLV_INFO_MAGIC) ||
	    (sys_le16_to_cpu(info.it_tlv_tot) < sizeof(info)) ||
	    (sys_le16_to_cpu(info.it_tlv_tot) > GOLDEN_SIZE - offset)) {
		return -EBADMSG;
	}
	end = offset + sys_le16_to_cpu(info.it_tlv_tot);
	offset += sizeof(info);
	while (end - offset >= sizeof(tlv)) {
		result = flash_area_read(area, offset, &tlv, sizeof(tlv));
		if (result != 0) {
			return result;
		}
		offset += sizeof(tlv);
		if (sys_le16_to_cpu(tlv.it_len) > end - offset) {
			return -EBADMSG;
		}
		if ((sys_le16_to_cpu(tlv.it_type) == IMAGE_TLV_SHA256) &&
		    (sys_le16_to_cpu(tlv.it_len) == GOLDEN_SHA256_SIZE)) {
			return flash_area_read(area, offset, digest, GOLDEN_SHA256_SIZE);
		}
		offset += sys_le16_to_cpu(tlv.it_len);
	}
	return -EBADMSG;
}

/* The bootloader checks the signature only after the secondary slot has been
   erased and rewritten. A damaged recovery image found here costs nothing; one
   found by the bootloader costs a slot erase and a restart on every attempt. */
static int check_image(const struct flash_area *area, uint8_t *chunk)
{
	psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
	uint8_t expected[GOLDEN_SHA256_SIZE];
	uint8_t actual[GOLDEN_SHA256_SIZE];
	size_t actual_size;
	uint32_t covered;
	uint32_t offset;
	int result = hashed_size(area, &covered);

	if (result == 0) {
		result = expected_digest(area, covered, expected);
	}
	if (result != 0) {
		return result;
	}
	if ((psa_crypto_init() != PSA_SUCCESS) ||
	    (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS)) {
		return -EIO;
	}
	for (offset = 0U; offset < covered; offset += GOLDEN_CHUNK) {
		size_t size = MIN(GOLDEN_CHUNK, covered - offset);

		result = flash_area_read(area, offset, chunk, size);
		if (result == 0 && psa_hash_update(&hash, chunk, size) != PSA_SUCCESS) {
			result = -EIO;
		}
		if (result != 0) {
			(void)psa_hash_abort(&hash);
			return result;
		}
	}
	if (psa_hash_finish(&hash, actual, sizeof(actual), &actual_size) != PSA_SUCCESS) {
		(void)psa_hash_abort(&hash);
		return -EIO;
	}
	if ((actual_size != sizeof(actual)) || (memcmp(actual, expected, sizeof(actual)) != 0)) {
		return -EBADMSG;
	}
	return 0;
}

static int install_image(void)
{
	const struct flash_area *area;
	uint8_t chunk[GOLDEN_CHUNK];
	const struct kfsw_fwu_owner owner = {.kind = KFSW_FWU_OWNER_LOCAL, .peer = 1U};
	struct kfsw_fwu_image image = {.size = GOLDEN_SIZE};
	struct kfsw_fwu_handle handle = {0};
	struct kfsw_fwu_status status;
	uint32_t crc = 0U;
	uint32_t offset;
	int result;

	if (!kfsw_golden_present()) {
		return -ENOENT;
	}

	result = flash_area_open(GOLDEN_ID, &area);
	if (result != 0) {
		return result;
	}
	result = check_image(area, chunk);
	if (result != 0) {
		flash_area_close(area);
		kfsw_log_error("Recovery image failed its own SHA-256 (%d); not installed", result);
		return result;
	}

	/* From the same bytes that will be written, so the update service checks
	   the copy and not the intention. */
	for (offset = 0U; offset < GOLDEN_SIZE; offset += GOLDEN_CHUNK) {
		result = flash_area_read(area, offset, chunk, GOLDEN_CHUNK);
		if (result != 0) {
			flash_area_close(area);
			return result;
		}
		crc = crc32_ieee_update(crc, chunk, GOLDEN_CHUNK);
	}

	image.crc32 = crc;
	result = kfsw_fwu_session_begin(&owner, &image, 0U, &handle, &status);
	if (result != 0) {
		flash_area_close(area);
		if (handle.generation != 0U) {
			(void)abort_install(&handle, &status);
		}
		return result;
	}

	for (offset = 0U; offset < GOLDEN_SIZE; offset += GOLDEN_CHUNK) {
		result = flash_area_read(area, offset, chunk, GOLDEN_CHUNK);
		if (result == 0) {
			result = kfsw_fwu_session_write(&handle, offset, chunk, GOLDEN_CHUNK,
							&status);
		}
		if (result != 0) {
			flash_area_close(area);
			(void)abort_install(&handle, &status);
			return result;
		}
	}
	flash_area_close(area);

	result = kfsw_fwu_session_verify(&handle, &status);
	if (result == 0) {
		result = kfsw_fwu_session_activate(&handle, &status);
	}
	if (result != 0) {
		(void)abort_install(&handle, &status);
		return result;
	}

	kfsw_log_warning("Recovery image installed; it boots at the next restart");
	return 0;
}
#endif

int kfsw_golden_install(void)
{
#if GOLDEN_PRESENT && !CONFIG_KFSW_GOLDEN_IMAGE
	struct kfsw_fwu_status status;
	struct kfsw_fwu_handle handle;
	int result = 0;

	/* Serialize the copy and retain failed cleanup across explicit install attempts. */
	k_mutex_lock(&install_lock, K_FOREVER);
	handle = pending_cleanup;
	if (handle.generation != 0U) {
		result = abort_install(&handle, &status);
	}
	if (result == 0) {
		result = install_image();
	}
	k_mutex_unlock(&install_lock);
	return result;
#else
	return -ENODEV;
#endif
}

int kfsw_golden_check(enum kfsw_golden_outcome *outcome)
{
	if (outcome == NULL) {
		return -EINVAL;
	}
	*outcome = KFSW_GOLDEN_NOT_NEEDED;
#if CONFIG_KFSW_BOOT_BUDGET && !CONFIG_KFSW_GOLDEN_IMAGE
	uint8_t left;
	uint8_t maximum;
	int result;

	kfsw_boot_budget_get(&left, &maximum);
	if (left == UINT8_MAX) {
		return -ENODATA;
	}
	if (left != 0U) {
		return 0;
	}
	if (!kfsw_golden_present()) {
		return -ENOENT;
	}
	result = kfsw_golden_install();
	if (result != 0) {
		return result;
	}
	*outcome = KFSW_GOLDEN_INSTALLED;
#endif
	return 0;
}
