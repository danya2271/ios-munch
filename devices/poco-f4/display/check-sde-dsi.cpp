#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include "../../../xnu/files/iokit/Kernel/munch-kona-display.h"
#include "sde-dsi-constants.inc"

using IOReturn = int;
using u32 = uint32_t;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnBusy, kIOReturnIOError, kIOReturnTimeout };
static uint32_t registers[0x400 / 4];
static std::vector<std::pair<unsigned, uint32_t>> writes, linuxWrites;
static bool inFlight, stalled, error, stickyDone;
static unsigned sleeps, triggers;

struct Register {
	unsigned offset;
	operator uint32_t() const { return registers[offset / 4]; }
	void operator=(uint32_t value)
	{
		writes.emplace_back(offset, value);
		if (offset == MUNCH_DSI_INT_CTRL) {
			registers[offset / 4] = (value & 0xaaaaaaaa) | (registers[offset / 4] & 0x55555555 & ~value);
			if (stickyDone) registers[offset / 4] |= 1;
		} else {
			registers[offset / 4] = value;
		}
		if (offset == MUNCH_DSI_CMD_DMA_SW_TRIGGER) {
			assert(!inFlight && value == 1);
			assert(registers[MUNCH_DSI_INT_CTRL / 4] & MUNCH_DSI_CMD_DMA_DONE_MASK);
			inFlight = true;
			triggers++;
		}
	}
};
static Register dsiRegister(unsigned offset) { return {offset}; }
static void OSSynchronizeIO() {}
static void delay(unsigned usec) { assert(usec == 1); }
static void IOSleep(unsigned msec)
{
	assert(msec == 1);
	sleeps++;
	if (inFlight && !stalled) {
		registers[MUNCH_DSI_INT_CTRL / 4] |= MUNCH_DSI_CMD_DMA_DONE;
		if (error) registers[MUNCH_DSI_INT_CTRL / 4] |= MUNCH_DSI_ERROR_STATUS;
		inFlight = false;
	}
}

struct IOMobileFramebuffer {
	uint32_t gcc[0x100 / 4] = {}, cc[0x100 / 4] = {}, power[1] = {1u << 31};
	volatile uint32_t *fDSIRegs = registers, *fDisplayGCC = gcc, *fDisplayCC = cc, *fDisplayPower = power;
	void *fDisplayLock = this;
	IOMobileFramebuffer() { gcc[0xc / 4] = cc[0x80 / 4] = 1; }
	IOReturn sendDcsCommand(uint8_t command, uint8_t parameter, bool hasParameter);
};
#include "sde-dsi-sender.inc"

#define BIT(shift) (1u << (shift))
#define DSI_TEST_PATTERN_GEN_CTRL MUNCH_DSI_TPG_CTRL
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL MUNCH_DSI_TPG_DMA_INIT
#define DSI_COMMAND_MODE_DMA_CTRL MUNCH_DSI_COMMAND_MODE_DMA_CTRL
#define DSI_DMA_CMD_LENGTH MUNCH_DSI_DMA_CMD_LENGTH
#define DSI_CMD_MODE_DMA_SW_TRIGGER MUNCH_DSI_CMD_DMA_SW_TRIGGER
#define DSI_TPG_DMA_FIFO_RESET MUNCH_DSI_TPG_DMA_FIFO_RESET
#define DSI_CTRL_HW_CMD_WAIT_FOR_TRIGGER 1
#define DSI_CTRL_HW_DBG(...) ((void)0)
#define DSI_R32(ctrl, offset) ((ctrl)->registers[(offset) / 4])
#define DSI_W32(ctrl, offset, value) ((void)(ctrl), linuxWrites.emplace_back(offset, value))
static void wmb() {}
static void udelay(unsigned usec) { assert(usec == 1); }
struct dsi_ctrl_hw { uint32_t *registers; };
struct dsi_ctrl_cmd_dma_fifo_info { u32 *command, size; bool en_broadcast, is_master, use_lpm; };
#include "sde-dsi-linux.inc"

static void resetRegisters()
{
	memset(registers, 0, sizeof(registers));
	registers[MUNCH_DSI_CTRL / 4] = 0x1f5;
	registers[MUNCH_DSI_COMMAND_MODE_DMA_CTRL / 4] = 0xc1234567;
	registers[MUNCH_DSI_DMA_FIFO_CTRL / 4] = 0x34;
	registers[MUNCH_DSI_TPG_CTRL / 4] = 0x80;
	registers[MUNCH_DSI_DMA_CMD_LENGTH / 4] = 123;
	registers[MUNCH_DSI_INT_CTRL / 4] = (1u << 9) | 1;
	writes.clear();
	linuxWrites.clear();
	inFlight = stalled = error = stickyDone = false;
	sleeps = triggers = 0;
}

int main()
{
	for (uint8_t command : {0x28, 0x29}) {
		resetRegisters();
		IOMobileFramebuffer panel;
		uint32_t original[0x400 / 4];
		memcpy(original, registers, sizeof(original));
		assert(panel.sendDcsCommand(command, 0, false) == kIOReturnSuccess);
		assert(triggers == 1 && sleeps == 1 && !inFlight);
		for (unsigned offset : {MUNCH_DSI_CTRL, MUNCH_DSI_COMMAND_MODE_DMA_CTRL, MUNCH_DSI_DMA_FIFO_CTRL,
		    MUNCH_DSI_TPG_CTRL, MUNCH_DSI_DMA_CMD_LENGTH}) assert(registers[offset / 4] == original[offset / 4]);
		assert(registers[MUNCH_DSI_INT_CTRL / 4] == (original[MUNCH_DSI_INT_CTRL / 4] & ~1u));
		uint32_t packet = command | (0x05u << 16) | (0x80u << 24);
		dsi_ctrl_hw controller = {original};
		dsi_ctrl_cmd_dma_fifo_info message = {&packet, 4, false, false, true};
		dsi_ctrl_hw_cmn_kickoff_fifo_command(&controller, &message, 0);
		dsi_ctrl_hw_cmn_reset_cmd_fifo(&controller);
		size_t cursor = 0;
		for (const auto &expected : linuxWrites) {
			while (cursor < writes.size() && writes[cursor] != expected) cursor++;
			assert(cursor < writes.size());
			cursor++;
		}
	}
	for (unsigned scenario = 0; scenario < 7; scenario++) {
		resetRegisters();
		IOMobileFramebuffer panel;
		if (scenario == 0) panel.gcc[0xc / 4] = 0;
		if (scenario == 1) registers[MUNCH_DSI_CTRL / 4] = 0;
		if (scenario == 2) registers[MUNCH_DSI_STATUS / 4] = 4;
		if (scenario == 3) registers[MUNCH_DSI_INT_CTRL / 4] |= MUNCH_DSI_ERROR_STATUS;
		if (scenario == 4) stickyDone = true;
		if (scenario == 5) stalled = true;
		if (scenario == 6) error = true;
		auto result = panel.sendDcsCommand(0x28, 0, false);
		assert(result == (scenario < 2 ? kIOReturnNotReady : scenario == 2 ? kIOReturnBusy :
		    scenario == 5 ? kIOReturnTimeout : kIOReturnIOError));
		assert(triggers == (scenario >= 5 ? 1u : 0u));
		assert(registers[MUNCH_DSI_COMMAND_MODE_DMA_CTRL / 4] == 0xc1234567);
		assert(registers[MUNCH_DSI_TPG_CTRL / 4] == 0x80);
	}
	puts("Actual DCS sender: fresh W1C completion, mask restoration, faults/timeouts and local Linux FIFO sequence passed");
}
