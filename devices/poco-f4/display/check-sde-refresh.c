#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../xnu/files/iokit/Kernel/munch-sde.h"

struct refresh_fixture {
	struct munch_sde_snapshot saved;
	struct munch_sde_route route;
	struct munch_sde_refresh refresh;
	struct munch_sde_io io;
	uint32_t regs[MUNCH_MDP_SIZE / 4 + 1];
	uint32_t offsets[64], values[64], dsi;
	unsigned writes, guards, fail_guard, delays, status_reads, ignore_write, corrupt_write;
	unsigned busy_status, stuck_line, resume_timeout, underrun;
	uint64_t elapsed;
	bool resumed, sticky_done;
};

static struct refresh_fixture fixture;
static const uint32_t auto_offset = MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH;
static const uint32_t vsync_offset = MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC;
static const uint32_t wr_count_offset = MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_WRCOUNT;
static const uint32_t start_offset = MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_START_POS;
static const uint32_t line_offset = MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE;

static uint32_t
read_register(void *cookie, uint32_t offset)
{
	struct refresh_fixture *state = cookie;
	assert(offset < MUNCH_MDP_SIZE && !(offset & 3));
	if (offset == MUNCH_SDE_DEBUG_STATUS) {
		assert(state->regs[MUNCH_SDE_DEBUG_PERIPH / 4] == 0x405);
		assert(state->regs[MUNCH_SDE_DEBUG_CTRL / 4] == 0x7001);
		state->status_reads++;
		return state->status_reads == state->busy_status ? 0x80 : 0;
	}
	return state->regs[offset / 4];
}

static void
write_register(void *cookie, uint32_t offset, uint32_t value)
{
	struct refresh_fixture *state = cookie;
	assert(state->writes < 64);
	assert(offset == auto_offset || offset == vsync_offset || offset == MUNCH_SDE_DEBUG_CTRL ||
	    offset == MUNCH_SDE_DEBUG_PERIPH || offset == wr_count_offset || offset == MUNCH_SDE_INTR_CLEAR);
	state->offsets[state->writes] = offset;
	state->values[state->writes++] = value;
	if (state->writes == state->ignore_write) return;
	if (state->writes == state->corrupt_write) value ^= 2;
	if (offset == MUNCH_SDE_INTR_CLEAR) {
		if (!state->sticky_done) state->regs[MUNCH_SDE_INTR_STATUS / 4] &= ~value;
		return;
	}
	state->regs[offset / 4] = value;
	if (offset == auto_offset && (value & MUNCH_SDE_AUTOREFRESH_ENABLE)) state->resumed = true;
}

static void
barrier(void *cookie)
{
	assert(cookie == &fixture);
}

static uint32_t
dsi_status(void *cookie)
{
	return ((struct refresh_fixture *)cookie)->dsi;
}

static enum munch_sde_result
guard(void *cookie)
{
	struct refresh_fixture *state = cookie;
	if (++state->guards == state->fail_guard) return MUNCH_SDE_FAULT;
	uint32_t config = state->refresh.owned ? state->refresh.config : state->saved.autorefresh;
	uint32_t vsync = state->refresh.owned ? state->refresh.vsync : state->saved.tear_vsync;
	if (state->regs[auto_offset / 4] != config || state->regs[vsync_offset / 4] != vsync) return MUNCH_SDE_STALE;
	return MUNCH_SDE_OK;
}

static void
delay(void *cookie, uint32_t usec)
{
	struct refresh_fixture *state = cookie;
	state->elapsed += usec;
	state->delays++;
	if (!state->stuck_line) {
		state->regs[line_offset / 4] = 2400;
		state->dsi = 0;
	}
	if (state->delays == state->underrun) state->regs[MUNCH_SDE_INTR_STATUS / 4] |= MUNCH_SDE_INTF1_UNDERRUN;
	if (state->resumed && !state->resume_timeout) state->regs[MUNCH_SDE_INTR_STATUS / 4] |= state->route.done_mask;
}

static void
reset(void)
{
	memset(&fixture, 0, sizeof(fixture));
	fixture.saved.autorefresh = 0x80000001;
	fixture.saved.tear_vsync = 0x00181234;
	fixture.regs[auto_offset / 4] = fixture.saved.autorefresh;
	fixture.regs[vsync_offset / 4] = fixture.saved.tear_vsync;
	fixture.regs[start_offset / 4] = 0x234;
	fixture.regs[wr_count_offset / 4] = 0xabcd0000;
	fixture.regs[line_offset / 4] = 1200;
	fixture.dsi = 4;
	fixture.route.ctl = 0;
	fixture.route.pipe = 0;
	fixture.route.source = 0x9c400000;
	fixture.route.done_mask = 1u << 8;
	fixture.regs[(munch_sde_sspps[0].offset + MUNCH_SDE_SSPP_SRC_ADDR) / 4] = fixture.route.source;
	fixture.regs[MUNCH_SDE_DEBUG_CTRL / 4] = 0x123;
	fixture.regs[MUNCH_SDE_DEBUG_PERIPH / 4] = 0x456;
	fixture.io.cookie = &fixture;
	fixture.io.read = read_register;
	fixture.io.write = write_register;
	fixture.io.barrier = barrier;
	fixture.io.guard = guard;
	fixture.io.dsi_status = dsi_status;
}

static enum munch_sde_result
pause_refresh(void)
{
	return munch_sde_pause_refresh(&fixture.refresh, &fixture.io, &fixture.saved, &fixture.route, delay);
}

static enum munch_sde_result
restore_refresh(void)
{
	return munch_sde_restore_refresh(&fixture.refresh, &fixture.io, &fixture.saved, &fixture.route,
	    fixture.route.source, delay);
}

static enum munch_sde_result
restore_refresh_safe(bool *recovered)
{
	return munch_sde_restore_refresh_safe(&fixture.refresh, &fixture.io, &fixture.saved, &fixture.route,
	    fixture.route.source, delay, recovered);
}

int main(void)
{
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	assert(fixture.refresh.owned && fixture.refresh.paused && !strcmp(fixture.refresh.phase, "paused"));
	assert(fixture.elapsed == 27000 && fixture.delays == 2 && fixture.status_reads == 2);
	assert(fixture.writes == 13);
	assert(fixture.offsets[0] == vsync_offset && fixture.values[0] == 0x00081234);
	assert(fixture.offsets[1] == auto_offset && fixture.values[1] == 1);
	assert(fixture.offsets[11] == wr_count_offset && fixture.values[11] == 0xabcd0234);
	assert(fixture.offsets[12] == vsync_offset && fixture.values[12] == fixture.saved.tear_vsync);
	assert(fixture.regs[wr_count_offset / 4] == 0xabcd0234);
	assert(fixture.regs[MUNCH_SDE_DEBUG_CTRL / 4] == 0x123 && fixture.regs[MUNCH_SDE_DEBUG_PERIPH / 4] == 0x456);
	unsigned pause_guards = fixture.guards;
	assert(pause_refresh() == MUNCH_SDE_OK && fixture.writes == 13 && fixture.elapsed == 27000);
	assert(restore_refresh() == MUNCH_SDE_OK);
	assert(!fixture.refresh.owned && !fixture.refresh.paused && !strcmp(fixture.refresh.phase, "restored"));
	assert(fixture.regs[auto_offset / 4] == 0x80000001 && fixture.regs[vsync_offset / 4] == 0x00181234);
	assert(fixture.writes == 16 && fixture.elapsed == 28000);
	assert(restore_refresh() == MUNCH_SDE_OK && fixture.writes == 16);
	for (unsigned checkpoint = 1; checkpoint <= pause_guards; checkpoint++) {
		reset();
		fixture.fail_guard = checkpoint;
		assert(pause_refresh() == MUNCH_SDE_FAULT);
		assert(!fixture.refresh.paused);
		unsigned writes = fixture.writes;
		if (fixture.refresh.owned) assert(restore_refresh() == MUNCH_SDE_STALE && fixture.writes == writes);
	}
	for (unsigned write = 1; write <= 2; write++) {
		reset();
		fixture.ignore_write = write;
		assert(pause_refresh() == MUNCH_SDE_STALE && !fixture.refresh.paused);
	}
	for (unsigned sample = 1; sample <= 2; sample++) {
		reset();
		fixture.busy_status = sample;
		assert(pause_refresh() == MUNCH_SDE_BUSY && !fixture.refresh.paused);
		assert(!strcmp(fixture.refresh.phase, "rollback") && !fixture.refresh.owned && fixture.refresh.status == 0x80);
		assert(fixture.regs[MUNCH_SDE_DEBUG_CTRL / 4] == 0x123 && fixture.regs[MUNCH_SDE_DEBUG_PERIPH / 4] == 0x456);
	}
	reset();
	fixture.stuck_line = 1;
	assert(pause_refresh() == MUNCH_SDE_TIMEOUT && fixture.elapsed == 50000 && fixture.writes == 29);
	assert(!strcmp(fixture.refresh.phase, "rollback") && !fixture.refresh.owned && !fixture.status_reads);
	for (unsigned sample = 1; sample <= 2; sample++) {
		reset();
		fixture.underrun = sample;
		assert(pause_refresh() == MUNCH_SDE_FAULT && !fixture.refresh.paused);
	}
	reset();
	fixture.regs[(munch_sde_ctl_offsets[0] + MUNCH_SDE_CTL_FLUSH) / 4] = 1;
	assert(pause_refresh() == MUNCH_SDE_BUSY && !fixture.writes);
	reset();
	fixture.saved.autorefresh = fixture.regs[auto_offset / 4] = 1;
	assert(pause_refresh() == MUNCH_SDE_OK && !fixture.writes && !fixture.delays);
	reset();
	fixture.saved.autorefresh = fixture.regs[auto_offset / 4] = 0x80000002;
	assert(pause_refresh() == MUNCH_SDE_UNSUPPORTED && !fixture.writes);
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	fixture.regs[(munch_sde_sspps[0].offset + MUNCH_SDE_SSPP_SRC_ADDR) / 4] += 0x1000;
	assert(restore_refresh() == MUNCH_SDE_STALE && fixture.writes == 13);
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	fixture.sticky_done = true;
	fixture.regs[MUNCH_SDE_INTR_STATUS / 4] = fixture.route.done_mask;
	assert(restore_refresh() == MUNCH_SDE_STALE && !fixture.resumed && fixture.refresh.paused);
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	fixture.resume_timeout = 1;
	assert(restore_refresh() == MUNCH_SDE_TIMEOUT && fixture.refresh.owned && fixture.refresh.paused);
	assert(fixture.elapsed == 77000 && restore_refresh() == MUNCH_SDE_TIMEOUT && fixture.elapsed == 127000);
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	fixture.resume_timeout = 1;
	bool recovered = false;
	assert(restore_refresh_safe(&recovered) == MUNCH_SDE_OK && recovered);
	assert(!fixture.refresh.owned && !fixture.refresh.paused);
	assert(fixture.regs[auto_offset / 4] == fixture.saved.autorefresh &&
	    fixture.regs[vsync_offset / 4] == fixture.saved.tear_vsync);
	fixture.resume_timeout = 0;
	assert(pause_refresh() == MUNCH_SDE_OK && restore_refresh_safe(&recovered) == MUNCH_SDE_OK && !recovered);
	for (unsigned checkpoint = 1; checkpoint <= 2; checkpoint++) {
		reset();
		assert(pause_refresh() == MUNCH_SDE_OK);
		fixture.resume_timeout = 1;
		fixture.corrupt_write = fixture.writes + 2 + checkpoint;
		assert(restore_refresh_safe(&recovered) == MUNCH_SDE_STALE && !recovered);
		assert(fixture.refresh.owned && fixture.refresh.paused);
		assert(!strcmp(fixture.refresh.phase, "rollback-failed"));
	}
	const unsigned recovery_guards[] = {1, 2, 52, 53};
	for (unsigned checkpoint = 0; checkpoint < sizeof(recovery_guards) / sizeof(recovery_guards[0]); checkpoint++) {
		reset();
		assert(pause_refresh() == MUNCH_SDE_OK);
		fixture.resume_timeout = 1;
		fixture.fail_guard = fixture.guards + recovery_guards[checkpoint];
		assert(restore_refresh_safe(&recovered) == MUNCH_SDE_FAULT && !recovered);
		assert(fixture.refresh.owned && fixture.refresh.paused);
	}
	reset();
	assert(pause_refresh() == MUNCH_SDE_OK);
	fixture.regs[(munch_sde_sspps[0].offset + MUNCH_SDE_SSPP_SRC_ADDR) / 4] += 0x1000;
	assert(restore_refresh_safe(&recovered) == MUNCH_SDE_STALE && !recovered);
	assert(fixture.refresh.owned && fixture.refresh.paused && fixture.writes == 13);
	for (unsigned checkpoint = 1; checkpoint <= 2; checkpoint++) {
		reset();
		assert(pause_refresh() == MUNCH_SDE_OK);
		fixture.fail_guard = fixture.guards + checkpoint;
		assert(restore_refresh() == MUNCH_SDE_FAULT && fixture.refresh.owned);
	}
	reset();
	fixture.saved.tear_vsync = fixture.regs[vsync_offset / 4] = 0x00081234;
	assert(pause_refresh() == MUNCH_SDE_OK && restore_refresh() == MUNCH_SDE_OK);
	assert(fixture.regs[vsync_offset / 4] == 0x00081234);
	puts("Autorefresh drain, exact controls, fresh completion, timeouts and fault pinning passed");
	return 0;
}
