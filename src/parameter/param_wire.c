#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "param_wire.h"

_Static_assert(sizeof(float) == sizeof(uint32_t),
	       "parameter files require a 32-bit IEEE-compatible float");

int kfsw_param_wire_type_of(const struct kfsw_param_entry *entry, uint8_t *type,
			    uint16_t *value_size)
{
	switch (entry->info.type) {
	case KFSW_PARAM_U8:
		*type = KFSW_PARAM_WIRE_U8;
		*value_size = sizeof(uint8_t);
		return 0;
	case KFSW_PARAM_U16:
		*type = KFSW_PARAM_WIRE_U16;
		*value_size = sizeof(uint16_t);
		return 0;
	case KFSW_PARAM_I16:
		*type = KFSW_PARAM_WIRE_I16;
		*value_size = sizeof(int16_t);
		return 0;
	case KFSW_PARAM_U32:
		*type = KFSW_PARAM_WIRE_U32;
		*value_size = sizeof(uint32_t);
		return 0;
	case KFSW_PARAM_I32:
		*type = KFSW_PARAM_WIRE_I32;
		*value_size = sizeof(int32_t);
		return 0;
	case KFSW_PARAM_FLOAT:
		*type = KFSW_PARAM_WIRE_FLOAT;
		*value_size = sizeof(float);
		return 0;
	case KFSW_PARAM_DATA:
		*type = KFSW_PARAM_WIRE_DATA;
		*value_size = entry->info.array_size;
		return 0;
	case KFSW_PARAM_STRING:
		*type = KFSW_PARAM_WIRE_STRING;
		/* The encoder uses the current length, including its terminator. */
		*value_size = 0U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

int kfsw_param_wire_encode(const struct kfsw_param_entry *entry, uint8_t *output,
			   size_t output_size, uint16_t *written)
{
	struct kfsw_param_value value = {0};
	uint16_t value_size;
	uint8_t type;
	uint32_t raw_value;
	int result;

	result = kfsw_param_wire_type_of(entry, &type, &value_size);
	if (result != 0) {
		return result;
	}

	result = kfsw_param_read_entry(entry, &value);
	if (result != 0) {
		return result;
	}
	if (type == KFSW_PARAM_WIRE_STRING) {
		value_size = (uint16_t)value.size;
	}
	if (output_size < value_size) {
		return -ENOSPC;
	}
	switch (type) {
	case KFSW_PARAM_WIRE_U8:
		output[0] = value.scalar.u8;
		break;
	case KFSW_PARAM_WIRE_U16:
		sys_put_be16(value.scalar.u16, output);
		break;
	case KFSW_PARAM_WIRE_I16:
		sys_put_be16((uint16_t)value.scalar.i16, output);
		break;
	case KFSW_PARAM_WIRE_U32:
		sys_put_be32(value.scalar.u32, output);
		break;
	case KFSW_PARAM_WIRE_I32:
		sys_put_be32((uint32_t)value.scalar.i32, output);
		break;
	case KFSW_PARAM_WIRE_FLOAT:
		memcpy(&raw_value, &value.scalar.f32, sizeof(raw_value));
		sys_put_be32(raw_value, output);
		break;
	case KFSW_PARAM_WIRE_STRING:
		memcpy(output, value.text, value_size);
		break;
	case KFSW_PARAM_WIRE_DATA:
		memcpy(output, value.bytes, value_size);
		break;
	default:
		return -ENOTSUP;
	}

	*written = value_size;
	return 0;
}

int kfsw_param_wire_decode(const struct kfsw_param_entry *entry, uint8_t type, const uint8_t *value,
			   uint16_t value_size, struct kfsw_param_value *decoded)
{
	uint32_t raw_value;
	uint16_t expected_size;
	uint8_t expected_type;
	int result;

	*decoded = (struct kfsw_param_value){
		.type = entry->info.type,
		.size = value_size,
	};

	/* The code has to be the one this parameter's type is written as, and the
	 * bytes have to be the width that code means. Coercing a U32 into a U8
	 * would quietly deliver a different value than the file asked for.
	 */
	result = kfsw_param_wire_type_of(entry, &expected_type, &expected_size);
	if (result != 0) {
		return result;
	}
	if (type != expected_type) {
		return -EBADMSG;
	}
	if ((expected_size != 0U) && (value_size != expected_size)) {
		return -EBADMSG;
	}

	switch (type) {
	case KFSW_PARAM_WIRE_U8:
		decoded->scalar.u8 = value[0];
		break;
	case KFSW_PARAM_WIRE_U16:
		decoded->scalar.u16 = sys_get_be16(value);
		break;
	case KFSW_PARAM_WIRE_I16:
		decoded->scalar.i16 = (int16_t)sys_get_be16(value);
		break;
	case KFSW_PARAM_WIRE_U32:
		decoded->scalar.u32 = sys_get_be32(value);
		break;
	case KFSW_PARAM_WIRE_I32:
		decoded->scalar.i32 = (int32_t)sys_get_be32(value);
		break;
	case KFSW_PARAM_WIRE_FLOAT:
		raw_value = sys_get_be32(value);
		memcpy(&decoded->scalar.f32, &raw_value, sizeof(decoded->scalar.f32));
		break;
	case KFSW_PARAM_WIRE_DATA:
		/* An array must have the same length. */
		if ((value_size == 0U) || (value_size != entry->info.array_size) ||
		    (value_size > sizeof(decoded->bytes))) {
			return -EBADMSG;
		}
		memcpy(decoded->bytes, value, value_size);
		decoded->size = value_size;
		break;
	case KFSW_PARAM_WIRE_STRING:
		/* A string without its terminator is corrupt. */
		if ((value_size == 0U) || (value_size > sizeof(decoded->text)) ||
		    (value[value_size - 1U] != '\0')) {
			return -EBADMSG;
		}
		memcpy(decoded->text, value, value_size);
		decoded->size = value_size;
		break;
	default:
		return -ENOTSUP;
	}

	return kfsw_param_validate_entry(entry, decoded);
}

void kfsw_param_wire_apply(const struct kfsw_param_entry *entry,
			   const struct kfsw_param_value *decoded)
{
	if (decoded->type == KFSW_PARAM_STRING) {
		kfsw_param_write_text_entry(entry, decoded->text);
	} else if (decoded->type == KFSW_PARAM_DATA) {
		kfsw_param_write_data_entry(entry, decoded->bytes, decoded->size);
	} else {
		kfsw_param_write_entry(entry, &decoded->scalar);
	}
}
