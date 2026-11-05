#ifndef _IOKIT_MUNCH_SDE_XNU_H
#define _IOKIT_MUNCH_SDE_XNU_H

#include "munch-sde-mode.h"

extern "C" int gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *context, uint32_t intid), void *context) __attribute__((weak));
extern "C" void gic_v3_spi_mask(uint32_t intid) __attribute__((weak));
extern "C" void gic_v3_spi_detach(uint32_t intid) __attribute__((weak));
extern "C" int thread_wakeup_prim(void *event, int one, int result) __attribute__((weak));

#define MUNCH_SDE_DISPLAY_IRQ_INTID 115
#define MUNCH_SDE_IRQ_DONE          (1u << 0)
#define MUNCH_SDE_IRQ_FAULT         (1u << 1)

static void munchSdeBarrier(void *);

static void
munchSdeWakeup(void *event)
{
	if (thread_wakeup_prim) thread_wakeup_prim(event, 0, 0);
}

struct MunchSDE {
	struct munch_mode mode;
	volatile uint32_t *modeMMIO[4];
	bool nativeRequested, nativeTakeover;
	uint32_t nativeRefreshHz;
	uint64_t vblankCount, vblankTime;
	uint64_t vblankLastHardwareUs, vblankFallbackNextUs;
	uint64_t vblankHardwareEvents, vblankFallbackEvents;
	uint32_t vblankRaw, vblankFrameCount, vblankLineCount, vblankTearLine;
	bool vblankFallback;
	const char *initPhase;
	volatile uint32_t *mdp, *dsi, *gcc, *cc, *power, *smmu, *gr1, *cb;
	struct munch_sde_snapshot baseline, current;
	struct munch_sde_route route;
	struct munch_smmu_domain domain;
	struct munch_smmu_domain observedDomain;
	struct munch_smmu_walk bootWalk;
	const char *tableAccess;
	uint64_t contextPhysical, faultAddress, expectedPhysical;
	uint32_t fsynr0, fsynr1, bootVerifiedPages;
	bool domainSnapshotValid;
	struct munch_smmu_stream streams[256];
	struct munch_sde_buffer buffers[3];
	struct munch_sde_presenter presenter;
	struct munch_sde_refresh refresh;
	IOBufferMemoryDescriptor *memory[3];
	IOBufferMemoryDescriptor *tables;
	uint8_t *poolBytes;
	uint64_t poolPhysical, poolLength;
	const char *poolCheck, *bufferCheck;
	uint8_t *pixels[3];
	uint64_t tablePhysical, rootPhysical, completedTime;
	uint64_t guardCalls, pollChecks, cleanTime, waitTime;
	uint64_t refreshRecoveries;
	bool nativeFrameStarted;
	uint32_t nativeGuardFrames;
	volatile uint32_t completionIRQPending;
	volatile uint32_t completionIRQSequence;
	volatile uint32_t completionIRQArmed;
	volatile uint32_t completionIRQFault, completionIRQMasked;
	volatile uint32_t completionIRQEvents;
	uint32_t completionIRQEnableSaved, completionIRQEnableMask, completionIRQIntid;
	bool completionIRQAttached;
	bool panelBlanked, panelCommandActive, panelCommandFailed;
	uint64_t *tableBytes;
	unsigned tablePages;
	struct {
		uint64_t physical;
		IOMemoryDescriptor *memory;
		IOMemoryMap *mapping;
		uint8_t *bytes;
	} ram[64];
	unsigned ramCount;
	uint32_t scr0, idr0, idr1, registerPage, streamSmr;
	volatile uint64_t *rootEntry;
	bool published, stopped, snapshotValid;
};

static MunchSDE *gMunchSDEOwner;

static uint32_t
munchSdeRequestedRefreshHz(void)
{
	uint32_t refresh = 60;

	PE_parse_boot_argn("munch_sde_hz", &refresh, sizeof(refresh));
	return refresh == 120 ? 120 : 60;
}

static void
munchSdeCompletionIRQ(void *cookie, uint32_t intid)
{
	MunchSDE *state = (MunchSDE *)cookie;
	if (!state || !state->mdp) return;
	uint32_t sources = state->mdp[MUNCH_SDE_GLOBAL_INTR_STATUS / 4];
	if (sources & ~MUNCH_SDE_GLOBAL_INTR_MDP) {
		__atomic_store_n(&state->completionIRQMasked, 1u, __ATOMIC_RELEASE);
		if (gic_v3_spi_mask) gic_v3_spi_mask(intid);
		__atomic_add_fetch(&state->completionIRQSequence, 1u, __ATOMIC_RELEASE);
		munchSdeWakeup((void *)&state->completionIRQSequence);
	}
	if (!(sources & MUNCH_SDE_GLOBAL_INTR_MDP)) return;
	uint32_t status = state->mdp[MUNCH_SDE_INTR_STATUS / 4];
	uint32_t pending = status & state->completionIRQEnableMask;
	if (!pending) {
		__atomic_store_n(&state->completionIRQMasked, 1u, __ATOMIC_RELEASE);
		if (gic_v3_spi_mask) gic_v3_spi_mask(intid);
		__atomic_add_fetch(&state->completionIRQSequence, 1u, __ATOMIC_RELEASE);
		munchSdeWakeup((void *)&state->completionIRQSequence);
		return;
	}
	state->mdp[MUNCH_SDE_INTR_CLEAR / 4] = pending;
	munchSdeBarrier(NULL);
	if (!__atomic_load_n(&state->completionIRQArmed, __ATOMIC_ACQUIRE)) return;
	if (pending & MUNCH_SDE_INTF1_UNDERRUN)
		__atomic_store_n(&state->completionIRQFault, 1u, __ATOMIC_RELEASE);
	if (pending & state->route.done_mask)
		__atomic_store_n(&state->completionIRQPending, 1u, __ATOMIC_RELEASE);
	__atomic_add_fetch(&state->completionIRQEvents, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&state->completionIRQSequence, 1u, __ATOMIC_RELEASE);
	munchSdeWakeup((void *)&state->completionIRQSequence);
}

static void munchSdeReleaseCompletionIRQ(MunchSDE *state);

static bool
munchSdePrepareCompletionIRQ(MunchSDE *state)
{
	if (!state || !state->nativeRequested || !state->mdp ||
	    !gic_v3_spi_attach || !gic_v3_spi_mask || !gic_v3_spi_detach) return false;
	state->completionIRQEnableSaved = state->mdp[MUNCH_SDE_INTR_ENABLE / 4];
	if (state->completionIRQEnableSaved != 0) return false;
	state->completionIRQEnableMask = state->route.done_mask | MUNCH_SDE_INTF1_UNDERRUN;
	state->completionIRQIntid = MUNCH_SDE_DISPLAY_IRQ_INTID;
	if (gic_v3_spi_attach(state->completionIRQIntid, false, munchSdeCompletionIRQ, state) != 0)
		return false;
	state->completionIRQAttached = true;
	state->mdp[MUNCH_SDE_INTR_ENABLE / 4] &= ~state->completionIRQEnableMask;
	state->mdp[MUNCH_SDE_INTR_CLEAR / 4] = state->completionIRQEnableMask;
	munchSdeBarrier(NULL);
	return true;
}

static void
munchSdeDisableCompletionIRQ(MunchSDE *state)
{
	if (!state || !state->completionIRQAttached) return;
	state->mdp[MUNCH_SDE_INTR_ENABLE / 4] &= ~state->completionIRQEnableMask;
	state->mdp[MUNCH_SDE_INTR_CLEAR / 4] = state->completionIRQEnableMask;
	munchSdeBarrier(NULL);
	__atomic_store_n(&state->completionIRQArmed, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&state->completionIRQPending, 0u, __ATOMIC_RELEASE);
}

static void
munchSdeReleaseCompletionIRQ(MunchSDE *state)
{
	if (!state || !state->completionIRQAttached) return;
	munchSdeDisableCompletionIRQ(state);
	if (gic_v3_spi_detach) gic_v3_spi_detach(state->completionIRQIntid);
	state->completionIRQAttached = false;
	state->mdp[MUNCH_SDE_INTR_ENABLE / 4] = state->completionIRQEnableSaved;
	munchSdeBarrier(NULL);
}

static void
munchSdeFallbackCompletionIRQ(MunchSDE *state)
{
	if (!state || !state->completionIRQAttached) return;
	munchSdeReleaseCompletionIRQ(state);
	IOLog("MUNCH SDE: PP_DONE IRQ masked by another MDSS source; reverted to status polling\n");
}

static void
munchSdeCompletionKickoff(void *cookie)
{
	MunchSDE *state = (MunchSDE *)cookie;
	if (state->completionIRQAttached)
		__atomic_store_n(&state->completionIRQArmed, 1u, __ATOMIC_RELEASE);
}

static bool
munchSdePowered(MunchSDE *state)
{
	if (!state->gcc || !state->cc || !state->power) return false;
	uint32_t gcc = state->gcc[0x00c / 4], cc = state->cc[0x080 / 4];
	return (gcc & 1) && !(gcc & (1u << 31)) && (cc & 1) && !(cc & (1u << 31)) &&
	    (state->power[0] & (1u << 31));
}

static void
munchSdeBarrier(void *)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
munchSdeCleanRange(void *address, uint32_t length)
{
	uintptr_t first = (uintptr_t)address & ~63UL, end = (uintptr_t)address + length;
	for (uintptr_t line = first; line < end; line += 64)
		__asm__ volatile ("dc cvac, %0" :: "r" (line) : "memory");
	munchSdeBarrier(NULL);
}

static uint32_t
munchSdeModeRead(void *cookie, enum munch_mode_space space, uint32_t offset)
{
	return ((MunchSDE *)cookie)->modeMMIO[space][offset / 4];
}

static void
munchSdeModeWrite(void *cookie, enum munch_mode_space space, uint32_t offset, uint32_t value)
{
	((MunchSDE *)cookie)->modeMMIO[space][offset / 4] = value;
	munchSdeBarrier(NULL);
}

static void
munchSdeModeDelay(void *, uint32_t usec)
{
	if (usec >= 1000) IOSleep(usec / 1000);
	if (usec % 1000) IODelay(usec % 1000);
}

static void
munchSdeModeClean(void *, uint8_t *bytes, uint32_t length)
{
	munchSdeCleanRange(bytes, length);
}

static bool
munchSdeModeHealthy(void *cookie)
{
	MunchSDE *state = (MunchSDE *)cookie;
	return !state->stopped && munchSdePowered(state) &&
	    !munch_smmu_fault_bits(state->cb[MUNCH_SMMU_CB_FSR / 4]) &&
	    state->cb[MUNCH_SMMU_CB_SCTLR / 4] == state->domain.sctlr;
}

static uint64_t
munchSdeRead64(volatile uint32_t *base, unsigned offset)
{
	return *(volatile uint64_t *)((volatile uint8_t *)base + offset);
}

static void
munchSdeDomainSnapshot(MunchSDE *state, struct munch_smmu_domain *domain)
{
	*domain = state->domain;
	domain->cbar = state->gr1[MUNCH_SMMU_CBAR(domain->context) / 4];
	domain->cba2r = state->gr1[MUNCH_SMMU_CBA2R(domain->context) / 4];
	domain->sctlr = state->cb[MUNCH_SMMU_CB_SCTLR / 4];
	domain->tcr = state->cb[MUNCH_SMMU_CB_TCR / 4];
	domain->tcr2 = state->cb[MUNCH_SMMU_CB_TCR2 / 4];
	domain->ttbr = munchSdeRead64(state->cb, MUNCH_SMMU_CB_TTBR0);
	domain->mair = munchSdeRead64(state->cb, MUNCH_SMMU_CB_MAIR0);
	domain->fsr = state->cb[MUNCH_SMMU_CB_FSR / 4];
	state->observedDomain = *domain;
	if (!state->domainSnapshotValid || munch_smmu_fault_bits(domain->fsr)) {
		state->faultAddress = munchSdeRead64(state->cb, MUNCH_SMMU_CB_FAR);
		state->fsynr0 = state->cb[MUNCH_SMMU_CB_FSYNR0 / 4];
		state->fsynr1 = state->cb[MUNCH_SMMU_CB_FSYNR1 / 4];
	}
	state->domainSnapshotValid = true;
}

static enum munch_sde_result
munchSdeStream(MunchSDE *state, struct munch_smmu_domain *domain)
{
	if (state->smmu[MUNCH_SMMU_SCR0 / 4] != state->scr0 ||
	    state->smmu[MUNCH_SMMU_IDR0 / 4] != state->idr0 ||
	    state->smmu[MUNCH_SMMU_IDR1 / 4] != state->idr1) return MUNCH_SDE_STALE;
	for (unsigned index = 0; index < (state->idr0 & 255); index++) {
		state->streams[index].smr = state->smmu[MUNCH_SMMU_SMR(index) / 4];
		state->streams[index].s2cr = state->smmu[MUNCH_SMMU_S2CR(index) / 4];
	}
	enum munch_sde_result result = munch_smmu_find_stream(state->scr0, state->idr0, state->idr1, state->streams, domain);
	if (result != MUNCH_SDE_OK) return result;
	for (unsigned index = 0; index < (state->idr0 & 255); index++) {
		uint32_t s2cr = state->streams[index].s2cr;
		bool active = state->scr0 & MUNCH_SMMU_SCR0_EXIDENABLE ?
		    (s2cr & MUNCH_SMMU_S2CR_EXIDVALID) != 0 : (state->streams[index].smr & MUNCH_SMMU_SMR_VALID) != 0;
		if (index != domain->stream && active && ((s2cr >> 16) & 3) == 0 &&
		    (s2cr & 255) == domain->context) return MUNCH_SDE_DOMAIN;
	}
	return MUNCH_SDE_OK;
}

static uint8_t *
munchSdeTableAddress(MunchSDE *state, uint64_t physical)
{
	state->tableAccess = "ok";
	if (state->tableBytes && physical >= state->tablePhysical &&
	    physical - state->tablePhysical < (uint64_t)state->tablePages * 4096)
		return (uint8_t *)state->tableBytes + (physical - state->tablePhysical);
	uint64_t page = physical & ~0x3fffULL;
	if (page < 0x9c000000 || page + 0x4000 > 0x9e400000) {
		state->tableAccess = "outside-display-reserved";
		return NULL;
	}
	if (munch_sde_overlaps(page, 0x4000, state->buffers[0].physical, state->buffers[0].length)) {
		state->tableAccess = "overlaps-boot-pixels";
		return NULL;
	}
	for (unsigned index = 0; index < state->ramCount; index++) {
		if (state->ram[index].physical == page) return state->ram[index].bytes + (physical - page);
	}
	if (state->ramCount == 64) { state->tableAccess = "mapping-limit"; return NULL; }
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(page, 0x4000, kIODirectionInOut);
	if (!memory) { state->tableAccess = "descriptor-allocation"; return NULL; }
	IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapCopybackCache);
	if (!mapping) {
		state->tableAccess = "cpu-mapping";
		memory->release();
		return NULL;
	}
	unsigned index = state->ramCount++;
	state->ram[index].physical = page;
	state->ram[index].memory = memory;
	state->ram[index].mapping = mapping;
	state->ram[index].bytes = (uint8_t *)mapping->getVirtualAddress();
	return state->ram[index].bytes + (physical - page);
}

static bool
munchSdeReadPte(void *cookie, uint64_t physical, uint64_t *value)
{
	if (physical & 7) return false;
	uint8_t *address = munchSdeTableAddress((MunchSDE *)cookie, physical);
	if (!address) return false;
	*value = *(volatile uint64_t *)address;
	return true;
}

static bool
munchSdePreviewPte(void *cookie, uint64_t physical, uint64_t *value)
{
	MunchSDE *state = (MunchSDE *)cookie;
	if (physical == state->rootPhysical) {
		*value = state->tablePhysical | 3;
		return true;
	}
	return munchSdeReadPte(cookie, physical, value);
}

static enum munch_sde_result
munchSdeGuard(void *cookie)
{
	MunchSDE *state = (MunchSDE *)cookie;
	state->guardCalls++;
	if (state->stopped || !munchSdePowered(state)) return MUNCH_SDE_FAULT;
	if (state->mode.configured && state->presenter.pending == 3 && !munch_mode_check(&state->mode))
		return MUNCH_SDE_STALE;
	struct munch_smmu_domain current = {};
	enum munch_sde_result result = munchSdeStream(state, &current);
	if (result != MUNCH_SDE_OK) return result;
	if (current.stream != state->domain.stream || current.context != state->domain.context ||
	    current.s2cr != state->domain.s2cr || state->streams[current.stream].smr != state->streamSmr)
		return MUNCH_SDE_STALE;
	munchSdeDomainSnapshot(state, &current);
	if (munch_smmu_fault_bits(current.fsr)) return MUNCH_SDE_FAULT;
	if (memcmp(&current, &state->domain, sizeof(current))) return MUNCH_SDE_STALE;
	if (state->published && *state->rootEntry != (state->tablePhysical | 3)) return MUNCH_SDE_STALE;
	munchSdeReadSnapshot(state->mdp, state->dsi, &state->current, false);
	if (state->refresh.owned) {
		if (state->current.autorefresh != state->refresh.config || state->current.tear_vsync != state->refresh.vsync)
			return MUNCH_SDE_STALE;
		state->current.autorefresh = state->baseline.autorefresh;
		state->current.tear_vsync = state->baseline.tear_vsync;
	}
	unsigned source = state->presenter.armed ?
	    (state->presenter.pending != 3 ? state->presenter.pending : state->presenter.front) : 0;
	if (state->current.pipes[state->route.pipe].source != state->buffers[source].iova) return MUNCH_SDE_STALE;
	state->current.pipes[state->route.pipe].source = state->baseline.pipes[state->route.pipe].source;
	state->current.ctls[state->route.ctl].start = state->current.ctls[state->route.ctl].flush = 0;
	state->current.dsi_status = 0;
	if (memcmp(&state->current, &state->baseline, sizeof(state->current))) return MUNCH_SDE_STALE;
	return MUNCH_SDE_OK;
}

static enum munch_sde_result
munchSdeGuardFast(MunchSDE *state)
{
	if (state->stopped || !munchSdePowered(state) || !state->cb ||
	    munch_smmu_fault_bits(state->cb[MUNCH_SMMU_CB_FSR / 4])) return MUNCH_SDE_FAULT;
	if (state->published && (!state->rootEntry || *state->rootEntry != (state->tablePhysical | 3)))
		return MUNCH_SDE_STALE;
	return MUNCH_SDE_OK;
}

static uint32_t
munchSdeRead(void *cookie, uint32_t offset)
{
	return ((MunchSDE *)cookie)->mdp[offset / 4];
}

static void
munchSdeWrite(void *cookie, uint32_t offset, uint32_t value)
{
	((MunchSDE *)cookie)->mdp[offset / 4] = value;
}

static uint32_t
munchSdeDsiStatus(void *cookie)
{
	return ((MunchSDE *)cookie)->dsi[MUNCH_DSI_STATUS / 4];
}

static bool
munchSdeClean(void *cookie, unsigned index, uint32_t length)
{
	MunchSDE *state = (MunchSDE *)cookie;
	if (index < 1 || index > 2 || !state->pixels[index] || length != state->buffers[index].length) return false;
	uint64_t began = mach_absolute_time();
	munchSdeCleanRange(state->pixels[index], length);
	state->cleanTime += mach_absolute_time() - began;
	return true;
}

static void
munchSdeDelay(void *, uint32_t usec)
{
	if (!usec) return;
	if (usec <= 5000) {
		IODelay(usec);
		return;
	}
	IOSleep(usec / 1000);
	if (usec % 1000) IODelay(usec % 1000);
}

static void
munchSdeBusyDelay(void *, uint32_t usec)
{
	IODelay(usec);
}

static bool
munchSdeTlbSync(MunchSDE *state)
{
	state->cb[0x610 / 4] = (uint32_t)(state->domain.ttbr >> 48);
	munchSdeBarrier(NULL);
	state->cb[0x7f0 / 4] = 0;
	munchSdeBarrier(NULL);
	for (unsigned attempt = 0; attempt < 1000; attempt++) {
		if (!(state->cb[0x7f4 / 4] & 1)) {
			munchSdeBarrier(NULL);
			return true;
		}
		IODelay(10);
	}
	return false;
}

static IOBufferMemoryDescriptor *
munchSdeAllocate(MunchSDE *state, uint32_t length, uint8_t **bytes, uint64_t *physical)
{
	state->bufferCheck = "vm-allocation";
	IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryHostPhysicallyContiguous | kIOMemoryMapperNone, length, 0xffffc000ULL);
	if (!memory) return NULL;
	state->bufferCheck = "vm-prepare";
	if (memory->prepare() != kIOReturnSuccess) {
		memory->release();
		return NULL;
	}
	IOByteCount segment = 0;
	uint64_t allocatedPhysical = memory->getPhysicalSegment(0, &segment, kIOMemoryMapperNone);
	uint8_t *allocatedBytes = (uint8_t *)memory->getBytesNoCopy();
	bool overlaps = false;
	for (unsigned index = 0; index < 3; index++) {
		if (state->buffers[index].physical && munch_sde_overlaps(allocatedPhysical, length,
		    state->buffers[index].physical, state->buffers[index].length)) overlaps = true;
	}
	if (!allocatedBytes || !allocatedPhysical || (allocatedPhysical & 0x3fff) || segment < length ||
	    allocatedPhysical >= (1ULL << 32) || allocatedPhysical + length > (1ULL << 32) || overlaps) {
		state->bufferCheck = "vm-physical-range";
		memory->complete();
		memory->release();
		return NULL;
	}
	*physical = allocatedPhysical;
	*bytes = allocatedBytes;
	bzero(*bytes, length);
	return memory;
}

static enum munch_sde_result
munchSdeImportPool(MunchSDE *state)
{
	DTEntry chosen;
	const void *property = NULL;
	unsigned propertySize = 0;
	state->poolCheck = "absent";
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "munch-sde-buffers", &property, &propertySize) != kSuccess)
		return MUNCH_SDE_DISABLED;
	state->poolCheck = "property-size";
	uint64_t region[2];
	if (!property || propertySize != sizeof(region)) return MUNCH_SDE_MAPPING;
	memcpy(region, property, sizeof(region));
	state->poolPhysical = region[0];
	state->poolLength = region[1];
	state->poolCheck = "static-range";
	const boot_args *args = (const boot_args *)PE_state.bootArgs;
	if (!args || args->memSize > UINT64_MAX - args->physBase ||
	    args->topOfKernelData > args->physBase + args->memSize ||
	    !munch_sde_pool_valid(region[0], region[1], args->physBase, args->topOfKernelData)) return MUNCH_SDE_MAPPING;
	state->poolCheck = "kernel-overlap";
	uint64_t kernelEnd = getlastkerneladdr();
	if (kernelEnd < args->virtBase || kernelEnd - args->virtBase > args->memSize ||
	    region[0] < args->physBase + (kernelEnd - args->virtBase)) return MUNCH_SDE_MAPPING;
	state->poolCheck = "boot-overlap";
	if (munch_sde_overlaps(region[0], (uint32_t)region[1], state->buffers[0].physical, state->buffers[0].length))
		return MUNCH_SDE_MAPPING;
	const char *paths[] = { "/chosen", "/chosen/memory-map", "/chosen/memory-map" };
	const char *names[] = { "munch-scm-metadata", "RAMDisk", "RAMDisk1" };
	state->poolCheck = "reserved-overlap";
	for (unsigned index = 0; index < 3; index++) {
		DTEntry entry;
		if (SecureDTLookupEntry(NULL, paths[index], &entry) != kSuccess ||
		    SecureDTGetProperty(entry, names[index], &property, &propertySize) != kSuccess) continue;
		uint64_t other[2];
		if (!property || propertySize != sizeof(other)) return MUNCH_SDE_MAPPING;
		memcpy(other, property, sizeof(other));
		if (!other[1] || other[1] > UINT64_MAX - other[0] ||
		    (region[0] < other[0] + other[1] && other[0] < region[0] + region[1])) return MUNCH_SDE_MAPPING;
	}
	state->poolCheck = "static-translation";
	uint8_t *bytes = (uint8_t *)ml_static_ptovirt(region[0]);
	if (!bytes) return MUNCH_SDE_MAPPING;
	for (uint32_t offset = 0; offset < region[1]; offset += 4096) {
		if (kvtophys((vm_offset_t)bytes + offset) != region[0] + offset) return MUNCH_SDE_MAPPING;
	}
	state->poolBytes = bytes;
	state->poolCheck = "ready";
	return MUNCH_SDE_OK;
}

static enum munch_sde_result
munchSdeInstallBuffers(MunchSDE *state)
{
	uint32_t length = (state->buffers[0].length + 0x3fff) & ~0x3fffu;
	enum munch_sde_result imported = munchSdeImportPool(state);
	if (imported != MUNCH_SDE_OK && imported != MUNCH_SDE_DISABLED) return imported;
	if (state->poolBytes && length > MUNCH_SDE_POOL_SLOT_SIZE) return MUNCH_SDE_MAPPING;
	for (unsigned index = 1; index < 3; index++) {
		state->buffers[index].length = length;
		if (state->poolBytes) {
			uint32_t offset = (index - 1) * MUNCH_SDE_POOL_SLOT_SIZE;
			state->pixels[index] = state->poolBytes + offset;
			state->buffers[index].physical = state->poolPhysical + offset;
			bzero(state->pixels[index], length);
		} else {
			state->memory[index] = munchSdeAllocate(state, length, &state->pixels[index], &state->buffers[index].physical);
			if (!state->memory[index]) return MUNCH_SDE_MAPPING;
		}
		state->buffers[index].iova = (uint32_t)state->buffers[index].physical;
	}
	state->bufferCheck = "address-validation";
	enum munch_sde_result result = munch_sde_check_buffers(&state->domain, munchSdeReadPte, state,
	    &state->route, state->buffers);
	if (result == MUNCH_SDE_OK) state->bufferCheck = "ready";
	if (result == MUNCH_SDE_OK || !(state->domain.sctlr & MUNCH_SMMU_SCTLR_M)) return result;
	if (result != MUNCH_SDE_MAPPING || (state->domain.tcr & 63) != 32) return result;
	uint64_t root = state->domain.ttbr & 0x0000fffffffff000ULL;
	unsigned slot;
	for (slot = 0; slot < 4; slot++) {
		uint64_t value;
		if (!munchSdeReadPte(state, root + slot * 8, &value)) return MUNCH_SDE_MAPPING;
		if (!value && !munch_sde_overlaps((uint64_t)slot << 30, 0x40000000,
		    state->buffers[0].iova, state->buffers[0].length)) break;
	}
	if (slot == 4) return MUNCH_SDE_MAPPING;
	unsigned attribute;
	for (attribute = 0; attribute < 8; attribute++) {
		unsigned value = (state->domain.mair >> (attribute * 8)) & 255;
		if (value == 0x44 || value == 0xff) break;
	}
	if (attribute == 8) return MUNCH_SDE_DOMAIN;
	uint8_t *tableBytes = NULL;
	if (state->poolBytes) {
		tableBytes = state->poolBytes + 2 * MUNCH_SDE_POOL_SLOT_SIZE;
		state->tablePhysical = state->poolPhysical + 2 * MUNCH_SDE_POOL_SLOT_SIZE;
		bzero(tableBytes, MUNCH_SDE_POOL_TABLE_SIZE);
	} else {
		state->tables = munchSdeAllocate(state, MUNCH_SDE_POOL_TABLE_SIZE, &tableBytes, &state->tablePhysical);
		if (!state->tables) return MUNCH_SDE_MAPPING;
	}
	state->tableBytes = (uint64_t *)tableBytes;
	state->tablePages = 66;
	unsigned used;
	result = munch_sde_build_branch(state->tableBytes, state->tablePages, state->tablePhysical,
	    slot << 30, attribute, state->buffers, &used);
	if (result != MUNCH_SDE_OK) return result;
	state->rootPhysical = root + slot * 8;
	result = munch_sde_check_buffers(&state->domain, munchSdePreviewPte, state, &state->route, state->buffers);
	if (result != MUNCH_SDE_OK) return result;
	state->rootEntry = (volatile uint64_t *)munchSdeTableAddress(state, state->rootPhysical);
	if (!state->rootEntry || *state->rootEntry || munchSdeGuard(state) != MUNCH_SDE_OK) return MUNCH_SDE_STALE;
	munchSdeCleanRange(tableBytes, used * 4096);
	state->published = true;
	munchSdeBarrier(NULL);
	*state->rootEntry = state->tablePhysical | 3;
	munchSdeCleanRange((void *)state->rootEntry, 8);
	if (!munchSdeTlbSync(state)) return MUNCH_SDE_TIMEOUT;
	result = munch_sde_check_buffers(&state->domain, munchSdeReadPte, state, &state->route, state->buffers);
	if (result == MUNCH_SDE_OK) state->bufferCheck = "ready";
	return result;
}

static enum munch_sde_result
munchSdeInitialize(MunchSDE *state, uint8_t *bootPixels, uint32_t stride, uint32_t width, uint32_t height)
{
	state->nativeRefreshHz = munchSdeRequestedRefreshHz();
	state->initPhase = "boot-geometry";
	if (width != MUNCH_PANEL_WIDTH || height != MUNCH_PANEL_HEIGHT || stride < width * 4 || stride > 0xffff)
		return MUNCH_SDE_FORMAT;
	state->initPhase = "boot-buffer";
	state->pixels[0] = bootPixels;
	state->buffers[0].physical = kvtophys((vm_offset_t)bootPixels);
	state->buffers[0].length = (stride * height + 4095) & ~4095u;
	if (!state->buffers[0].physical || (state->buffers[0].physical & 4095) ||
	    state->buffers[0].physical >= (1ULL << 48) ||
	    state->buffers[0].physical + state->buffers[0].length > (1ULL << 48)) return MUNCH_SDE_MAPPING;
	for (uint32_t offset = 0; offset < stride * height; offset += 4096) {
		if (kvtophys((vm_offset_t)bootPixels + offset) != state->buffers[0].physical + offset)
			return MUNCH_SDE_MAPPING;
	}
	state->initPhase = "display-power";
	state->gcc = (volatile uint32_t *)ml_io_map(MUNCH_DISPLAY_GCC_BASE, 0x1000);
	state->cc = (volatile uint32_t *)ml_io_map(MUNCH_DISPLAY_CC_BASE, 0x1000);
	state->power = (volatile uint32_t *)ml_io_map(MUNCH_DISPLAY_POWER_BASE, 0x1000);
	if (!munchSdePowered(state)) return MUNCH_SDE_DISABLED;
	state->initPhase = "register-mapping";
	state->mdp = (volatile uint32_t *)ml_io_map(MUNCH_MDP_PHYS, MUNCH_MDP_SIZE);
	state->dsi = (volatile uint32_t *)ml_io_map(MUNCH_DSI_CTRL_BASE, MUNCH_DSI_CTRL_SIZE);
	state->smmu = (volatile uint32_t *)ml_io_map(MUNCH_APPS_SMMU_PHYS, 0x2000);
	if (!state->mdp || !state->dsi || !state->smmu) return MUNCH_SDE_DISABLED;
	state->initPhase = "inherited-route";
	munchSdeReadSnapshot(state->mdp, state->dsi, &state->baseline, false);
	state->snapshotValid = true;
	enum munch_sde_result result = munch_sde_find_route(&state->baseline, &state->route);
	if (result != MUNCH_SDE_OK) return result;
	state->initPhase = "sspp-layout";
	if (state->route.stride != stride) return MUNCH_SDE_UNSUPPORTED;
	state->baseline.dsi_status = 0;
	state->buffers[0].iova = state->route.source;
	if ((uint64_t)state->buffers[0].iova + state->buffers[0].length > (1ULL << 32)) return MUNCH_SDE_MAPPING;
	state->initPhase = "smmu-stream";
	state->scr0 = state->smmu[MUNCH_SMMU_SCR0 / 4];
	state->idr0 = state->smmu[MUNCH_SMMU_IDR0 / 4];
	state->idr1 = state->smmu[MUNCH_SMMU_IDR1 / 4];
	result = munchSdeStream(state, &state->domain);
	if (result != MUNCH_SDE_OK) return result;
	state->initPhase = "smmu-context";
	state->streamSmr = state->streams[state->domain.stream].smr;
	state->registerPage = state->idr1 & (1u << 31) ? 0x10000 : 0x1000;
	uint64_t contextOffset = (uint64_t)((2u << ((state->idr1 >> 28) & 7)) + state->domain.context) * state->registerPage;
	if (contextOffset + 0x1000 > MUNCH_APPS_SMMU_WINDOW_SIZE) return MUNCH_SDE_DOMAIN;
	state->contextPhysical = MUNCH_APPS_SMMU_PHYS + contextOffset;
	state->gr1 = (volatile uint32_t *)ml_io_map(MUNCH_APPS_SMMU_PHYS + state->registerPage, state->registerPage);
	state->cb = (volatile uint32_t *)ml_io_map(MUNCH_APPS_SMMU_PHYS + contextOffset, 0x1000);
	if (!state->gr1 || !state->cb) return MUNCH_SDE_DOMAIN;
	state->initPhase = "boot-translation";
	munchSdeDomainSnapshot(state, &state->domain);
	for (uint32_t offset = 0; offset < state->buffers[0].length; offset += 4096) {
		uint64_t physical;
		state->expectedPhysical = state->buffers[0].physical + offset;
		result = munch_smmu_translate_checked(&state->domain, munchSdeReadPte, state,
		    state->buffers[0].iova + offset, &physical, &state->bootWalk);
		if (result != MUNCH_SDE_OK) return result;
		if (physical != state->expectedPhysical) {
			state->bootWalk.check = "boot-physical";
			return MUNCH_SDE_MAPPING;
		}
		state->bootVerifiedPages++;
	}
	state->initPhase = "back-buffer-mapping";
	result = munchSdeInstallBuffers(state);
	if (result != MUNCH_SDE_OK) return result;
	state->initPhase = "presenter-prepare";
	struct munch_sde_io io = { state, munchSdeRead, munchSdeWrite, munchSdeBarrier, munchSdeClean,
	    munchSdeGuard, munchSdeDsiStatus, munchSdeCompletionKickoff };
	result = munch_sde_prepare(&state->presenter, &io, &state->baseline, &state->domain, munchSdeReadPte, state->buffers);
	if (result == MUNCH_SDE_OK && state->nativeRequested) {
		if (munchSdePrepareCompletionIRQ(state))
			IOLog("MUNCH SDE: PP_DONE IRQ enabled on GIC INTID %u\n", state->completionIRQIntid);
		else
			IOLog("MUNCH SDE: PP_DONE IRQ unavailable; using timed status fallback\n");
	}
	if (result == MUNCH_SDE_OK && state->nativeTakeover) {
		state->initPhase = "native-resources";
		if (state->route.ctl || state->route.mixer || state->route.pipe || state->route.pingpong ||
		    state->route.stage != 4 || state->baseline.ctls[0].dsc != 1 || !state->poolBytes)
			return MUNCH_SDE_UNSUPPORTED;
		state->modeMMIO[MUNCH_MODE_MDP] = state->mdp;
		state->modeMMIO[MUNCH_MODE_DSI] = state->dsi;
		state->modeMMIO[MUNCH_MODE_PHY] = (volatile uint32_t *)ml_io_map(0x0ae94400, 0x800);
		state->modeMMIO[MUNCH_MODE_CC] = (volatile uint32_t *)ml_io_map(0x0af00000, 0x4000);
		if (!state->modeMMIO[MUNCH_MODE_PHY] || !state->modeMMIO[MUNCH_MODE_CC]) return MUNCH_SDE_DISABLED;
		state->mode.io = (struct munch_mode_io){ state, munchSdeModeRead, munchSdeModeWrite,
		    munchSdeModeDelay, munchSdeModeClean, munchSdeModeHealthy };
		state->mode.phase = "native-await-first-frame";
	}
	if (result == MUNCH_SDE_OK) state->initPhase = "ready";
	return result;
}

static void
munchSdeReleaseUnused(MunchSDE *state)
{
	if (state->published || state->presenter.armed) return;
	for (unsigned index = 1; index < 3; index++) {
		if (state->memory[index]) {
			state->memory[index]->complete();
			state->memory[index]->release();
			state->memory[index] = NULL;
		}
	}
	if (state->tables) {
		state->tables->complete();
		state->tables->release();
		state->tables = NULL;
	}
	for (unsigned index = 0; index < state->ramCount; index++) {
		state->ram[index].mapping->release();
		state->ram[index].memory->release();
	}
	state->ramCount = 0;
}

static enum munch_sde_result
munchSdeBeginFrame(MunchSDE *state, uint8_t **pixels)
{
	if (state->panelBlanked || state->panelCommandActive || state->panelCommandFailed) return MUNCH_SDE_DISABLED;
	lck_mtx_lock(&hakios_smmu_lock);
	bool fullGuard = !state->nativeRequested || !state->nativeFrameStarted || ++state->nativeGuardFrames >= 30;
	enum munch_sde_result result = fullGuard ? munchSdeGuard(state) : munchSdeGuardFast(state);
	if (fullGuard && result == MUNCH_SDE_OK) state->nativeGuardFrames = 0;
	unsigned index;
	if (result == MUNCH_SDE_OK) result = munch_sde_acquire(&state->presenter, &index);
	if (result == MUNCH_SDE_OK) *pixels = state->pixels[index];
	else state->presenter.fault = result;
	lck_mtx_unlock(&hakios_smmu_lock);
	return result;
}

static enum munch_sde_result
munchSdePollFrame(MunchSDE *state)
{
	state->pollChecks++;
	if (state->presenter.fault != MUNCH_SDE_OK) return state->presenter.fault;
	if (state->stopped || !munchSdePowered(state) || munch_smmu_fault_bits(state->cb[MUNCH_SMMU_CB_FSR / 4]))
		return state->presenter.fault = MUNCH_SDE_FAULT;
	if (__atomic_load_n(&state->completionIRQFault, __ATOMIC_ACQUIRE))
		return state->presenter.fault = MUNCH_SDE_FAULT;
	uint64_t now = micros(mach_absolute_time());
	bool completionHint = __atomic_load_n(&state->completionIRQPending, __ATOMIC_ACQUIRE) != 0;
	uint32_t status = munchSdeRead(state, MUNCH_SDE_INTR_STATUS);
	uint32_t ctl = munch_sde_ctl_offsets[state->route.ctl];
	if (!state->nativeRequested && (completionHint || (status & state->route.done_mask)) &&
	    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_START) &&
	    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_FLUSH)) {
		enum munch_sde_result guarded = munchSdeGuard(state);
		if (guarded != MUNCH_SDE_OK) return state->presenter.fault = guarded;
	}
	enum munch_sde_result result = munch_sde_poll_fast_hint(&state->presenter, now, completionHint);
	if (result == MUNCH_SDE_OK && state->presenter.pending == 3 && completionHint)
		__atomic_store_n(&state->completionIRQPending, 0u, __ATOMIC_RELEASE);
	return result;
}

static enum munch_sde_result
munchSdeRestoreRefresh(MunchSDE *state)
{
	uint32_t source = state->buffers[state->presenter.front].iova;
	if (state->nativeRequested && !state->nativeTakeover)
		return munch_sde_restore_refresh_fast(&state->refresh, &state->presenter.io, &state->baseline,
		    &state->route, source, munchSdeBusyDelay);
	if (!state->nativeRequested || state->nativeTakeover)
		return munch_sde_restore_refresh(&state->refresh, &state->presenter.io, &state->baseline,
		    &state->route, source, munchSdeBusyDelay);
	return MUNCH_SDE_UNSUPPORTED;
}

static enum munch_sde_result
munchSdeSwitchRefresh(MunchSDE *state, uint32_t refresh_hz)
{
	if (!state || !state->nativeTakeover || !state->mode.configured ||
	    (refresh_hz != 60 && refresh_hz != 120)) return MUNCH_SDE_UNSUPPORTED;
	enum munch_sde_result result = munch_sde_pause_refresh(&state->refresh, &state->presenter.io,
	    &state->baseline, &state->route, munchSdeBusyDelay);
	if (result != MUNCH_SDE_OK) return result;
	if (!munch_mode_switch_refresh(&state->mode, refresh_hz)) return MUNCH_SDE_FAULT;
	state->nativeRefreshHz = refresh_hz;
	state->baseline.tear_vsync = state->mdp[MUNCH_SDE_INTF1_OFFSET / 4 + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4];
	state->refresh.vsync = state->baseline.tear_vsync;
	/* Native takeover keeps autorefresh paused between command-mode frames. */
	return MUNCH_SDE_OK;
}

static enum munch_sde_result
munchSdeSubmitFrame(MunchSDE *state)
{
	lck_mtx_lock(&hakios_smmu_lock);
	enum munch_sde_result result = state->nativeRequested ? munchSdeGuardFast(state) : munchSdeGuard(state);
	if (result == MUNCH_SDE_OK) {
		if (state->nativeRequested && !state->nativeTakeover) {
			uint32_t ctl = munch_sde_ctl_offsets[state->route.ctl];
			bool idle = !(munchSdeDsiStatus(state) & 4) &&
			    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_START) &&
			    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_FLUSH);
			if (state->nativeFrameStarted && state->refresh.owned && state->refresh.paused && idle)
				result = MUNCH_SDE_OK;
			else
				result = munch_sde_pause_refresh_fast(&state->refresh, &state->presenter.io,
				    &state->baseline, &state->route, munchSdeBusyDelay);
		}
		else
			result = munch_sde_pause_refresh(&state->refresh, &state->presenter.io,
			    &state->baseline, &state->route, munchSdeDelay);
	}
	if (result == MUNCH_SDE_OK && state->nativeTakeover && !state->mode.configured) {
		unsigned index = state->presenter.drawing;
		uint32_t offset = MUNCH_PANEL_HEIGHT * MUNCH_PANEL_WIDTH * 4;
		if (index < 1 || index > 2 || state->buffers[index].length < offset + 136) result = MUNCH_SDE_MAPPING;
		else {
			state->mode.scratch = state->pixels[index] + offset;
			state->mode.scratch_iova = state->buffers[index].iova + offset;
			state->mode.scratch_size = state->buffers[index].length - offset;
			uint32_t init_refresh = state->nativeRefreshHz == 120 ? 60 : state->nativeRefreshHz;
			if (!munch_mode_configure(&state->mode, init_refresh) ||
			    (state->nativeRefreshHz != init_refresh &&
			    !munch_mode_switch_refresh(&state->mode, state->nativeRefreshHz))) {
				volatile uint32_t *dma = (volatile uint32_t *)state->mode.scratch;
				IOLog("MUNCH SDE DSI FAIL: ctrl=%08x status=%08x fifo=%08x dma=%08x addr=%08x len=%08x "
				    "dfifo=%08x tpgfifo=%08x trig=%08x sched=%08x/%08x sw=%08x int=%08x "
				    "ack=%08x timeout=%08x lane=%08x clk=%08x idle=%08x failed=%u:%x=%x "
				    "batches=%u last=%u/%u words=%08x/%08x\n",
				    state->dsi[0x004 / 4], state->dsi[0x008 / 4], state->dsi[0x00c / 4],
				    state->dsi[0x03c / 4], state->dsi[0x048 / 4], state->dsi[0x04c / 4],
				    state->dsi[0x050 / 4], state->dsi[0x1dc / 4], state->dsi[0x084 / 4],
				    state->dsi[0x100 / 4], state->dsi[0x104 / 4], state->dsi[0x090 / 4],
				    state->dsi[0x110 / 4], state->dsi[0x068 / 4], state->dsi[0x0c0 / 4],
				    state->dsi[0x0b4 / 4], state->dsi[0x120 / 4], state->dsi[0x194 / 4],
				    state->mode.failed, state->mode.failed_offset, state->mode.failed_value,
				    state->mode.batches, state->mode.last_batch_count, state->mode.last_batch_bytes,
				    state->mode.last_batch_word0, state->mode.last_batch_word1);
				IOLog("MUNCH SDE DSI DMA: %08x %08x %08x %08x %08x %08x\n",
				    dma[0], dma[1], dma[2], dma[3], dma[4], dma[5]);
				result = MUNCH_SDE_FAULT;
			}
			else {
				munchSdeReadSnapshot(state->mdp, state->dsi, &state->baseline, false);
				state->baseline.dsi_status = 0;
				state->refresh.config = state->baseline.autorefresh;
				state->refresh.vsync = state->baseline.tear_vsync;
				result = munch_sde_find_route(&state->baseline, &state->route);
				state->presenter.route = state->route;
				state->presenter.route.flush_mask |= (1u << 6) | (1u << 17) | (1u << 22) | (1u << 31);
				munchSdeWrite(state, 0x2104, 1);
				munchSdeWrite(state, 0x2110, 2);
				if (result == MUNCH_SDE_OK) result = munchSdeGuard(state);
			}
		}
	}
	if (result == MUNCH_SDE_OK) {
		if (state->completionIRQAttached) {
			__atomic_store_n(&state->completionIRQPending, 0u, __ATOMIC_RELEASE);
			__atomic_store_n(&state->completionIRQArmed, 0u, __ATOMIC_RELEASE);
			__atomic_store_n(&state->completionIRQFault, 0u, __ATOMIC_RELEASE);
			state->mdp[MUNCH_SDE_INTR_ENABLE / 4] |= state->completionIRQEnableMask;
			munchSdeBarrier(NULL);
		}
		result = state->nativeRequested ?
		    munch_sde_submit_fast(&state->presenter, micros(mach_absolute_time()), 50000) :
		    munch_sde_submit(&state->presenter, micros(mach_absolute_time()), 50000);
	}
	if (result == MUNCH_SDE_OK && state->nativeRequested && !state->nativeTakeover)
		state->nativeFrameStarted = true;
	if (result == MUNCH_SDE_OK && state->nativeTakeover && !state->mode.teConnected) {
		if (!munch_mode_connect_te(&state->mode, true)) result = state->presenter.fault = MUNCH_SDE_FAULT;
		else {
			state->baseline.tear_vsync |= MUNCH_SDE_EXTERNAL_TE;
			state->refresh.vsync = state->baseline.tear_vsync;
		}
	}
	if (result != MUNCH_SDE_OK) {
		munchSdeDisableCompletionIRQ(state);
		if (state->presenter.pending == 3) state->presenter.drawing = 3;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return result;
}

static enum munch_sde_result
munchSdeCompleteFrame(MunchSDE *state)
{
	uint64_t began = mach_absolute_time();
	lck_mtx_lock(&hakios_smmu_lock);
	enum munch_sde_result result = munchSdePollFrame(state);
	if (result == MUNCH_SDE_BUSY &&
	    __atomic_load_n(&state->completionIRQMasked, __ATOMIC_ACQUIRE))
		munchSdeFallbackCompletionIRQ(state);
	if (result != MUNCH_SDE_BUSY) munchSdeDisableCompletionIRQ(state);
	if (result == MUNCH_SDE_OK && state->presenter.pending != 3)
		result = MUNCH_SDE_BUSY;
	if (result == MUNCH_SDE_OK && state->presenter.completed == state->presenter.submitted) {
		state->completedTime = mach_absolute_time();
		if (state->nativeRequested) {
			state->presenter.route.flush_mask = state->route.flush_mask;
			if (state->nativeTakeover && !munch_mode_light(&state->mode))
				result = state->presenter.fault = MUNCH_SDE_FAULT;
			/* Native mode keeps command-mode ownership between frames.  Restoring
			 * autorefresh here forces a full pause/restore transaction before the
			 * next frame and is the dominant source of pacing stalls. */
		} else {
			result = munchSdeRestoreRefresh(state);
		}
		if (result != MUNCH_SDE_OK) state->presenter.fault = result;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	state->waitTime += mach_absolute_time() - began;
	return result;
}

static enum munch_sde_result
munchSdeEndFrame(MunchSDE *state)
{
	enum munch_sde_result result = munchSdeSubmitFrame(state);
	if (result != MUNCH_SDE_OK) return result;
	for (unsigned attempt = 0; attempt < 500; attempt++) {
		result = munchSdeCompleteFrame(state);
		if (result != MUNCH_SDE_BUSY) return result;
		IODelay(100);
	}
	state->presenter.fault = MUNCH_SDE_TIMEOUT;
	return MUNCH_SDE_TIMEOUT;
}

static inline bool
munchSdeSampleVBlank(MunchSDE *state)
{
	if (!state || !state->nativeRequested || state->stopped || state->panelBlanked ||
	    state->panelCommandActive || state->presenter.fault != MUNCH_SDE_OK) return false;
	uint64_t now = mach_absolute_time();
	uint64_t nowUs = micros(now);
	volatile uint32_t *intf = state->mdp + MUNCH_SDE_INTF1_OFFSET / 4;
	state->vblankRaw = intf[MUNCH_SDE_INTF_TEAR_INT_COUNT / 4];
	state->vblankFrameCount = intf[MUNCH_SDE_INTF_FRAME_COUNT / 4];
	state->vblankLineCount = intf[MUNCH_SDE_INTF_LINE_COUNT / 4];
	state->vblankTearLine = intf[MUNCH_SDE_INTF_TEAR_LINE / 4];
	if (!state->vblankFallbackNextUs) {
		state->vblankLastHardwareUs = nowUs;
		state->vblankFallbackNextUs = nowUs + 16667;
	}
	uint32_t frame = state->vblankFrameCount;
	uint32_t events;
	if (!state->mode.vblankFrameValid) {
		state->mode.vblankFrame = frame;
		state->mode.vblankFrameValid = true;
		events = 0;
	} else {
		events = frame - state->mode.vblankFrame;
		state->mode.vblankFrame = frame;
	}
	if (events) {
		state->vblankCount += events;
		state->vblankTime = now;
		state->vblankHardwareEvents += events;
		state->vblankLastHardwareUs = nowUs;
		state->vblankFallbackNextUs = nowUs + 16667;
		state->vblankFallback = false;
		return true;
	}
	/* Native command-mode presentation must not synthesize a display clock when
	 * the panel's frame counter has stopped.  A timer fallback hides a broken
	 * TE/DSI path and makes CoreAnimation queue frames into a black display. */
	return false;
}

static enum munch_sde_result
munchSdePreparePanel(MunchSDE *state)
{
	if (!state || !state->presenter.armed || state->panelCommandActive || state->panelCommandFailed)
		return MUNCH_SDE_DISABLED;
	if (state->presenter.fault != MUNCH_SDE_OK) return state->presenter.fault;
	if (state->presenter.pending != 3 || state->presenter.drawing != 3) return MUNCH_SDE_BUSY;
	lck_mtx_lock(&hakios_smmu_lock);
	enum munch_sde_result result = munchSdeGuard(state);
	if (result == MUNCH_SDE_OK) result = munch_sde_pause_refresh(&state->refresh, &state->presenter.io,
	    &state->baseline, &state->route, munchSdeDelay);
	uint32_t ctl = munch_sde_ctl_offsets[state->route.ctl];
	if (result == MUNCH_SDE_OK && ((munchSdeDsiStatus(state) & 4) ||
	    munchSdeRead(state, ctl + MUNCH_SDE_CTL_START) || munchSdeRead(state, ctl + MUNCH_SDE_CTL_FLUSH)))
		result = MUNCH_SDE_BUSY;
	if (result == MUNCH_SDE_OK) state->panelCommandActive = true;
	lck_mtx_unlock(&hakios_smmu_lock);
	return result;
}

static enum munch_sde_result
munchSdeFinishPanel(MunchSDE *state, bool on, bool completed)
{
	if (!state->panelCommandActive) return MUNCH_SDE_STALE;
	lck_mtx_lock(&hakios_smmu_lock);
	enum munch_sde_result result = completed ? munchSdeGuard(state) : MUNCH_SDE_FAULT;
	if (result == MUNCH_SDE_OK) {
		state->panelBlanked = !on;
		if (state->mode.configured) munchSdeWrite(state, 0x6e808, 4);
		if (on && !(state->nativeTakeover && state->mode.configured))
			result = munchSdeRestoreRefresh(state);
	}
	if (result != MUNCH_SDE_OK) {
		state->panelCommandFailed = true;
		state->presenter.fault = result;
	}
	state->panelCommandActive = false;
	lck_mtx_unlock(&hakios_smmu_lock);
	return result;
}

extern "C" int
munch_sde_native_owned(void)
{
	/* Merely allocating the native presenter must not suppress early-boot diagnostics. */
	return gMunchSDEOwner && gMunchSDEOwner->nativeRequested && gMunchSDEOwner->mode.touched;
}

extern "C" int
munch_sde_quiesce(void)
{
	MunchSDE *state = gMunchSDEOwner;
	if (state && state->mode.touched) return 0;
	if (state && (state->panelBlanked || state->panelCommandActive || state->panelCommandFailed)) return 0;
	if (!state || state->stopped || (!state->presenter.armed && !state->published)) return 1;
	if (state->presenter.armed && state->presenter.drawing != 3) return 0;
	if (state->presenter.fault == MUNCH_SDE_TIMEOUT ||
	    (state->presenter.fault == MUNCH_SDE_FAULT &&
	     (state->published || state->presenter.pending != 3 || state->presenter.submitted))) return 0;
	if (!munchSdePowered(state)) return 0;
	if (state->refresh.owned && !state->refresh.paused) return 0;
	if (!state->presenter.submitted && munchSdeGuard(state) != MUNCH_SDE_OK) return 0;
	uint32_t ctl = munch_sde_ctl_offsets[state->route.ctl];
	if (state->presenter.submitted) {
		if (!state->refresh.owned &&
		    munch_sde_pause_refresh_for_return(&state->refresh, &state->presenter.io, &state->baseline,
		        &state->route, munchSdeBusyDelay) != MUNCH_SDE_OK) return 0;
		bool idle = false;
		for (unsigned attempt = 0; attempt < 1000; attempt++) {
			if (!(munchSdeDsiStatus(state) & 4) && !munchSdeRead(state, ctl + MUNCH_SDE_CTL_START) &&
			    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_FLUSH)) { idle = true; break; }
			IODelay(50);
		}
		if (!idle || munchSdeGuard(state) != MUNCH_SDE_OK) return 0;
		state->presenter.io.write(state->presenter.io.cookie, MUNCH_SDE_INTR_CLEAR, state->route.done_mask);
		munchSdeBarrier(NULL);
		if (munchSdeRead(state, MUNCH_SDE_INTR_STATUS) & state->route.done_mask) return 0;
		munchSdeWrite(state, munch_sde_sspps[state->route.pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR, state->buffers[0].iova);
		munchSdeBarrier(NULL);
		munchSdeWrite(state, ctl + MUNCH_SDE_CTL_FLUSH, state->route.flush_mask);
		munchSdeBarrier(NULL);
		munchSdeWrite(state, ctl + MUNCH_SDE_CTL_START, 1);
		munchSdeBarrier(NULL);
		bool done = false;
		for (unsigned attempt = 0; attempt < 1000; attempt++) {
			uint32_t status = munchSdeRead(state, MUNCH_SDE_INTR_STATUS);
			if ((status & MUNCH_SDE_INTF1_UNDERRUN) || munch_smmu_fault_bits(state->cb[MUNCH_SMMU_CB_FSR / 4])) return 0;
			if ((status & state->route.done_mask) && !(munchSdeDsiStatus(state) & 4) &&
			    !munchSdeRead(state, ctl + MUNCH_SDE_CTL_START) && !munchSdeRead(state, ctl + MUNCH_SDE_CTL_FLUSH)) {
				done = true;
				break;
			}
			IODelay(50);
		}
		if (!done) return 0;
		state->presenter.io.write(state->presenter.io.cookie, MUNCH_SDE_INTR_CLEAR, state->route.done_mask);
		munchSdeBarrier(NULL);
		state->presenter.front = 0;
		state->presenter.pending = 3;
		if (munchSdeGuard(state) != MUNCH_SDE_OK) return 0;
	}
	if (munch_sde_restore_refresh(&state->refresh, &state->presenter.io, &state->baseline,
	    &state->route, state->buffers[0].iova, munchSdeBusyDelay) != MUNCH_SDE_OK) return 0;
	if (state->published) {
		if (*state->rootEntry != (state->tablePhysical | 3)) return 0;
		*state->rootEntry = 0;
		munchSdeCleanRange((void *)state->rootEntry, 8);
		if (!munchSdeTlbSync(state)) {
			*state->rootEntry = state->tablePhysical | 3;
			munchSdeCleanRange((void *)state->rootEntry, 8);
			return 0;
		}
		state->published = false;
	}
	munchSdeReleaseCompletionIRQ(state);
	state->stopped = true;
	return 1;
}

#endif
