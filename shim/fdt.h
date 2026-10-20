#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Return true only when [base, base + size) is ordinary, unreserved DRAM. */
int fdt_memory_range_usable(uintptr_t fdt, uint64_t base, uint64_t size);
