static int testBypass(unsigned scenario)
{
	assert(scenario < 58);
	bool tableMode = scenario == 54 || scenario == 55;
	if (nativeScenario) {
		addRegion(0x0af00000, 0x4000);
		addRegion(0x0ae94400, 0x800);
	}
	unsigned expectedAllocations = reservedPool ? 0 : 2;
	uint32_t *mdp = reinterpret_cast<uint32_t *>(addRegion(MUNCH_MDP_PHYS, MUNCH_MDP_SIZE));
	uint32_t *dsi = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DSI_CTRL_BASE, MUNCH_DSI_CTRL_SIZE));
	uint32_t *gcc = reinterpret_cast<uint32_t *>(addRegion(MUNCH_DISPLAY_GCC_BASE, 0x1000));
	uint32_t *cc = reinterpret_cast<uint32_t *>(nativeScenario ? address(MUNCH_DISPLAY_CC_BASE, 0x1000) : addRegion(MUNCH_DISPLAY_CC_BASE, 0x1000));
	uint32_t *power = reinterpret_cast<uint32_t *>(nativeScenario ? address(MUNCH_DISPLAY_POWER_BASE, 0x1000) : addRegion(MUNCH_DISPLAY_POWER_BASE, 0x1000));
	uint32_t *smmu = reinterpret_cast<uint32_t *>(addRegion(MUNCH_APPS_SMMU_PHYS, MUNCH_APPS_SMMU_WINDOW_SIZE));
	uint8_t *boot = addRegion(0x9c000000, 0x9e4000);
	memset(boot, 0x7c, 0x9e4000);
	gcc[0xc / 4] = cc[0x80 / 4] = 1;
	power[0] = 1u << 31;
	dsi[MUNCH_DSI_CTRL / 4] = 0x1f5;
	mdp[0] = 0x60000000;
	uint32_t *intf = mdp + MUNCH_SDE_INTF1_OFFSET / 4;
	intf[MUNCH_SDE_INTF_TEAR_ENABLE / 4] = 1;
	intf[MUNCH_SDE_INTF_TEAR_AUTOREFRESH / 4] = 0x80000001;
	intf[MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4] = 0x00180885;
	intf[MUNCH_SDE_INTF_TEAR_LINE / 4] = 0;
	uint32_t *ctl = mdp + munch_sde_ctl_offsets[0] / 4;
	ctl[MUNCH_SDE_CTL_TOP / 4] = 0x20000;
	ctl[MUNCH_SDE_CTL_INTF_ACTIVE / 4] = 2;
	ctl[MUNCH_SDE_CTL_DSC_ACTIVE / 4] = 1;
	ctl[munch_sde_ctl_layer_offsets[0][0] / 4] = 0x01000005;
	uint32_t *pipe = mdp + munch_sde_sspps[0].offset / 4;
	pipe[MUNCH_SDE_SSPP_SRC_SIZE / 4] = pipe[MUNCH_SDE_SSPP_OUT_SIZE / 4] = 0x09600438;
	pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] = 0x9c000000;
	pipe[MUNCH_SDE_SSPP_SRC_YSTRIDE / 4] = 0x10e0;
	pipe[MUNCH_SDE_SSPP_SRC_FORMAT / 4] = 0x237ff;
	pipe[MUNCH_SDE_SSPP_SRC_UNPACK / 4] = 0x03020001;
	pipe[0x38 / 4] = 0x80000000;
	pipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4] = 2;
	for (unsigned component = 0; component < 3; component++) pipe[(0x108 + component * 0x10) / 4] = 0x09600438;
	uint32_t *mixer = mdp + MUNCH_SDE_LM_BASE / 4;
	mixer[MUNCH_SDE_LM_OUT_SIZE / 4] = 0x09600438;
	for (unsigned stage = 0; stage < 6; stage++) {
		mixer[(0x20 + stage * 0x18) / 4] = 0x400;
		mixer[(0x24 + stage * 0x18) / 4] = 0x00ff0000;
	}
	smmu[MUNCH_SMMU_SCR0 / 4] = 0x00240406;
	smmu[MUNCH_SMMU_IDR0 / 4] = 0x4c017e57;
	smmu[MUNCH_SMMU_IDR1 / 4] = 0x6000004a;
	smmu[MUNCH_SMMU_SMR(4) / 4] = 0x84020820;
	smmu[MUNCH_SMMU_S2CR(4) / 4] = 4;
	uint32_t *gr1 = smmu + 0x1000 / 4, *cb = smmu + 0x84000 / 4;
	gr1[MUNCH_SMMU_CBAR(4) / 4] = 0x1f000;
	gr1[MUNCH_SMMU_CBA2R(4) / 4] = 1;
	cb[MUNCH_SMMU_CB_SCTLR / 4] = 0xe0;
	cb[MUNCH_SMMU_CB_TCR2 / 4] = 0x30071;
	cb[MUNCH_SMMU_CB_FSR / 4] = 0x400;
	cb[MUNCH_SMMU_CB_FAR / 4] = 0xb724caac;
	cb[(MUNCH_SMMU_CB_FAR + 4) / 4] = 0x1201e;
	cb[MUNCH_SMMU_CB_FSYNR0 / 4] = 0x70d2e;
	cb[MUNCH_SMMU_CB_FSYNR1 / 4] = 0xf9;
	uint64_t *root = nullptr;
	if (tableMode) {
		root = reinterpret_cast<uint64_t *>(addRegion(0x9d800000, 0x4000));
		root[2] = 0x80000000ULL | (1u << 10) | (1u << 6) | 1;
		cb[MUNCH_SMMU_CB_SCTLR / 4] |= 1;
		cb[MUNCH_SMMU_CB_TCR / 4] = 32;
		cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0x9d800000;
		cb[MUNCH_SMMU_CB_MAIR0 / 4] = 0x44;
		if (scenario == 55) cb[0x7f4 / 4] = 1;
	}
	if (scenario >= 2 && scenario <= 11) {
		const unsigned faultBits[] = { 1, 2, 3, 4, 5, 6, 7, 8, 30, 31 };
		cb[MUNCH_SMMU_CB_FSR / 4] |= 1u << faultBits[scenario - 2];
	}
	if (scenario == 12) gr1[MUNCH_SMMU_CBAR(4) / 4] = 0x3f000;
	if (scenario == 13) smmu[MUNCH_SMMU_S2CR(4) / 4] |= 1u << 16;
	if (scenario == 14) {
		smmu[MUNCH_SMMU_SMR(5) / 4] = 0x80001234;
		smmu[MUNCH_SMMU_S2CR(5) / 4] = 4;
	}
	if (scenario == 15) smmu[MUNCH_SMMU_SCR0 / 4] |= 1;
	if (scenario == 16) pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] += 0x4000;
	if (scenario == 17) nextPhysical = 1ULL << 32;
	if (scenario == 18) shortSegment = true;
	if (scenario == 19) nextPhysical = 0x9c000000;
	if (scenario == 20) overlappingAllocation = true;
	if (scenario == 29 || scenario == 38) {
		cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0xdead0000;
		cb[MUNCH_SMMU_CB_TCR / 4] = 32;
		cb[MUNCH_SMMU_CB_MAIR0 / 4] = 0x44;
		if (scenario == 38) overlappingAllocation = true;
	}
	if (scenario == 30) cb[MUNCH_SMMU_CB_SCTLR / 4] |= 1u << 20;
	if (scenario == 31) brokenBoot = true;
	if (scenario == 32) nextPhysical = 0xffffc000;
	if (scenario == 33) smmu[MUNCH_SMMU_SMR(4) / 4] &= ~MUNCH_SMMU_SMR_VALID;
	if (scenario == 34) {
		smmu[MUNCH_SMMU_SMR(5) / 4] = smmu[MUNCH_SMMU_SMR(4) / 4];
		smmu[MUNCH_SMMU_S2CR(5) / 4] = 5;
	}
	if (scenario == 35) gr1[MUNCH_SMMU_CBA2R(4) / 4] = 0;
	uint8_t *pool = nullptr;
	if (reservedPool) {
		failVmAllocation = true;
		poolPresent = true;
		poolRegion[0] = 0xf9000000;
		poolRegion[1] = MUNCH_SDE_POOL_SIZE;
		pool = addRegion(poolRegion[0], poolRegion[1]);
		memset(pool, 0x5a, poolRegion[1]);
		testBootArgs = { 0xfffffff004000000ULL, 0xf4000000, 0x180000000, 0x194000000 };
		PE_state.bootArgs = &testBootArgs;
		protectedRegions[0][0] = 0xf8000000;
		protectedRegions[0][1] = 0x10000;
		protectedRegions[1][0] = 0xfc000000;
		protectedRegions[1][1] = 0x93900000;
		protectedRegions[2][0] = poolRegion[0] + poolRegion[1];
		protectedRegions[2][1] = 0x400000;
		if (scenario == 39) poolPropertySize = 8;
		if (scenario == 40) poolRegion[0] += 4096;
		if (scenario == 41) poolRegion[1] -= 0x4000;
		if (scenario == 42) poolRegion[0] = 1ULL << 32;
		if (scenario == 43) poolRegion[0] = 0xf7000000;
		if (scenario == 44) testBootArgs.topOfKernelData = poolRegion[0] + poolRegion[1] - 0x4000;
		if (scenario >= 45 && scenario <= 47) protectedRegions[scenario - 45][0] = poolRegion[0];
		if (scenario == 48) poolRegion[0] = 0xfa800000;
		if (scenario == 49) brokenPool = true;
		if (scenario == 50) poolPresent = false;
		if (scenario == 51) PE_state.bootArgs = nullptr;
		if (scenario == 52) testBootArgs.memSize = UINT64_MAX;
		if (scenario == 53) poolRegion[1] = 0;
		if (scenario == 56) {
			testBootArgs.physBase = 0x98000000;
			poolRegion[0] = 0x9c000000;
		}
		if (scenario == 57) { protectedRegions[0][0] = UINT64_MAX - 0x3fff; protectedRegions[0][1] = 0x4000; }
	}
	std::vector<uint32_t> savedSmmu(smmu, smmu + MUNCH_APPS_SMMU_WINDOW_SIZE / 4);
	std::vector<uint32_t> savedMdp(mdp, mdp + MUNCH_MDP_SIZE / 4);
	std::vector<uint8_t> savedBoot(boot, boot + 0x9e4000);
	if (!tableMode) assert(!mprotect(smmu, MUNCH_APPS_SMMU_WINDOW_SIZE, PROT_READ));
	MunchSDE state = {};
	state.nativeRequested = nativeScenario != 0;
	state.nativeTakeover = nativeScenario != 0 && nativeScenario < 4;
	gMunchSDEOwner = &state;
	refreshScenario = 87;
	auto result = munchSdeInitialize(&state, boot, 4320, 1080, 2400);
	assert(!memcmp(savedSmmu.data(), smmu, MUNCH_APPS_SMMU_WINDOW_SIZE));
	if (scenario == 55) savedMdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] =
	    mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4];
	assert(!memcmp(savedMdp.data(), mdp, MUNCH_MDP_SIZE));
	assert(!state.tables);
	if (!tableMode) assert(!state.published && !state.rootEntry && !state.ramCount && !physicalMappings);
	if (reservedPool) assert(vmRequests == (scenario == 50 ? 1u : 0u));
	assert(!state.presenter.submitted && !state.presenter.completed && !state.refresh.owned);
	if (nativeScenario) {
		assert(result == MUNCH_SDE_OK);
		if (nativeScenario >= 4) testSafeNativeFrames(&state);
		else testNativeFrames(&state);
		assert(!memcmp(savedSmmu.data(), smmu, MUNCH_APPS_SMMU_WINDOW_SIZE));
		assert(!memcmp(savedBoot.data(), boot, 0x9e4000));
		assert(!allocations && !physicalMappings && !state.published);
		printf("Native XNU integration scenario %u passed: real runtime, immutable bypass SMMU and boot pixels\n", nativeScenario);
		return 0;
	}
	if (scenario == 55) {
		assert(result == MUNCH_SDE_TIMEOUT && state.published && !state.presenter.armed);
		assert(state.tableBytes == reinterpret_cast<uint64_t *>(pool + 2 * MUNCH_SDE_POOL_SLOT_SIZE));
		munchSdeReleaseUnused(&state);
		assert(state.published && root[0] && !allocations);
		cb[0x7f4 / 4] = 0;
		assert(munch_sde_quiesce() == 1 && !state.published && !root[0]);
		puts("Reserved table workspace: failed TLBI retains ownership until safe return");
		return 0;
	}
	if ((scenario >= 2 && scenario <= 20) || (scenario >= 30 && scenario <= 35) || scenario == 38 ||
	    (scenario >= 39 && !tableMode)) {
		auto expected = scenario <= 11 ? MUNCH_SDE_FAULT :
		    (scenario >= 16 && scenario <= 20) || scenario == 31 || scenario == 32 || scenario == 38 || scenario >= 39 ?
		    MUNCH_SDE_MAPPING : MUNCH_SDE_DOMAIN;
		assert(result == expected && !state.presenter.armed);
		if (scenario >= 2 && scenario <= 11) assert(!strcmp(state.bootWalk.check, "fsr-fault"));
		if (scenario == 16) assert(!strcmp(state.bootWalk.check, "boot-physical"));
		munchSdeReleaseUnused(&state);
		assert(!allocations && munch_sde_quiesce() == 1);
		if (pool) for (unsigned offset = 0; offset < MUNCH_SDE_POOL_SIZE; offset++) assert(pool[offset] == 0x5a);
	} else {
		assert(result == MUNCH_SDE_OK && state.presenter.armed && allocations == expectedAllocations);
		assert(state.contextPhysical == 0x15084000 && state.domain.stream == 4 && state.domain.context == 4);
		assert(!strcmp(state.bootWalk.check, tableMode ? "ok" : "stage1-bypass") && state.bootWalk.read == tableMode);
		assert(state.bootVerifiedPages == 0x9e4 && state.bootWalk.physical == 0x9c9e3000);
		if (!tableMode) for (unsigned index = 0; index < 3; index++) assert(state.buffers[index].physical == state.buffers[index].iova);
		if (pool) {
			assert(state.poolBytes == pool && !strcmp(state.poolCheck, "ready") && !strcmp(state.bufferCheck, "ready"));
			assert(state.pixels[1] == pool && state.pixels[2] == pool + MUNCH_SDE_POOL_SLOT_SIZE);
		}
		state.presenter.io.write = refreshWrite;
		if (scenario == 1) {
			assert(munch_sde_quiesce() == 1 && state.stopped && !refreshWrites);
			assert(!memcmp(savedMdp.data(), mdp, MUNCH_MDP_SIZE));
		} else {
			if (scenario == 27) transferEnabled = false;
			if (scenario == 28) mdp[MUNCH_SDE_DEBUG_STATUS / 4] = 0x80;
			bool refused = false;
			for (unsigned frame = 0; frame < 4; frame++) {
				uint8_t *back = nullptr;
				assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_OK && back != boot);
				memset(back, frame + 1, 4320 * 2400);
				if (scenario == 21 || (frame == 1 && (scenario == 22 || scenario == 23 || scenario == 24 || scenario == 36))) {
					assert(!mprotect(smmu, MUNCH_APPS_SMMU_WINDOW_SIZE, PROT_READ | PROT_WRITE));
					if (scenario == 21) cb[MUNCH_SMMU_CB_SCTLR / 4] |= 1;
					if (scenario == 22) cb[MUNCH_SMMU_CB_TTBR0 / 4] = 0x88000000;
					if (scenario == 23) smmu[MUNCH_SMMU_S2CR(4) / 4] = 5;
					if (scenario == 24) cb[MUNCH_SMMU_CB_FSR / 4] |= 2;
					if (scenario == 36) cb[MUNCH_SMMU_CB_FSR / 4] = 0x600;
					assert(!mprotect(smmu, MUNCH_APPS_SMMU_WINDOW_SIZE, PROT_READ));
				}
				result = munchSdeEndFrame(&state);
				if (result != MUNCH_SDE_OK) {
					auto expected = scenario == 27 ? MUNCH_SDE_TIMEOUT :
					    scenario == 28 ? MUNCH_SDE_BUSY :
					    scenario == 24 ? MUNCH_SDE_FAULT : MUNCH_SDE_STALE;
					assert(result == expected && state.presenter.completed == frame);
					assert((scenario >= 21 && scenario <= 24) || scenario == 27 || scenario == 28 || scenario == 36);
					if (scenario == 28) {
						assert(state.presenter.pending == 3 && state.presenter.drawing == 3 &&
						    state.presenter.submitted == 0);
						assert(munch_sde_quiesce() == 1 && state.stopped);
						refused = true;
						break;
					}
					assert(munch_sde_quiesce() == 0 && !state.stopped);
					if (scenario == 21 || scenario == 28) assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c000000);
					refused = true;
					break;
				}
				assert(state.presenter.completed == frame + 1 && state.completedTime &&
				    !state.refresh.owned && !state.refresh.paused);
				assert(state.presenter.front == (frame % 2) + 1);
				assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == state.buffers[state.presenter.front].iova);
				if (scenario == 0 && frame == 1) {
					assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK);
					assert(munchSdeFinishPanel(&state, false, true) == MUNCH_SDE_OK && state.panelBlanked);
					back = nullptr;
					assert(munchSdeBeginFrame(&state, &back) == MUNCH_SDE_DISABLED && !back);
					assert(munch_sde_quiesce() == 0 && !state.stopped);
					assert(munchSdePreparePanel(&state) == MUNCH_SDE_OK);
					assert(munchSdeFinishPanel(&state, true, true) == MUNCH_SDE_OK && !state.panelBlanked);
					assert(state.presenter.completed == frame + 1);
				}
			}
			if (!refused) {
				if (scenario == 25 || scenario == 26) returnMutation = scenario - 24;
				if (scenario == 37) refreshScenario = 93;
				returning = true;
				mdp[MUNCH_SDE_INTR_STATUS / 4] = 0;
				unsigned savedSleeps = sleeps;
				int returned = munch_sde_quiesce();
				assert(sleeps == savedSleeps);
				if (scenario == 25 || scenario == 26 || scenario == 37) {
					assert(!returned && !state.stopped && state.presenter.armed);
					assert(munch_sde_quiesce() == 0);
				} else {
					assert(returned && state.stopped && !state.refresh.owned);
					assert(pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4] == 0x9c000000);
					assert(intf[MUNCH_SDE_INTF_TEAR_AUTOREFRESH / 4] == 0x80000001);
					assert(intf[MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4] == 0x00180885);
				}
			}
		}
		munchSdeReleaseUnused(&state);
		assert(allocations == expectedAllocations);
		if (!reservedPool) assert(state.memory[1]->prepared && state.memory[2]->prepared);
		else assert(!state.memory[1] && !state.memory[2] && state.poolBytes == pool);
	}
	assert(!state.tables);
	if (!tableMode) assert(!state.published && !state.rootEntry && !state.ramCount && !physicalMappings);
	else assert(!state.published && !root[0] && root[2] == (0x80000000ULL | (1u << 10) | (1u << 6) | 1));
	assert(!memcmp(savedBoot.data(), boot, 0x9e4000) && !lockDepth);
	if (scenario < 21 || scenario == 27 || scenario == 28 || scenario == 29 || (scenario >= 30 && scenario != 36))
		assert(!memcmp(savedSmmu.data(), smmu, MUNCH_APPS_SMMU_WINDOW_SIZE));
	printf("SDE %s scenario %u passed: %s, guarded physical buffers\n", reservedPool ? "reserved" : "inherited bypass",
	    scenario, tableMode ? "owned tables restored" : "read-only SMMU, no page tables");
	return 0;
}
