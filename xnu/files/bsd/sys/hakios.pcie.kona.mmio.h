#ifndef _SYS_HAKIOS_PCIE_KONA_MMIO_H_
#define _SYS_HAKIOS_PCIE_KONA_MMIO_H_

#include <stdint.h>
#include <sys/hakios.pcie.kona.platform.h>

#define HAKIOS_PCIE_KONA_MMIO_VERSION 1u

typedef int (*hakios_pcie_kona_wake_t)(void *context, void *ownership);

struct hakios_pcie_kona_mmio;

int hakios_pcie_kona_mmio_create(struct hakios_pcie_kona_mmio **result);
int hakios_pcie_kona_mmio_destroy(struct hakios_pcie_kona_mmio **mmio);
int hakios_pcie_kona_mmio_authorize(struct hakios_pcie_kona_mmio *mmio, void *ownership);
int hakios_pcie_kona_mmio_revoke(struct hakios_pcie_kona_mmio *mmio, void **ownership);
int hakios_pcie_kona_mmio_set_wake(struct hakios_pcie_kona_mmio *mmio,
    hakios_pcie_kona_wake_t wake, void *context);
int hakios_pcie_kona_mmio_access_begin(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard);
int hakios_pcie_kona_mmio_access_end(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard);
int hakios_pcie_kona_mmio_read(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset, uint8_t width,
    uint32_t *value);
int hakios_pcie_kona_mmio_write(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset, uint8_t width,
    uint32_t value);
int hakios_pcie_kona_mmio_bind(struct hakios_pcie_kona_mmio *mmio,
    struct hakios_pcie_kona_platform_operations *operations);

#endif
