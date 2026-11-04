#ifndef _ARM64_GIC_V3_SPI_H_
#define _ARM64_GIC_V3_SPI_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/kern_return.h>

typedef uint64_t gic_v3_spi_lease_t;
typedef void (*gic_v3_spi_handler_t)(void *context, uint32_t intid);

kern_return_t gic_v3_spi_bank_reserve(uint32_t first, uint32_t count, gic_v3_spi_lease_t *lease);
kern_return_t gic_v3_spi_bank_attach(gic_v3_spi_lease_t lease, uint32_t vector, bool edge,
    gic_v3_spi_handler_t handler, void *context);
kern_return_t gic_v3_spi_bank_intid(gic_v3_spi_lease_t lease, uint32_t vector, uint32_t *intid);
kern_return_t gic_v3_spi_bank_enable(gic_v3_spi_lease_t lease, uint32_t vector, bool enable);
kern_return_t gic_v3_spi_bank_quiesce(gic_v3_spi_lease_t lease);
kern_return_t gic_v3_spi_bank_release(gic_v3_spi_lease_t *lease);

#endif
