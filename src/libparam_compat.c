#include <stddef.h>

size_t kfsw_libparam_strlcpy(char *destination, const char *source, size_t destination_size)
{
	const char *cursor = source;
	size_t copied = 0U;

	if (destination_size != 0U) {
		while ((copied + 1U < destination_size) && (*cursor != '\0')) {
			destination[copied++] = *cursor++;
		}
		destination[copied] = '\0';
	}

	while (*cursor != '\0') {
		cursor++;
	}

	return (size_t)(cursor - source);
}
