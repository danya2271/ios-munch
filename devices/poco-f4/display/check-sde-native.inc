static void nativeWrite(void *cookie, munch_mode_space space, uint32_t offset, uint32_t value)
{
	MunchSDE *state = static_cast<MunchSDE *>(cookie);
	assert(offset != 0x5014 && offset != 0x2018 && offset != 0x201c);
	if (space == MUNCH_MODE_DSI && offset == 0x110)
		value = (value & 0xaaaaaaaa) | (state->dsi[0x110 / 4] & ~value & 0x55555555);
	if (space == MUNCH_MODE_MDP && offset == 0x6e808) state->mdp[0x6e804 / 4] &= ~value;
	if (space == MUNCH_MODE_MDP && offset >= 0x810ec && offset <= 0x81124) value &= 63;
	if (space == MUNCH_MODE_CC) {
		if (offset == 0x2110 || offset == 0x2098 || offset == 0x2148 || offset == 0x20f8) value &= ~1u;
		if (offset == 0x2004 || offset == 0x202c || offset == 0x2028 || offset == 0x2038 || offset == 0x2024)
			value = value & 1 ? 1 : 0x80000000;
	}
	munchSdeModeWrite(cookie, space, offset, value);
	if (space == MUNCH_MODE_PHY && offset == 0x03c && value == 1 && nativeScenario != 2)
		state->modeMMIO[MUNCH_MODE_PHY][0x6b0 / 4] = 1;
	if (space == MUNCH_MODE_DSI && offset == 0x090) {
		assert(state->dsi[0x048 / 4] == state->buffers[1].iova + 0x9e3400);
		assert(state->mode.scratch == state->pixels[1] + 0x9e3400);
		assert(state->dsi[0x04c / 4] <= 136);
		assert((state->dsi[0x084 / 4] & 0xf) == 0xc);
		assert(state->dsi[0x100 / 4] == 0);
		assert(state->dsi[0x104 / 4] == 0x09600965);
		state->dsi[0x110 / 4] |= 1;
	}
}

static void testNativeFrames(MunchSDE *state)
{
	unsigned panelCommands = 0;
	for (unsigned offset = 0; offset < sizeof(munch_l11r_on);) {
		assert(sizeof(munch_l11r_on) - offset >= 7);
		unsigned length = ((unsigned)munch_l11r_on[offset + 5] << 8) | munch_l11r_on[offset + 6];
		assert(length <= sizeof(munch_l11r_on) - offset - 7);
		offset += 7 + length;
		panelCommands++;
	}
	assert(state->modeMMIO[MUNCH_MODE_CC] == reinterpret_cast<volatile uint32_t *>(address(0x0af00000, 0x4000)));
	assert(state->modeMMIO[MUNCH_MODE_PHY] == reinterpret_cast<volatile uint32_t *>(address(0x0ae94400, 0x800)));
	assert(!state->mode.touched && !state->mode.configured && !state->presenter.submitted);
	state->modeMMIO[MUNCH_MODE_PHY][0x140 / 4] = 1;
	state->modeMMIO[MUNCH_MODE_PHY][0x6b0 / 4] = 1;
	state->modeMMIO[MUNCH_MODE_PHY][0x010 / 4] = 0x31;
	state->modeMMIO[MUNCH_MODE_PHY][0x014 / 4] = 0x31;
	if (nativeScenario == 2) state->modeMMIO[MUNCH_MODE_PHY][0x140 / 4] = 0;
	for (unsigned offset : {0x2004, 0x202c, 0x2028, 0x2038, 0x2024}) state->modeMMIO[MUNCH_MODE_CC][offset / 4] = 1;
	state->mode.io.write = nativeWrite;
	state->presenter.io.write = refreshWrite;
	if (nativeScenario == 3) transferEnabled = false;
	for (unsigned frame = 0; frame < 2; frame++) {
		uint8_t *back = nullptr;
		assert(munchSdeBeginFrame(state, &back) == MUNCH_SDE_OK && back);
		back[0] = 0x35;
		auto result = munchSdeEndFrame(state);
		if (nativeScenario == 2) {
			assert(result == MUNCH_SDE_FAULT && state->mode.failed && !state->mode.configured);
			assert(state->mdp[0x5014 / 4] == 0x9c000000 && !state->presenter.submitted);
			assert(!state->mode.lit && !munch_sde_native_owned() && munch_sde_quiesce() == 1);
			return;
		}
		if (nativeScenario == 3) {
			assert(result == MUNCH_SDE_TIMEOUT && state->mode.configured && !state->mode.lit);
			assert(!state->presenter.completed && state->presenter.pending == 1 && !munch_sde_quiesce());
			return;
		}
		if (result != MUNCH_SDE_OK)
			printf("Native integration failed: %s at %s reg=%u:%x value=%x\n", munch_sde_result_name(result),
			    state->mode.phase, state->mode.failed_space, state->mode.failed_offset, state->mode.failed_value);
		assert(result == MUNCH_SDE_OK && state->mode.configured && state->mode.lit);
		assert(state->presenter.completed == frame + 1 &&
		    state->mode.commands == panelCommands + 4);
		assert(state->mode.teConnected);
		assert(state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4] ==
		    (0x00080059 | MUNCH_SDE_EXTERNAL_TE));
		assert(state->refresh.owned && state->refresh.paused &&
		    !(state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] &
		        MUNCH_SDE_AUTOREFRESH_ENABLE));
		assert(state->mdp[0x5014 / 4] == state->buffers[state->presenter.front].iova);
		assert(state->presenter.route.flush_mask == 1);
	}
	state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_FRAME_COUNT) / 4] += 1;
	assert(munchSdeSampleVBlank(state) && state->vblankCount == 1);
	assert(!munchSdeSampleVBlank(state) && state->vblankCount == 1);
	state->mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_FRAME_COUNT) / 4] += 1;
	assert(munchSdeSampleVBlank(state) && state->vblankCount == 2);
	advance(40000);
	assert(!munchSdeSampleVBlank(state) && !state->vblankFallback && state->vblankFallbackEvents == 0);
	assert(munchSdePreparePanel(state) == MUNCH_SDE_OK);
	assert(munchSdeFinishPanel(state, false, true) == MUNCH_SDE_OK && !munchSdeSampleVBlank(state));
	assert(munchSdePreparePanel(state) == MUNCH_SDE_OK);
	assert(munchSdeFinishPanel(state, true, true) == MUNCH_SDE_OK);
	assert(munch_sde_native_owned() && !munch_sde_quiesce());
}
