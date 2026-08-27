#include <zephyr/linker/sections.h>

SECTION_PROLOGUE(param, , SUBALIGN(8))
{
	__start_param = .;
	KEEP(*(SORT_BY_NAME(param.*)))
	__stop_param = .;
} GROUP_ROM_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)
