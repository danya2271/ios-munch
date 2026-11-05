#include <kern/locks.h>
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.qca6390.h>
#include <sys/systm.h>

static LCK_GRP_DECLARE(qca6390_lock_group, "hakios-qca6390");
static LCK_MTX_DECLARE(qca6390_pcie_lock, &qca6390_lock_group);

#define QCA6390_VENDOR                 0x17cb
#define QCA6390_DEVICE                 0x1101
#define QCA6390_REVISION               0x01
#define QCA6390_CLASS                  0x028000
#define QCA6390_BAR0                   0x60300000ull

#define QCA6390_BAR0_SIZE              0x00100000u
#define QCA6390_REQUESTER_ID           0x0100u
#define QCA6390_STREAM_ID              0x1c01u
#define QCA6390_WINDOW_REGISTER        0x310cu
#define QCA6390_WINDOW_ENABLE          0x40000000u
#define QCA6390_WINDOW_START           0x80000u
#define QCA6390_WINDOW_SHIFT           19u
#define QCA6390_WINDOW_MASK            0x3fu
#define QCA6390_REGISTER_LIMIT         0x02000000u
#define QCA6390_ACCESS_ALWAYS_LIMIT    0x0fe0u

static hakios_pcie_lease_t qca_endpoint;
static bool qca_endpoint_owned;
static struct hakios_qca6390_dma_provider qca_dma_provider;
static void *qca_dma_context;
static bool qca_dma_registered;
static struct hakios_qca6390_msi_provider qca_msi_provider;
static void *qca_msi_context;
static bool qca_msi_registered;
static void *qca_msi_handle;
static struct hakios_qca6390_msi_info qca_msi_info;
static uint16_t qca_msi_capability;

static const struct hakios_pcie_endpoint_request qca_endpoint_request = {
	.domain = 0,
	.requester_id = QCA6390_REQUESTER_ID,
	.stream_id = QCA6390_STREAM_ID,
	.vendor = QCA6390_VENDOR,
	.device = QCA6390_DEVICE,
	.class_code = QCA6390_CLASS,
	.revision = QCA6390_REVISION,
	.bar0_address = QCA6390_BAR0,
	.bar0_size = QCA6390_BAR0_SIZE,
};

int
hakios_qca6390_dma_register(const struct hakios_qca6390_dma_provider *provider, void *context)
{
	if (provider == NULL || context == NULL || provider->version != HAKIOS_QCA6390_DMA_PROVIDER_VERSION ||
	    provider->status == NULL || provider->stop == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = qca_dma_registered ? EBUSY : 0;
	if (error == 0) {
		qca_dma_provider = *provider;
		qca_dma_context = context;
		qca_dma_registered = true;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_dma_unregister(void *context)
{
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = !qca_dma_registered || context != qca_dma_context ? ENODEV :
	    qca_endpoint != 0 ? EBUSY : 0;
	if (error == 0) {
		bzero(&qca_dma_provider, sizeof(qca_dma_provider));
		qca_dma_context = NULL;
		qca_dma_registered = false;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_msi_register(const struct hakios_qca6390_msi_provider *provider, void *context)
{
	if (provider == NULL || context == NULL || provider->version != HAKIOS_QCA6390_MSI_PROVIDER_VERSION ||
	    provider->allocate == NULL || provider->attach == NULL || provider->enable == NULL ||
	    provider->detach_all == NULL || provider->count == NULL || provider->release == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = qca_msi_registered ? EBUSY : 0;
	if (error == 0) {
		qca_msi_provider = *provider;
		qca_msi_context = context;
		qca_msi_registered = true;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_msi_unregister(void *context)
{
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = !qca_msi_registered || context != qca_msi_context ? ENODEV :
	    qca_endpoint != 0 || qca_msi_handle != NULL ? EBUSY : 0;
	if (error == 0) {
		bzero(&qca_msi_provider, sizeof(qca_msi_provider));
		qca_msi_context = NULL;
		qca_msi_registered = false;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

bool
hakios_qca6390_platform_matches(void)
{
	static const char device_type[] = "munch";
	static const uint64_t expected_ranges[] = { 0, 0x00400000ull, 0x3fc00000ull };
	DTEntry arm_io;
	const void *value;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/arm-io", &arm_io) != kSuccess ||
	    SecureDTGetProperty(arm_io, "device_type", &value, &size) != kSuccess ||
	    size != sizeof(device_type) || memcmp(value, device_type, sizeof(device_type)) != 0 ||
	    SecureDTGetProperty(arm_io, "ranges", &value, &size) != kSuccess ||
	    size != sizeof(expected_ranges)) {
		return false;
	}
	return memcmp(value, expected_ranges, sizeof(expected_ranges)) == 0;
}

static int
qca_provider_error(int error)
{
	return error < 0 ? EIO : error;
}

int
hakios_qca6390_preflight(uint32_t required, uint32_t *missing)
{
	const uint32_t known_requirements = HAKIOS_QCA6390_RC0_HANDOFF |
	    HAKIOS_QCA6390_DMA_DOMAIN | HAKIOS_QCA6390_MSI_ROUTES;

	if (missing == NULL) {
		return EINVAL;
	}
	*missing = required;
	if (required == 0 || (required & ~known_requirements) != 0) {
		return EINVAL;
	}
	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_endpoint_owned && hakios_pcie_endpoint_status(qca_endpoint) == 0) {
		*missing &= ~HAKIOS_QCA6390_RC0_HANDOFF;
	}
	if ((*missing & HAKIOS_QCA6390_DMA_DOMAIN) != 0 && qca_dma_registered && qca_endpoint_owned &&
	    qca_dma_provider.status(qca_dma_context, qca_endpoint) == 0) {
		*missing &= ~HAKIOS_QCA6390_DMA_DOMAIN;
	}
	if ((*missing & HAKIOS_QCA6390_MSI_ROUTES) != 0 && qca_msi_handle != NULL) {
		*missing &= ~HAKIOS_QCA6390_MSI_ROUTES;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return *missing == 0 ? 0 : ENOTSUP;
}

static int
qca_config_find_msi_locked(uint16_t *cap)
{
	uint32_t value, status;
	uint16_t next;
	bool visited[64] = { false };

	if (cap == NULL || hakios_pcie_config_read(qca_endpoint, 0x04, 4, &status) != 0 ||
	    ((status >> 16) & (1u << 4)) == 0 || hakios_pcie_config_read(qca_endpoint, 0x34, 4, &value) != 0) {
		return EIO;
	}
	next = value & 0xff;
	while (next != 0) {
		uint32_t header;
		unsigned index;

		if (next < 0x40 || next > 0xfc || (next & 3) != 0) {
			return EINVAL;
		}
		index = (next - 0x40) / 4;
		if (visited[index]) {
			return EINVAL;
		}
		visited[index] = true;
		if (hakios_pcie_config_read(qca_endpoint, next, 4, &header) != 0) {
			return EIO;
		}
		if ((header & 0xff) == 0x05) {
			*cap = next;
			return 0;
		}
		next = (header >> 8) & 0xff;
	}
	return ENODEV;
}

static int
qca_release_locked(void)
{
	int error;

	if (qca_endpoint == 0) {
		return 0;
	}
	if (qca_msi_handle != NULL) {
		void *handle = qca_msi_handle;
		error = qca_provider_error(qca_msi_provider.release(qca_msi_context, &handle));
		if (error != 0 || handle != NULL) {
			return error != 0 ? error : EIO;
		}
		qca_msi_handle = NULL;
		bzero(&qca_msi_info, sizeof(qca_msi_info));
	}
	if (qca_dma_registered) {
		error = qca_provider_error(qca_dma_provider.stop(qca_dma_context, qca_endpoint));
		if (error != 0) {
			return error;
		}
	}
	error = hakios_pcie_endpoint_release(qca_endpoint);
	if (error == 0) {
		qca_endpoint = 0;
		qca_endpoint_owned = false;
	}
	return error;
}

static int
qca_claim_locked(void)
{
	uint16_t msi_cap;
	int error, release_error;

	if (qca_endpoint != 0) {
		error = hakios_pcie_endpoint_status(qca_endpoint);
		if (error != 0) {
			return error;
		}
		return qca_endpoint_owned ? 0 : EBUSY;
	}
	error = hakios_pcie_endpoint_acquire(&qca_endpoint_request, &qca_endpoint);
	if (error != 0) {
		return error;
	}
	if ((error = qca_config_find_msi_locked(&msi_cap)) != 0) {
		release_error = qca_release_locked();
		return release_error != 0 ? release_error : error;
	}
	qca_endpoint_owned = true;
	qca_msi_capability = msi_cap;
	printf("qca6390: owned RC0 endpoint %04x:%04x rev %02x, SID %#x, MSI cap %#x; bus mastering disabled\n",
	    QCA6390_VENDOR, QCA6390_DEVICE, QCA6390_REVISION, QCA6390_STREAM_ID, msi_cap);
	return 0;
}

int
hakios_qca6390_probe(void)
{
	bool already_claimed;
	int error, release_error;

	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	already_claimed = qca_endpoint != 0;
	error = qca_claim_locked();
	if (!already_claimed && error == 0 && qca_endpoint != 0) {
		release_error = qca_release_locked();
		if (release_error != 0) {
			error = release_error;
		}
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	if (error == ENOTSUP) {
		printf("qca6390: RC0 ownership acquisition unsupported; probe refused\n");
	}
	return error;
}

int
hakios_qca6390_claim(void)
{
	int error;

	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_claim_locked();
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_release(void)
{
	int error;

	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_release_locked();
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

static int
qca_register_access(uint32_t offset, uint32_t *value, bool write)
{
	struct hakios_pcie_bar_operation operations[3] = { 0 };
	uint32_t data_index = 0;
	int error;

	if (value == NULL || (offset & 3) != 0 || offset >= QCA6390_REGISTER_LIMIT) {
		return EINVAL;
	}
	if (offset >= QCA6390_WINDOW_START) {
		uint32_t selector = QCA6390_WINDOW_ENABLE | ((offset >> QCA6390_WINDOW_SHIFT) & QCA6390_WINDOW_MASK);

		operations[0] = (struct hakios_pcie_bar_operation) {
			.type = HAKIOS_PCIE_BAR_WRITE32, .offset = QCA6390_WINDOW_REGISTER, .value = selector,
		};
		operations[1] = (struct hakios_pcie_bar_operation) {
			.type = HAKIOS_PCIE_BAR_VERIFY32, .offset = QCA6390_WINDOW_REGISTER, .value = selector,
			.mask = QCA6390_WINDOW_ENABLE | QCA6390_WINDOW_MASK,
		};
		data_index = 2;
	}
	operations[data_index] = (struct hakios_pcie_bar_operation) {
		.type = write ? HAKIOS_PCIE_BAR_WRITE32 : HAKIOS_PCIE_BAR_READ32,
		.offset = data_index == 0 ? offset : QCA6390_WINDOW_START + (offset & (QCA6390_WINDOW_START - 1)),
		.value = write ? *value : 0,
	};
	lck_mtx_lock(&qca6390_pcie_lock);
	if (!qca_endpoint_owned) {
		error = ENXIO;
	} else {
		error = hakios_pcie_bar0_transfer(qca_endpoint, operations, data_index + 1,
		    offset >= QCA6390_ACCESS_ALWAYS_LIMIT);
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	if (error == 0 && !write) {
		*value = operations[data_index].value;
	}
	return error;
}

int
hakios_qca6390_read32(uint32_t offset, uint32_t *value)
{
	if (value != NULL) {
		*value = 0;
	}
	return qca_register_access(offset, value, false);
}

int
hakios_qca6390_write32(uint32_t offset, uint32_t value)
{
	return qca_register_access(offset, &value, true);
}

bool
hakios_qca6390_msi(uint64_t *address, uint32_t *data, uint32_t *first_intid)
{
	int error;

	if (address != NULL) {
		*address = 0;
	}
	if (data != NULL) {
		*data = 0;
	}
	if (first_intid != NULL) {
		*first_intid = 0;
	}
	if (!hakios_qca6390_platform_matches()) {
		return false;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_claim_locked();
	if (error == 0 && qca_msi_handle == NULL) {
		if (!qca_msi_registered) {
			error = ENOTSUP;
		} else {
			struct hakios_qca6390_msi_info info = { 0 };
			void *handle = NULL;
				error = qca_provider_error(qca_msi_provider.allocate(qca_msi_context, qca_endpoint, qca_msi_capability,
				    &info, &handle));
			if (error != 0 || handle == NULL || info.address == 0 || info.data == 0 ||
			    info.data > UINT16_MAX - (HAKIOS_MSI_VECTORS - 1) || (info.data & (HAKIOS_MSI_VECTORS - 1)) != 0 ||
			    info.first_intid < 32 || info.first_intid > 1019 - (HAKIOS_MSI_VECTORS - 1)) {
				if (handle != NULL) {
					int cleanup = qca_provider_error(qca_msi_provider.release(qca_msi_context, &handle));
					if (cleanup != 0 || handle != NULL) {
						qca_msi_handle = handle;
						qca_msi_info = info;
						if (error == 0) {
							error = cleanup != 0 ? cleanup : EIO;
						}
					}
				}
				if (error == 0) {
					error = EPROTO;
				}
			}
			if (error == 0) {
				qca_msi_info = info;
				qca_msi_handle = handle;
			}
		}
	}
	if (error == 0) {
		if (address != NULL) {
			*address = qca_msi_info.address;
		}
		if (data != NULL) {
			*data = qca_msi_info.data;
		}
		if (first_intid != NULL) {
			*first_intid = qca_msi_info.first_intid;
		}
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error == 0;
}

int
hakios_qca6390_irq_attach(uint32_t vector, hakios_qca6390_irq_handler_t handler, void *context)
{
	int error;

	if (vector >= HAKIOS_MSI_VECTORS || handler == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = ENXIO;
	} else {
		error = qca_provider_error(qca_msi_provider.attach(qca_msi_context, qca_msi_handle, vector, handler, context));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_irq_enable(uint32_t vector, bool enable)
{
	int error;

	if (vector >= HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = ENXIO;
	} else {
		error = qca_provider_error(qca_msi_provider.enable(qca_msi_context, qca_msi_handle, vector, enable));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_irq_detach_all(void)
{
	int error;

	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = 0;
	} else {
		error = qca_provider_error(qca_msi_provider.detach_all(qca_msi_context, qca_msi_handle));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

uint64_t
hakios_qca6390_irq_count(uint32_t vector)
{
	uint64_t count = 0;

	if (vector >= HAKIOS_MSI_VECTORS) {
		return 0;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle != NULL && qca_msi_registered) {
		count = qca_msi_provider.count(qca_msi_context, qca_msi_handle, vector);
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return count;
}
