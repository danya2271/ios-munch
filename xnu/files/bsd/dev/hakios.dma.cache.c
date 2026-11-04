#include <sys/errno.h>
#include <sys/hakios.dma.endpoint.h>

int
hakios_dma_cache_streaming(void *cpu, size_t size, enum hakios_dma_direction direction, bool for_device)
{
	if (cpu == NULL || size == 0 || ((uintptr_t)cpu | size) % HAKIOS_DMA_PT_PAGE_SIZE != 0 ||
	    (uintptr_t)cpu > UINTPTR_MAX - size || (unsigned)direction > HAKIOS_DMA_FROM_DEVICE) {
		return EINVAL;
	}
#if defined(__aarch64__)
	uint64_t cache_type;
	__asm__ volatile("mrs %0, CTR_EL0" : "=r"(cache_type));
	size_t line_size = 4u << ((cache_type >> 16) & 15);
	if (line_size > HAKIOS_DMA_PT_PAGE_SIZE) {
		return ENOTSUP;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (uintptr_t address = (uintptr_t)cpu; address < (uintptr_t)cpu + size; address += line_size) {
		if (for_device && direction == HAKIOS_DMA_TO_DEVICE) {
			__asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
		} else if (for_device) {
			__asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
		} else if (direction != HAKIOS_DMA_TO_DEVICE) {
			__asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
#else
	(void)for_device;
	return ENOTSUP;
#endif
}
