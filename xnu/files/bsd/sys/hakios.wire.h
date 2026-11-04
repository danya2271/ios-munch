#ifndef _SYS_HAKIOS_WIRE_H_
#define _SYS_HAKIOS_WIRE_H_

#include <stdint.h>

static inline uint32_t
hakios_wire_get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static inline uint64_t
hakios_wire_get64(const uint8_t *bytes)
{
	return hakios_wire_get32(bytes) | (uint64_t)hakios_wire_get32(bytes + 4) << 32;
}

static inline void
hakios_wire_put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned index = 0; index < 4; index++) {
		bytes[index] = (uint8_t)(value >> (index * 8));
	}
}

static inline void
hakios_wire_put64(uint8_t *bytes, uint64_t value)
{
	hakios_wire_put32(bytes, (uint32_t)value);
	hakios_wire_put32(bytes + 4, (uint32_t)(value >> 32));
}

#endif
