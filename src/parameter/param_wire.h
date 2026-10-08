#ifndef KFSW_SERVICES_PARAM_WIRE_H
#define KFSW_SERVICES_PARAM_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "parameter_internal.h"

/*
 * The type codes and byte layout a parameter value takes in a file: the
 * snapshot the node writes itself, and a table image uploaded from the ground.
 * One encoding means one decoder on the ground rather than two that drift.
 *
 * Values are big-endian and the codes never change meaning. An unknown code is
 * refused rather than guessed at.
 */
enum kfsw_param_wire_type {
	KFSW_PARAM_WIRE_U8 = 1,
	KFSW_PARAM_WIRE_U32 = 2,
	KFSW_PARAM_WIRE_I32 = 3,
	KFSW_PARAM_WIRE_FLOAT = 4,
	/* Added later. */
	KFSW_PARAM_WIRE_U16 = 5,
	KFSW_PARAM_WIRE_I16 = 6,
	/* Carried with its terminator, so the size covers the whole thing and a
	 * shorter string needs no padding. */
	KFSW_PARAM_WIRE_STRING = 7,
	/* Fixed length: the size is the element count. Any other size is refused. */
	KFSW_PARAM_WIRE_DATA = 8,
};

/** Longest value any type occupies on the wire. */
#define KFSW_PARAM_WIRE_MAX_VALUE_SIZE KFSW_PARAM_STRING_MAX

/**
 * @brief The wire code and byte count for a parameter's type.
 *
 * @param entry Registry entry.
 * @param[out] type One of @ref kfsw_param_wire_type.
 * @param[out] value_size Bytes the value occupies, or zero for a string, whose
 *                        length is only known once it has been read.
 * @retval 0 The type is carried in a file.
 * @retval -ENOTSUP It is not.
 */
int kfsw_param_wire_type_of(const struct kfsw_param_entry *entry, uint8_t *type,
			    uint16_t *value_size);

/**
 * @brief Encode a parameter's current value.
 *
 * @param entry Registry entry, read through the usual sampling path.
 * @param output Destination.
 * @param output_size Room in @p output.
 * @param[out] written Bytes encoded.
 * @retval 0 Encoded.
 * @retval -ENOSPC @p output is too small.
 * @retval -ENOTSUP The type is not carried in a file.
 * @retval <0 An errno from reading the parameter.
 */
int kfsw_param_wire_encode(const struct kfsw_param_entry *entry, uint8_t *output,
			   size_t output_size, uint16_t *written);

/**
 * @brief Decode a value and check it against the parameter, without writing it.
 *
 * Separate from applying it, so a caller can reject a whole file before any of
 * it takes effect.
 *
 * @param entry Registry entry the value is destined for.
 * @param type One of @ref kfsw_param_wire_type.
 * @param value Encoded bytes.
 * @param value_size How many.
 * @param[out] decoded The value, ready for @ref kfsw_param_wire_apply.
 * @retval 0 The value is valid for this parameter.
 * @retval -EBADMSG The code is not this parameter's, or the width is wrong.
 * @retval -ENOTSUP The type code is unknown, or the parameter is not carried in
 *                  a file.
 * @retval <0 An errno from the parameter's own validation.
 */
int kfsw_param_wire_decode(const struct kfsw_param_entry *entry, uint8_t type, const uint8_t *value,
			   uint16_t value_size, struct kfsw_param_value *decoded);

/**
 * @brief Write a value that @ref kfsw_param_wire_decode accepted.
 *
 * The caller holds the table lock.
 */
void kfsw_param_wire_apply(const struct kfsw_param_entry *entry,
			   const struct kfsw_param_value *decoded);

#endif /* KFSW_SERVICES_PARAM_WIRE_H */
