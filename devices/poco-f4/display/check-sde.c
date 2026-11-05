#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../xnu/files/iokit/Kernel/munch-sde.h"

struct fixture {
	struct munch_sde_snapshot snapshot;
	struct munch_sde_route route;
	struct munch_smmu_domain domain;
	struct munch_sde_buffer buffers[3];
	struct munch_sde_presenter presenter;
	struct munch_sde_io io;
	uint64_t tables[3][512];
	uint32_t regs[MUNCH_MDP_SIZE / 4 + 1];
	uint32_t offsets[32], values[32];
	unsigned writes, barriers, cleans, pte_reads;
	uint32_t dsi_status;
	enum munch_sde_result guard;
	bool clean_ok, sticky_done;
};

static struct fixture fixture;
static const uint64_t table_base = 0x90000000;
static const uint64_t leaf_flags = (1u << 10) | (1u << 6) | 1;
static uint64_t branch[66 * 512];
static const uint64_t branch_base = 0xa0000000;
static void reset(void);
static void complete_hardware(void);

static bool
read_pte(void *cookie, uint64_t address, uint64_t *value)
{
	struct fixture *state = cookie;
	state->pte_reads++;
	if (address < table_base || address >= table_base + sizeof(state->tables) || (address & 7)) return false;
	unsigned offset = (unsigned)(address - table_base);
	*value = state->tables[offset / 4096][(offset % 4096) / 8];
	return true;
}

static uint32_t
read_register(void *cookie, uint32_t offset)
{
	struct fixture *state = cookie;
	assert(offset < MUNCH_MDP_SIZE && !(offset & 3));
	return state->regs[offset / 4];
}

static bool
read_branch(void *cookie, uint64_t address, uint64_t *value)
{
	if (address >= branch_base && address < branch_base + sizeof(branch) && !(address & 7)) {
		*value = branch[(address - branch_base) / 8];
		return true;
	}
	return read_pte(cookie, address, value);
}

static void
test_branch(void)
{
	unsigned used;
	uint64_t physical;
	for (unsigned slot = 1; slot < 4; slot++) {
		reset();
		uint64_t original[512];
		memcpy(original, fixture.tables[0], sizeof(original));
		assert(munch_sde_build_branch(branch, 66, branch_base, slot << 30, 0,
		    fixture.buffers, &used) == MUNCH_SDE_OK);
		assert(used == 11);
		assert(fixture.buffers[1].iova == (slot << 30) + 0x20000);
		assert(munch_sde_check_buffers(&fixture.domain, read_branch, &fixture,
		    &fixture.route, fixture.buffers) == MUNCH_SDE_MAPPING);
		fixture.tables[0][slot] = branch_base | 3;
		assert(munch_sde_check_buffers(&fixture.domain, read_branch, &fixture,
		    &fixture.route, fixture.buffers) == MUNCH_SDE_OK);
		assert(munch_smmu_translate(&fixture.domain, read_branch, &fixture,
		    fixture.buffers[1].iova - 4096, &physical) == MUNCH_SDE_MAPPING);
		assert(munch_smmu_translate(&fixture.domain, read_branch, &fixture,
		    fixture.buffers[2].iova + fixture.buffers[2].length, &physical) == MUNCH_SDE_MAPPING);
		fixture.tables[0][slot] = 0;
		assert(memcmp(original, fixture.tables[0], sizeof(original)) == 0);
		assert(munch_smmu_translate(&fixture.domain, read_branch, &fixture,
		    fixture.buffers[1].iova, &physical) == MUNCH_SDE_MAPPING);
		assert(munch_smmu_translate(&fixture.domain, read_branch, &fixture,
		    fixture.buffers[0].iova, &physical) == MUNCH_SDE_OK);
	}
	reset();
	struct munch_sde_buffer original[3];
	memcpy(original, fixture.buffers, sizeof(original));
	memset(branch, 0xa5, sizeof(branch));
	assert(munch_sde_build_branch(branch, 2, branch_base, 0x40000000, 0,
	    fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	assert(memcmp(original, fixture.buffers, sizeof(original)) == 0);
	assert(branch[0] == 0xa5a5a5a5a5a5a5a5ULL);
	assert(munch_sde_build_branch(branch, 66, branch_base, 0, 0, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40001000, 0, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 8, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 0, fixture.buffers, NULL) == MUNCH_SDE_MAPPING);
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 0, NULL, &used) == MUNCH_SDE_MAPPING);
	fixture.buffers[1].physical = fixture.buffers[0].physical;
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 0, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	fixture.buffers[1].physical = branch_base;
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 0, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	fixture.buffers[1].physical = UINT64_MAX - 4095;
	assert(munch_sde_build_branch(branch, 66, branch_base, 0x40000000, 0, fixture.buffers, &used) == MUNCH_SDE_MAPPING);
	fixture.buffers[1].physical = 0xb0000000;
	fixture.buffers[2].physical = 0xb4000000;
	fixture.buffers[1].length = fixture.buffers[2].length = 64 * 1024 * 1024;
	assert(munch_sde_build_branch(branch, 66, branch_base, 0xc0000000, 0, fixture.buffers, &used) == MUNCH_SDE_OK);
	assert(used == 66);
	fixture.tables[0][3] = branch_base | 3;
	assert(munch_sde_check_buffers(&fixture.domain, read_branch, &fixture,
	    &fixture.route, fixture.buffers) == MUNCH_SDE_OK);
}

static void
write_register(void *cookie, uint32_t offset, uint32_t value)
{
	struct fixture *state = cookie;
	assert(state->writes < 32);
	assert(offset < MUNCH_MDP_SIZE && !(offset & 3));
	state->offsets[state->writes] = offset;
	state->values[state->writes++] = value;
	if (offset == MUNCH_SDE_INTR_CLEAR) {
		if (!state->sticky_done) state->regs[MUNCH_SDE_INTR_STATUS / 4] &= ~value;
	} else {
		state->regs[offset / 4] = value;
	}
}

static void
barrier(void *cookie)
{
	struct fixture *state = cookie;
	state->barriers++;
}

static bool
clean(void *cookie, unsigned index, uint32_t length)
{
	struct fixture *state = cookie;
	assert(index == 1 || index == 2);
	assert(length == state->buffers[index].length);
	state->cleans++;
	return state->clean_ok;
}

static enum munch_sde_result
guard(void *cookie)
{
	return ((struct fixture *)cookie)->guard;
}

static uint32_t
dsi_status(void *cookie)
{
	return ((struct fixture *)cookie)->dsi_status;
}

static void
reset(void)
{
	memset(&fixture, 0, sizeof(fixture));
	fixture.clean_ok = true;
	fixture.snapshot.revision = 0x60000000;
	fixture.snapshot.mux = 2;
	fixture.snapshot.tear_enable = 1;
	fixture.snapshot.dsi_ctrl = 0x1f5;
	fixture.snapshot.ctls[2].top = MUNCH_SDE_CTL_CMD_MODE;
	fixture.snapshot.ctls[2].active = MUNCH_SDE_INTF1_ACTIVE;
	fixture.snapshot.ctls[2].dsc = 1;
	fixture.snapshot.mixer_size[3] = (2400u << 16) | 1080;
	fixture.snapshot.ctls[2].layers[0][3] = 1u << 18;
	fixture.snapshot.pipes[4].size = fixture.snapshot.pipes[4].out_size = (2400u << 16) | 1080;
	fixture.snapshot.pipes[4].source = 0x01000000;
	fixture.snapshot.pipes[4].stride = 4320;
	fixture.snapshot.pipes[4].format = 0x236ff;
	fixture.snapshot.pipes[4].unpack = 0x03020001;
	fixture.domain.cbar = 1u << 16;
	fixture.domain.cba2r = fixture.domain.sctlr = 1;
	fixture.domain.tcr = 32;
	fixture.domain.ttbr = table_base;
	fixture.domain.mair = 0x44;
	fixture.tables[0][0] = 0x80000000ULL | leaf_flags;
	for (unsigned index = 0; index < 3; index++) {
		fixture.buffers[index].iova = 0x01000000 * (index + 1);
		fixture.buffers[index].physical = 0x80000000ULL + fixture.buffers[index].iova;
		fixture.buffers[index].length = (4320u * 2400 + 4095) & ~4095u;
	}
	fixture.io.cookie = &fixture;
	fixture.io.read = read_register;
	fixture.io.write = write_register;
	fixture.io.barrier = barrier;
	fixture.io.clean = clean;
	fixture.io.guard = guard;
	fixture.io.dsi_status = dsi_status;
	assert(munch_sde_find_route(&fixture.snapshot, &fixture.route) == MUNCH_SDE_OK);
}

static enum munch_sde_result
prepare(void)
{
	return munch_sde_prepare(&fixture.presenter, &fixture.io, &fixture.snapshot,
	    &fixture.domain, read_pte, fixture.buffers);
}

static void
reset_vig_border(void)
{
	reset();
	fixture.snapshot.ctls[0] = fixture.snapshot.ctls[2];
	memset(&fixture.snapshot.ctls[2], 0, sizeof(fixture.snapshot.ctls[2]));
	memset(fixture.snapshot.ctls[0].layers, 0, sizeof(fixture.snapshot.ctls[0].layers));
	fixture.snapshot.ctls[0].layers[0][0] = 0x01000001;
	fixture.snapshot.pipes[0] = fixture.snapshot.pipes[4];
	memset(&fixture.snapshot.pipes[4], 0, sizeof(fixture.snapshot.pipes[4]));
	fixture.snapshot.pipes[0].format = 0x237ff;
	fixture.snapshot.mixer_size[0] = fixture.snapshot.mixer_size[3];
	fixture.snapshot.mixer_size[3] = 0;
	fixture.snapshot.mux = 0;
	fixture.snapshot.autorefresh = 1;
}

static void
test_vig_border(void)
{
	static const unsigned shifts[8] = { 0, 3, 6, 26, 18, 21, 0, 4 };
	for (unsigned pipe = 0; pipe < 8; pipe++) {
		for (unsigned combination = 0; combination < 56; combination++) {
			unsigned stage = combination / 8, variant = combination % 8;
			reset_vig_border();
			fixture.snapshot.pipes[pipe] = fixture.snapshot.pipes[0];
			if (pipe) memset(&fixture.snapshot.pipes[0], 0, sizeof(fixture.snapshot.pipes[0]));
			fixture.snapshot.ctls[0].layers[0][0] = 1u << 24;
			fixture.snapshot.ctls[0].layers[pipe < 6 ? 0 : 2][0] |= (stage + 1) << shifts[pipe];
			fixture.snapshot.mixers[0].opmode = 1u << stage;
			unsigned blend = stage ? stage - 1 : 0;
			fixture.snapshot.mixers[0].blend_op[blend] = variant & 1 ? 0x400 : 0x100;
			fixture.snapshot.mixers[0].blend_alpha[blend] = 0x00ff0000;
			if (pipe < 4) {
				fixture.snapshot.pipes[pipe].scaler = 0x60004400;
				fixture.snapshot.pipes[pipe].csc = variant & 2;
			}
			if (variant & 4) {
				fixture.snapshot.pipes[pipe].opmode = 0x80000000;
				for (unsigned component = 0; component < 3; component++)
					fixture.snapshot.pipes[pipe].pixel_ext[component].required = 0x09600438;
			}
			struct munch_sde_snapshot before = fixture.snapshot;
			assert(prepare() == MUNCH_SDE_OK);
			assert(fixture.presenter.route.ctl == 0 && fixture.presenter.route.mixer == 0);
			assert(fixture.presenter.route.pipe == pipe && fixture.presenter.route.stage == stage);
			assert(fixture.writes == 0);
			unsigned buffer;
			assert(munch_sde_acquire(&fixture.presenter, &buffer) == MUNCH_SDE_OK && buffer == 1);
			assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_OK);
			assert(fixture.writes == 4);
			assert(fixture.offsets[1] == munch_sde_sspps[pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR);
			assert(fixture.values[1] == fixture.buffers[1].iova);
			assert(fixture.offsets[2] == 0x2018 && fixture.values[2] == fixture.presenter.route.flush_mask);
			assert(fixture.offsets[3] == 0x201c && fixture.values[3] == 1);
			complete_hardware();
			assert(munch_sde_poll(&fixture.presenter, 200) == MUNCH_SDE_OK);
			assert(fixture.presenter.completed == 1 && fixture.presenter.front == 1);
			assert(!memcmp(&before, &fixture.snapshot, sizeof(before)));
		}
	}
}

static void
test_dsi_link(void)
{
	for (unsigned checks = 0; checks < 4; checks++) {
		reset();
		uint32_t control = 0x1f5 | ((checks & 1) << 20) | ((checks >> 1) << 24);
		fixture.snapshot.mux = 0;
		fixture.snapshot.autorefresh = 1;
		fixture.snapshot.dsi_ctrl = control;
		assert(prepare() == MUNCH_SDE_OK);
		assert(fixture.presenter.route.pingpong == 0 && fixture.presenter.route.done_mask == (1u << 8));
		assert(fixture.snapshot.dsi_ctrl == control && fixture.writes == 0);
	}
	for (unsigned bit = 0; bit < 32; bit++) {
		if (bit == 20 || bit == 24) continue;
		reset();
		fixture.snapshot.dsi_ctrl = 0x1f5 ^ (1u << bit);
		assert(prepare() == MUNCH_SDE_UNSUPPORTED);
		assert(!fixture.presenter.armed && fixture.writes == 0);
	}
	reset();
	fixture.snapshot.dsi_ctrl = 5;
	assert(prepare() == MUNCH_SDE_UNSUPPORTED);
	assert(!fixture.presenter.armed && fixture.writes == 0);
	reset();
	fixture.snapshot.dsi_ctrl = 0x1f5;
	fixture.snapshot.autorefresh = (1u << 31) | 1;
	fixture.snapshot.dsi_status = 4;
	assert(prepare() == MUNCH_SDE_OK);
	assert(fixture.presenter.armed && fixture.writes == 0);
	fixture.regs[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4] = (1u << 31) | 1;
	unsigned index;
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK);
	assert(munch_sde_submit(&fixture.presenter, 0, 50000) == MUNCH_SDE_BUSY);
	assert(!fixture.writes && !fixture.presenter.submitted);
	for (unsigned bit = 1; bit < 31; bit++) {
		reset();
		fixture.snapshot.autorefresh = (1u << 31) | 1 | (1u << bit);
		assert(prepare() == MUNCH_SDE_UNSUPPORTED && !fixture.writes);
	}
}

static void
expect_route_check(enum munch_sde_result expected, const char *check, int ctl, int mixer, int pipe)
{
	struct munch_sde_snapshot before = fixture.snapshot;
	struct munch_sde_route route = fixture.route;
	struct munch_sde_route_diagnostic diagnostic;
	assert(munch_sde_find_route_checked(&fixture.snapshot, &route, &diagnostic) == expected);
	assert(!strcmp(diagnostic.check, check));
	assert(diagnostic.ctl == ctl && diagnostic.mixer == mixer && diagnostic.pipe == pipe);
	if (expected != MUNCH_SDE_OK) assert(!memcmp(&route, &fixture.route, sizeof(route)));
	assert(munch_sde_find_route(&fixture.snapshot, &route) == expected);
	assert(prepare() == expected);
	assert(!memcmp(&fixture.snapshot, &before, sizeof(before)) && fixture.writes == 0);
}

static void
test_route_diagnostics(void)
{
	reset();
	expect_route_check(MUNCH_SDE_OK, "ok", 2, 3, 4);
	reset();
	fixture.snapshot.revision = 0;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "revision", -1, -1, -1);
	reset();
	fixture.snapshot.mux = 4;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "pingpong-mux", -1, -1, -1);
	reset();
	fixture.snapshot.dsi_ctrl = 5;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "dsi-link", -1, -1, -1);
	reset();
	fixture.snapshot.tear_enable = 0;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "tear-disabled", -1, -1, -1);
	reset();
	fixture.snapshot.ctls[5].active = 1;
	expect_route_check(MUNCH_SDE_ROUTE, "ctl-intf-mask", 5, -1, -1);
	reset();
	fixture.snapshot.ctls[5].active = MUNCH_SDE_INTF1_ACTIVE;
	expect_route_check(MUNCH_SDE_ROUTE, "multiple-ctls", 5, -1, -1);
	reset();
	fixture.snapshot.ctls[2].active = 0;
	expect_route_check(MUNCH_SDE_ROUTE, "no-intf1-ctl", -1, -1, -1);
	reset();
	fixture.snapshot.ctls[2].top |= 1;
	expect_route_check(MUNCH_SDE_ROUTE, "ctl-mode", 2, -1, -1);
	for (unsigned index = 0; index < 4; index++) {
		reset();
		uint32_t *fields[] = { &fixture.snapshot.ctls[2].merge, &fixture.snapshot.ctls[2].wb,
		    &fixture.snapshot.ctls[2].cwb, &fixture.snapshot.ctls[2].cdm };
		*fields[index] = 1;
		expect_route_check(MUNCH_SDE_ROUTE, "ctl-aux-blocks", 2, -1, -1);
	}
	const uint32_t dsc_masks[] = { 0, 3, 16 };
	for (unsigned index = 0; index < 3; index++) {
		reset();
		fixture.snapshot.ctls[2].dsc = dsc_masks[index];
		expect_route_check(MUNCH_SDE_ROUTE, "ctl-dsc-mask", 2, -1, -1);
	}
	reset();
	fixture.snapshot.ctls[2].layers[0][4] = 1u << 18;
	expect_route_check(MUNCH_SDE_ROUTE, "multiple-mixers", 2, 4, -1);
	for (unsigned extension = 1; extension < 4; extension++) {
		reset();
		fixture.snapshot.ctls[2].layers[extension][3] = 1u << 8;
		expect_route_check(MUNCH_SDE_ROUTE, "layer-extensions", 2, 3, -1);
	}
	reset();
	fixture.snapshot.ctls[2].layers[0][3] = 2u << 18;
	expect_route_check(MUNCH_SDE_ROUTE, "border-stage", 2, 3, 4);
	reset();
	fixture.snapshot.ctls[2].layers[0][3] = 3u << 18;
	expect_route_check(MUNCH_SDE_ROUTE, "border-stage", 2, 3, 4);
	reset();
	fixture.snapshot.ctls[2].layers[0][3] |= 1u << 21;
	expect_route_check(MUNCH_SDE_ROUTE, "multiple-pipes", 2, 3, 5);
	reset();
	fixture.snapshot.ctls[2].layers[0][3] |= 1u << 9;
	expect_route_check(MUNCH_SDE_ROUTE, "layer-encoding", 2, 3, 4);
	reset();
	fixture.snapshot.ctls[2].layers[0][3] = 0;
	expect_route_check(MUNCH_SDE_ROUTE, "no-staged-pipe", 2, -1, -1);
	reset();
	fixture.snapshot.pipes[4].opmode = 1;
	expect_route_check(MUNCH_SDE_FORMAT, "pipe-opmode", 2, 3, 4);
	reset();
	fixture.snapshot.autorefresh |= 1u << 31;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "autorefresh-config", 2, 3, 4);
	reset();
	fixture.snapshot.ctls[2].flush = 1;
	expect_route_check(MUNCH_SDE_BUSY, "transfer-busy", 2, 3, 4);
}

static void
test_vig_rejection(void)
{
	for (unsigned bit = 0; bit < 32; bit++) {
		if (bit == 10 || bit == 14 || bit == 29 || bit == 30) continue;
		reset_vig_border();
		fixture.snapshot.pipes[0].scaler = 1u << bit;
		expect_route_check(MUNCH_SDE_UNSUPPORTED, "vig-scaler", 0, 0, 0);
	}
	for (unsigned field = 0; field < 3; field++) {
		reset_vig_border();
		uint32_t *fields[] = { &fixture.snapshot.pipes[0].csc,
		    &fixture.snapshot.pipes[0].pre_downscale, &fixture.snapshot.pipes[0].decimation };
		*fields[field] = 1;
		expect_route_check(MUNCH_SDE_UNSUPPORTED, field ? "vig-resample" : "vig-csc", 0, 0, 0);
	}
	reset_vig_border();
	fixture.snapshot.mixers[0].opmode = 1u << 31;
	expect_route_check(MUNCH_SDE_ROUTE, "mixer-mode", 0, 0, 0);
	reset_vig_border();
	fixture.snapshot.ctls[0].layers[0][0] = 0x01000002;
	expect_route_check(MUNCH_SDE_ROUTE, "mixer-blend", 0, 0, 0);
	for (unsigned combination = 0; combination < 64; combination++) {
		unsigned variant = combination / 32, bit = combination % 32;
		reset_vig_border();
		fixture.snapshot.ctls[0].layers[0][0] = 0x01000002;
		fixture.snapshot.mixers[0].blend_op[0] = (variant ? 0x400 : 0x100) ^ (1u << bit);
		fixture.snapshot.mixers[0].blend_alpha[0] = 0x00ff0000;
		expect_route_check(MUNCH_SDE_ROUTE, "mixer-blend", 0, 0, 0);
		reset_vig_border();
		fixture.snapshot.ctls[0].layers[0][0] = 0x01000002;
		fixture.snapshot.mixers[0].blend_op[0] = variant ? 0x400 : 0x100;
		fixture.snapshot.mixers[0].blend_alpha[0] = 0x00ff0000 ^ (1u << bit);
		expect_route_check(MUNCH_SDE_ROUTE, "mixer-blend", 0, 0, 0);
	}
	for (unsigned bit = 0; bit < 32; bit++) {
		if (bit == 1) continue;
		reset_vig_border();
		fixture.snapshot.pipes[0].csc = 2 | (1u << bit);
		expect_route_check(MUNCH_SDE_UNSUPPORTED, "vig-csc", 0, 0, 0);
	}
	for (unsigned stage = 0; stage < 11; stage++) {
		reset_vig_border();
		fixture.snapshot.mixers[0].blend_op[stage] = 1u << 16;
		expect_route_check(MUNCH_SDE_ROUTE, "mixer-dim-layer", 0, 0, 0);
	}
	reset_vig_border();
	fixture.snapshot.ctls[0].layers[0][0] |= 1u << 3;
	expect_route_check(MUNCH_SDE_ROUTE, "multiple-pipes", 0, 0, 1);
	reset_vig_border();
	fixture.snapshot.ctls[0].layers[0][0] = 0x01000003;
	expect_route_check(MUNCH_SDE_ROUTE, "mixer-blend", 0, 0, 0);
	for (unsigned encoding = 8; encoding < 16; encoding++) {
		reset_vig_border();
		fixture.snapshot.ctls[0].layers[0][0] = 0x01000000;
		fixture.snapshot.ctls[0].layers[2][0] = encoding;
		expect_route_check(MUNCH_SDE_ROUTE, "blend-stage", 0, 0, 6);
	}
}

static void
test_transcribed_route(void)
{
	reset_vig_border();
	fixture.snapshot.revision = 0;
	fixture.snapshot.ctls[0].top = 0x21f00;
	fixture.snapshot.ctls[0].layers[0][0] = 0x01000005;
	fixture.snapshot.pipes[0].format = 0x23ff7;
	fixture.snapshot.pipes[0].csc = 2;
	fixture.snapshot.mixers[0].blend_op[0] = 0x400;
	fixture.snapshot.mixers[0].blend_alpha[0] = 0x00ff0000;
	expect_route_check(MUNCH_SDE_UNSUPPORTED, "revision", -1, -1, -1);
	fixture.snapshot.revision = 0x60000000;
	expect_route_check(MUNCH_SDE_ROUTE, "ctl-mode", 0, -1, -1);
	fixture.snapshot.ctls[0].top = 0x20000;
	expect_route_check(MUNCH_SDE_FORMAT, "pipe-format", 0, 0, 0);
	fixture.snapshot.pipes[0].format = 0x237ff;
	expect_route_check(MUNCH_SDE_ROUTE, "mixer-blend", 0, 0, 0);
	fixture.snapshot.mixers[0].blend_op[3] = 0x400;
	fixture.snapshot.mixers[0].blend_alpha[3] = 0x00ff0000;
	expect_route_check(MUNCH_SDE_OK, "ok", 0, 0, 0);
	assert(fixture.presenter.route.stage == 4);
}

static bool
r7_layout_rejected(const struct munch_sde_pipe_state *pipe, uint32_t mixer_size)
{
	return pipe->size != 0x09600438 || mixer_size != pipe->size || pipe->out_size != pipe->size ||
	    pipe->src_xy || pipe->out_xy || pipe->opmode || pipe->secure || pipe->multirect ||
	    pipe->stride < 4320 || pipe->stride > 0xffff || (pipe->stride & 3) || (pipe->source & 0xfff) ||
	    pipe->source < 0x20000 || (pipe->format != 0x236ff && pipe->format != 0x237ff) ||
	    pipe->unpack != 0x03020001;
}

static void
test_layout_diagnostics(void)
{
	struct munch_sde_pipe_state *pipe = &fixture.snapshot.pipes[0];
	uint32_t *fields[] = { &pipe->size, &fixture.snapshot.mixer_size[0], &pipe->out_size,
	    &pipe->src_xy, &pipe->out_xy, &pipe->opmode, &pipe->secure, &pipe->multirect,
	    &pipe->stride, &pipe->stride, &pipe->stride, &pipe->source, &pipe->source, &pipe->format, &pipe->unpack };
	const char *names[] = { "pipe-size", "pipe-mixer-size", "pipe-out-size", "pipe-src-xy", "pipe-out-xy",
	    "pipe-opmode", "pipe-secure", "pipe-multirect", "pipe-stride-min", "pipe-stride-max",
	    "pipe-stride-align", "pipe-source-align", "pipe-source-min", "pipe-format", "pipe-unpack" };
	const uint32_t values[] = { 0x09600439, 0x09600439, 0x09600439, 1, 1, 1, 1, 1,
	    4316, 0x10000, 4321, 0x9c000004, 0x10000, 0x23ff7, 0x03020100 };
	const uint32_t expected[] = { 0x09600438, 0x09600438, 0x09600438, 0, 0, 0x7fffffff, 0, 0,
	    4320, 0xffff, 3, 0xfff, 0x20000, 0x236ff, 0x03020001 };
	const char *rules[] = { "equal", "equal", "equal", "equal", "equal", "bits-clear", "equal", "equal",
	    "at-least", "at-most", "bits-clear", "bits-clear", "at-least", "one-of", "equal" };
	for (unsigned index = 0; index < 15; index++) {
		reset_vig_border();
		*fields[index] = values[index];
		struct munch_sde_layout_check first;
		uint32_t failures = munch_sde_pipe_layout_failures(pipe, fixture.snapshot.mixer_size[0], &first);
		assert(failures == (index ? 1u << index : 7));
		assert(!strcmp(first.name, names[index]) && first.actual == values[index] && first.expected == expected[index]);
		assert(!strcmp(munch_sde_layout_rule_name(first.rule), rules[index]));
		assert(first.alternative == (index == 13 ? 0x237ff : 0));
		expect_route_check(MUNCH_SDE_FORMAT, names[index], 0, 0, 0);
		for (unsigned bit = 0; bit < 32; bit++) {
			reset_vig_border();
			*fields[index] ^= 1u << bit;
			struct munch_sde_snapshot before = fixture.snapshot;
			failures = munch_sde_pipe_layout_failures(pipe, fixture.snapshot.mixer_size[0], &first);
			assert((failures != 0) == r7_layout_rejected(pipe, fixture.snapshot.mixer_size[0]));
			assert(failures == munch_sde_pipe_layout_failures(pipe, fixture.snapshot.mixer_size[0], NULL));
			assert(!memcmp(&before, &fixture.snapshot, sizeof(before)) && !fixture.writes);
		}
	}
	const uint32_t strides[] = { 0, 4316, 4319, 4320, 4321, 4324, 0xfffc, 0xffff, 0x10000, 0x100010e0, UINT32_MAX };
	for (unsigned index = 0; index < sizeof(strides) / sizeof(strides[0]); index++) {
		reset_vig_border();
		pipe->stride = strides[index];
		assert((munch_sde_pipe_layout_failures(pipe, fixture.snapshot.mixer_size[0], NULL) != 0) ==
		    r7_layout_rejected(pipe, fixture.snapshot.mixer_size[0]));
	}
}

static void
test_r7_transcription(void)
{
	reset_vig_border();
	fixture.snapshot.ctls[0].layers[0][0] = 0x01800005;
	fixture.snapshot.pipes[0].source = 0x9c000000;
	fixture.snapshot.pipes[0].csc = 2;
	for (unsigned index = 0; index < 6; index++) {
		fixture.snapshot.mixers[0].blend_op[index] = 0x400;
		fixture.snapshot.mixers[0].blend_alpha[index] = 0x00ff0000;
		if (index) fixture.snapshot.ctls[index].top = 0x1f00;
	}
	struct munch_sde_route route;
	struct munch_sde_route_diagnostic diagnostic;
	assert(munch_sde_find_route_checked(&fixture.snapshot, &route, &diagnostic) == MUNCH_SDE_ROUTE);
	assert(!strcmp(diagnostic.check, "multiple-pipes") && diagnostic.pipe == 5);
	fixture.snapshot.ctls[0].layers[0][0] = 0x01000005;
	assert(munch_sde_find_route_checked(&fixture.snapshot, &route, &diagnostic) == MUNCH_SDE_OK);
	assert(route.pipe == 0 && route.stage == 4 && route.source == 0x9c000000 && !fixture.writes);
	struct munch_sde_layout_check first;
	assert(!munch_sde_pipe_layout_failures(&fixture.snapshot.pipes[0], fixture.snapshot.mixer_size[0], &first));
	assert(!strcmp(first.name, "ok") && !first.actual && !first.expected && !first.alternative);
}

static void
reset_pixel_ext(void)
{
	reset_vig_border();
	fixture.snapshot.pipes[0].opmode = 0x80000000;
	for (unsigned component = 0; component < 3; component++)
		fixture.snapshot.pipes[0].pixel_ext[component].required = 0x09600438;
}

static void
test_pixel_ext(void)
{
	const char *names[] = { "pipe-pe-c0-lr", "pipe-pe-c0-tb", "pipe-pe-c0-size",
	    "pipe-pe-c1c2-lr", "pipe-pe-c1c2-tb", "pipe-pe-c1c2-size",
	    "pipe-pe-c3-lr", "pipe-pe-c3-tb", "pipe-pe-c3-size" };
	reset_pixel_ext();
	assert(r7_layout_rejected(&fixture.snapshot.pipes[0], fixture.snapshot.mixer_size[0]));
	expect_route_check(MUNCH_SDE_OK, "ok", 0, 0, 0);
	for (unsigned component = 0; component < 3; component++) {
		for (unsigned field = 0; field < 3; field++) {
			for (unsigned bit = 0; bit < 32; bit++) {
				reset_pixel_ext();
				struct munch_sde_pipe_state *pipe = &fixture.snapshot.pipes[0];
				uint32_t *fields[] = { &pipe->pixel_ext[component].lr, &pipe->pixel_ext[component].tb,
				    &pipe->pixel_ext[component].required };
				*fields[field] ^= 1u << bit;
				struct munch_sde_layout_check first;
				assert(munch_sde_pipe_layout_failures(pipe, fixture.snapshot.mixer_size[0], &first) ==
				    (1u << (15 + component * 3 + field)));
				assert(first.actual == *fields[field] && first.expected == (field == 2 ? 0x09600438 : 0));
				expect_route_check(MUNCH_SDE_FORMAT, names[component * 3 + field], 0, 0, 0);
			}
		}
	}
	for (unsigned bit = 0; bit < 31; bit++) {
		reset_pixel_ext();
		fixture.snapshot.pipes[0].opmode |= 1u << bit;
		expect_route_check(MUNCH_SDE_FORMAT, "pipe-opmode", 0, 0, 0);
	}
	reset_vig_border();
	memset(fixture.snapshot.pipes[0].pixel_ext, 0xff, sizeof(fixture.snapshot.pipes[0].pixel_ext));
	expect_route_check(MUNCH_SDE_OK, "ok", 0, 0, 0);
	assert(!munch_sde_pipe_layout_failures(&fixture.snapshot.pipes[0], fixture.snapshot.mixer_size[0], NULL));
}

static void
test_routes(void)
{
	static const unsigned shifts[8] = { 0, 3, 6, 26, 18, 21, 0, 4 };
	static const unsigned flush[8] = { 0, 1, 2, 18, 3, 4, 5, 19 };
	for (unsigned index = 0; index < 8; index++) {
		reset();
		fixture.snapshot.pipes[index] = fixture.snapshot.pipes[4];
		fixture.snapshot.ctls[2].layers[0][3] = 0;
		fixture.snapshot.ctls[2].layers[index < 6 ? 0 : 2][3] = 1u << shifts[index];
		assert(munch_sde_find_route(&fixture.snapshot, &fixture.route) == MUNCH_SDE_OK);
		assert(fixture.route.ctl == 2 && fixture.route.mixer == 3 && fixture.route.pingpong == 2);
		assert(fixture.route.pipe == index && fixture.route.flush_mask == (1u << flush[index]));
		assert(prepare() == MUNCH_SDE_OK);
	}
	reset();
	fixture.snapshot.ctls[0] = fixture.snapshot.ctls[2];
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.ctls[2].layers[0][2] = 1;
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.ctls[2].layers[2][3] = 1u << 8;
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.ctls[2].layers[0][3] |= 1u << 9;
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.ctls[2].merge = 1;
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.ctls[2].dsc = 3;
	assert(prepare() == MUNCH_SDE_ROUTE);
	reset();
	fixture.snapshot.mixer_size[3] = (2400u << 16) | 540;
	assert(prepare() == MUNCH_SDE_FORMAT);
	reset();
	fixture.snapshot.autorefresh = 1u << 31;
	assert(prepare() == MUNCH_SDE_UNSUPPORTED);
	reset();
	fixture.snapshot.pipes[4].opmode = 1;
	assert(prepare() == MUNCH_SDE_FORMAT);
	reset();
	fixture.snapshot.pipes[4].secure = 1;
	assert(prepare() == MUNCH_SDE_FORMAT);
	reset();
	fixture.snapshot.pipes[4].unpack = 0x03010002;
	assert(prepare() == MUNCH_SDE_FORMAT);
	reset();
	fixture.snapshot.dsi_status = 4;
	assert(prepare() == MUNCH_SDE_BUSY);
	assert(fixture.writes == 0);
}

static void
test_streams(void)
{
	struct munch_smmu_stream streams[3] = { { 0, 0 }, { 0x84020820, 7 }, { 0, 0 } };
	struct munch_smmu_domain domain = { 0 };
	uint32_t idr0 = (1u << 27) | 3;
	assert(munch_smmu_find_stream(0, idr0, 16, streams, &domain) == MUNCH_SDE_OK);
	assert(domain.stream == 1 && domain.context == 7);
	streams[2] = streams[1];
	assert(munch_smmu_find_stream(0, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
	streams[2].smr = 0;
	streams[1].smr = 0x80000820;
	assert(munch_smmu_find_stream(0, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
	streams[1].smr = 0x84020821;
	assert(munch_smmu_find_stream(0, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
	streams[1].smr = 0x84020820;
	streams[1].s2cr = (1u << 16) | 7;
	assert(munch_smmu_find_stream(0, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
	streams[1].s2cr = 7;
	assert(munch_smmu_find_stream(1, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
	assert(munch_smmu_find_stream(0, idr0, 7, streams, &domain) == MUNCH_SDE_DOMAIN);
	streams[1].smr = 0x04020820;
	streams[1].s2cr = MUNCH_SMMU_S2CR_EXIDVALID | 7;
	assert(munch_smmu_find_stream(8, idr0 | MUNCH_SMMU_IDR0_EXIDS, 16, streams, &domain) == MUNCH_SDE_OK);
	assert(munch_smmu_find_stream(8, idr0, 16, streams, &domain) == MUNCH_SDE_DOMAIN);
}

static void
test_translation(void)
{
	uint64_t physical;
	reset();
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_OK);
	assert(physical == 0x81234567);
	fixture.tables[0][0] = (table_base + 4096) | 3;
	fixture.tables[1][9] = 0x81200000 | leaf_flags;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_OK);
	assert(physical == 0x81234567);
	fixture.tables[1][9] = (table_base + 8192) | 3;
	fixture.tables[2][0x34] = 0x81234000 | leaf_flags | 2;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_OK);
	assert(physical == 0x81234567);
	fixture.tables[2][0x34] &= ~(1u << 10);
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_MAPPING);
	reset();
	fixture.tables[0][0] |= 0x1000;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_MAPPING);
	reset();
	fixture.domain.tcr |= 1u << 14;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0, &physical) == MUNCH_SDE_DOMAIN);
	reset();
	fixture.domain.cbar = 3u << 16;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0, &physical) == MUNCH_SDE_DOMAIN);
	reset();
	fixture.domain.fsr = 2;
	assert(prepare() == MUNCH_SDE_FAULT);
	reset();
	fixture.domain.mair = 4;
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.domain.ttbr = 0xdead0000;
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.domain.ttbr |= 0x1234ULL << 48;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0x1234567, &physical) == MUNCH_SDE_OK);
	assert(physical == 0x81234567);
	fixture.tables[0][0] = 0x180000000ULL | leaf_flags;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0, &physical) == MUNCH_SDE_MAPPING);
	fixture.domain.tcr2 = 1;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0, &physical) == MUNCH_SDE_OK);
	assert(physical == 0x180000000ULL);
	fixture.domain.tcr2 = 6;
	assert(munch_smmu_translate(&fixture.domain, read_pte, &fixture, 0, &physical) == MUNCH_SDE_DOMAIN);
	reset();
	fixture.domain.sctlr |= 1u << 4;
	assert(prepare() == MUNCH_SDE_DOMAIN);
	reset();
	fixture.tables[0][0] &= ~(1u << 6);
	assert(prepare() == MUNCH_SDE_MAPPING);
}

static void
test_buffers(void)
{
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(fixture.pte_reads == 3 * fixture.buffers[0].length / 4096);
	assert(fixture.writes == 0);
	assert(prepare() == MUNCH_SDE_BUSY);
	reset();
	fixture.io.write = NULL;
	assert(prepare() == MUNCH_SDE_DISABLED);
	reset();
	fixture.buffers[2] = fixture.buffers[1];
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.buffers[1].physical = fixture.buffers[0].physical;
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.buffers[2].iova = 0xfffff000;
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.buffers[1].length -= 4096;
	assert(prepare() == MUNCH_SDE_MAPPING);
	reset();
	fixture.tables[0][0] = (table_base + 4096) | 3;
	for (unsigned index = 0; index < 512; index++)
		fixture.tables[1][index] = (0x80000000ULL + index * 0x200000) | leaf_flags;
	fixture.tables[1][16] = (table_base + 8192) | 3;
	for (unsigned index = 0; index < 512; index++)
		fixture.tables[2][index] = (0x82000000ULL + index * 4096) | leaf_flags | 2;
	fixture.tables[2][127] = 0;
	assert(prepare() == MUNCH_SDE_MAPPING);
	assert(fixture.writes == 0);
}

static void
test_smmu_status(void)
{
	for (unsigned format = 0; format < 4; format++) {
		for (unsigned bit = 0; bit < 32; bit++) {
			reset();
			fixture.domain.fsr = (format << 9) | (1u << bit);
			bool fault = (bit >= 1 && bit <= 8) || bit >= 30;
			uint64_t physical = UINT64_MAX;
			struct munch_smmu_walk walk;
			assert(munch_smmu_translate_checked(&fixture.domain, read_pte, &fixture, 0x1234567,
			    &physical, &walk) == (fault ? MUNCH_SDE_FAULT : MUNCH_SDE_OK));
			assert(!strcmp(walk.check, fault ? "fsr-fault" : "ok"));
			assert(walk.iova == 0x1234567 && walk.read == !fault);
			assert(fixture.pte_reads == (fault ? 0u : 1u));
			assert(physical == (fault ? UINT64_MAX : 0x81234567));
			assert(walk.level == (fault ? 4u : 1u));
			assert(!fixture.writes);
		}
		reset();
		fixture.domain.fsr = format << 9;
		assert(prepare() == MUNCH_SDE_OK && !fixture.writes);
	}
}

static void
test_smmu_bypass(void)
{
	for (unsigned scenario = 0; scenario < 45; scenario++) {
		reset();
		fixture.domain.cbar = 0x1f000;
		fixture.domain.cba2r = 1;
		fixture.domain.s2cr = 4;
		fixture.domain.sctlr = 0xe0;
		fixture.domain.tcr = 0;
		fixture.domain.tcr2 = 0x30071;
		fixture.domain.ttbr = fixture.domain.mair = 0;
		fixture.domain.fsr = 0x400;
		for (unsigned index = 0; index < 3; index++)
			fixture.buffers[index].physical = fixture.buffers[index].iova;
		enum munch_sde_result expected = MUNCH_SDE_OK;
		if (scenario < 32) {
			fixture.domain.fsr |= 1u << scenario;
			if ((scenario >= 1 && scenario <= 8) || scenario >= 30) expected = MUNCH_SDE_FAULT;
		} else {
			switch (scenario) {
			case 32: fixture.domain.cbar = 3u << 16; expected = MUNCH_SDE_DOMAIN; break;
			case 33: fixture.domain.s2cr |= 1u << 16; expected = MUNCH_SDE_DOMAIN; break;
			case 34: fixture.domain.cba2r = 0; expected = MUNCH_SDE_DOMAIN; break;
			case 35: fixture.domain.sctlr |= 1u << 20; expected = MUNCH_SDE_DOMAIN; break;
			case 36: fixture.domain.sctlr |= 1u << 4; expected = MUNCH_SDE_DOMAIN; break;
			case 37: fixture.domain.ttbr = UINT64_MAX; fixture.domain.tcr = UINT32_MAX; break;
			case 38: fixture.domain.sctlr |= 1; expected = MUNCH_SDE_DOMAIN; break;
			case 39: fixture.buffers[1].physical += 1ULL << 32; expected = MUNCH_SDE_MAPPING; break;
			case 40: fixture.buffers[1] = fixture.buffers[0]; expected = MUNCH_SDE_MAPPING; break;
			case 41: fixture.buffers[2] = fixture.buffers[1]; expected = MUNCH_SDE_MAPPING; break;
			case 42: fixture.buffers[2].iova = 0xfffff000; expected = MUNCH_SDE_MAPPING; break;
			case 43: fixture.buffers[1].physical += 4096; expected = MUNCH_SDE_MAPPING; break;
			case 44: fixture.buffers[2].length -= 4096; expected = MUNCH_SDE_MAPPING; break;
			}
		}
		assert(munch_sde_find_route(&fixture.snapshot, &fixture.route) == MUNCH_SDE_OK);
		assert(munch_sde_check_buffers(&fixture.domain, read_pte, &fixture, &fixture.route,
		    fixture.buffers) == expected);
		assert(!fixture.pte_reads && !fixture.writes);
		if (expected != MUNCH_SDE_OK) continue;
		for (unsigned sample = 0; sample < 3; sample++) {
			const uint32_t addresses[] = { 0, 0x9c000000, UINT32_MAX };
			uint64_t physical = UINT64_MAX;
			struct munch_smmu_walk walk;
			assert(munch_smmu_translate_checked(&fixture.domain, NULL, NULL, addresses[sample],
			    &physical, &walk) == MUNCH_SDE_OK);
			assert(physical == addresses[sample] && walk.physical == physical && !walk.read);
			assert(!strcmp(walk.check, "stage1-bypass") && walk.level == 4 && !walk.entry);
		}
	}
}

static void
test_walk_diagnostics(void)
{
	static const char *names[] = { "pte-reader", "cbar-stage", "cba2r-format", "stream-type",
	    "sctlr-mode", "sctlr-mode", "tcr-granule", "tcr-walk-disabled", "physical-size",
	    "input-size", "ttbr-align", "ttbr-zero", "ttbr-range", "pte-unreadable", "pte-address-bits",
	    "pte-physical-range", "table-permissions", "table-zero", "pte-type", "pte-access-flag",
	    "pte-permissions", "pte-memory-type", "pte-block-align" };
	for (unsigned scenario = 0; scenario < sizeof(names) / sizeof(names[0]); scenario++) {
		reset();
		fixture.domain.fsr = 0x600;
		munch_smmu_read_pte reader = read_pte;
		switch (scenario) {
		case 0: reader = NULL; break;
		case 1: fixture.domain.cbar = 3u << 16; break;
		case 2: fixture.domain.cba2r = 0; break;
		case 3: fixture.domain.s2cr = 1u << 16; break;
		case 4: fixture.domain.sctlr = 1u << 20; break;
		case 5: fixture.domain.sctlr |= 1u << 4; break;
		case 6: fixture.domain.tcr |= 1u << 14; break;
		case 7: fixture.domain.tcr |= 1u << 7; break;
		case 8: fixture.domain.tcr2 = 6; break;
		case 9: fixture.domain.tcr = 33; break;
		case 10: fixture.domain.ttbr |= 8; break;
		case 11: fixture.domain.ttbr = 0; break;
		case 12: fixture.domain.ttbr = 1ULL << 32; break;
		case 13: fixture.domain.ttbr = 0x88000000; break;
		case 14: fixture.tables[0][0] |= 1ULL << 48; break;
		case 15: fixture.tables[0][0] |= 1ULL << 32; break;
		case 16: fixture.tables[0][0] = table_base | 3 | (1ULL << 61); break;
		case 17: fixture.tables[0][0] = 3; break;
		case 18: fixture.tables[0][0] = 0; break;
		case 19: fixture.tables[0][0] &= ~(1u << 10); break;
		case 20: fixture.tables[0][0] &= ~(1u << 6); break;
		case 21: fixture.domain.mair = 0; break;
		case 22: fixture.tables[0][0] |= 0x1000; break;
		}
		uint64_t physical = UINT64_MAX;
		struct munch_smmu_walk walk;
		enum munch_sde_result result = munch_smmu_translate_checked(&fixture.domain, reader, &fixture, 0x1234567, &physical, &walk);
		assert(result == (scenario < 12 ? MUNCH_SDE_DOMAIN : MUNCH_SDE_MAPPING));
		assert(!strcmp(walk.check, names[scenario]) && physical == UINT64_MAX && !fixture.writes);
		if (scenario < 13) assert(walk.level == 4 && !walk.read && !fixture.pte_reads);
		else assert(walk.level == 1 && walk.entry && walk.read == (scenario > 13));
	}
}

static void
complete_hardware(void)
{
	uint32_t ctl = munch_sde_ctl_offsets[fixture.presenter.route.ctl];
	fixture.regs[(ctl + MUNCH_SDE_CTL_START) / 4] = 0;
	fixture.regs[(ctl + MUNCH_SDE_CTL_FLUSH) / 4] = 0;
	fixture.regs[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] = 1;
	fixture.regs[MUNCH_SDE_INTR_STATUS / 4] |= fixture.presenter.route.done_mask;
}

static void
test_present(void)
{
	unsigned index;
	reset();
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_DISABLED);
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 1);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_BUSY);
	fixture.regs[MUNCH_SDE_INTR_STATUS / 4] = fixture.route.done_mask | (1u << 9);
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_OK);
	assert(fixture.writes == 4 && fixture.barriers == 6 && fixture.cleans == 1);
	assert(fixture.offsets[0] == MUNCH_SDE_INTR_CLEAR && fixture.values[0] == (1u << 10));
	assert(fixture.offsets[1] == 0x25014 && fixture.values[1] == fixture.buffers[1].iova);
	assert(fixture.offsets[2] == 0x2418 && fixture.values[2] == (1u << 3));
	assert(fixture.offsets[3] == 0x241c && fixture.values[3] == 1);
	assert(munch_sde_poll(&fixture.presenter, 200) == MUNCH_SDE_BUSY);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 2);
	assert(munch_sde_submit(&fixture.presenter, 250, 50000) == MUNCH_SDE_BUSY);
	complete_hardware();
	fixture.dsi_status = 4;
	assert(munch_sde_poll(&fixture.presenter, 300) == MUNCH_SDE_OK);
	assert(fixture.presenter.front == 1 && fixture.presenter.completed == 1);
	assert(fixture.regs[MUNCH_SDE_INTR_STATUS / 4] == (1u << 9));
	fixture.dsi_status = 0;
	assert(munch_sde_submit(&fixture.presenter, 500, 50000) == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 1);
	assert(munch_sde_poll(&fixture.presenter, 50500) == MUNCH_SDE_TIMEOUT);
	assert(fixture.presenter.front == 1 && fixture.presenter.pending == 2 && fixture.presenter.completed == 1);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_TIMEOUT);
	complete_hardware();
	assert(munch_sde_poll(&fixture.presenter, 50501) == MUNCH_SDE_TIMEOUT);
	assert(prepare() == MUNCH_SDE_BUSY);
}

static void
test_partial_overlap(void)
{
	unsigned index;
	uint32_t pixels[3][4] = {{1, 2, 3, 4}, {0}, {0}};
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_latest_buffer(&fixture.presenter) == 0);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 1);
	memcpy(pixels[index], pixels[munch_sde_latest_buffer(&fixture.presenter)], sizeof(pixels[index]));
	pixels[index][0] = 10;
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_OK);
	assert(munch_sde_latest_buffer(&fixture.presenter) == 1);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 2);
	memcpy(pixels[index], pixels[munch_sde_latest_buffer(&fixture.presenter)], sizeof(pixels[index]));
	pixels[index][1] = 20;
	assert(pixels[1][0] == 10 && pixels[1][1] == 2);
	complete_hardware();
	assert(munch_sde_poll(&fixture.presenter, 200) == MUNCH_SDE_OK);
	assert(munch_sde_submit(&fixture.presenter, 300, 50000) == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK && index == 1);
	assert(munch_sde_latest_buffer(&fixture.presenter) == 2);
	memcpy(pixels[index], pixels[munch_sde_latest_buffer(&fixture.presenter)], sizeof(pixels[index]));
	pixels[index][2] = 30;
	assert(pixels[1][0] == 10 && pixels[1][1] == 20 && pixels[1][2] == 30 && pixels[1][3] == 4);
	assert(pixels[2][0] == 10 && pixels[2][1] == 20 && pixels[2][2] == 3 && pixels[2][3] == 4);
	complete_hardware();
	assert(munch_sde_poll(&fixture.presenter, 400) == MUNCH_SDE_OK);
	assert(munch_sde_latest_buffer(&fixture.presenter) == 2);
}

static void
test_failure(void)
{
	unsigned index;
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK);
	fixture.guard = MUNCH_SDE_STALE;
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_STALE);
	assert(fixture.writes == 0);
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK);
	fixture.clean_ok = false;
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_MAPPING);
	assert(fixture.writes == 0);
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK);
	fixture.regs[MUNCH_SDE_INTR_STATUS / 4] = fixture.route.done_mask;
	fixture.sticky_done = true;
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_STALE);
	assert(fixture.writes == 1 && fixture.presenter.front == 0);
	reset();
	assert(prepare() == MUNCH_SDE_OK);
	assert(munch_sde_acquire(&fixture.presenter, &index) == MUNCH_SDE_OK);
	assert(munch_sde_submit(&fixture.presenter, UINT64_MAX, 50000) == MUNCH_SDE_UNSUPPORTED);
	assert(fixture.writes == 0);
	assert(munch_sde_submit(&fixture.presenter, 100, 50000) == MUNCH_SDE_OK);
	complete_hardware();
	fixture.regs[MUNCH_SDE_INTR_STATUS / 4] |= MUNCH_SDE_INTF1_UNDERRUN;
	assert(munch_sde_poll(&fixture.presenter, 200) == MUNCH_SDE_FAULT);
	assert(fixture.presenter.completed == 0 && fixture.presenter.pending == 1);
}

int
main(void)
{
	test_vig_border();
	test_vig_rejection();
	test_transcribed_route();
	test_layout_diagnostics();
	test_r7_transcription();
	test_pixel_ext();
	test_dsi_link();
	test_route_diagnostics();
	test_routes();
	test_streams();
	test_translation();
	test_smmu_status();
	test_smmu_bypass();
	test_walk_diagnostics();
	test_buffers();
	test_branch();
	test_present();
	test_partial_overlap();
	test_failure();
	puts("SDE route, SMMU translation, buffer ownership and completion tests passed");
	return 0;
}
