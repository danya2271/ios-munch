#ifndef _SYS_HAKIOS_MMIO_H_
#define _SYS_HAKIOS_MMIO_H_

#include <stdint.h>

int hakios_mmio_read8(const volatile void *address, uint8_t *value);
int hakios_mmio_read16(const volatile void *address, uint16_t *value);
int hakios_mmio_read32(const volatile void *address, uint32_t *value);
int hakios_mmio_write8(volatile void *address, uint8_t value);
int hakios_mmio_write16(volatile void *address, uint16_t value);
int hakios_mmio_write32(volatile void *address, uint32_t value);

#endif
