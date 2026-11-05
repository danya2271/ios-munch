#ifndef _SYS_HAKIOS_QCA6390_H_
#define _SYS_HAKIOS_QCA6390_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.msi.h>

enum hakios_qca6390_requirement {
	HAKIOS_QCA6390_RC0_HANDOFF = 1u << 0,
	HAKIOS_QCA6390_DMA_DOMAIN = 1u << 1,
	HAKIOS_QCA6390_MSI_ROUTES = 1u << 2,
};

#define HAKIOS_QCA6390_DMA_PROVIDER_VERSION 1u
#define HAKIOS_QCA6390_MSI_PROVIDER_VERSION 1u

typedef void (*hakios_qca6390_irq_handler_t)(void *context, uint32_t intid);

struct hakios_qca6390_dma_provider {
	uint32_t version;
	int (*status)(void *context, hakios_pcie_lease_t endpoint);
	int (*map)(void *context, hakios_pcie_lease_t endpoint, uint64_t physical, uint64_t size,
	    uint64_t *iova);
	int (*stop)(void *context, hakios_pcie_lease_t endpoint);
};

struct hakios_qca6390_msi_info {
	uint64_t address;
	uint32_t data;
	uint32_t first_intid;
};

struct hakios_qca6390_msi_provider {
	uint32_t version;
	int (*allocate)(void *context, hakios_pcie_lease_t endpoint, uint16_t capability,
	    struct hakios_qca6390_msi_info *info, void **handle);
	int (*attach)(void *context, void *handle, uint32_t vector,
	    hakios_qca6390_irq_handler_t handler, void *handler_context);
	int (*enable)(void *context, void *handle, uint32_t vector, bool enable);
	int (*detach_all)(void *context, void *handle);
	uint64_t (*count)(void *context, void *handle, uint32_t vector);
	int (*release)(void *context, void **handle);
};

int hakios_qca6390_preflight(uint32_t required, uint32_t *missing);
bool hakios_qca6390_platform_matches(void);
int hakios_qca6390_dma_register(const struct hakios_qca6390_dma_provider *provider, void *context);
int hakios_qca6390_dma_unregister(void *context);
int hakios_qca6390_msi_register(const struct hakios_qca6390_msi_provider *provider, void *context);
int hakios_qca6390_msi_unregister(void *context);
int hakios_qca6390_probe(void);
int hakios_qca6390_claim(void);
int hakios_qca6390_release(void);
int hakios_qca6390_read32(uint32_t offset, uint32_t *value);
int hakios_qca6390_write32(uint32_t offset, uint32_t value);
bool hakios_qca6390_msi(uint64_t *address, uint32_t *data, uint32_t *first_intid);
int hakios_qca6390_irq_attach(uint32_t vector, hakios_qca6390_irq_handler_t handler, void *context);
int hakios_qca6390_irq_enable(uint32_t vector, bool enable);
int hakios_qca6390_irq_detach_all(void);
uint64_t hakios_qca6390_irq_count(uint32_t vector);

#endif
