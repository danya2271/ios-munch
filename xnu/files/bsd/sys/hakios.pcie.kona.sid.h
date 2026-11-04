#ifndef _SYS_HAKIOS_PCIE_KONA_SID_H_
#define _SYS_HAKIOS_PCIE_KONA_SID_H_

#include <sys/hakios.pcie.kona.mmio.h>

#define HAKIOS_PCIE_KONA_SID_VERSION 1u

struct hakios_pcie_kona_sid;

int hakios_pcie_kona_sid_create(struct hakios_pcie_kona_mmio *mmio,
    struct hakios_pcie_kona_sid **result);
int hakios_pcie_kona_sid_apply(struct hakios_pcie_kona_sid *sid, void *ownership,
    void **cookie);
int hakios_pcie_kona_sid_restore(struct hakios_pcie_kona_sid *sid, void *ownership,
    void **cookie);
int hakios_pcie_kona_sid_destroy(struct hakios_pcie_kona_sid **sid);

#endif
