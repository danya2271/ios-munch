#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../xnu/files/iokit/Kernel/munch-sde-mode.h"

struct Machine {
	uint32_t mdp[0x85000 / 4] = {}, dsi[0x400 / 4] = {}, phy[0x800 / 4] = {}, cc[0x4000 / 4] = {};
	uint8_t scratch[3072] = {};
	munch_mode mode = {};
	unsigned writes = 0, failAfter = UINT32_MAX, packets = 0;
	unsigned noLock = 0, noClock = 0, noCommand = 0, badRead = 0, dirty = 0;
	bool fault = false, staleDone = false;
	uint64_t elapsed = 0;
	std::vector<std::vector<uint8_t>> payloads;

	Machine()
	{
		mode.io = {this, read, write, delay, clean, healthy};
		mode.scratch = scratch;
		mode.scratch_iova = 0xc09e3400;
		mode.scratch_size = sizeof(scratch);
		dsi[0x004 / 4] = 0x1f5;
		phy[0x140 / 4] = 1;
		phy[0x6b0 / 4] = 1;
		phy[0x010 / 4] = 0x31;
		phy[0x014 / 4] = 0x31;
		for (unsigned offset : {0x2004, 0x202c, 0x2028, 0x2038, 0x2024}) cc[offset / 4] = 1;
	}

	uint32_t &reg(munch_mode_space space, unsigned offset)
	{
		assert(!(offset & 3));
		switch (space) {
		case MUNCH_MODE_MDP: assert(offset < sizeof(mdp)); return mdp[offset / 4];
		case MUNCH_MODE_DSI: assert(offset < sizeof(dsi)); return dsi[offset / 4];
		case MUNCH_MODE_PHY: assert(offset < sizeof(phy)); return phy[offset / 4];
		case MUNCH_MODE_CC: assert(offset < sizeof(cc)); return cc[offset / 4];
		}
		assert(false);
		return mdp[0];
	}

	static bool healthy(void *cookie)
	{
		auto &machine = *(Machine *)cookie;
		return !machine.fault && machine.writes < machine.failAfter;
	}

	static uint32_t read(void *cookie, munch_mode_space space, uint32_t offset)
	{
		auto &machine = *(Machine *)cookie;
		assert(healthy(cookie));
		uint32_t value = machine.reg(space, offset);
		if (space == MUNCH_MODE_PHY && machine.badRead && offset == 0x140) value ^= 1;
		return value;
	}

	static void write(void *cookie, munch_mode_space space, uint32_t offset, uint32_t value)
	{
		auto &machine = *(Machine *)cookie;
		assert(healthy(cookie));
		machine.writes++;
		uint32_t &reg = machine.reg(space, offset);
		if (space == MUNCH_MODE_MDP) {
			assert(offset != 0x5014 && offset != 0x2018 && offset != 0x201c);
			if (offset == 0x6e808) machine.mdp[0x6e804 / 4] &= ~value;
			if (offset >= 0x810ec && offset <= 0x81124) value &= 63;
		}
		if (space == MUNCH_MODE_PHY && offset == 0x03c && value == 1 && !machine.noLock)
			machine.phy[0x6b0 / 4] = 1;
		if (space == MUNCH_MODE_CC) {
			if (offset == 0x2110 || offset == 0x2098 || offset == 0x2148 || offset == 0x20f8) {
				if (!machine.noClock) value &= ~1u;
			} else if (offset == 0x2004 || offset == 0x202c || offset == 0x2028 || offset == 0x2038 || offset == 0x2024)
				value = value & 1 ? 1 : 0x80000000;
		}
		if (space == MUNCH_MODE_DSI && offset == 0x110) {
			value = (value & 0xaaaaaaaau) | (reg & ~value & 0x55555555);
			if (machine.staleDone) value |= 1;
		}
		reg = value;
		if (space == MUNCH_MODE_DSI && offset == 0x090) {
			assert(value == 1 && machine.dsi[0x048 / 4] == machine.mode.scratch_iova);
			assert(machine.dsi[0x03c / 4] == 0x14000000 && machine.dsi[0x19c / 4] == 0);
			unsigned bytes = machine.dsi[0x04c / 4];
			assert(bytes <= sizeof(machine.scratch) && machine.dirty == bytes);
			assert(machine.scratch[3] & 0x80);
			unsigned length = machine.scratch[0] | (unsigned)machine.scratch[1] << 8;
			if (machine.scratch[3] & 0x40) {
				assert(machine.scratch[2] == 0x39 && bytes == ((length + 7) & ~3u));
				machine.payloads.emplace_back(machine.scratch + 4, machine.scratch + 4 + length);
			} else {
				assert(bytes == 4);
				machine.payloads.push_back({machine.scratch[0]});
			}
			machine.dirty = 0;
			machine.packets++;
			if (!machine.noCommand) machine.dsi[0x110 / 4] |= 1;
		}
	}

	static void delay(void *cookie, uint32_t usec) { ((Machine *)cookie)->elapsed += usec; }
	static void clean(void *cookie, uint8_t *bytes, uint32_t length)
	{
		auto &machine = *(Machine *)cookie;
		assert(bytes == machine.scratch && length <= sizeof(machine.scratch));
		machine.dirty = length;
	}
};

static unsigned panelCommandCount()
{
	unsigned count = 0;
	for (unsigned offset = 0; offset < sizeof(munch_l11r_on);) {
		assert(sizeof(munch_l11r_on) - offset >= 7);
		unsigned length = ((unsigned)munch_l11r_on[offset + 5] << 8) | munch_l11r_on[offset + 6];
		assert(length <= sizeof(munch_l11r_on) - offset - 7);
		offset += 7 + length;
		count++;
	}
	return count;
}

static unsigned panelCommandCount120()
{
	unsigned count = 0;
	for (unsigned offset = 0; offset < sizeof(munch_l11r_on_120hz);) {
		assert(sizeof(munch_l11r_on_120hz) - offset >= 7);
		unsigned length = ((unsigned)munch_l11r_on_120hz[offset + 5] << 8) | munch_l11r_on_120hz[offset + 6];
		assert(length <= sizeof(munch_l11r_on_120hz) - offset - 7);
		offset += 7 + length;
		count++;
	}
	return count;
}

int main()
{
	unsigned totalWrites;
	{
		Machine machine;
		assert(munch_mode_configure(&machine.mode, 60));
		assert(machine.mode.configured && !machine.mode.lit);
		assert(!machine.mode.teConnected);
		assert(machine.mdp[0x6ba88 / 4] == 0x80059);
		assert(!munch_mode_vblank(&machine.mode));
		assert(munch_mode_check(&machine.mode));
		assert(munch_mode_connect_te(&machine.mode, true));
		assert(machine.mode.teConnected);
		assert(machine.mdp[0x6ba88 / 4] == 0x180059);
		assert(munch_mode_light(&machine.mode));
		assert(munch_mode_check(&machine.mode));
		assert(machine.phy[0x528 / 4] == 0xc0 && machine.phy[0x010 / 4] == 0x31);
		assert(machine.cc[0x20fc / 4] == 1 && machine.cc[0x2128 / 4] == 1);
		assert(machine.mdp[0x6ba8c / 4] == 0x0000fff0);
		for (const auto &reg : munch_l11r_dsc) {
			uint32_t mask = reg.offset >= 0xec ? 63 : UINT32_MAX;
			assert(machine.mdp[(0x81000 + reg.offset) / 4] == (reg.value & mask));
		}
		for (const auto &reg : munch_l11r_stream)
			assert(machine.dsi[reg.offset / 4] == reg.value);
		assert(machine.mdp[0x6bab4 / 4] == 0);
		assert(machine.payloads.size() == panelCommandCount() + 4);
		assert(machine.payloads.back() == std::vector<uint8_t>({0x51, 2, 0}));
		assert(machine.mode.commands == panelCommandCount() + 4 &&
		    machine.packets == machine.mode.commands && machine.elapsed == 240002);
		totalWrites = machine.writes;
		for (unsigned event = 0; event < 10; event++) {
			assert(!munch_mode_vblank(&machine.mode));
			machine.mdp[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_FRAME_COUNT) / 4] += 1;
			assert(munch_mode_vblank(&machine.mode));
			assert(!munch_mode_vblank(&machine.mode));
		}
	}
	{
		Machine machine;
		assert(munch_mode_configure(&machine.mode, 60));
		machine.payloads.clear();
		assert(munch_mode_panel_on(&machine.mode) && !machine.mode.lit);
		assert(machine.payloads.back() == std::vector<uint8_t>({0x60, 0}));
		assert(munch_mode_light(&machine.mode));
	}
	{
		Machine machine;
		assert(munch_mode_configure(&machine.mode, 120));
		assert(machine.mode.refresh_hz == 120);
		assert(machine.mdp[0x6ba88 / 4] == 0x80042);
		assert(munch_mode_check(&machine.mode));
		machine.payloads.clear();
		assert(munch_mode_panel_on(&machine.mode));
		assert(machine.payloads.back() == std::vector<uint8_t>({0x60, 0x10}));
		assert(machine.mode.commands == panelCommandCount120() * 2 + 3);
	}
	for (unsigned drift = 0; drift < 3; drift++) {
		Machine machine;
		assert(munch_mode_configure(&machine.mode, 60));
		if (drift == 0) machine.mdp[0x6ba84 / 4] ^= 1;
		if (drift == 1) machine.phy[0x6b0 / 4] = 0;
		if (drift == 2) machine.dsi[0x004 / 4] ^= 1;
		unsigned writes = machine.writes;
		assert(!munch_mode_check(&machine.mode) && machine.mode.failed);
		assert(machine.writes == writes && !munch_mode_light(&machine.mode));
	}
	for (unsigned boundary = 0; boundary < totalWrites; boundary++) {
		Machine machine;
		machine.failAfter = boundary;
		bool configured = munch_mode_configure(&machine.mode, 60);
		if (configured) assert(!munch_mode_light(&machine.mode));
		assert(machine.mode.failed && machine.writes <= boundary);
		assert(!munch_mode_vblank(&machine.mode));
	}
	for (unsigned scenario = 0; scenario < 13; scenario++) {
		Machine machine;
		switch (scenario) {
		case 0: machine.phy[0x140 / 4] = 0; break;
		case 1: machine.dsi[0x004 / 4] = 0x1f0; break;
		case 2: machine.dsi[0x008 / 4] = 4; break;
		case 3: machine.badRead = 1; break;
		case 4: machine.mode.touched = true; break;
		case 5: machine.dsi[0x110 / 4] = 1u << 24; break;
		case 6: machine.phy[0x014 / 4] |= 4; break;
		case 7: machine.mode.scratch_size = 128; break;
		case 8: machine.mode.scratch_iova = 0xfffffff8; break;
		case 9: machine.dsi[0x008 / 4] = 4; break;
		case 10: machine.mdp[0x2018 / 4] = 1; break;
		case 11: machine.mdp[0x201c / 4] = 1; break;
		case 12: machine.mdp[0x6bab4 / 4] = 0x80000001; break;
		}
		assert(!munch_mode_configure(&machine.mode, 60));
		assert(!machine.mode.configured && !machine.mode.lit);
		assert(machine.elapsed < 1000000);
		if (scenario >= 6) assert(!machine.writes);
	}
	printf("Native mode: full Android-style transaction, panel init, TE validation, brightness DMA, independent hardware vblank, %u fault boundaries and 13 rejection paths passed\n", totalWrites);
}
