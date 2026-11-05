#ifndef _IOKIT_MUNCH_SDE_MEMORY_H
#define _IOKIT_MUNCH_SDE_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

#define MUNCH_SDE_POOL_SLOT_SIZE  0x00a00000u
#define MUNCH_SDE_POOL_TABLE_SIZE 0x00044000u
#define MUNCH_SDE_POOL_SIZE       (2u * MUNCH_SDE_POOL_SLOT_SIZE + MUNCH_SDE_POOL_TABLE_SIZE)

static inline bool
munch_sde_pool_valid(uint64_t base, uint64_t size, uint64_t memory_base, uint64_t memory_end)
{
	return base && !(base & 0x3fff) && size == MUNCH_SDE_POOL_SIZE &&
	    base < (1ULL << 32) && size <= (1ULL << 32) - base &&
	    memory_base < memory_end && base >= memory_base && base < memory_end && size <= memory_end - base;
}

static inline bool
munch_sde_pool_plan(uint64_t after_kernel, uint64_t window_base, uint64_t window_size,
    uint64_t resident_base, uint64_t helper_size, uint64_t *pool_base, uint64_t *helper_base)
{
	if (!window_size || window_size > UINT64_MAX - window_base || (helper_size & 0x3fff)) return false;
	uint64_t limit = window_base + window_size;
	if (resident_base && resident_base < limit) limit = resident_base;
	if (!munch_sde_pool_valid(after_kernel, MUNCH_SDE_POOL_SIZE, window_base, limit)) return false;
	uint64_t end = after_kernel + MUNCH_SDE_POOL_SIZE;
	if (helper_size > limit - end) return false;
	*pool_base = after_kernel;
	*helper_base = end;
	return true;
}

#endif
