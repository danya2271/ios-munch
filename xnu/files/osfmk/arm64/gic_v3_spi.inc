#include "gic_v3_spi.h"

#define SPI_CTLR 0x0000u
#define SPI_TYPER 0x0004u
#define SPI_GROUP(number) (0x0080u + 4u * ((number) / 32))
#define SPI_ENABLED(number) (0x0100u + 4u * ((number) / 32))
#define SPI_DISABLE(number) (0x0180u + 4u * ((number) / 32))
#define SPI_PENDING(number) (0x0200u + 4u * ((number) / 32))
#define SPI_CLEAR_PENDING(number) (0x0280u + 4u * ((number) / 32))
#define SPI_ACTIVE(number) (0x0300u + 4u * ((number) / 32))
#define SPI_PRIORITY(number) (0x0400u + (number))
#define SPI_CONFIG(number) (0x0c00u + 4u * ((number) / 16))
#define SPI_ROUTER(number) (0x6000u + 8u * (number))
#define SPI_RWP (1u << 31)
#define SPI_WAIT_BUDGET 100000u

struct gic_spi {
	uint32_t intid, vector, busy;
	gic_v3_spi_lease_t lease;
	gic_v3_spi_handler_t handler;
	void *context;
	uint64_t count, router;
	uint32_t config;
	uint8_t priority;
	kern_return_t fault;
	bool saved, attached, enabled, closing, legacy;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);
static gic_v3_spi_lease_t gic_spi_next;

static boolean_t spi_lock(void)
{
	boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	return interrupts;
}
static void spi_unlock(boolean_t interrupts)
{
	simple_unlock(&gic_spi_lock);
	ml_set_interrupts_enabled(interrupts);
}
static struct gic_spi *spi_find(gic_v3_spi_lease_t lease, uint32_t vector)
{
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		if (lease != 0 && gic_spi[index].lease == lease && gic_spi[index].vector == vector) { return &gic_spi[index]; }
	}
	return NULL;
}
static kern_return_t spi_wait(void)
{
	for (unsigned attempt = 0; attempt < SPI_WAIT_BUDGET; attempt++) {
		if ((gic_read32(gicd_base + SPI_CTLR) & SPI_RWP) == 0) { gic_spi_barrier(); return KERN_SUCCESS; }
	}
	return KERN_OPERATION_TIMED_OUT;
}
static kern_return_t spi_fail(struct gic_spi *slot, kern_return_t error)
{
	if (slot->fault == KERN_SUCCESS) { slot->fault = error; }
	return error;
}
static kern_return_t spi_mask(struct gic_spi *slot)
{
	slot->enabled = false;
	gic_write32(gicd_base + SPI_DISABLE(slot->intid), 1u << (slot->intid % 32));
	kern_return_t error = spi_wait();
	if (error == KERN_SUCCESS && (gic_read32(gicd_base + SPI_ENABLED(slot->intid)) & (1u << (slot->intid % 32))) != 0) {
		error = KERN_FAILURE;
	}
	return error == KERN_SUCCESS ? error : spi_fail(slot, error);
}
static kern_return_t spi_reserve(uint32_t first, uint32_t count, bool legacy, gic_v3_spi_lease_t *lease)
{
	if (lease == NULL) { return KERN_INVALID_ARGUMENT; }
	*lease = 0;
	if (gicd_base == 0 || gicd_size < 0x8000 || ml_at_interrupt_context() || first < 32 || first >= 1020 ||
	    count == 0 || count > 32 || count > 1020 - first) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	uint32_t implemented = ((gic_read32(gicd_base + SPI_TYPER) & 31u) + 1) * 32;
	unsigned available = 0;
	kern_return_t error = first + count > implemented ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		struct gic_spi *slot = &gic_spi[index];
		if (slot->lease == 0) { available++; }
		else if (slot->intid >= first && slot->intid - first < count) { error = KERN_NAME_EXISTS; }
	}
	if (error == KERN_SUCCESS && (available < count || gic_spi_next == UINT64_MAX)) { error = KERN_RESOURCE_SHORTAGE; }
	if (error == KERN_SUCCESS) {
		*lease = ++gic_spi_next;
		uint32_t vector = 0;
		for (unsigned index = 0; index < GIC_SPI_MOST && vector < count; index++) {
			if (gic_spi[index].lease != 0) { continue; }
			gic_spi[index] = (struct gic_spi) {.intid = first + vector, .vector = vector, .lease = *lease, .legacy = legacy};
			vector++;
		}
	}
	spi_unlock(interrupts);
	return error;
}
kern_return_t gic_v3_spi_bank_reserve(uint32_t first, uint32_t count, gic_v3_spi_lease_t *lease)
{ return spi_reserve(first, count, false, lease); }

kern_return_t gic_v3_spi_bank_attach(gic_v3_spi_lease_t lease, uint32_t vector, bool edge,
    gic_v3_spi_handler_t handler, void *context)
{
	if (gicd_base == 0 || lease == 0 || handler == NULL || ml_at_interrupt_context()) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	struct gic_spi *slot = spi_find(lease, vector);
	kern_return_t error = slot == NULL ? KERN_INVALID_ARGUMENT : slot->fault;
	if (error == KERN_SUCCESS && (slot->saved || slot->closing || slot->busy != 0)) { error = KERN_FAILURE; }
	if (error == KERN_SUCCESS) {
		uint32_t number = slot->intid, mask = 1u << (number % 32), shift = (number % 16) * 2;
		if ((gic_read32(gicd_base + SPI_CTLR) & 0x12u) != 0x12u ||
		    (gic_read32(gicd_base + SPI_GROUP(number)) & mask) == 0 ||
		    (gic_read32(gicd_base + SPI_ENABLED(number)) & mask) != 0 ||
		    (gic_read32(gicd_base + SPI_ACTIVE(number)) & mask) != 0 ||
		    (gic_read32(gicd_base + SPI_PENDING(number)) & mask) != 0) {
			error = KERN_PROTECTION_FAILURE;
		} else if ((error = spi_wait()) == KERN_SUCCESS) {
			uint32_t config = gic_read32(gicd_base + SPI_CONFIG(number));
			slot->config = (config >> shift) & 3u;
			slot->priority = gic_read8(gicd_base + SPI_PRIORITY(number));
			slot->router = gic_read64(gicd_base + SPI_ROUTER(number));
			slot->saved = true;
			uint64_t router = gic_spi_affinity();
			gic_write8(gicd_base + SPI_PRIORITY(number), GIC_PRIORITY_DEFAULT);
			gic_write32(gicd_base + SPI_CONFIG(number), (config & ~(3u << shift)) | ((edge ? 2u : 0u) << shift));
			gic_write64(gicd_base + SPI_ROUTER(number), router);
			error = spi_wait();
			if (error == KERN_SUCCESS && (((gic_read32(gicd_base + SPI_CONFIG(number)) >> shift) & 3u) != (edge ? 2u : 0u) ||
			    gic_read8(gicd_base + SPI_PRIORITY(number)) != GIC_PRIORITY_DEFAULT ||
			    gic_read64(gicd_base + SPI_ROUTER(number)) != router)) { error = KERN_FAILURE; }
			if (error == KERN_SUCCESS) { slot->handler = handler; slot->context = context; slot->attached = true; }
		}
		if (error != KERN_SUCCESS) { spi_fail(slot, error); }
	}
	spi_unlock(interrupts);
	return error;
}
kern_return_t gic_v3_spi_bank_intid(gic_v3_spi_lease_t lease, uint32_t vector, uint32_t *intid)
{
	if (intid == NULL) { return KERN_INVALID_ARGUMENT; }
	*intid = 0;
	if (gicd_base == 0 || lease == 0) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	struct gic_spi *slot = spi_find(lease, vector);
	kern_return_t error = slot == NULL ? KERN_INVALID_ARGUMENT : slot->fault;
	if (error == KERN_SUCCESS && (!slot->attached || slot->closing)) { error = KERN_FAILURE; }
	if (error == KERN_SUCCESS) { *intid = slot->intid; }
	spi_unlock(interrupts);
	return error;
}
kern_return_t gic_v3_spi_bank_count(gic_v3_spi_lease_t lease, uint32_t vector, uint64_t *count)
{
	if (count == NULL) { return KERN_INVALID_ARGUMENT; }
	*count = 0;
	if (gicd_base == 0 || lease == 0) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	struct gic_spi *slot = spi_find(lease, vector);
	kern_return_t error = slot == NULL ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
	if (error == KERN_SUCCESS) { *count = slot->count; }
	spi_unlock(interrupts);
	return error;
}
kern_return_t gic_v3_spi_bank_enable(gic_v3_spi_lease_t lease, uint32_t vector, bool enable)
{
	if (gicd_base == 0 || lease == 0) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	struct gic_spi *slot = spi_find(lease, vector);
	kern_return_t error = slot == NULL ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
	if (error == KERN_SUCCESS && enable) {
		for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
			struct gic_spi *peer = &gic_spi[index];
			if (peer->lease == lease && (peer->fault != KERN_SUCCESS || !peer->attached || peer->closing)) {
				error = peer->fault != KERN_SUCCESS ? peer->fault : KERN_FAILURE;
				break;
			}
		}
		if (error == KERN_SUCCESS) {
			uint32_t mask = 1u << (slot->intid % 32);
			gic_write32(gicd_base + SPI_ENABLED(slot->intid), mask);
			gic_spi_barrier();
			if ((gic_read32(gicd_base + SPI_ENABLED(slot->intid)) & mask) != mask) { error = spi_fail(slot, KERN_FAILURE); }
			else { slot->enabled = true; }
		}
	} else if (error == KERN_SUCCESS && slot->saved) { error = spi_mask(slot); }
	spi_unlock(interrupts);
	return error;
}
kern_return_t gic_v3_spi_bank_quiesce(gic_v3_spi_lease_t lease)
{
	if (gicd_base == 0 || lease == 0 || ml_at_interrupt_context()) { return KERN_INVALID_ARGUMENT; }
	boolean_t interrupts = spi_lock();
	if (spi_find(lease, 0) == NULL) { spi_unlock(interrupts); return KERN_INVALID_ARGUMENT; }
	kern_return_t error = KERN_SUCCESS;
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		struct gic_spi *slot = &gic_spi[index];
		if (slot->lease != lease) { continue; }
		slot->closing = true;
		kern_return_t masked = slot->saved ? spi_mask(slot) : KERN_SUCCESS;
		if (error == KERN_SUCCESS) { error = masked; }
	}
	spi_unlock(interrupts);
	if (error != KERN_SUCCESS) { return error; }
	for (unsigned attempt = 0; attempt < SPI_WAIT_BUDGET; attempt++) {
		interrupts = spi_lock();
		if (spi_find(lease, 0) == NULL) { spi_unlock(interrupts); return KERN_INVALID_ARGUMENT; }
		bool busy = false;
		for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
			struct gic_spi *slot = &gic_spi[index];
			if (slot->lease != lease) { continue; }
			if (slot->busy != 0 || (slot->saved &&
			    (gic_read32(gicd_base + SPI_ACTIVE(slot->intid)) & (1u << (slot->intid % 32))) != 0)) { busy = true; }
		}
		spi_unlock(interrupts);
		if (!busy) { return KERN_SUCCESS; }
	}
	return KERN_OPERATION_TIMED_OUT;
}
kern_return_t gic_v3_spi_bank_release(gic_v3_spi_lease_t *lease)
{
	if (lease == NULL || *lease == 0) { return KERN_INVALID_ARGUMENT; }
	kern_return_t error = gic_v3_spi_bank_quiesce(*lease);
	if (error != KERN_SUCCESS) { return error; }
	boolean_t interrupts = spi_lock();
	if (spi_find(*lease, 0) == NULL) { spi_unlock(interrupts); return KERN_INVALID_ARGUMENT; }
	for (unsigned index = 0; index < GIC_SPI_MOST && error == KERN_SUCCESS; index++) {
		struct gic_spi *slot = &gic_spi[index];
		if (slot->lease != *lease || !slot->saved) { continue; }
		uint32_t number = slot->intid, shift = (number % 16) * 2, mask = 1u << (number % 32);
		uint32_t config = gic_read32(gicd_base + SPI_CONFIG(number));
		gic_write32(gicd_base + SPI_CLEAR_PENDING(number), mask);
		gic_write32(gicd_base + SPI_CONFIG(number), (config & ~(3u << shift)) | (slot->config << shift));
		gic_write8(gicd_base + SPI_PRIORITY(number), slot->priority);
		gic_write64(gicd_base + SPI_ROUTER(number), slot->router);
		error = spi_wait();
		if (error == KERN_SUCCESS && (((gic_read32(gicd_base + SPI_CONFIG(number)) >> shift) & 3u) != slot->config ||
		    gic_read8(gicd_base + SPI_PRIORITY(number)) != slot->priority || gic_read64(gicd_base + SPI_ROUTER(number)) != slot->router ||
		    ((gic_read32(gicd_base + SPI_PENDING(number)) | gic_read32(gicd_base + SPI_ACTIVE(number)) |
		    gic_read32(gicd_base + SPI_ENABLED(number))) & mask) != 0)) { error = KERN_FAILURE; }
		if (error != KERN_SUCCESS) { spi_fail(slot, error); }
	}
	if (error == KERN_SUCCESS) {
		for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
			if (gic_spi[index].lease == *lease) { gic_spi[index] = (struct gic_spi) {0}; }
		}
		*lease = 0;
	}
	spi_unlock(interrupts);
	return error;
}
static struct gic_spi *gic_spi_dispatch(uint32_t intid)
{
	boolean_t interrupts = spi_lock();
	struct gic_spi *found = NULL;
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		struct gic_spi *slot = &gic_spi[index];
		if (slot->lease == 0 || slot->intid != intid || !slot->attached || !slot->enabled || slot->closing || slot->fault != KERN_SUCCESS) { continue; }
		if (slot->busy == UINT32_MAX) { spi_fail(slot, KERN_RESOURCE_SHORTAGE); break; }
		slot->busy++;
		slot->count++;
		found = slot;
		break;
	}
	spi_unlock(interrupts);
	if (found != NULL) { found->handler(found->context, intid); }
	return found;
}
static void gic_spi_complete(struct gic_spi *slot)
{
	if (slot == NULL) { return; }
	boolean_t interrupts = spi_lock();
	if (slot->busy != 0) { slot->busy--; }
	spi_unlock(interrupts);
}
static gic_v3_spi_lease_t spi_legacy(uint32_t intid)
{
	if (gicd_base == 0) { return 0; }
	boolean_t interrupts = spi_lock();
	gic_v3_spi_lease_t lease = 0;
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		if (gic_spi[index].intid == intid && gic_spi[index].legacy) { lease = gic_spi[index].lease; break; }
	}
	spi_unlock(interrupts);
	return lease;
}
void gic_v3_spi_mask(uint32_t intid)
{ gic_v3_spi_lease_t lease = spi_legacy(intid); if (lease != 0) { (void)gic_v3_spi_bank_enable(lease, 0, false); } }
void gic_v3_spi_unmask(uint32_t intid)
{ gic_v3_spi_lease_t lease = spi_legacy(intid); if (lease != 0) { (void)gic_v3_spi_bank_enable(lease, 0, true); } }
void gic_v3_spi_detach(uint32_t intid)
{ gic_v3_spi_lease_t lease = spi_legacy(intid); if (lease != 0) { (void)gic_v3_spi_bank_release(&lease); } }
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, gic_v3_spi_handler_t handler, void *context)
{
	if (handler == NULL) { return KERN_INVALID_ARGUMENT; }
	gic_v3_spi_lease_t lease;
	kern_return_t error = spi_reserve(intid, 1, true, &lease);
	if (error == KERN_SUCCESS) { error = gic_v3_spi_bank_attach(lease, 0, edge, handler, context); }
	if (error == KERN_SUCCESS) { error = gic_v3_spi_bank_enable(lease, 0, true); }
	if (error != KERN_SUCCESS && lease != 0) { (void)gic_v3_spi_bank_release(&lease); }
	return error;
}
uint64_t gic_v3_spi_count(uint32_t intid)
{
	if (gicd_base == 0) { return 0; }
	boolean_t interrupts = spi_lock();
	uint64_t count = 0;
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) {
		if (gic_spi[index].lease != 0 && gic_spi[index].intid == intid) { count = gic_spi[index].count; break; }
	}
	spi_unlock(interrupts);
	return count;
}
