#ifndef _IOKIT_MUNCH_SDE_MODE_H
#define _IOKIT_MUNCH_SDE_MODE_H

#include "munch-kona-display.h"
#include "munch-sde-mode-data.h"

enum munch_mode_space { MUNCH_MODE_MDP, MUNCH_MODE_DSI, MUNCH_MODE_PHY, MUNCH_MODE_CC };

struct munch_mode_io {
	void *cookie;
	uint32_t (*read)(void *, enum munch_mode_space, uint32_t);
	void (*write)(void *, enum munch_mode_space, uint32_t, uint32_t);
	void (*delay)(void *, uint32_t);
	void (*clean)(void *, uint8_t *, uint32_t);
	bool (*healthy)(void *);
};

struct munch_mode {
	struct munch_mode_io io;
	uint8_t *scratch;
	uint32_t scratch_iova, scratch_size;
	const char *phase;
	uint32_t commands, batches, last_batch_count, last_batch_bytes;
	uint32_t last_batch_word0, last_batch_word1, failed_offset, failed_value;
	enum munch_mode_space failed_space;
	bool touched, configured, inherited, timingProgrammed, lit, failed, teConnected, vblankFrameValid;
	uint32_t vblankFrame;
	uint32_t refresh_hz, inheritedVsync;
};

static inline bool
munch_mode_healthy(struct munch_mode *mode)
{
	if (!mode->failed && mode->io.healthy(mode->io.cookie)) return true;
	mode->failed = true;
	return false;
}

static inline uint32_t
munch_mode_read(struct munch_mode *mode, enum munch_mode_space space, uint32_t offset)
{
	return munch_mode_healthy(mode) ? mode->io.read(mode->io.cookie, space, offset) : 0;
}

static inline void
munch_mode_write(struct munch_mode *mode, enum munch_mode_space space, uint32_t offset, uint32_t value)
{
	if (!munch_mode_healthy(mode)) return;
	mode->touched = true;
	mode->io.write(mode->io.cookie, space, offset, value);
}

static inline bool
munch_mode_poll(struct munch_mode *mode, enum munch_mode_space space, uint32_t offset,
    uint32_t mask, uint32_t expected, uint32_t timeout)
{
	for (uint32_t elapsed = 0; elapsed <= timeout; elapsed += 10) {
		uint32_t value = munch_mode_read(mode, space, offset);
		if (mode->failed) return false;
		if ((value & mask) == expected) return true;
		mode->failed_space = space;
		mode->failed_offset = offset;
		mode->failed_value = value;
		mode->io.delay(mode->io.cookie, 10);
	}
	mode->failed = true;
	return false;
}

static inline bool
munch_mode_register_readable(enum munch_mode_space space, uint32_t offset)
{
	return !(space == MUNCH_MODE_DSI && offset == 0x1e4) &&
	    !(space == MUNCH_MODE_DSI && offset == 0x10c) &&
	    !(space == MUNCH_MODE_MDP && offset == MUNCH_SDE_INTF1_OFFSET + 0x0a8);
}

static inline bool
munch_mode_program(struct munch_mode *mode, enum munch_mode_space space, uint32_t base,
    const struct munch_mode_register *registers, unsigned count, bool verify)
{
	for (unsigned index = 0; index < count; index++) {
		uint32_t offset = base + registers[index].offset;
		munch_mode_write(mode, space, offset, registers[index].value);
		uint32_t mask = space == MUNCH_MODE_MDP && base == 0x81000 && registers[index].offset >= 0xec ? 63 : UINT32_MAX;
		if (verify && munch_mode_register_readable(space, offset) &&
		    !munch_mode_poll(mode, space, offset, mask, registers[index].value & mask, 0))
			return false;
	}
	return munch_mode_healthy(mode);
}

static inline bool
munch_mode_command_batch(struct munch_mode *mode, const struct munch_mode_command *commands,
    unsigned count)
{
	uint32_t bytes = 0;
	if (!count || !mode->scratch || !mode->scratch_iova || (mode->scratch_iova & 7) ||
	    (uint64_t)mode->scratch_iova + mode->scratch_size > (1ULL << 32)) {
		mode->failed = true;
		return false;
	}
	if (!munch_mode_poll(mode, MUNCH_MODE_DSI, 0x008, 6, 0, 10000)) return false;
	uint32_t mask = munch_mode_read(mode, MUNCH_MODE_DSI, 0x110) & 0xaaaaaaaau;
	if (munch_mode_read(mode, MUNCH_MODE_DSI, 0x110) & (1u << 24)) {
		mode->failed = true;
		return false;
	}
	for (unsigned command_index = 0; command_index < count; command_index++) {
		const struct munch_mode_command *command = &commands[command_index];
		bool long_packet = command->type == 0x39;
		uint32_t packet_bytes = long_packet ? (4 + command->length + 3) & ~3u : 4;
		if (!command->length || (!long_packet && (command->type != 0x05 && command->type != 0x15)) ||
		    (!long_packet && command->length > 2) || bytes + packet_bytes > mode->scratch_size ||
		    (command_index + 1 == count && !command->last)) {
			mode->failed = true;
			return false;
		}
		for (uint32_t index = 0; index < packet_bytes; index++) mode->scratch[bytes + index] = 0xff;
		mode->scratch[bytes] = long_packet ? (uint8_t)(command->length >> 8) :
		    (command->length > 1 ? command->payload[1] : 0);
		mode->scratch[bytes + 1] = command->type;
		mode->scratch[bytes + 2] = long_packet ? (uint8_t)command->length : command->payload[0];
		mode->scratch[bytes + 3] = (long_packet ? 0x40 : 0) | (command->last ? 0x80 : 0);
		if (long_packet) {
			for (unsigned index = 0; index < command->length; index++)
				mode->scratch[bytes + 4 + index] = command->payload[index];
		}
		bytes += packet_bytes;
	}
	mode->io.clean(mode->io.cookie, mode->scratch, bytes);
	mode->batches++;
	mode->last_batch_count = count;
	mode->last_batch_bytes = bytes;
	mode->last_batch_word0 = ((uint32_t *)mode->scratch)[0];
	mode->last_batch_word1 = bytes >= 8 ? ((uint32_t *)mode->scratch)[1] : 0;
	bool fifoMode = bytes <= 64;
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x110, mask | 3);
	if (!munch_mode_poll(mode, MUNCH_MODE_DSI, 0x110, 1, 0, 0)) return false;
	uint32_t dmaControl = munch_mode_read(mode, MUNCH_MODE_DSI, 0x03c);
	uint32_t fifoControl = munch_mode_read(mode, MUNCH_MODE_DSI, 0x050);
	if (fifoMode) {
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x15c, (1u << 1) | (1u << 2) | (3u << 16));
		for (uint32_t offset = 0; offset < bytes; offset += 4)
			munch_mode_write(mode, MUNCH_MODE_DSI, 0x17c, ((uint32_t *)mode->scratch)[offset / 4]);
		if (((bytes / 4) & 1u) != 0) munch_mode_write(mode, MUNCH_MODE_DSI, 0x17c, 0);
		dmaControl &= ~((1u << 31) | (1u << 30));
		dmaControl |= (1u << 28) | (1u << 26);
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x03c, dmaControl);
	} else {
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x19c, 0);
		dmaControl &= ~((1u << 31) | (1u << 30) | (1u << 24) | (1u << 29));
		dmaControl |= (1u << 28) | (1u << 26);
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x03c, dmaControl);
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x050, fifoControl | (1u << 20) | (1u << 16));
		munch_mode_write(mode, MUNCH_MODE_DSI, 0x048, mode->scratch_iova);
	}
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x04c, bytes);
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x090, 1);
	for (uint32_t elapsed = 0; elapsed < 100000; elapsed += 10) {
		uint32_t status = munch_mode_read(mode, MUNCH_MODE_DSI, 0x110);
		if (mode->failed || (status & (1u << 24))) break;
		if (status & 1) {
			munch_mode_write(mode, MUNCH_MODE_DSI, 0x15c, 0);
			munch_mode_write(mode, MUNCH_MODE_DSI, 0x1ec, 1);
			mode->io.delay(mode->io.cookie, 1);
			munch_mode_write(mode, MUNCH_MODE_DSI, 0x1ec, 0);
			munch_mode_write(mode, MUNCH_MODE_DSI, 0x110, (mask & ~2u) | 1u);
			mode->commands += count;
			return munch_mode_healthy(mode);
		}
		mode->io.delay(mode->io.cookie, 10);
	}
	mode->failed_offset = 0x110;
	mode->failed_space = MUNCH_MODE_DSI;
	mode->failed_value = munch_mode_read(mode, MUNCH_MODE_DSI, 0x110);
	mode->failed = true;
	return false;
}

static inline bool
munch_mode_command(struct munch_mode *mode, uint8_t type, const uint8_t *payload,
    unsigned length, bool last)
{
	struct munch_mode_command command = { type, (uint8_t)length, 0, last, payload };
	return munch_mode_command_batch(mode, &command, 1);
}

static inline bool
munch_mode_command_sequence(struct munch_mode *mode, const struct munch_mode_command *commands,
    unsigned count)
{
	for (unsigned index = 0; index < count;) {
		unsigned end = index;
		while (end < count && !commands[end].last) end++;
		if (end == count || !munch_mode_command_batch(mode, commands + index, end - index + 1))
			return false;
		if (commands[end].delay_ms) mode->io.delay(mode->io.cookie, commands[end].delay_ms * 1000);
		index = end + 1;
	}
	return munch_mode_healthy(mode);
}

static inline bool
munch_mode_program_dsi_timing_shadow(struct munch_mode *mode,
    const struct munch_mode_register *registers, unsigned count)
{
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x1e8, 1);
	for (unsigned index = 0; index < count; index++) {
		if (registers[index].offset == 0x1e4) continue;
		uint32_t offset = registers[index].offset;
		munch_mode_write(mode, MUNCH_MODE_DSI, offset, registers[index].value);
		if (munch_mode_register_readable(MUNCH_MODE_DSI, offset) &&
		    !munch_mode_poll(mode, MUNCH_MODE_DSI, offset, UINT32_MAX, registers[index].value, 0))
			return false;
	}
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x1e4, 1);
	return munch_mode_healthy(mode);
}

static inline bool
munch_mode_switch_refresh(struct munch_mode *mode, uint32_t refresh_hz)
{
	const struct munch_mode_command *commands;
	unsigned command_count;
	const struct munch_mode_register *tear;
	unsigned tear_count;
	const struct munch_mode_register *dsi_timing;
	unsigned dsi_timing_count;
	const struct munch_mode_register *intf_timing;
	unsigned intf_timing_count;

	if (!mode->configured || mode->failed || (refresh_hz != 60 && refresh_hz != 120)) return false;
	if (mode->refresh_hz == refresh_hz) return true;
	commands = refresh_hz == 120 ? munch_l11r_120hz : munch_l11r_60hz;
	command_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_120hz) / sizeof(munch_l11r_120hz[0]) :
	    sizeof(munch_l11r_60hz) / sizeof(munch_l11r_60hz[0]);
	tear = refresh_hz == 120 ? munch_l11r_tear_120hz : munch_l11r_tear;
	tear_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_tear_120hz) / sizeof(munch_l11r_tear_120hz[0]) :
	    sizeof(munch_l11r_tear) / sizeof(munch_l11r_tear[0]);
	dsi_timing = refresh_hz == 120 ? munch_l11r_dsi_timing_120 : munch_l11r_dsi_timing_60;
	dsi_timing_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_dsi_timing_120) / sizeof(munch_l11r_dsi_timing_120[0]) :
	    sizeof(munch_l11r_dsi_timing_60) / sizeof(munch_l11r_dsi_timing_60[0]);
	intf_timing = refresh_hz == 120 ? munch_l11r_intf_timing_120 : munch_l11r_intf_timing_60;
	intf_timing_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_intf_timing_120) / sizeof(munch_l11r_intf_timing_120[0]) :
	    sizeof(munch_l11r_intf_timing_60) / sizeof(munch_l11r_intf_timing_60[0]);
	mode->phase = refresh_hz == 120 ? "native-switch-120hz" : "native-switch-60hz";
	if (!munch_mode_program_dsi_timing_shadow(mode, dsi_timing, dsi_timing_count) ||
	    !munch_mode_command_sequence(mode, commands, command_count) ||
	    !munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, intf_timing, intf_timing_count, true) ||
	    !munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, tear, tear_count, true) ||
	    !munch_mode_healthy(mode)) {
		return false;
	}
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x1e8, 0);
	mode->refresh_hz = refresh_hz;
	mode->timingProgrammed = mode->inherited;
	mode->vblankFrameValid = false;
	return true;
}

static inline bool
munch_mode_byte_sequence(struct munch_mode *mode, const uint8_t *bytes, unsigned size)
{
	struct munch_mode_command commands[128];
	for (unsigned offset = 0; offset < size;) {
		unsigned count = 0;
		unsigned start = offset;
		while (offset < size) {
			if (count == sizeof(commands) / sizeof(commands[0]) || size - offset < 7) {
				mode->failed = true;
				return false;
			}
			uint8_t type = bytes[offset];
			uint16_t delay = bytes[offset + 4];
			unsigned length = ((unsigned)bytes[offset + 5] << 8) | bytes[offset + 6];
			if (!length || length > size - offset - 7 ||
			    (type != 0x05 && type != 0x15 && type != 0x39)) {
				mode->failed = true;
				return false;
			}
			commands[count++] = (struct munch_mode_command){
			    type, (uint8_t)length, delay, bytes[offset + 1] != 0, bytes + offset + 7
			};
			offset += 7 + length;
			if (commands[count - 1].last) break;
		}
		if (offset == start || !munch_mode_command_batch(mode, commands, count)) return false;
		if (commands[count - 1].delay_ms)
			mode->io.delay(mode->io.cookie, commands[count - 1].delay_ms * 1000);
	}
	return munch_mode_healthy(mode);
}

static inline bool
munch_mode_clock(struct munch_mode *mode, uint32_t offset, uint32_t config)
{
	munch_mode_write(mode, MUNCH_MODE_CC, offset + 4, config);
	uint32_t command = munch_mode_read(mode, MUNCH_MODE_CC, offset);
	munch_mode_write(mode, MUNCH_MODE_CC, offset, command | 1);
	return munch_mode_poll(mode, MUNCH_MODE_CC, offset, 1, 0, 1000) &&
	    munch_mode_poll(mode, MUNCH_MODE_CC, offset + 4, 0x371f, config, 0);
}

static inline bool
munch_mode_branch(struct munch_mode *mode, uint32_t offset, bool enable)
{
	uint32_t original = munch_mode_read(mode, MUNCH_MODE_CC, offset);
	munch_mode_write(mode, MUNCH_MODE_CC, offset, (original & ~1u) | (enable ? 1 : 0));
	return munch_mode_poll(mode, MUNCH_MODE_CC, offset, 0x80000001u, enable ? 1 : 0x80000000u, 1000);
}

static inline bool
munch_mode_adopt(struct munch_mode *mode, uint32_t requested_refresh)
{
	uint32_t vsync = munch_mode_read(mode, MUNCH_MODE_MDP, 0x6b800 + 0x288);
	uint32_t intf_vtotal = munch_mode_read(mode, MUNCH_MODE_MDP,
	    MUNCH_SDE_INTF1_OFFSET + munch_l11r_intf_timing_60[1].offset);
	uint32_t dsi_vtotal = munch_mode_read(mode, MUNCH_MODE_DSI, munch_l11r_dsi_timing_60[1].offset);
	uint32_t inherited_refresh;

	mode->phase = "native-handoff-detect";
	if (mode->touched || mode->configured || !mode->scratch || mode->scratch_size < 136 ||
	    !mode->scratch_iova || (mode->scratch_iova & 7) ||
	    (uint64_t)mode->scratch_iova + mode->scratch_size > (1ULL << 32) ||
	    (munch_mode_read(mode, MUNCH_MODE_DSI, 0x004) & 0x1f7) != 0x1f5) {
		mode->phase = "native-handoff-rejected";
		return false;
	}
	if (intf_vtotal == munch_l11r_intf_timing_60[1].value ||
	    dsi_vtotal == munch_l11r_dsi_timing_60[1].value ||
	    (vsync & 0xffff) == (munch_l11r_tear[0].value & 0xffff))
		inherited_refresh = 60;
	else if (intf_vtotal == munch_l11r_intf_timing_120[1].value ||
	    dsi_vtotal == munch_l11r_dsi_timing_120[1].value ||
	    (vsync & 0xffff) == (munch_l11r_tear_120hz[0].value & 0xffff))
		inherited_refresh = 120;
	else if (requested_refresh == 60 || requested_refresh == 120)
		inherited_refresh = requested_refresh;
	else {
		mode->phase = "native-handoff-unknown-timing";
		return false;
	}
	mode->refresh_hz = inherited_refresh;
	mode->inheritedVsync = vsync & 0xffff;
	mode->teConnected = (vsync & MUNCH_SDE_EXTERNAL_TE) != 0;
	mode->configured = true;
	mode->inherited = true;
	mode->timingProgrammed = false;
	mode->lit = true;
	mode->phase = "native-inherited";
	mode->touched = true;
	mode->vblankFrameValid = false;
	return true;
}

static inline bool
munch_mode_configure(struct munch_mode *mode, uint32_t refresh_hz)
{
	static const uint32_t branches[] = { 0x2004, 0x202c, 0x2028, 0x2038, 0x2024 };
	const struct munch_mode_register *tear;
	unsigned tear_count;
	const uint8_t *panel_on;
	unsigned panel_on_size;
	const uint8_t *panel_off;
	unsigned panel_off_size;
	const struct munch_mode_register *dsi_timing;
	unsigned dsi_timing_count;
	const struct munch_mode_register *intf_timing;
	unsigned intf_timing_count;
	static const struct munch_mode_register host[] = {
		{ 0x004, 0x1f0 }, { 0x084, 0x80000004 }, { 0x0c4, 0 }, { 0x0cc, 1 },
		{ 0x10c, 0x03f03fc0 }, { 0x110, 0x02200202 }, { 0x11c, 0x23f },
	};
	static const struct munch_mode_register route[] = {
		{ 0x05000, 0x09600438 }, { 0x05008, 0 }, { 0x0500c, 0x09600438 }, { 0x05010, 0 },
		{ 0x05024, 0x10e0 }, { 0x05030, 0x237ff }, { 0x05034, 0x03020001 },
		{ 0x05038, 0x80000000 }, { 0x05050, 0 }, { 0x05070, 0 }, { 0x050b4, 0 }, { 0x05170, 0 },
		{ 0x05100, 0 }, { 0x05104, 0 }, { 0x05108, 0x09600438 },
		{ 0x05110, 0 }, { 0x05114, 0 }, { 0x05118, 0x09600438 },
		{ 0x05120, 0 }, { 0x05124, 0 }, { 0x05128, 0x09600438 },
		{ 0x05a04, 0 }, { 0x06a00, 2 }, { 0x45000, 0 }, { 0x45004, 0x09600438 },
		{ 0x45068, 0x400 }, { 0x4506c, 0x00ff0000 },
		{ 0x02000, 0x01000005 }, { 0x02014, 0x20000 }, { 0x020f4, 2 },
		{ 0x6ba5c, 0 },
	};
	if (refresh_hz != 60 && refresh_hz != 120) return false;
	mode->refresh_hz = refresh_hz;
	tear = refresh_hz == 120 ? munch_l11r_tear_120hz : munch_l11r_tear;
	tear_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_tear_120hz) / sizeof(munch_l11r_tear_120hz[0]) :
	    sizeof(munch_l11r_tear) / sizeof(munch_l11r_tear[0]);
	dsi_timing = refresh_hz == 120 ? munch_l11r_dsi_timing_120 : munch_l11r_dsi_timing_60;
	dsi_timing_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_dsi_timing_120) / sizeof(munch_l11r_dsi_timing_120[0]) :
	    sizeof(munch_l11r_dsi_timing_60) / sizeof(munch_l11r_dsi_timing_60[0]);
	intf_timing = refresh_hz == 120 ? munch_l11r_intf_timing_120 : munch_l11r_intf_timing_60;
	intf_timing_count = refresh_hz == 120 ?
	    sizeof(munch_l11r_intf_timing_120) / sizeof(munch_l11r_intf_timing_120[0]) :
	    sizeof(munch_l11r_intf_timing_60) / sizeof(munch_l11r_intf_timing_60[0]);
	panel_on = refresh_hz == 120 ? munch_l11r_on_120hz : munch_l11r_on;
	panel_on_size = refresh_hz == 120 ? sizeof(munch_l11r_on_120hz) : sizeof(munch_l11r_on);
	panel_off = refresh_hz == 120 ? munch_l11r_off_120hz : munch_l11r_off;
	panel_off_size = refresh_hz == 120 ? sizeof(munch_l11r_off_120hz) : sizeof(munch_l11r_off);
	mode->phase = "native-preflight";
	if (mode->touched || mode->configured || !mode->scratch || mode->scratch_size < 136 ||
	    !mode->scratch_iova || (mode->scratch_iova & 7) ||
	    (uint64_t)mode->scratch_iova + mode->scratch_size > (1ULL << 32) ||
	    !munch_mode_healthy(mode)) return false;
	if (munch_mode_read(mode, MUNCH_MODE_MDP, 0x2018) || munch_mode_read(mode, MUNCH_MODE_MDP, 0x201c) ||
	    (munch_mode_read(mode, MUNCH_MODE_MDP, 0x6bab4) & (1u << 31))) return false;
	if ((munch_mode_read(mode, MUNCH_MODE_PHY, 0x014) & 0x0c) ||
	    (munch_mode_read(mode, MUNCH_MODE_PHY, 0x018) & 0x40) ||
	    (munch_mode_read(mode, MUNCH_MODE_DSI, 0x004) & 0x1f7) != 0x1f5) return false;
	if (!munch_mode_poll(mode, MUNCH_MODE_PHY, 0x140, 1, 1, 1000)) return false;
	mode->phase = "native-panel-sleep";
	if (!munch_mode_byte_sequence(mode, panel_off, panel_off_size)) return false;
	mode->phase = "native-link-stop";
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6ba84, 0);
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x004, 0x1f0);
	for (unsigned index = 0; index < sizeof(branches) / sizeof(branches[0]); index++) {
		if (!munch_mode_branch(mode, branches[index], false)) return false;
	}
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x014, 0);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x03c, 0);
	mode->phase = "native-phy";
	if ((munch_mode_read(mode, MUNCH_MODE_PHY, 0) & 0xf0) == 0x20)
		munch_mode_write(mode, MUNCH_MODE_PHY, 0x114, 4);
	if (!munch_mode_program(mode, MUNCH_MODE_PHY, 0, munch_l11r_phy,
	    sizeof(munch_l11r_phy) / sizeof(munch_l11r_phy[0]), true)) return false;
	mode->phase = "native-pll";
	if (!munch_mode_program(mode, MUNCH_MODE_PHY, 0x500, munch_l11r_pll,
	    sizeof(munch_l11r_pll) / sizeof(munch_l11r_pll[0]), false)) return false;
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x010, 0x31);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x014, 1);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x528, 0xc0);
	mode->io.delay(mode->io.cookie, 1);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x03c, 1);
	if (!munch_mode_poll(mode, MUNCH_MODE_PHY, 0x6b0, 1, 1, 5000)) return false;
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x128, 1);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x128, 0);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x030, 4);
	munch_mode_write(mode, MUNCH_MODE_PHY, 0x014, 0x31);
	mode->phase = "native-clocks";
	if (!munch_mode_clock(mode, 0x2110, 0x101) || !munch_mode_clock(mode, 0x2098, 0x101) ||
	    !munch_mode_clock(mode, 0x2148, 1) || !munch_mode_clock(mode, 0x20f8, 1)) return false;
	munch_mode_write(mode, MUNCH_MODE_CC, 0x2128, 1);
	for (unsigned index = sizeof(branches) / sizeof(branches[0]); index > 0; index--) {
		if (!munch_mode_branch(mode, branches[index - 1], true)) return false;
	}
	mode->phase = "native-host";
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x11c, 0x200b3f);
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x118, 1);
	mode->io.delay(mode->io.cookie, 1);
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x118, 0);
	if (!munch_mode_program(mode, MUNCH_MODE_DSI, 0, host, sizeof(host) / sizeof(host[0]), true)) return false;
	if (!munch_mode_program(mode, MUNCH_MODE_DSI, 0, munch_l11r_stream,
	    sizeof(munch_l11r_stream) / sizeof(munch_l11r_stream[0]), true)) return false;
	if (!munch_mode_program(mode, MUNCH_MODE_DSI, 0, dsi_timing, dsi_timing_count, true)) return false;
	munch_mode_write(mode, MUNCH_MODE_DSI, 0x004, 0x1f5);
	mode->phase = "native-dsc";
	if (!munch_mode_program(mode, MUNCH_MODE_MDP, 0x81000, munch_l11r_dsc,
	    sizeof(munch_l11r_dsc) / sizeof(munch_l11r_dsc[0]), true)) return false;
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x82800, 0);
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x710a0, 1);
	mode->phase = "native-route";
	if (!munch_mode_program(mode, MUNCH_MODE_MDP, 0, route, sizeof(route) / sizeof(route[0]), true)) return false;
	if (!munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, intf_timing, intf_timing_count, true)) return false;
	mode->phase = "native-panel-init";
	if (!munch_mode_byte_sequence(mode, panel_on, panel_on_size)) return false;
	mode->phase = "native-tearcheck";
	if (!munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, tear, tear_count, true)) return false;
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6ba80, 0);
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6bab4, 0);
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6ba84, 1);
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6e808, 4);
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6e800, munch_mode_read(mode, MUNCH_MODE_MDP, 0x6e800) | 4);
	mode->configured = munch_mode_healthy(mode);
	mode->phase = "native-first-frame";
	mode->teConnected = false;
	return mode->configured;
}

static inline bool
munch_mode_connect_te(struct munch_mode *mode, bool enable)
{
	if (!mode->configured || mode->failed || !munch_mode_healthy(mode)) return false;
	uint32_t value = munch_mode_read(mode, MUNCH_MODE_MDP, 0x6b800 + 0x288);
	value = enable ? value | MUNCH_SDE_EXTERNAL_TE : value & ~MUNCH_SDE_EXTERNAL_TE;
	munch_mode_write(mode, MUNCH_MODE_MDP, 0x6b800 + 0x288, value);
	if (!munch_mode_healthy(mode)) return false;
	mode->teConnected = enable;
	return (munch_mode_read(mode, MUNCH_MODE_MDP, 0x6b800 + 0x288) & MUNCH_SDE_EXTERNAL_TE) ==
	    (enable ? MUNCH_SDE_EXTERNAL_TE : 0);
}

static inline bool
munch_mode_timing_matches(struct munch_mode *mode)
{
	const struct munch_mode_register *tear = mode->refresh_hz == 120 ?
	    munch_l11r_tear_120hz : munch_l11r_tear;
	unsigned tear_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_tear_120hz) / sizeof(munch_l11r_tear_120hz[0]) :
	    sizeof(munch_l11r_tear) / sizeof(munch_l11r_tear[0]);
	const struct munch_mode_register *dsi_timing = mode->refresh_hz == 120 ?
	    munch_l11r_dsi_timing_120 : munch_l11r_dsi_timing_60;
	unsigned dsi_timing_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_dsi_timing_120) / sizeof(munch_l11r_dsi_timing_120[0]) :
	    sizeof(munch_l11r_dsi_timing_60) / sizeof(munch_l11r_dsi_timing_60[0]);
	const struct munch_mode_register *intf_timing = mode->refresh_hz == 120 ?
	    munch_l11r_intf_timing_120 : munch_l11r_intf_timing_60;
	unsigned intf_timing_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_intf_timing_120) / sizeof(munch_l11r_intf_timing_120[0]) :
	    sizeof(munch_l11r_intf_timing_60) / sizeof(munch_l11r_intf_timing_60[0]);

	for (unsigned index = 0; index < dsi_timing_count; index++) {
		if (!munch_mode_register_readable(MUNCH_MODE_DSI, dsi_timing[index].offset)) continue;
		if (munch_mode_read(mode, MUNCH_MODE_DSI, dsi_timing[index].offset) != dsi_timing[index].value)
			return false;
	}
	for (unsigned index = 0; index < intf_timing_count; index++) {
		if (munch_mode_read(mode, MUNCH_MODE_MDP, MUNCH_SDE_INTF1_OFFSET + intf_timing[index].offset) !=
		    intf_timing[index].value)
			return false;
	}
	for (unsigned index = 0; index < tear_count; index++) {
		uint32_t expected = tear[index].value;
		if (index == 0 && mode->teConnected) expected |= MUNCH_SDE_EXTERNAL_TE;
		if (munch_mode_read(mode, MUNCH_MODE_MDP, MUNCH_SDE_INTF1_OFFSET + tear[index].offset) != expected)
			return false;
	}
	return true;
}

static inline bool
munch_mode_check(struct munch_mode *mode)
{
	const struct munch_mode_register *tear;
	const struct munch_mode_register *dsi_timing;
	const struct munch_mode_register *intf_timing;
	unsigned tear_count;
	unsigned dsi_timing_count;
	unsigned intf_timing_count;

	if (!mode->configured || mode->failed || !munch_mode_healthy(mode)) return false;
	if (mode->inherited) {
		uint32_t vsync = munch_mode_read(mode, MUNCH_MODE_MDP, 0x6b800 + 0x288);
		uint32_t expected = mode->refresh_hz == 120 ?
		    munch_l11r_tear_120hz[0].value : munch_l11r_tear[0].value;
		return (munch_mode_read(mode, MUNCH_MODE_DSI, 0x004) & 0x1f7) == 0x1f5 &&
		    (vsync & 0xffff) == (mode->timingProgrammed ? expected : mode->inheritedVsync) &&
		    ((vsync & MUNCH_SDE_EXTERNAL_TE) != 0) == mode->teConnected &&
		    (!mode->timingProgrammed || munch_mode_timing_matches(mode));
	}
	tear = mode->refresh_hz == 120 ? munch_l11r_tear_120hz : munch_l11r_tear;
	tear_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_tear_120hz) / sizeof(munch_l11r_tear_120hz[0]) :
	    sizeof(munch_l11r_tear) / sizeof(munch_l11r_tear[0]);
	dsi_timing = mode->refresh_hz == 120 ? munch_l11r_dsi_timing_120 : munch_l11r_dsi_timing_60;
	dsi_timing_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_dsi_timing_120) / sizeof(munch_l11r_dsi_timing_120[0]) :
	    sizeof(munch_l11r_dsi_timing_60) / sizeof(munch_l11r_dsi_timing_60[0]);
	intf_timing = mode->refresh_hz == 120 ? munch_l11r_intf_timing_120 : munch_l11r_intf_timing_60;
	intf_timing_count = mode->refresh_hz == 120 ?
	    sizeof(munch_l11r_intf_timing_120) / sizeof(munch_l11r_intf_timing_120[0]) :
	    sizeof(munch_l11r_intf_timing_60) / sizeof(munch_l11r_intf_timing_60[0]);
	if (!munch_mode_poll(mode, MUNCH_MODE_PHY, 0x6b0, 1, 1, 0) ||
	    !munch_mode_poll(mode, MUNCH_MODE_PHY, 0x010, 0xff, 0x31, 0) ||
	    !munch_mode_poll(mode, MUNCH_MODE_PHY, 0x014, 0x3f, 0x31, 0) ||
	    !munch_mode_poll(mode, MUNCH_MODE_DSI, 0x004, 0x1f7, 0x1f5, 0) ||
	    !munch_mode_poll(mode, MUNCH_MODE_DSI, 0x110, 1u << 24, 0, 0)) return false;
	for (unsigned index = 0; index < sizeof(munch_l11r_dsc) / sizeof(munch_l11r_dsc[0]); index++) {
		uint32_t mask = munch_l11r_dsc[index].offset >= 0xec ? 63 : UINT32_MAX;
		if (!munch_mode_poll(mode, MUNCH_MODE_MDP, 0x81000 + munch_l11r_dsc[index].offset,
		    mask, munch_l11r_dsc[index].value & mask, 0)) return false;
	}
	for (unsigned index = 0; index < sizeof(munch_l11r_stream) / sizeof(munch_l11r_stream[0]); index++) {
		if (!munch_mode_poll(mode, MUNCH_MODE_DSI, munch_l11r_stream[index].offset,
		    UINT32_MAX, munch_l11r_stream[index].value, 0)) return false;
	}
	for (unsigned index = 0; index < dsi_timing_count; index++) {
		if (!munch_mode_register_readable(MUNCH_MODE_DSI, dsi_timing[index].offset)) continue;
		if (!munch_mode_poll(mode, MUNCH_MODE_DSI, dsi_timing[index].offset,
		    UINT32_MAX, dsi_timing[index].value, 0)) return false;
	}
	for (unsigned index = 0; index < intf_timing_count; index++) {
		if (!munch_mode_poll(mode, MUNCH_MODE_MDP, MUNCH_SDE_INTF1_OFFSET + intf_timing[index].offset,
		    UINT32_MAX, intf_timing[index].value, 0)) return false;
	}
	for (unsigned index = 0; index < tear_count; index++) {
		uint32_t value = tear[index].value;
		if (index == 0 && mode->teConnected) value |= MUNCH_SDE_EXTERNAL_TE;
		if (!munch_mode_poll(mode, MUNCH_MODE_MDP, 0x6b800 + tear[index].offset,
		    UINT32_MAX, value, 0)) return false;
	}
	if (!munch_mode_poll(mode, MUNCH_MODE_MDP, 0x6ba84, 1, 1, 0)) return false;
	return true;
}

static inline uint32_t
munch_mode_vblank_count(struct munch_mode *mode)
{
	if (!mode->configured || !mode->lit || mode->failed || !munch_mode_healthy(mode)) return 0;
	uint32_t frame = munch_mode_read(mode, MUNCH_MODE_MDP,
	    MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_FRAME_COUNT);
	if (!mode->vblankFrameValid) {
		mode->vblankFrame = frame;
		mode->vblankFrameValid = true;
		return 0;
	}
	uint32_t count = frame - mode->vblankFrame;
	mode->vblankFrame = frame;
	return count;
}

static inline bool
munch_mode_vblank(struct munch_mode *mode)
{
	return munch_mode_vblank_count(mode) != 0;
}

static inline bool
munch_mode_set_brightness(struct munch_mode *mode, uint16_t brightness)
{
	if (!mode->configured || mode->failed) return false;
	uint8_t payload[] = { 0x51, (uint8_t)(brightness >> 8), (uint8_t)brightness };
	mode->phase = brightness ? "native-backlight-on" : "native-backlight-off";
	bool sent = munch_mode_command(mode, 0x39, payload, sizeof(payload), true);
	if (sent) {
		mode->lit = brightness != 0;
	}
	if (sent && mode->lit) {
		mode->vblankFrame = munch_mode_read(mode, MUNCH_MODE_MDP,
		    MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_FRAME_COUNT);
		mode->vblankFrameValid = true;
	}
	return sent;
}

static inline bool
munch_mode_light(struct munch_mode *mode)
{
	if (!mode->configured || mode->failed) return false;
	if (mode->lit) return true;
	return munch_mode_set_brightness(mode, 0x0200);
}

static inline bool
munch_mode_panel_on(struct munch_mode *mode)
{
	if (!mode->configured || mode->failed) return false;
	mode->phase = "native-panel-on";
	mode->lit = false;
	const uint8_t *panel_on = mode->refresh_hz == 120 ? munch_l11r_on_120hz : munch_l11r_on;
	unsigned panel_on_size = mode->refresh_hz == 120 ? sizeof(munch_l11r_on_120hz) : sizeof(munch_l11r_on);
	return munch_mode_byte_sequence(mode, panel_on, panel_on_size);
}

#endif
