#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>

#include "../../../xnu/files/iokit/Kernel/munch-sde.h"

static_assert(MUNCH_SDE_VIG_SCALER_OP_MODE == 0xa04 && MUNCH_SDE_VIG_CSC_OP_MODE == 0x1a00);
static_assert(MUNCH_SDE_SSPP_PRE_DOWNSCALE == 0x50 && MUNCH_SDE_SSPP_DECIMATION == 0xb4);
static_assert(MUNCH_SDE_LM_OP_MODE == 0 && MUNCH_SDE_LM_BLEND0_OP == 0x20 && MUNCH_SDE_LM_BLEND0_ALPHA == 0x24);
static_assert(MUNCH_SDE_SSPP_PE_OVERRIDE == 0x80000000 && MUNCH_SDE_SSPP_PE_COUNT == 3);
static_assert(MUNCH_SDE_SSPP_PE_BASE == 0x100 && MUNCH_SDE_SSPP_PE_STRIDE == 0x10);

using vm_offset_t = uintptr_t;
using IOByteCount = size_t;
static constexpr int kernel_task = 0, kIODirectionInOut = 1, kIOMemoryPhysicallyContiguous = 2;
static constexpr int kIOMemoryHostPhysicallyContiguous = 4, kIOMemoryMapperNone = 8;
static constexpr int kIOMapAnywhere = 1, kIOMapCopybackCache = 2, kIOReturnSuccess = 0;
static int hakios_smmu_lock, lockDepth;
static uint64_t clockNow, nextPhysical = 0xc1000000;
static unsigned allocations, sleeps;
static bool transferEnabled = true;
static unsigned refreshScenario, refreshWrites;
static bool resumeSeen, returning;
static bool shortSegment, overlappingAllocation, brokenBoot;
static unsigned physicalMappings, returnMutation;
static bool reservedPool, poolPresent, failVmAllocation, brokenPool;
static unsigned pacingScenario, panelScenario, transferTicks;
static unsigned nativeScenario;
static unsigned vmRequests, poolPropertySize = 16;
static uint64_t poolRegion[2], protectedRegions[3][2];
struct boot_args { uint64_t virtBase, physBase, memSize, topOfKernelData; };
static boot_args testBootArgs;
static struct { void *bootArgs; } PE_state;
using DTEntry = const char *;
static constexpr int kSuccess = 0;
static int SecureDTLookupEntry(void *, const char *path, DTEntry *entry) { *entry = path; return 0; }
static int SecureDTGetProperty(DTEntry entry, const char *name, const void **property, unsigned *size)
{
	if (!poolPresent) return -1;
	if (!strcmp(entry, "/chosen") && !strcmp(name, "munch-sde-buffers")) {
		*property = poolRegion;
		*size = poolPropertySize;
		return 0;
	}
	const char *names[] = { "munch-scm-metadata", "RAMDisk", "RAMDisk1" };
	for (unsigned index = 0; index < 3; index++) {
		if (strcmp(entry, index ? "/chosen/memory-map" : "/chosen") || strcmp(name, names[index]) ||
		    !protectedRegions[index][1]) continue;
		*property = protectedRegions[index];
		*size = 16;
		return 0;
	}
	return -1;
}
static uint64_t getlastkerneladdr() { return testBootArgs.virtBase + 0x4000000; }

struct Region {
	uint64_t physical;
	size_t length;
	uint8_t *bytes;
};
static std::vector<Region> regions;

static uint8_t *addRegion(uint64_t physical, size_t length)
{
	void *bytes;
	assert(posix_memalign(&bytes, 0x4000, (length + 0x3fff) & ~0x3fffUL) == 0);
	memset(bytes, 0, length);
	regions.push_back({physical, length, static_cast<uint8_t *>(bytes)});
	return static_cast<uint8_t *>(bytes);
}

static uint8_t *address(uint64_t physical, size_t length)
{
	for (auto &region : regions)
		if (physical >= region.physical && physical - region.physical + length <= region.length)
			return region.bytes + physical - region.physical;
	return nullptr;
}

static vm_offset_t ml_io_map(vm_offset_t physical, size_t length)
{
	return reinterpret_cast<vm_offset_t>(address(physical, length));
}

static vm_offset_t ml_static_ptovirt(vm_offset_t physical)
{
	return reinterpret_cast<vm_offset_t>(address(physical, MUNCH_SDE_POOL_SIZE));
}

static uint64_t kvtophys(vm_offset_t pointer)
{
	for (auto &region : regions) {
		uintptr_t base = reinterpret_cast<uintptr_t>(region.bytes);
		if (pointer >= base && pointer - base < region.length)
			return region.physical + pointer - base +
			    ((brokenBoot || (brokenPool && region.physical == poolRegion[0])) && pointer - base == 4096 ? 4096 : 0);
	}
	return 0;
}

class IOMemoryMap {
	uint8_t *bytes;
public:
	explicit IOMemoryMap(uint8_t *value) : bytes(value) {}
	uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(bytes); }
	void release() { delete this; }
};

class IOMemoryDescriptor {
protected:
	uint64_t physical;
	size_t length;
public:
	IOMemoryDescriptor(uint64_t base, size_t size) : physical(base), length(size) {}
	virtual ~IOMemoryDescriptor() = default;
	static IOMemoryDescriptor *withPhysicalAddress(uint64_t base, size_t size, int)
	{
		physicalMappings++;
		return address(base, size) ? new IOMemoryDescriptor(base, size) : nullptr;
	}
	IOMemoryMap *map(int) { return new IOMemoryMap(address(physical, length)); }
	void release() { delete this; }
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
	bool prepared = false;
	IOBufferMemoryDescriptor(uint64_t base, size_t size) : IOMemoryDescriptor(base, size) { allocations++; }
	~IOBufferMemoryDescriptor() override { assert(!prepared); allocations--; }
	static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int, int options, uint32_t size, uint64_t mask)
	{
		vmRequests++;
		assert(options == (kIODirectionInOut | kIOMemoryHostPhysicallyContiguous | kIOMemoryMapperNone));
		if (failVmAllocation) return nullptr;
		assert(mask == 0xffffc000ULL);
		uint64_t physical = nextPhysical;
		if (!overlappingAllocation) nextPhysical += (size + 0x3fff) & ~0x3fffU;
		addRegion(physical, size);
		return new IOBufferMemoryDescriptor(physical, size);
	}
	int prepare() { prepared = true; return kIOReturnSuccess; }
	void complete() { assert(prepared); prepared = false; }
	uint64_t getPhysicalSegment(unsigned, IOByteCount *size, int options)
	{
		assert(options == kIOMemoryMapperNone);
		*size = shortSegment ? 4096 : length;
		return physical;
	}
	void *getBytesNoCopy() { return address(physical, length); }
};

static void lck_mtx_lock(int *) { assert(lockDepth++ == 0); }
static void lck_mtx_unlock(int *) { assert(--lockDepth == 0); }
static uint64_t mach_absolute_time() { return clockNow; }
static uint64_t micros(uint64_t value) { return value; }
static void advance(unsigned usec);
static void IOSleep(unsigned msec);
static void IODelay(unsigned usec) { advance(usec); }
#define IOLog(...) ((void)0)

#include "sde-snapshot.inc"
#include "../../../xnu/files/iokit/Kernel/munch-sde-xnu.h"

static void IOSleep(unsigned msec)
{
	if (gMunchSDEOwner && gMunchSDEOwner->presenter.pending != 3) assert(!lockDepth);
	sleeps++;
	advance(msec * 1000);
}

static void advance(unsigned usec)
{
	clockNow += usec;
	if (returning && returnMutation) {
		uint8_t *smmu = address(MUNCH_APPS_SMMU_PHYS, MUNCH_APPS_SMMU_WINDOW_SIZE);
		assert(!mprotect(smmu, MUNCH_APPS_SMMU_WINDOW_SIZE, PROT_READ | PROT_WRITE));
		if (returnMutation == 1) gMunchSDEOwner->cb[MUNCH_SMMU_CB_SCTLR / 4] |= 1;
		else gMunchSDEOwner->cb[MUNCH_SMMU_CB_FSR / 4] |= 2;
		assert(!mprotect(smmu, MUNCH_APPS_SMMU_WINDOW_SIZE, PROT_READ));
		returnMutation = 0;
	}
	uint32_t *mdp = reinterpret_cast<uint32_t *>(address(MUNCH_MDP_PHYS, MUNCH_MDP_SIZE));
	if (refreshScenario) {
		MunchSDE *state = gMunchSDEOwner;
		uint32_t *intf = mdp + MUNCH_SDE_INTF1_OFFSET / 4;
		if (state->refresh.owned && !state->refresh.paused && !resumeSeen) {
			assert(mdp[(munch_sde_sspps[state->route.pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR) / 4] ==
			    state->buffers[state->presenter.front].iova);
			if (!returning) assert(!state->presenter.submitted);
		}
		if (refreshScenario != 88) {
			intf[MUNCH_SDE_INTF_TEAR_LINE / 4] = 2400;
			state->dsi[MUNCH_DSI_STATUS / 4] = 0;
		}
		if (refreshScenario == 90 && clockNow >= 27000) mdp[MUNCH_SDE_DEBUG_STATUS / 4] = 0x80;
		if (refreshScenario == 91) state->cb[MUNCH_SMMU_CB_FSR / 4] = 2;
		if (refreshScenario == 92) intf[MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4] ^= 1;
		if (refreshScenario == 95) mdp[MUNCH_SDE_INTR_STATUS / 4] |= MUNCH_SDE_INTF1_UNDERRUN;
		if (refreshScenario == 120 && returning) state->cb[MUNCH_SMMU_CB_FSR / 4] = 0x602;
		if (resumeSeen && refreshScenario != 93) mdp[MUNCH_SDE_INTR_STATUS / 4] |= state->route.done_mask;
	}
	if (!transferEnabled) return;
	for (unsigned index = 0; index < MUNCH_SDE_CTL_COUNT; index++) {
		uint32_t ctl = munch_sde_ctl_offsets[index];
		if (mdp[(ctl + MUNCH_SDE_CTL_START) / 4]) {
			mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] = 1;
			if (pacingScenario && !returning) {
				transferTicks++;
				if (transferTicks == 3) {
					if (pacingScenario == 2) gMunchSDEOwner->cb[MUNCH_SMMU_CB_FSR / 4] |= 2;
					if (pacingScenario == 3) mdp[(MUNCH_SDE_LM_BASE + MUNCH_SDE_LM_OUT_SIZE) / 4] ^= 1;
					if (pacingScenario == 5) mdp[MUNCH_SDE_INTR_STATUS / 4] |= MUNCH_SDE_INTF1_UNDERRUN;
					if (pacingScenario == 6) gMunchSDEOwner->gcc[0xc / 4] = 0;
				}
				if (transferTicks < 12 || pacingScenario == 4) continue;
			}
			mdp[(ctl + MUNCH_SDE_CTL_START) / 4] = 0;
			mdp[(ctl + MUNCH_SDE_CTL_FLUSH) / 4] = 0;
			mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] = 2400;
			unsigned pingpong = mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_MUX) / 4];
			assert(pingpong < 4);
			mdp[MUNCH_SDE_INTR_STATUS / 4] |= 1u << (8 + pingpong);
		}
	}
}

static void refreshWrite(void *cookie, uint32_t offset, uint32_t value)
{
	MunchSDE *state = static_cast<MunchSDE *>(cookie);
	refreshWrites++;
	if (offset == MUNCH_SDE_INTR_CLEAR) {
		state->mdp[MUNCH_SDE_INTR_STATUS / 4] &= ~value;
		return;
	}
	if (offset == MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) {
		if (refreshScenario == 102 && !(value & 0x80000000)) return;
		if (value & 0x80000000) resumeSeen = true;
	}
	if (refreshScenario == 103 && offset == MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) return;
	if (offset == munch_sde_sspps[state->route.pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR ||
	    offset == munch_sde_ctl_offsets[state->route.ctl] + MUNCH_SDE_CTL_START) {
			assert(state->refresh.paused && state->refresh.config == (state->mode.configured ? 0u : 1u));
		assert(!state->mdp[MUNCH_SDE_DEBUG_STATUS / 4]);
		assert(state->nativeRequested && !state->nativeTakeover ?
		    clockNow > 0 && clockNow < 27000 : clockNow >= 27000);
		if (offset == munch_sde_ctl_offsets[state->route.ctl] + MUNCH_SDE_CTL_START && value == 1 &&
		    state->nativeRequested && state->mode.configured && state->presenter.submitted == 1)
			assert(!(state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4] &
			    MUNCH_SDE_EXTERNAL_TE));
	}
	munchSdeWrite(cookie, offset, value);
}

static void testSafeNativeFrames(MunchSDE *state)
{
	assert(!state->mode.touched && !state->mode.configured && !state->mode.lit);
	state->presenter.io.write = refreshWrite;
	bool restoreTimeout = nativeScenario == 5;
	if (restoreTimeout) refreshScenario = 93;
	unsigned sleepsBefore = sleeps;
	for (unsigned frame = 0; frame < 2; frame++) {
		uint8_t *back = nullptr;
		assert(munchSdeBeginFrame(state, &back) == MUNCH_SDE_OK && back);
		back[0] = (uint8_t)(frame + 1);
		if (nativeScenario == 6) transferEnabled = false;
		auto result = munchSdeEndFrame(state);
		if (nativeScenario == 6) {
			assert(result == MUNCH_SDE_TIMEOUT && state->presenter.pending == 1 && !state->presenter.completed);
			assert(state->refresh.owned && state->refresh.paused && !state->refreshRecoveries);
			assert(!munch_sde_quiesce() && !state->stopped);
			puts("Safe native mode retains pending DMA on frame timeout without refresh recovery");
			return;
		}
		assert(result == MUNCH_SDE_OK && state->presenter.fault == MUNCH_SDE_OK);
		assert(state->presenter.completed == frame + 1 && state->refresh.owned && state->refresh.paused);
		assert(!state->refreshRecoveries);
		assert(state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] ==
		    1);
	}
	assert(sleeps == sleepsBefore);
	assert(munchSdePreparePanel(state) == MUNCH_SDE_OK);
	assert(munchSdeFinishPanel(state, false, true) == MUNCH_SDE_OK && state->panelBlanked);
	assert(munchSdePreparePanel(state) == MUNCH_SDE_OK);
	assert(munchSdeFinishPanel(state, true, true) == MUNCH_SDE_OK && !state->panelBlanked);
	assert(!state->panelCommandFailed && state->presenter.fault == MUNCH_SDE_OK);
	assert(!state->refresh.owned && !state->refresh.paused);
	assert(!state->refreshRecoveries);
	refreshScenario = 87;
	assert(munch_sde_quiesce() == 1 && state->stopped && !state->published);
	assert(state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] == 0x80000001);
	puts(restoreTimeout ? "Safe native keeps autorefresh paused across frames and restores it on wake" :
	    "Safe native keeps autorefresh paused between frames and restores it on wake");
}

#include "check-sde-native.inc"
#include "check-sde-bypass.inc"

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--native")) {
		nativeScenario = static_cast<unsigned>(atoi(argv[2])) + 1;
		reservedPool = true;
		return testBypass(0);
	}
	if (argc == 3 && !strcmp(argv[1], "--reserved")) {
		reservedPool = true;
		return testBypass(static_cast<unsigned>(atoi(argv[2])));
	}
	if (argc == 3 && !strcmp(argv[1], "--bypass")) return testBypass(static_cast<unsigned>(atoi(argv[2])));
	if (argc == 3 && !strcmp(argv[1], "--pacing")) pacingScenario = static_cast<unsigned>(atoi(argv[2]));
	if (argc == 3 && !strcmp(argv[1], "--panel")) panelScenario = static_cast<unsigned>(atoi(argv[2])) + 1;
	assert(argc == 2 || pacingScenario || panelScenario);
	unsigned scenario = pacingScenario || panelScenario ? 99 : static_cast<unsigned>(atoi(argv[1]));
	bool vigRoute = scenario >= 21 && scenario != 70 && scenario != 97 && scenario != 108;
	bool pixelOverride = scenario >= 65 && scenario != 86;
	unsigned stage = scenario == 22 || scenario == 27 || scenario == 28 || scenario == 34 || scenario == 36 ? 1 :
	    scenario == 38 ? 6 : scenario == 37 || scenario == 39 || (scenario >= 45 && scenario <= 49) ||
	    (scenario >= 66 && scenario <= 69) || scenario == 85 || scenario == 87 || scenario == 99 || scenario >= 104 ? 4 : 0;
	uint32_t blendOp = scenario == 36 || stage == 4 ? 0x400 : 0x100;
	uint32_t cscMode = scenario >= 35 ? 2 : 0;
	unsigned blendOffset = stage ? 0x20 + (stage - 1) * 0x18 : 0x20;
	for (unsigned index = 0; index < 11; index++) assert(munch_sde_blend_offsets[index] == 0x20 + index * 0x18);
	bool identityRoute = scenario == 1 || scenario == 31 || scenario == 67 || scenario == 97 || scenario == 108;
	bool timeoutScenario = scenario == 4 || scenario == 29 || scenario == 68;
	if (identityRoute) nextPhysical = 0xa1000000;
	uint32_t *mdp = reinterpret_cast<uint32_t *>(addRegion(MUNCH_MDP_PHYS, MUNCH_MDP_SIZE));
	uint32_t *dsi = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DSI_CTRL_BASE, MUNCH_DSI_CTRL_SIZE));
	uint32_t *gcc = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DISPLAY_GCC_BASE, 0x1000));
	uint32_t *cc = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DISPLAY_CC_BASE, 0x1000));
	uint32_t *power = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DISPLAY_POWER_BASE, 0x1000));
	uint32_t *smmu = reinterpret_cast<uint32_t *>(addRegion(MUNCH_APPS_SMMU_PHYS, MUNCH_APPS_SMMU_WINDOW_SIZE));
	uint64_t *root = reinterpret_cast<uint64_t *>(addRegion(0x9c000000, 0x4000));
	uint8_t *boot = addRegion(0x9c400000, (4320 * 2400 + 0x3fff) & ~0x3fff);
	gcc[0xc / 4] = cc[0x80 / 4] = 1;
	power[0] = 1u << 31;
	dsi[MUNCH_DSI_CTRL / 4] = 0x1f5;
	if (scenario >= 9 && scenario <= 11) {
		unsigned checks = scenario - 8;
		dsi[MUNCH_DSI_CTRL / 4] |= ((checks & 1) << 20) | ((checks >> 1) << 24);
	}
	mdp[0] = 0x60000000;
	mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_MUX) / 4] = 0;
	mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_ENABLE) / 4] = 1;
	mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] = 1;
	uint32_t *ctl = mdp + munch_sde_ctl_offsets[vigRoute ? 0 : 2] / 4;
	ctl[MUNCH_SDE_CTL_TOP / 4] = MUNCH_SDE_CTL_CMD_MODE;
	ctl[MUNCH_SDE_CTL_INTF_ACTIVE / 4] = MUNCH_SDE_INTF1_ACTIVE;
	ctl[MUNCH_SDE_CTL_DSC_ACTIVE / 4] = 1;
	ctl[munch_sde_ctl_layer_offsets[0][vigRoute ? 0 : 3] / 4] = vigRoute ? 0x01000001 + stage : 1u << 18;
	uint32_t *pipe = mdp + munch_sde_sspps[vigRoute ? 0 : 4].offset / 4;
	pipe[MUNCH_SDE_SSPP_SRC_SIZE / 4] = pipe[MUNCH_SDE_SSPP_OUT_SIZE / 4] = (2400u << 16) | 1080;
	uint32_t *mixer = mdp + (MUNCH_SDE_LM_BASE + (vigRoute ? 0 : 3) * MUNCH_SDE_LM_STRIDE) / 4;
	mixer[MUNCH_SDE_LM_OUT_SIZE / 4] = pipe[0];
	pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] = 0x9c400000;
	pipe[MUNCH_SDE_SSPP_SRC_YSTRIDE / 4] = 4320;
	pipe[MUNCH_SDE_SSPP_SRC_FORMAT / 4] = vigRoute ? 0x237ff : 0x236ff;
	pipe[MUNCH_SDE_SSPP_SRC_UNPACK / 4] = 0x03020001;
	if (pixelOverride) {
		pipe[0x38 / 4] = 0x80000000;
		for (unsigned component = 0; component < 3; component++)
			pipe[(0x108 + component * 0x10) / 4] = 0x09600438;
	}
	if (vigRoute) {
		pipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4] = 0x4400;
		pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] = cscMode;
		mixer[MUNCH_SDE_LM_OP_MODE / 4] = 1u << stage;
		mixer[MUNCH_SDE_LM_BLEND0_OP / 4] = 0x100;
		mixer[MUNCH_SDE_LM_BLEND0_ALPHA / 4] = 0x00ff0000;
		mixer[blendOffset / 4] = blendOp;
		mixer[(blendOffset + 4) / 4] = 0x00ff0000;
	}
	smmu[MUNCH_SMMU_IDR0 / 4] = (1u << 27) | 4;
	smmu[MUNCH_SMMU_IDR1 / 4] = 4;
	smmu[MUNCH_SMMU_SMR(0) / 4] = MUNCH_SMMU_SMR_VALID | (0x402u << 16) | 0x820;
	smmu[MUNCH_SMMU_S2CR(0) / 4] = 1;
	uint32_t *gr1 = smmu + 0x1000 / 4, *cb = smmu + 0x3000 / 4;
	gr1[MUNCH_SMMU_CBAR(1) / 4] = 1u << 16;
	gr1[MUNCH_SMMU_CBA2R(1) / 4] = 1;
	cb[MUNCH_SMMU_CB_SCTLR / 4] = 1;
	cb[MUNCH_SMMU_CB_TCR / 4] = 32;
	cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0x9c000000;
	cb[MUNCH_SMMU_CB_MAIR0 / 4] = 0x44;
	root[2] = 0x80000000ULL | (1u << 10) | (1u << 6) | 1;
	uint64_t original[512];
	memcpy(original, root, sizeof(original));
	if (scenario == 2) mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] = 1u << 31;
	if (scenario == 3) cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0x88000000;
	if (scenario == 6) cb[0x7f4 / 4] = 1;
	if (scenario == 8) gcc[0xc / 4] = 0;
	if (scenario == 15) dsi[MUNCH_DSI_CTRL / 4] = 5;
	const char *routeFailure = nullptr;
	if (scenario == 16) { ctl[MUNCH_SDE_CTL_TOP / 4] |= 1; routeFailure = "ctl-mode"; }
	if (scenario == 17) { ctl[munch_sde_ctl_layer_offsets[0][3] / 4] = 2u << 18; routeFailure = "border-stage"; }
	if (scenario == 18) { ctl[MUNCH_SDE_CTL_INTF_ACTIVE / 4] = 0; routeFailure = "no-intf1-ctl"; }
	if (scenario == 19) {
		mdp[(munch_sde_ctl_offsets[5] + MUNCH_SDE_CTL_INTF_ACTIVE) / 4] = 2;
		routeFailure = "multiple-ctls";
	}
	if (scenario == 20) { ctl[MUNCH_SDE_CTL_MERGE_ACTIVE / 4] = 1; routeFailure = "ctl-aux-blocks"; }
	if (scenario == 23) { pipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4] |= 1; routeFailure = "vig-scaler"; }
	if (scenario == 24) { pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] = 1; routeFailure = "vig-csc"; }
	if (scenario == 25) { pipe[MUNCH_SDE_SSPP_PRE_DOWNSCALE / 4] = 1; routeFailure = "vig-resample"; }
	if (scenario == 26) { pipe[MUNCH_SDE_SSPP_DECIMATION / 4] = 1; routeFailure = "vig-resample"; }
	if (scenario == 27) { mixer[MUNCH_SDE_LM_BLEND0_ALPHA / 4] |= 1; routeFailure = "mixer-blend"; }
	if (scenario == 32) { mixer[MUNCH_SDE_LM_OP_MODE / 4] |= 1u << 31; routeFailure = "mixer-mode"; }
	if (scenario == 33) { pipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4] |= 1u << 8; routeFailure = "vig-scaler"; }
	if (scenario == 34) { mixer[MUNCH_SDE_LM_BLEND0_OP / 4] |= 1u << 16; routeFailure = "mixer-blend"; }
	if (scenario == 39) { mixer[blendOffset / 4] = 0; routeFailure = "mixer-blend"; }
	if (scenario == 40) { mixer[0x110 / 4] = 1u << 16; routeFailure = "mixer-dim-layer"; }
	if (scenario == 41) { pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] = 3; routeFailure = "vig-csc"; }
	if (scenario == 42) { mdp[0] = 0; routeFailure = "revision"; }
	if (scenario == 43) { ctl[MUNCH_SDE_CTL_TOP / 4] = 0x21f00; routeFailure = "ctl-mode"; }
	if (scenario == 44) { pipe[MUNCH_SDE_SSPP_SRC_FORMAT / 4] = 0x23ff7; routeFailure = "pipe-format"; }
	if (scenario == 48) { mixer[(blendOffset + 4) / 4] |= 1; routeFailure = "mixer-blend"; }
	if (scenario == 49) { mixer[blendOffset / 4] |= 2; routeFailure = "mixer-blend"; }
	if (scenario >= 50 && scenario < 65) {
		uint32_t *fields[] = { &pipe[0], &mixer[1], &pipe[0xc / 4], &pipe[0x8 / 4], &pipe[0x10 / 4],
		    &pipe[0x38 / 4], &pipe[0x70 / 4], &pipe[0x170 / 4], &pipe[0x24 / 4], &pipe[0x24 / 4],
		    &pipe[0x24 / 4], &pipe[0x14 / 4], &pipe[0x14 / 4], &pipe[0x30 / 4], &pipe[0x34 / 4] };
		const uint32_t values[] = { 0x09600439, 0x09600439, 0x09600439, 1, 1, 1, 1, 1,
		    4316, 0x10000, 4321, 0x9c400004, 0x10000, 0x23ff7, 0x03020100 };
		const char *names[] = { "pipe-size", "pipe-mixer-size", "pipe-out-size", "pipe-src-xy", "pipe-out-xy",
		    "pipe-opmode", "pipe-secure", "pipe-multirect", "pipe-stride-min", "pipe-stride-max",
		    "pipe-stride-align", "pipe-source-align", "pipe-source-min", "pipe-format", "pipe-unpack" };
		*fields[scenario - 50] = values[scenario - 50];
		routeFailure = names[scenario - 50];
	}
	if (scenario >= 71 && scenario <= 79) {
		unsigned component = (scenario - 71) / 3, field = (scenario - 71) % 3;
		const char *names[] = { "pipe-pe-c0-lr", "pipe-pe-c0-tb", "pipe-pe-c0-size",
		    "pipe-pe-c1c2-lr", "pipe-pe-c1c2-tb", "pipe-pe-c1c2-size",
		    "pipe-pe-c3-lr", "pipe-pe-c3-tb", "pipe-pe-c3-size" };
		pipe[(0x100 + component * 0x10) / 4 + field] ^= 1;
		routeFailure = names[scenario - 71];
	}
	if (scenario >= 80 && scenario <= 83) {
		const unsigned bits[] = { 0, 13, 14, 30 };
		pipe[0x38 / 4] |= 1u << bits[scenario - 80];
		routeFailure = "pipe-opmode";
	}
	if (scenario == 84) {
		for (unsigned component = 0; component < 3; component++) pipe[(0x108 + component * 0x10) / 4] = 0;
		routeFailure = "pipe-pe-c0-size";
	}
	if (scenario >= 87) {
		refreshScenario = scenario;
		mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] = 0x80000001;
		mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4] = 0x00181234;
		mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] = 1200;
		if (scenario == 88 || scenario == 96) dsi[MUNCH_DSI_STATUS / 4] = 4;
		if (scenario == 89) mdp[MUNCH_SDE_DEBUG_STATUS / 4] = 0x80;
		if (scenario == 104) {
			mdp[MUNCH_SDE_DEBUG_CTRL / 4] = 0x123;
			mdp[MUNCH_SDE_DEBUG_PERIPH / 4] = 0x456;
		}
	}
	if (scenario >= 105) {
		cb[MUNCH_SMMU_CB_FSR / 4] = scenario < 108 ? (scenario - 104) << 9 : 0x600;
		cb[MUNCH_SMMU_CB_FAR / 4] = 0xdead0000;
		cb[(MUNCH_SMMU_CB_FAR + 4) / 4] = 1;
		cb[MUNCH_SMMU_CB_FSYNR0 / 4] = 0x10;
		cb[MUNCH_SMMU_CB_FSYNR1 / 4] = 0x820;
		if (scenario >= 109 && scenario <= 118) {
			const unsigned faultBits[] = { 1, 2, 3, 4, 5, 6, 7, 8, 30, 31 };
			cb[MUNCH_SMMU_CB_FSR / 4] |= 1u << faultBits[scenario - 109];
		}
		if (scenario == 122) root[2] = 0;
		if (scenario == 123) gr1[MUNCH_SMMU_CBA2R(1) / 4] = 0;
		if (scenario == 124) cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0x88000000;
		if (scenario == 125) root[2] = 0x40000000ULL | (1u << 10) | (1u << 6) | 1;
		if (scenario == 126) cb[MUNCH_SMMU_CB_TCR / 4] = 33;
		if (scenario == 127) gr1[MUNCH_SMMU_CBAR(1) / 4] = 3u << 16;
	}
	uint32_t originalFsr = cb[MUNCH_SMMU_CB_FSR / 4];
	uint32_t originalSource = pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4];
	MunchSDE state = {};
	gMunchSDEOwner = &state;
	auto result = munchSdeInitialize(&state, boot, 4320, scenario == 7 ? 1081 : 1080, 2400);
	if (scenario == 128) {
		assert(result == MUNCH_SDE_OK && state.presenter.armed);
		auto presenter = state.presenter;
		unsigned ctl = munch_sde_ctl_offsets[presenter.route.ctl];
		uint32_t oldStatus = mdp[MUNCH_SDE_INTR_STATUS / 4];
		uint32_t oldStart = mdp[(ctl + MUNCH_SDE_CTL_START) / 4];
		uint32_t oldFlush = mdp[(ctl + MUNCH_SDE_CTL_FLUSH) / 4];
		mdp[MUNCH_SDE_INTR_STATUS / 4] = 0;
		mdp[(ctl + MUNCH_SDE_CTL_START) / 4] = 1;
		mdp[(ctl + MUNCH_SDE_CTL_FLUSH) / 4] = 1;
		presenter.pending = 1;
		presenter.drawing = 3;
		presenter.started = true;
		presenter.submitted = 1;
		presenter.completed = 0;
		presenter.deadline_us = 50000;
		assert(munch_sde_poll_fast_hint(&presenter, 1, true) == MUNCH_SDE_OK);
		assert(presenter.pending == 3 && presenter.front == 1 && presenter.completed == 1);
		mdp[MUNCH_SDE_INTR_STATUS / 4] = oldStatus;
		mdp[(ctl + MUNCH_SDE_CTL_START) / 4] = oldStart;
		mdp[(ctl + MUNCH_SDE_CTL_FLUSH) / 4] = oldFlush;
		assert(munch_sde_quiesce() == 1 && !state.published);
		puts("PP_DONE IRQ hint completes after W1C status acknowledgement");
		return 0;
	}
	if ((scenario >= 109 && scenario <= 118) || scenario >= 122) {
		const char *check = "fsr-fault";
		auto expected = MUNCH_SDE_FAULT;
		if (scenario >= 122) {
			const char *checks[] = { "pte-type", "cba2r-format", "pte-unreadable", "boot-physical", "input-size", "cbar-stage" };
			check = checks[scenario - 122];
			expected = scenario == 123 || scenario >= 126 ? MUNCH_SDE_DOMAIN : MUNCH_SDE_MAPPING;
		}
		assert(result == expected && !strcmp(state.initPhase, "boot-translation"));
		assert(state.domainSnapshotValid && !strcmp(state.bootWalk.check, check));
		assert(!state.bootVerifiedPages && !state.published && !state.presenter.armed && !state.refresh.owned);
		assert(state.observedDomain.fsr == originalFsr && cb[MUNCH_SMMU_CB_FSR / 4] == originalFsr);
		assert(state.faultAddress == 0x1dead0000ULL && state.fsynr0 == 0x10 && state.fsynr1 == 0x820);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource && !refreshWrites && !allocations);
		if (scenario <= 118) assert(!state.ramCount && !state.bootWalk.read && state.bootWalk.level == 4);
		if (scenario == 124) assert(!strcmp(state.tableAccess, "outside-display-reserved"));
		if (scenario == 125) assert(state.bootWalk.physical == 0x5c400000 && state.expectedPhysical == 0x9c400000);
		munchSdeReleaseUnused(&state);
		assert(!state.ramCount && !allocations && !lockDepth && munch_sde_quiesce() == 1);
		printf("SDE XNU runtime scenario %u passed: %s, no writes or fault acknowledgement\n", scenario, check);
		return 0;
	}
	if (scenario >= 87) {
		assert(result == MUNCH_SDE_OK && state.presenter.armed && state.published == !identityRoute);
		assert(!state.refresh.owned && !refreshWrites && !state.presenter.submitted);
		assert(state.domainSnapshotValid && state.observedDomain.fsr == originalFsr);
		assert(!strcmp(state.bootWalk.check, "ok") && state.bootVerifiedPages == state.buffers[0].length / 4096);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource);
		state.presenter.io.write = refreshWrite;
		uint32_t *autoRegister = mdp + (MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4;
		assert(*autoRegister == 0x80000001);
		if (scenario == 98) {
			assert(munch_sde_quiesce() == 1 && !state.published && !refreshWrites);
			assert(*autoRegister == 0x80000001 && !memcmp(original, root, sizeof(original)));
			puts("Autorefresh untouched without a compositor frame");
			return 0;
		}
		if (scenario == 94) {
			*autoRegister ^= 2;
			uint8_t *back = nullptr;
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_STALE && !back);
			assert(!refreshWrites && munch_sde_quiesce() == 0 && state.published);
			puts("Autorefresh raw-state drift rejected before any control write");
			return 0;
		}
		for (unsigned frame = 0; frame < (scenario == 99 ? 3u : 1u); frame++) {
			if (panelScenario == 4 && !frame) {
				assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK);
				assert(munchSdeFinishPanel(&state, false, true) == MUNCH_SDE_OK);
				assert(!state.presenter.submitted && pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource);
				assert(munch_sde_quiesce() == 0);
				assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK);
				assert(munchSdeFinishPanel(&state, true, true) == MUNCH_SDE_OK);
			}
			uint8_t *back = nullptr;
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_OK && back != boot);
			memset(back, frame + 1, 4320 * 2400);
			if (scenario == 100) transferEnabled = false;
			uint64_t guardsBefore = state.guardCalls, pollsBefore = state.pollChecks;
			transferTicks = 0;
			result = munchSdeEndFrame(&state);
			if (pacingScenario > 1) {
				auto expected = pacingScenario == 3 ? MUNCH_SDE_STALE : pacingScenario == 4 ? MUNCH_SDE_TIMEOUT : MUNCH_SDE_FAULT;
				assert(result == expected && state.presenter.pending == 1 && !state.presenter.completed);
				assert(!state.completedTime && !lockDepth && state.published && allocations == 3);
				assert(state.guardCalls - guardsBefore <= 12);
				puts("Fast poll rejects fault/drift/timeout without acknowledging or recycling pending memory");
				return 0;
			}
			if (pacingScenario) {
				assert(state.pollChecks - pollsBefore == 13);
				assert(state.guardCalls - guardsBefore <= 12u);
				assert(transferTicks == 12 && !lockDepth);
			}
			if ((scenario >= 88 && scenario <= 92) || scenario == 95 || scenario == 102 || scenario == 103) {
				assert(result != MUNCH_SDE_OK && !state.refresh.owned && !state.refresh.paused);
				assert(!state.presenter.submitted && !state.presenter.completed && !state.completedTime);
				assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource && !ctl[MUNCH_SDE_CTL_START / 4]);
				assert(*autoRegister == 0x80000001 && mdp[(MUNCH_SDE_INTF1_OFFSET +
				    MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4] == 0x00181234);
				int quiesced = munch_sde_quiesce();
				assert((quiesced == 1 && !state.published) || (quiesced == 0 && state.published));
				munchSdeReleaseUnused(&state);
				assert(allocations == 3 && !lockDepth);
				printf("SDE XNU runtime scenario %u passed: pause refused, boot source and mappings retained\n", scenario);
				return 0;
			}
			if (scenario == 100) {
				assert(result == MUNCH_SDE_TIMEOUT && state.presenter.pending == 1 && !state.presenter.completed);
				assert(state.refresh.paused && munch_sde_quiesce() == 0 && state.published && allocations == 3);
				puts("Autorefresh paused, stalled compositor DMA remains pinned");
				return 0;
			}
			if (scenario == 93) {
				assert(result == MUNCH_SDE_TIMEOUT && state.presenter.pending == 3 && state.presenter.completed == frame + 1);
				assert(state.refresh.owned && state.refresh.paused && state.presenter.fault == MUNCH_SDE_TIMEOUT);
				assert(munch_sde_quiesce() == 0 && state.published && allocations == 3);
				puts("Autorefresh restoration timeout retains the completed frame and refuses handoff");
				return 0;
			}
			assert(result == MUNCH_SDE_OK && state.presenter.completed == frame + 1);
			assert(*autoRegister == 0x80000001 && !state.refresh.owned && !state.refresh.paused && !boot[0]);
		}
		if (scenario == 99) {
			uint8_t *back = nullptr;
			uint64_t completed = state.presenter.completed;
			assert(munchSdeFinishPanel(&state, false, true) == MUNCH_SDE_STALE);
			state.presenter.pending = 1;
			assert(munchSdePreparePanel(&state) == MUNCH_SDE_BUSY && !state.panelCommandActive);
			state.presenter.pending = 3;
			assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK && state.panelCommandActive);
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_DISABLED && !back);
			assert(munch_sde_quiesce() == 0 && state.published);
			if (panelScenario == 3) dsi[MUNCH_DSI_CTRL / 4] ^= 8;
			auto panelResult = munchSdeFinishPanel(&state, false, panelScenario != 2);
			if (panelScenario == 2 || panelScenario == 3) {
				assert(panelResult != MUNCH_SDE_OK && state.panelCommandFailed && !state.panelCommandActive);
				assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_DISABLED && !back);
				assert(munch_sde_quiesce() == 0 && state.published && allocations == 3);
				puts("Failed panel command/state drift pins buffers and refuses unsafe loader return");
				return 0;
			}
			assert(panelResult == MUNCH_SDE_OK && state.panelBlanked && !state.panelCommandActive);
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_DISABLED && !back);
			assert(munch_sde_quiesce() == 0 && state.published);
			assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK);
			assert(munchSdeFinishPanel(&state, true, true) == MUNCH_SDE_OK && !state.panelBlanked);
			assert(state.presenter.completed == completed && state.presenter.pending == 3);
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_OK && back);
			transferTicks = 0;
			assert(munchSdeEndFrame(&state) == MUNCH_SDE_OK && state.presenter.completed == completed + 1);
			puts("Panel blank/resume preserves route and uses fresh hardware completion after wake");
		}
		if (scenario == 101) {
			*autoRegister ^= 2;
			assert(munchSdeGuard(&state) == MUNCH_SDE_STALE && munch_sde_quiesce() == 0 && state.published);
			puts("Owned autorefresh drift refuses unsafe handoff");
			return 0;
		}
		if (scenario == 119 || scenario == 121) {
			cb[MUNCH_SMMU_CB_FSR / 4] = scenario == 119 ? 0x602 : 0x400;
			uint8_t *back = nullptr;
			assert(munchSdeBeginFrame(&state, &back) == (scenario == 119 ? MUNCH_SDE_FAULT : MUNCH_SDE_STALE));
			assert(!back && munch_sde_quiesce() == 0 && state.published && allocations == 3);
			assert(state.observedDomain.fsr == cb[MUNCH_SMMU_CB_FSR / 4]);
			puts("SMMU changed status after completion refuses the next frame and handoff");
			return 0;
		}
		mdp[MUNCH_SDE_INTR_STATUS / 4] = 0;
		unsigned sleepsBeforeReturn = sleeps;
		if (scenario == 120) resumeSeen = false;
		returning = true;
		int returned = munch_sde_quiesce();
		assert(sleeps == sleepsBeforeReturn);
		if (scenario == 120) {
			assert(!returned && state.published && allocations == 3 && !state.stopped);
			assert(cb[MUNCH_SMMU_CB_FSR / 4] == 0x602 && !resumeSeen);
			puts("Real SMMU fault during return prevents mapping removal and refresh restart");
			return 0;
		}
		assert(returned && state.stopped && !state.published && !state.refresh.owned);
		assert(*autoRegister == 0x80000001 && !memcmp(original, root, sizeof(original)));
		assert(mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4] == 0x00181234);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource);
		assert(cb[MUNCH_SMMU_CB_FSR / 4] == originalFsr);
		if (scenario == 104) assert(mdp[MUNCH_SDE_DEBUG_CTRL / 4] == 0x123 && mdp[MUNCH_SDE_DEBUG_PERIPH / 4] == 0x456);
		assert(!lockDepth);
		printf("SDE XNU runtime scenario %u passed: autorefresh drained and restored\n", scenario);
		return 0;
	}
	if (routeFailure) {
		auto expected = (scenario >= 23 && scenario <= 26) || scenario == 33 || scenario == 41 || scenario == 42 ?
		    MUNCH_SDE_UNSUPPORTED : scenario == 44 || scenario >= 50 ? MUNCH_SDE_FORMAT : MUNCH_SDE_ROUTE;
		assert(result == expected && !strcmp(state.initPhase, "inherited-route"));
		assert(state.snapshotValid);
		munch_sde_route route = {};
		munch_sde_route_diagnostic diagnostic = {};
		assert(munch_sde_find_route_checked(&state.baseline, &route, &diagnostic) == result);
		assert(!strcmp(diagnostic.check, routeFailure));
		assert(!state.published && !state.presenter.armed && !state.presenter.submitted);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == originalSource);
		assert(!ctl[MUNCH_SDE_CTL_START / 4] && !ctl[MUNCH_SDE_CTL_FLUSH / 4]);
		munchSdeReleaseUnused(&state);
		assert(!allocations && !lockDepth && !memcmp(original, root, sizeof(original)));
		assert(munch_sde_quiesce() == 1);
		printf("SDE XNU runtime scenario %u passed: %s, no takeover\n", scenario, routeFailure);
		return 0;
	}
	if (scenario == 2 || scenario == 3 || scenario == 7 || scenario == 8 || scenario == 15) {
		const char *expectedPhase = (scenario == 2 || scenario == 15) ? "inherited-route" : scenario == 3 ? "boot-translation" :
		    scenario == 7 ? "boot-geometry" : "display-power";
		assert(strcmp(state.initPhase, expectedPhase) == 0);
		assert(result != MUNCH_SDE_OK && !state.published && !state.presenter.armed);
		if (scenario == 2 || scenario == 15) assert(result == MUNCH_SDE_UNSUPPORTED);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c400000);
		munchSdeReleaseUnused(&state);
		assert(allocations == 0 && !memcmp(original, root, sizeof(original)));
		assert(munch_sde_quiesce() == 1);
	} else if (scenario == 6) {
		assert(strcmp(state.initPhase, "back-buffer-mapping") == 0);
		assert(result == MUNCH_SDE_TIMEOUT && state.published && !state.presenter.armed);
		munchSdeReleaseUnused(&state);
		assert(allocations == 3 && pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c400000);
		cb[0x7f4 / 4] = 0;
		assert(munch_sde_quiesce() == 1);
		assert(!memcmp(original, root, sizeof(original)));
	} else if (scenario >= 12 && scenario <= 14) {
		assert(result == MUNCH_SDE_OK && state.presenter.armed && state.published);
		unsigned changedBit = scenario == 12 ? 4 : scenario == 13 ? 20 : 24;
		dsi[MUNCH_DSI_CTRL / 4] ^= 1u << changedBit;
		uint8_t *back = nullptr;
		assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_STALE && !back);
		assert(!state.presenter.submitted && !state.presenter.completed);
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c400000);
		assert(!ctl[MUNCH_SDE_CTL_START / 4] && !ctl[MUNCH_SDE_CTL_FLUSH / 4]);
		assert(munch_sde_quiesce() == 0 && state.published);
		munchSdeReleaseUnused(&state);
		assert(allocations == 3);
		dsi[MUNCH_DSI_CTRL / 4] ^= 1u << changedBit;
		assert(munch_sde_quiesce() == 1 && !state.published);
		assert(!memcmp(original, root, sizeof(original)));
	} else {
		assert(strcmp(state.initPhase, "ready") == 0);
		assert(result == MUNCH_SDE_OK && state.presenter.armed && state.published == !identityRoute);
		assert(state.route.pingpong == 0 && state.route.done_mask == (1u << 8));
		unsigned pipeIndex = vigRoute ? 0 : 4;
		assert(state.baseline.pipes[pipeIndex].opmode == (pixelOverride ? 0x80000000 : 0));
		for (unsigned component = 0; component < 3; component++) {
			unsigned offset = (0x100 + component * 0x10) / 4;
			assert(state.baseline.pipes[pipeIndex].pixel_ext[component].lr == pipe[offset]);
			assert(state.baseline.pipes[pipeIndex].pixel_ext[component].tb == pipe[offset + 1]);
			assert(state.baseline.pipes[pipeIndex].pixel_ext[component].required == pipe[offset + 2]);
			for (unsigned field = 0; field < 3; field++) {
				pipe[offset + field] ^= 1;
				assert(munchSdeGuard(&state) == MUNCH_SDE_STALE);
				pipe[offset + field] ^= 1;
				assert(munchSdeGuard(&state) == MUNCH_SDE_OK);
			}
		}
		pipe[0x38 / 4] ^= 0x80000000;
		assert(munchSdeGuard(&state) == MUNCH_SDE_STALE);
		pipe[0x38 / 4] ^= 0x80000000;
		assert(munchSdeGuard(&state) == MUNCH_SDE_OK);
		if (vigRoute) {
			assert(state.route.ctl == 0 && state.route.mixer == 0 && state.route.pipe == 0);
			assert(state.route.stage == stage && state.route.flush_mask == 1);
			assert(state.baseline.pipes[0].scaler == 0x4400);
			assert(state.baseline.pipes[0].csc == cscMode);
			unsigned blend = stage ? stage - 1 : 0;
			assert(state.baseline.mixers[0].blend_op[blend] == blendOp && state.baseline.mixers[0].blend_alpha[blend] == 0x00ff0000);
			uint32_t *registers[] = { &pipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4], &pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4],
			    &pipe[MUNCH_SDE_SSPP_PRE_DOWNSCALE / 4], &pipe[MUNCH_SDE_SSPP_DECIMATION / 4],
			    &mixer[MUNCH_SDE_LM_OP_MODE / 4], &mixer[MUNCH_SDE_LM_BLEND0_OP / 4], &mixer[MUNCH_SDE_LM_BLEND0_ALPHA / 4] };
			for (uint32_t *controlRegister : registers) {
				*controlRegister ^= 1;
				assert(munchSdeGuard(&state) == MUNCH_SDE_STALE);
				*controlRegister ^= 1;
				assert(munchSdeGuard(&state) == MUNCH_SDE_OK);
			}
			for (unsigned index = 0; index < 11; index++) {
				unsigned offset = 0x20 + index * 0x18;
				assert(state.baseline.mixers[0].blend_op[index] == mixer[offset / 4]);
				assert(state.baseline.mixers[0].blend_alpha[index] == mixer[(offset + 4) / 4]);
				for (unsigned field = 0; field < 2; field++) {
					mixer[offset / 4 + field] ^= 1;
					assert(munchSdeGuard(&state) == MUNCH_SDE_STALE);
					mixer[offset / 4 + field] ^= 1;
					assert(munchSdeGuard(&state) == MUNCH_SDE_OK);
				}
			}
		}
		assert(state.baseline.dsi_ctrl == dsi[MUNCH_DSI_CTRL / 4]);
		uint32_t control = dsi[MUNCH_DSI_CTRL / 4];
		for (unsigned bit = 0; bit < 32; bit++) {
			dsi[MUNCH_DSI_CTRL / 4] = control ^ (1u << bit);
			assert(munchSdeGuard(&state) == MUNCH_SDE_STALE);
			dsi[MUNCH_DSI_CTRL / 4] = control;
			assert(munchSdeGuard(&state) == MUNCH_SDE_OK);
		}
		assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c400000);
		for (unsigned frame = 0; frame < 3; frame++) {
			uint8_t *back = nullptr;
			mdp[MUNCH_SDE_INTR_STATUS / 4] = 0;
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_OK);
			assert(back && back != boot && back != state.pixels[state.presenter.front]);
			memset(back, frame + 1, 4320 * 2400);
			if (timeoutScenario) transferEnabled = false;
			result = munchSdeEndFrame(&state);
			if (timeoutScenario) {
				assert(result == MUNCH_SDE_TIMEOUT && state.presenter.pending == 1 && state.presenter.completed == 0);
				assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_TIMEOUT);
				munchSdeReleaseUnused(&state);
				assert(allocations == 3 && munch_sde_quiesce() == 0);
				puts("SDE XNU runtime timeout/pinning scenario passed");
				return 0;
			}
			assert(result == MUNCH_SDE_OK && state.presenter.completed == frame + 1);
			assert(dsi[MUNCH_DSI_CTRL / 4] == control);
			assert(boot[0] == 0 && state.completedTime == clockNow);
		}
		mdp[MUNCH_SDE_INTR_STATUS / 4] = 0;
		if (scenario == 28 || (scenario >= 45 && scenario <= 47) || scenario == 69 || scenario == 85 || scenario == 86) {
			uint32_t *changed = scenario == 69 ? &pipe[0x128 / 4] : scenario == 85 ? &pipe[0x38 / 4] :
			    scenario == 86 ? &pipe[0x118 / 4] : scenario == 46 ? &pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] :
			    scenario == 47 ? &mixer[0x110 / 4] : &mixer[(blendOffset + 4) / 4];
			uint32_t mask = scenario == 85 ? 0x80000000 : scenario == 46 ? 2 : 1;
			*changed ^= mask;
			uint8_t *back = nullptr;
			assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_STALE && !back);
			assert(munch_sde_quiesce() == 0 && state.published);
			munchSdeReleaseUnused(&state);
			assert(allocations == 3);
			*changed ^= mask;
		}
		if (scenario == 5 || scenario == 30) {
			cb[MUNCH_SMMU_CB_FSR / 4] = 2;
			assert(munch_sde_quiesce() == 0 && state.published);
			munchSdeReleaseUnused(&state);
			assert(allocations == 3);
		} else {
			assert(munch_sde_quiesce() == 1 && state.stopped && !state.published);
			assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c400000);
			assert(!memcmp(original, root, sizeof(original)));
			assert(pipe[0x38 / 4] == (pixelOverride ? 0x80000000 : 0));
			for (unsigned component = 0; component < 3; component++) {
				unsigned offset = (0x100 + component * 0x10) / 4;
				assert(!pipe[offset] && !pipe[offset + 1]);
				assert(pipe[offset + 2] == (pixelOverride ? 0x09600438 : 0));
			}
			if (vigRoute) {
				assert(pipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4] == 0x4400);
				assert(pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] == cscMode);
				assert(mixer[blendOffset / 4] == blendOp && mixer[(blendOffset + 4) / 4] == 0x00ff0000);
				assert(ctl[munch_sde_ctl_layer_offsets[0][0] / 4] == 0x01000001 + stage);
			}
			assert(munch_sde_quiesce() == 1);
			uint8_t *back;
			assert(munchSdeBeginFrame(&state, &back) != MUNCH_SDE_OK);
		}
	}
	assert(lockDepth == 0);
	printf("SDE XNU runtime scenario %u passed\n", scenario);
	return 0;
}
