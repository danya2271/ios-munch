#ifndef _IOKIT_MUNCH_SDE_H
#define _IOKIT_MUNCH_SDE_H

#include <stdint.h>
#include <stdbool.h>
#include "munch-kona-display.h"
#include "munch-sde-memory.h"

enum munch_sde_result {
	MUNCH_SDE_OK, MUNCH_SDE_DISABLED, MUNCH_SDE_UNSUPPORTED,
	MUNCH_SDE_ROUTE, MUNCH_SDE_FORMAT, MUNCH_SDE_DOMAIN,
	MUNCH_SDE_MAPPING, MUNCH_SDE_BUSY, MUNCH_SDE_FAULT,
	MUNCH_SDE_TIMEOUT, MUNCH_SDE_STALE
};

static inline const char *
munch_sde_result_name(enum munch_sde_result result)
{
	switch (result) {
	case MUNCH_SDE_OK: return "ok";
	case MUNCH_SDE_DISABLED: return "disabled";
	case MUNCH_SDE_UNSUPPORTED: return "unsupported";
	case MUNCH_SDE_ROUTE: return "ambiguous-or-unsupported-route";
	case MUNCH_SDE_FORMAT: return "unsupported-format-or-geometry";
	case MUNCH_SDE_DOMAIN: return "unproven-smmu-domain";
	case MUNCH_SDE_MAPPING: return "unproven-buffer-mapping";
	case MUNCH_SDE_BUSY: return "busy";
	case MUNCH_SDE_FAULT: return "hardware-fault";
	case MUNCH_SDE_TIMEOUT: return "completion-timeout";
	case MUNCH_SDE_STALE: return "hardware-state-changed";
	}
	return "invalid-result";
}

struct munch_sde_pixel_ext_state {
	uint32_t lr, tb, required;
};

struct munch_sde_pipe_state {
	uint32_t size, out_size, source, stride, format, unpack;
	uint32_t src_xy, out_xy, opmode, secure, multirect;
	uint32_t scaler, csc, pre_downscale, decimation;
	struct munch_sde_pixel_ext_state pixel_ext[MUNCH_SDE_SSPP_PE_COUNT];
};

struct munch_sde_mixer_state {
	uint32_t opmode;
	uint32_t blend_op[MUNCH_SDE_LM_BLEND_COUNT], blend_alpha[MUNCH_SDE_LM_BLEND_COUNT];
};

struct munch_sde_ctl_state {
	uint32_t top, active, start, flush, merge, dsc, wb, cwb, cdm;
	uint32_t layers[4][6];
};

struct munch_sde_snapshot {
	uint32_t revision, mux, tear_enable, tear_vsync, autorefresh, dsi_ctrl, dsi_status;
	uint32_t mixer_size[6];
	struct munch_sde_mixer_state mixers[6];
	struct munch_sde_pipe_state pipes[8];
	struct munch_sde_ctl_state ctls[6];
};

struct munch_sde_route {
	uint32_t ctl, mixer, pipe, stage, pingpong, source, stride, format, unpack;
	uint32_t flush_mask, done_mask;
};

struct munch_sde_route_diagnostic {
	const char *check;
	int ctl, mixer, pipe;
};

enum munch_sde_layout_rule {
	MUNCH_SDE_LAYOUT_EQUAL, MUNCH_SDE_LAYOUT_MIN, MUNCH_SDE_LAYOUT_MAX,
	MUNCH_SDE_LAYOUT_CLEAR, MUNCH_SDE_LAYOUT_EITHER
};

struct munch_sde_layout_check {
	const char *name;
	enum munch_sde_layout_rule rule;
	uint32_t actual, expected, alternative;
};

static inline const char *
munch_sde_layout_rule_name(enum munch_sde_layout_rule rule)
{
	switch (rule) {
	case MUNCH_SDE_LAYOUT_EQUAL: return "equal";
	case MUNCH_SDE_LAYOUT_MIN: return "at-least";
	case MUNCH_SDE_LAYOUT_MAX: return "at-most";
	case MUNCH_SDE_LAYOUT_CLEAR: return "bits-clear";
	case MUNCH_SDE_LAYOUT_EITHER: return "one-of";
	}
	return "invalid-rule";
}

static inline uint32_t
munch_sde_pipe_layout_failures(const struct munch_sde_pipe_state *pipe, uint32_t mixer_size,
    struct munch_sde_layout_check *first)
{
	const struct munch_sde_layout_check checks[] = {
		{ "pipe-size", MUNCH_SDE_LAYOUT_EQUAL, pipe->size, (MUNCH_PANEL_HEIGHT << 16) | MUNCH_PANEL_WIDTH, 0 },
		{ "pipe-mixer-size", MUNCH_SDE_LAYOUT_EQUAL, mixer_size, pipe->size, 0 },
		{ "pipe-out-size", MUNCH_SDE_LAYOUT_EQUAL, pipe->out_size, pipe->size, 0 },
		{ "pipe-src-xy", MUNCH_SDE_LAYOUT_EQUAL, pipe->src_xy, 0, 0 },
		{ "pipe-out-xy", MUNCH_SDE_LAYOUT_EQUAL, pipe->out_xy, 0, 0 },
		{ "pipe-opmode", MUNCH_SDE_LAYOUT_CLEAR, pipe->opmode, ~MUNCH_SDE_SSPP_PE_OVERRIDE, 0 },
		{ "pipe-secure", MUNCH_SDE_LAYOUT_EQUAL, pipe->secure, 0, 0 },
		{ "pipe-multirect", MUNCH_SDE_LAYOUT_EQUAL, pipe->multirect, 0, 0 },
		{ "pipe-stride-min", MUNCH_SDE_LAYOUT_MIN, pipe->stride, MUNCH_PANEL_WIDTH * 4, 0 },
		{ "pipe-stride-max", MUNCH_SDE_LAYOUT_MAX, pipe->stride, 0xffff, 0 },
		{ "pipe-stride-align", MUNCH_SDE_LAYOUT_CLEAR, pipe->stride, 3, 0 },
		{ "pipe-source-align", MUNCH_SDE_LAYOUT_CLEAR, pipe->source, 0xfff, 0 },
		{ "pipe-source-min", MUNCH_SDE_LAYOUT_MIN, pipe->source, MUNCH_SDE_IOMMU_IOVA_BASE, 0 },
		{ "pipe-format", MUNCH_SDE_LAYOUT_EITHER, pipe->format, 0x236ff, 0x237ff },
		{ "pipe-unpack", MUNCH_SDE_LAYOUT_EQUAL, pipe->unpack, 0x03020001, 0 },
		{ "pipe-pe-c0-lr", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[0].lr, 0, 0 },
		{ "pipe-pe-c0-tb", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[0].tb, 0, 0 },
		{ "pipe-pe-c0-size", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[0].required, pipe->size, 0 },
		{ "pipe-pe-c1c2-lr", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[1].lr, 0, 0 },
		{ "pipe-pe-c1c2-tb", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[1].tb, 0, 0 },
		{ "pipe-pe-c1c2-size", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[1].required, pipe->size, 0 },
		{ "pipe-pe-c3-lr", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[2].lr, 0, 0 },
		{ "pipe-pe-c3-tb", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[2].tb, 0, 0 },
		{ "pipe-pe-c3-size", MUNCH_SDE_LAYOUT_EQUAL, pipe->pixel_ext[2].required, pipe->size, 0 },
	};
	uint32_t failures = 0;
	if (first) {
		first->name = "ok";
		first->rule = MUNCH_SDE_LAYOUT_EQUAL;
		first->actual = first->expected = first->alternative = 0;
	}
	for (unsigned index = 0; index < sizeof(checks) / sizeof(checks[0]); index++) {
		if (index == 15 && !(pipe->opmode & MUNCH_SDE_SSPP_PE_OVERRIDE)) break;
		const struct munch_sde_layout_check *check = &checks[index];
		bool valid = false;
		switch (check->rule) {
		case MUNCH_SDE_LAYOUT_EQUAL: valid = check->actual == check->expected; break;
		case MUNCH_SDE_LAYOUT_MIN: valid = check->actual >= check->expected; break;
		case MUNCH_SDE_LAYOUT_MAX: valid = check->actual <= check->expected; break;
		case MUNCH_SDE_LAYOUT_CLEAR: valid = !(check->actual & check->expected); break;
		case MUNCH_SDE_LAYOUT_EITHER: valid = check->actual == check->expected || check->actual == check->alternative; break;
		}
		if (valid) continue;
		if (first && !failures) *first = *check;
		failures |= 1u << index;
	}
	return failures;
}

static inline enum munch_sde_result
munch_sde_route_result(struct munch_sde_route_diagnostic *diagnostic, enum munch_sde_result result,
    const char *check, unsigned ctl, unsigned mixer, unsigned pipe)
{
	if (diagnostic) {
		diagnostic->check = check;
		diagnostic->ctl = ctl < 6 ? (int)ctl : -1;
		diagnostic->mixer = mixer < 6 ? (int)mixer : -1;
		diagnostic->pipe = pipe < 8 ? (int)pipe : -1;
	}
	return result;
}

static inline enum munch_sde_result
munch_sde_find_route_checked(const struct munch_sde_snapshot *snapshot, struct munch_sde_route *route,
    struct munch_sde_route_diagnostic *diagnostic)
{
	static const unsigned pipe_shift[8] = { 0, 3, 6, 26, 18, 21, 0, 4 };
	static const unsigned pipe_flush[8] = { 0, 1, 2, 18, 3, 4, 5, 19 };
	unsigned ctl_index = 6, mixer_index = 6, pipe_index = 8;
	unsigned stage = 0;
	const struct munch_sde_ctl_state *ctl;
	const struct munch_sde_pipe_state *pipe;
	if ((snapshot->revision >> 16) != 0x6000)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "revision", 6, 6, 8);
	if (snapshot->mux > 3)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "pingpong-mux", 6, 6, 8);
	if ((snapshot->dsi_ctrl & ~(MUNCH_DSI_RX_ECC_ENABLED | MUNCH_DSI_RX_CRC_ENABLED)) !=
	    (MUNCH_DSI_ENGINE_ENABLED | MUNCH_DSI_CMD_MODE_ENABLED |
	    MUNCH_DSI_DATA_LANES_ENABLED | MUNCH_DSI_CLOCK_LANE_ENABLED))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "dsi-link", 6, 6, 8);
	if (!(snapshot->tear_enable & 1))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "tear-disabled", 6, 6, 8);
	for (unsigned index = 0; index < 6; index++) {
		if (snapshot->ctls[index].active && snapshot->ctls[index].active != MUNCH_SDE_INTF1_ACTIVE)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "ctl-intf-mask", index, 6, 8);
		if (!(snapshot->ctls[index].active & MUNCH_SDE_INTF1_ACTIVE)) continue;
		if (ctl_index != 6)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "multiple-ctls", index, 6, 8);
		ctl_index = index;
	}
	if (ctl_index == 6)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "no-intf1-ctl", 6, 6, 8);
	ctl = &snapshot->ctls[ctl_index];
	if (ctl->top != MUNCH_SDE_CTL_CMD_MODE || ctl->active != MUNCH_SDE_INTF1_ACTIVE)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "ctl-mode", ctl_index, 6, 8);
	if (ctl->merge || ctl->wb || ctl->cwb || ctl->cdm)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "ctl-aux-blocks", ctl_index, 6, 8);
	if (!ctl->dsc || (ctl->dsc & (ctl->dsc - 1)) || (ctl->dsc & ~15u))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "ctl-dsc-mask", ctl_index, 6, 8);
	for (unsigned mixer = 0; mixer < 6; mixer++) {
		uint32_t main = ctl->layers[0][mixer];
		uint32_t ext = ctl->layers[1][mixer];
		uint32_t ext2 = ctl->layers[2][mixer];
		uint32_t ext3 = ctl->layers[3][mixer];
		uint32_t border = main & MUNCH_SDE_CTL_BORDER_OUT;
		if (!(main & ~MUNCH_SDE_CTL_BORDER_OUT) && !ext && !ext2 && !ext3) continue;
		if (mixer_index != 6)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "multiple-mixers", ctl_index, mixer, 8);
		if (ext || ext3 || (ext2 & ~0xffu))
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "layer-extensions", ctl_index, mixer, 8);
		mixer_index = mixer;
		for (unsigned index = 0; index < 8; index++) {
			uint32_t bits = index < 6 ? (main >> pipe_shift[index]) & 7 :
			    (ext2 >> pipe_shift[index]) & 15;
			if (!bits) continue;
			if (bits > MUNCH_SDE_ROUTE_MAX_STAGE + 1)
				return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "blend-stage", ctl_index, mixer, index);
			if (bits > 1 && !border)
				return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "border-stage", ctl_index, mixer, index);
			if (pipe_index != 8)
				return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "multiple-pipes", ctl_index, mixer, index);
			pipe_index = index;
			stage = bits - 1;
		}
		if (pipe_index == 8 || (pipe_index < 6 ?
		    (main != (((stage + 1) << pipe_shift[pipe_index]) | border) || ext2) :
		    (main != border || ext2 != ((stage + 1) << pipe_shift[pipe_index]))))
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "layer-encoding", ctl_index, mixer, pipe_index);
	}
	if (mixer_index == 6 || pipe_index == 8)
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "no-staged-pipe", ctl_index, mixer_index, pipe_index);
	pipe = &snapshot->pipes[pipe_index];
	struct munch_sde_layout_check layout;
	if (munch_sde_pipe_layout_failures(pipe, snapshot->mixer_size[mixer_index], &layout))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_FORMAT, layout.name, ctl_index, mixer_index, pipe_index);
	if (pipe_index < 4) {
		if (pipe->scaler & ~MUNCH_SDE_VIG_SCALER_BYPASS_MASK)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "vig-scaler", ctl_index, mixer_index, pipe_index);
		if (pipe->csc & ~MUNCH_SDE_VIG_CSC_SRC_DATAFMT)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "vig-csc", ctl_index, mixer_index, pipe_index);
		if (pipe->pre_downscale || pipe->decimation)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "vig-resample", ctl_index, mixer_index, pipe_index);
	}
	const struct munch_sde_mixer_state *mixer = &snapshot->mixers[mixer_index];
	if ((stage || pipe_index < 4) && (mixer->opmode & ~(1u << stage)))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "mixer-mode", ctl_index, mixer_index, pipe_index);
	if (stage && ((mixer->blend_op[stage - 1] != MUNCH_SDE_LM_OPAQUE_BLEND &&
	    mixer->blend_op[stage - 1] != MUNCH_SDE_LM_INVERSE_BG_BLEND) ||
	    mixer->blend_alpha[stage - 1] != MUNCH_SDE_LM_OPAQUE_ALPHA))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "mixer-blend", ctl_index, mixer_index, pipe_index);
	for (unsigned index = 0; index < MUNCH_SDE_LM_BLEND_COUNT; index++) {
		if (mixer->blend_op[index] & MUNCH_SDE_LM_DIM_LAYER)
			return munch_sde_route_result(diagnostic, MUNCH_SDE_ROUTE, "mixer-dim-layer", ctl_index, mixer_index, pipe_index);
	}
	bool autorefresh = (snapshot->autorefresh & MUNCH_SDE_AUTOREFRESH_ENABLE) != 0;
	if (autorefresh && snapshot->autorefresh != (MUNCH_SDE_AUTOREFRESH_ENABLE | 1))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_UNSUPPORTED, "autorefresh-config", ctl_index, mixer_index, pipe_index);
	if (ctl->start || ctl->flush || (!autorefresh && (snapshot->dsi_status & (1u << 2))))
		return munch_sde_route_result(diagnostic, MUNCH_SDE_BUSY, "transfer-busy", ctl_index, mixer_index, pipe_index);
	route->ctl = ctl_index;
	route->mixer = mixer_index;
	route->pipe = pipe_index;
	route->stage = stage;
	route->pingpong = snapshot->mux;
	route->source = pipe->source;
	route->stride = pipe->stride;
	route->format = pipe->format;
	route->unpack = pipe->unpack;
	route->flush_mask = 1u << pipe_flush[pipe_index];
	route->done_mask = 1u << (8 + snapshot->mux);
	return munch_sde_route_result(diagnostic, MUNCH_SDE_OK, "ok", ctl_index, mixer_index, pipe_index);
}

static inline enum munch_sde_result
munch_sde_find_route(const struct munch_sde_snapshot *snapshot, struct munch_sde_route *route)
{
	return munch_sde_find_route_checked(snapshot, route, 0);
}

struct munch_smmu_stream {
	uint32_t smr, s2cr;
};

struct munch_smmu_domain {
	uint32_t stream, context, s2cr, cbar, cba2r, sctlr, tcr, tcr2, fsr;
	uint64_t ttbr, mair;
};

static inline enum munch_sde_result
munch_smmu_find_stream(uint32_t scr0, uint32_t idr0, uint32_t idr1,
    const struct munch_smmu_stream *streams, struct munch_smmu_domain *domain)
{
	uint32_t count = idr0 & 0xff, match = 256;
	bool extended = (scr0 & MUNCH_SMMU_SCR0_EXIDENABLE) != 0;
	uint32_t sid_mask = extended ? 0xffff : 0x7fff;
	if ((scr0 & 1) || !count || !(idr0 & (1u << 27)) ||
	    (extended && !(idr0 & MUNCH_SMMU_IDR0_EXIDS))) return MUNCH_SDE_DOMAIN;
	for (unsigned index = 0; index < count; index++) {
		uint32_t smr = streams[index].smr, s2cr = streams[index].s2cr;
		uint32_t mask = (smr >> 16) & sid_mask, sid = smr & sid_mask;
		if (!(extended ? s2cr & MUNCH_SMMU_S2CR_EXIDVALID : smr & MUNCH_SMMU_SMR_VALID)) continue;
		if (((sid ^ MUNCH_SDE_IOMMU_UNSECURE_SID) &
		    ~(mask | MUNCH_SDE_IOMMU_UNSECURE_MASK) & sid_mask) != 0) continue;
		if (match != 256 || (MUNCH_SDE_IOMMU_UNSECURE_MASK & ~mask) ||
		    ((sid ^ MUNCH_SDE_IOMMU_UNSECURE_SID) & ~mask & sid_mask) ||
		    (((s2cr >> 16) & 3) != 0) || (s2cr & 0xff) >= (idr1 & 0xff))
			return MUNCH_SDE_DOMAIN;
		match = index;
	}
	if (match == 256) return MUNCH_SDE_DOMAIN;
	domain->stream = match;
	domain->s2cr = streams[match].s2cr;
	domain->context = streams[match].s2cr & 0xff;
	return MUNCH_SDE_OK;
}

typedef bool (*munch_smmu_read_pte)(void *, uint64_t, uint64_t *);

struct munch_smmu_walk {
	const char *check;
	uint32_t iova, level;
	uint64_t entry, descriptor, physical;
	bool read;
};

static inline uint32_t
munch_smmu_fault_bits(uint32_t fsr)
{
	return fsr & MUNCH_SMMU_FSR_FAULT;
}

static inline enum munch_sde_result
munch_smmu_walk_result(struct munch_smmu_walk *walk, enum munch_sde_result result, const char *check)
{
	if (walk) walk->check = check;
	return result;
}

static inline enum munch_sde_result
munch_smmu_translate_checked(const struct munch_smmu_domain *domain, munch_smmu_read_pte read_pte,
    void *cookie, uint32_t iova, uint64_t *physical, struct munch_smmu_walk *walk)
{
	const uint64_t address_mask = 0x0000fffffffff000ULL;
	static const unsigned physical_bits[6] = { 32, 36, 40, 42, 44, 48 };
	unsigned bits = 64 - (domain->tcr & 63);
	unsigned ips = domain->tcr2 & 7;
	uint64_t table = domain->ttbr & address_mask;
	if (walk) {
		walk->check = "domain";
		walk->iova = iova;
		walk->level = 4;
		walk->entry = walk->descriptor = walk->physical = 0;
		walk->read = false;
	}
	if (munch_smmu_fault_bits(domain->fsr))
		return munch_smmu_walk_result(walk, MUNCH_SDE_FAULT, "fsr-fault");
	if (((domain->cbar >> 16) & 3) != 1)
		return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "cbar-stage");
	if (!(domain->cba2r & 1)) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "cba2r-format");
	if (((domain->s2cr >> 16) & 3) != 0)
		return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "stream-type");
	if (domain->sctlr & ((1u << 4) | (1u << 20)))
		return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "sctlr-mode");
	if (!(domain->sctlr & MUNCH_SMMU_SCTLR_M)) {
		*physical = iova;
		if (walk) walk->physical = *physical;
		return munch_smmu_walk_result(walk, MUNCH_SDE_OK, "stage1-bypass");
	}
	if (!read_pte) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "pte-reader");
	if (domain->tcr & (3u << 14)) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "tcr-granule");
	if (domain->tcr & (1u << 7)) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "tcr-walk-disabled");
	if (ips > 5) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "physical-size");
	if (bits < 32 || bits > 48) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "input-size");
	if (domain->ttbr & 4095) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "ttbr-align");
	if (!table) return munch_smmu_walk_result(walk, MUNCH_SDE_DOMAIN, "ttbr-zero");
	uint64_t physical_limit = 1ULL << physical_bits[ips];
	if (table >= physical_limit) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "ttbr-range");
	unsigned level = 4 - (bits - 12 + 8) / 9;
	for (; level < 4; level++) {
		unsigned shift = 12 + (3 - level) * 9;
		uint64_t descriptor, entry = table + (((uint64_t)iova >> shift) & 511) * 8;
		if (walk) {
			walk->level = level;
			walk->entry = entry;
			walk->descriptor = 0;
			walk->read = false;
		}
		if (!read_pte(cookie, entry, &descriptor))
			return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-unreadable");
		if (walk) { walk->descriptor = descriptor; walk->read = true; }
		unsigned type = descriptor & 3;
		if (descriptor & 0x000f000000000000ULL)
			return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-address-bits");
		if ((descriptor & address_mask) >= physical_limit)
			return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-physical-range");
		if (level < 3 && type == 3) {
			if (descriptor & (3ULL << 61)) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "table-permissions");
			table = descriptor & address_mask;
			if (!table) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "table-zero");
			continue;
		}
		if ((level == 3 ? type != 3 : type != 1) || level == 0)
			return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-type");
		if (!(descriptor & (1u << 10))) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-access-flag");
		if (!(descriptor & (1u << 6))) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-permissions");
		unsigned attribute = (descriptor >> 2) & 7;
		uint32_t mair = (domain->mair >> (attribute * 8)) & 255;
		if (mair != 0x44 && mair != 0xff) return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-memory-type");
		uint64_t offset_mask = (1ULL << shift) - 1;
		if (descriptor & address_mask & offset_mask)
			return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "pte-block-align");
		*physical = (descriptor & address_mask) | (iova & offset_mask);
		if (walk) walk->physical = *physical;
		return munch_smmu_walk_result(walk, MUNCH_SDE_OK, "ok");
	}
	return munch_smmu_walk_result(walk, MUNCH_SDE_MAPPING, "walk-end");
}

static inline enum munch_sde_result
munch_smmu_translate(const struct munch_smmu_domain *domain, munch_smmu_read_pte read_pte,
    void *cookie, uint32_t iova, uint64_t *physical)
{
	return munch_smmu_translate_checked(domain, read_pte, cookie, iova, physical, 0);
}

struct munch_sde_buffer {
	uint32_t iova, length;
	uint64_t physical;
};

static inline bool
munch_sde_overlaps(uint64_t first, uint32_t first_length, uint64_t second, uint32_t second_length)
{
	return first < second + second_length && second < first + first_length;
}

static inline enum munch_sde_result
munch_sde_check_buffers(const struct munch_smmu_domain *domain, munch_smmu_read_pte read_pte,
    void *cookie, const struct munch_sde_route *route, const struct munch_sde_buffer buffers[3])
{
	uint64_t required = (uint64_t)route->stride * MUNCH_PANEL_HEIGHT;
	if (buffers[0].iova != route->source) return MUNCH_SDE_MAPPING;
	for (unsigned index = 0; index < 3; index++) {
		const struct munch_sde_buffer *buffer = &buffers[index];
		if (buffer->length < required || buffer->length > 64 * 1024 * 1024 ||
		    ((buffer->iova | buffer->physical | buffer->length) & 4095) ||
		    buffer->iova < MUNCH_SDE_IOMMU_IOVA_BASE ||
		    (uint64_t)buffer->iova + buffer->length > (1ULL << 32) ||
		    buffer->physical >= (1ULL << 48) ||
		    buffer->physical + buffer->length > (1ULL << 48)) return MUNCH_SDE_MAPPING;
		for (unsigned previous = 0; previous < index; previous++) {
			if (munch_sde_overlaps(buffer->iova, buffer->length, buffers[previous].iova,
			    buffers[previous].length) || munch_sde_overlaps(buffer->physical, buffer->length,
			    buffers[previous].physical, buffers[previous].length)) return MUNCH_SDE_MAPPING;
		}
		for (uint32_t offset = 0; offset < buffer->length; offset += 4096) {
			uint64_t physical;
			enum munch_sde_result result = munch_smmu_translate(domain, read_pte, cookie,
			    buffer->iova + offset, &physical);
			if (result != MUNCH_SDE_OK) return result;
			if (physical != buffer->physical + offset) return MUNCH_SDE_MAPPING;
		}
	}
	return MUNCH_SDE_OK;
}

struct munch_sde_io {
	void *cookie;
	uint32_t (*read)(void *, uint32_t);
	void (*write)(void *, uint32_t, uint32_t);
	void (*barrier)(void *);
	bool (*clean)(void *, unsigned, uint32_t);
	enum munch_sde_result (*guard)(void *);
	uint32_t (*dsi_status)(void *);
	void (*kickoff)(void *);
};

struct munch_sde_refresh {
	uint32_t config, vsync, status, line;
	const char *phase;
	bool owned, paused;
};

typedef void (*munch_sde_delay)(void *, uint32_t);

static inline uint32_t
munch_sde_refresh_status(const struct munch_sde_io *io)
{
	uint32_t peripheral = io->read(io->cookie, MUNCH_SDE_DEBUG_PERIPH);
	uint32_t control = io->read(io->cookie, MUNCH_SDE_DEBUG_CTRL);
	io->write(io->cookie, MUNCH_SDE_DEBUG_PERIPH, MUNCH_SDE_INTF1_AUTOREFRESH_TEST);
	io->write(io->cookie, MUNCH_SDE_DEBUG_CTRL, 0x7001);
	io->barrier(io->cookie);
	uint32_t status = io->read(io->cookie, MUNCH_SDE_DEBUG_STATUS);
	io->write(io->cookie, MUNCH_SDE_DEBUG_CTRL, control);
	io->write(io->cookie, MUNCH_SDE_DEBUG_PERIPH, peripheral);
	io->barrier(io->cookie);
	return status;
}

static inline void
munch_sde_refresh_reset_tearcheck(const struct munch_sde_io *io)
{
	uint32_t start = io->read(io->cookie,
		MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_START_POS) & 0xffff;
	uint32_t write = io->read(io->cookie,
		MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_WRCOUNT);
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_WRCOUNT,
		(write & 0xffff0000) | start);
	io->barrier(io->cookie);
}

static inline void
munch_sde_refresh_rollback(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
	const struct munch_sde_snapshot *saved)
{
	if (!refresh->owned) return;
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC,
		saved->tear_vsync);
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH,
		saved->autorefresh);
	io->barrier(io->cookie);
	refresh->config = saved->autorefresh;
	refresh->vsync = saved->tear_vsync;
	refresh->owned = false;
	refresh->paused = false;
	refresh->phase = "rollback";
}

static inline enum munch_sde_result
munch_sde_refresh_guard(const struct munch_sde_io *io)
{
	enum munch_sde_result result = io->guard(io->cookie);
	if (result != MUNCH_SDE_OK) return result;
	return io->read(io->cookie, MUNCH_SDE_INTR_STATUS) & MUNCH_SDE_INTF1_UNDERRUN ?
	    MUNCH_SDE_FAULT : MUNCH_SDE_OK;
}

static inline bool
munch_sde_refresh_idle(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
    const struct munch_sde_route *route)
{
	uint32_t ctl = munch_sde_ctl_offsets[route->ctl];
	refresh->line = io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) & 0xffff;
	return (!refresh->line || refresh->line >= MUNCH_PANEL_HEIGHT) &&
	    !(io->dsi_status(io->cookie) & 4) && !io->read(io->cookie, ctl + MUNCH_SDE_CTL_START) &&
	    !io->read(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH);
}

static inline enum munch_sde_result
munch_sde_pause_refresh_internal(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
    const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, munch_sde_delay delay,
    bool rollback, bool strict_wait)
{
	if (!(saved->autorefresh & MUNCH_SDE_AUTOREFRESH_ENABLE)) return MUNCH_SDE_OK;
	if (refresh->paused) return munch_sde_refresh_guard(io);
	if (refresh->owned) return MUNCH_SDE_STALE;
	if (saved->autorefresh != (MUNCH_SDE_AUTOREFRESH_ENABLE | 1) || !delay) return MUNCH_SDE_UNSUPPORTED;
	enum munch_sde_result result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	uint32_t ctl = munch_sde_ctl_offsets[route->ctl];
	if (io->read(io->cookie, ctl + MUNCH_SDE_CTL_START) || io->read(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH))
		return MUNCH_SDE_BUSY;
	refresh->config = saved->autorefresh;
	refresh->vsync = saved->tear_vsync & ~MUNCH_SDE_EXTERNAL_TE;
	refresh->owned = true;
	refresh->phase = "disconnect-te";
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC, refresh->vsync);
	io->barrier(io->cookie);
	result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) {
		if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
		return result;
	}
	refresh->config &= ~MUNCH_SDE_AUTOREFRESH_ENABLE;
	refresh->phase = "drain";
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH, refresh->config);
	io->barrier(io->cookie);
	bool idle = false;
	for (unsigned attempt = 0; attempt < 25; attempt++) {
		delay(io->cookie, 2000);
		result = munch_sde_refresh_guard(io);
		if (result != MUNCH_SDE_OK) {
			if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
			return result;
		}
		munch_sde_refresh_reset_tearcheck(io);
		if (munch_sde_refresh_idle(refresh, io, route)) { idle = true; break; }
	}
	if (!idle) {
		if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
		return MUNCH_SDE_TIMEOUT;
	}
	refresh->phase = "verify-off";
	refresh->status = munch_sde_refresh_status(io);
	if (refresh->status & MUNCH_SDE_AUTOREFRESH_BUSY) {
		if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
		return MUNCH_SDE_BUSY;
	}
	if (strict_wait) {
		delay(io->cookie, 25000);
		result = munch_sde_refresh_guard(io);
		if (result != MUNCH_SDE_OK) {
			if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
			return result;
		}
		refresh->status = munch_sde_refresh_status(io);
		if ((refresh->status & MUNCH_SDE_AUTOREFRESH_BUSY) || !munch_sde_refresh_idle(refresh, io, route)) {
			if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
			return MUNCH_SDE_BUSY;
		}
	}
	refresh->phase = "reset-tearcheck";
	munch_sde_refresh_reset_tearcheck(io);
	refresh->phase = "reconnect-te";
	refresh->vsync = saved->tear_vsync;
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC, refresh->vsync);
	io->barrier(io->cookie);
	result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) {
		if (rollback) munch_sde_refresh_rollback(refresh, io, saved);
		return result;
	}
	refresh->paused = true;
	refresh->phase = "paused";
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_pause_refresh(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
    const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, munch_sde_delay delay)
{
	return munch_sde_pause_refresh_internal(refresh, io, saved, route, delay, true, true);
}

static inline enum munch_sde_result
munch_sde_pause_refresh_fast(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
	const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, munch_sde_delay delay)
{
	return munch_sde_pause_refresh_internal(refresh, io, saved, route, delay, true, false);
}

static inline enum munch_sde_result
munch_sde_pause_refresh_for_return(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
    const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, munch_sde_delay delay)
{
	return munch_sde_pause_refresh_internal(refresh, io, saved, route, delay, false, true);
}

static inline enum munch_sde_result
munch_sde_restore_refresh(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
    const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, uint32_t source,
    munch_sde_delay delay)
{
	if (!refresh->owned) return MUNCH_SDE_OK;
	if (!refresh->paused || !delay) return MUNCH_SDE_STALE;
	enum munch_sde_result result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	if (io->read(io->cookie, munch_sde_sspps[route->pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR) != source)
		return MUNCH_SDE_STALE;
	if (!munch_sde_refresh_idle(refresh, io, route)) return MUNCH_SDE_BUSY;
	io->write(io->cookie, MUNCH_SDE_INTR_CLEAR, route->done_mask);
	io->barrier(io->cookie);
	if (io->read(io->cookie, MUNCH_SDE_INTR_STATUS) & route->done_mask) return MUNCH_SDE_STALE;
	refresh->phase = "restore-autorefresh";
	refresh->config = saved->autorefresh;
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH, refresh->config);
	io->barrier(io->cookie);
	for (unsigned attempt = 0; attempt < 50; attempt++) {
		delay(io->cookie, 1000);
		result = munch_sde_refresh_guard(io);
		if (result != MUNCH_SDE_OK) return result;
		if ((io->read(io->cookie, MUNCH_SDE_INTR_STATUS) & route->done_mask) &&
		    munch_sde_refresh_idle(refresh, io, route)) {
			io->write(io->cookie, MUNCH_SDE_INTR_CLEAR, route->done_mask);
			io->barrier(io->cookie);
			refresh->paused = false;
			refresh->owned = false;
			refresh->phase = "restored";
			return MUNCH_SDE_OK;
		}
	}
	return MUNCH_SDE_TIMEOUT;
}

static inline enum munch_sde_result
munch_sde_restore_refresh_fast(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
	const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, uint32_t source,
	munch_sde_delay delay)
{
	if (!refresh->owned) return MUNCH_SDE_OK;
	if (!refresh->paused || !delay) return MUNCH_SDE_STALE;
	enum munch_sde_result result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	if (io->read(io->cookie, munch_sde_sspps[route->pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR) != source)
		return MUNCH_SDE_STALE;
	uint32_t ctl = munch_sde_ctl_offsets[route->ctl];
	if (io->read(io->cookie, ctl + MUNCH_SDE_CTL_START) ||
	    io->read(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH)) return MUNCH_SDE_BUSY;
	refresh->phase = "restore-autorefresh-fast";
	refresh->config = saved->autorefresh;
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH, refresh->config);
	io->barrier(io->cookie);
	if (io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) !=
	    saved->autorefresh) {
		refresh->phase = "restore-autorefresh-fast-failed";
		return MUNCH_SDE_STALE;
	}
	result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	refresh->vsync = saved->tear_vsync;
	refresh->owned = false;
	refresh->paused = false;
	refresh->phase = "restored-fast";
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_restore_refresh_safe(struct munch_sde_refresh *refresh, const struct munch_sde_io *io,
	const struct munch_sde_snapshot *saved, const struct munch_sde_route *route, uint32_t source,
	munch_sde_delay delay, bool *recovered)
{
	if (recovered) *recovered = false;
	enum munch_sde_result result = munch_sde_restore_refresh(refresh, io, saved, route, source, delay);
	if (result != MUNCH_SDE_TIMEOUT || !refresh->owned || !refresh->paused) return result;
	result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	if (io->read(io->cookie, munch_sde_sspps[route->pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR) != source)
		return MUNCH_SDE_STALE;
	if (!munch_sde_refresh_idle(refresh, io, route)) return MUNCH_SDE_BUSY;
	refresh->phase = "restore-controls";
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC, saved->tear_vsync);
	io->write(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH, saved->autorefresh);
	io->barrier(io->cookie);
	if (io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) != saved->tear_vsync ||
	    io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) != saved->autorefresh) {
		refresh->phase = "rollback-failed";
		return MUNCH_SDE_STALE;
	}
	result = munch_sde_refresh_guard(io);
	if (result != MUNCH_SDE_OK) return result;
	refresh->config = saved->autorefresh;
	refresh->vsync = saved->tear_vsync;
	refresh->owned = false;
	refresh->paused = false;
	refresh->phase = "restored-controls";
	if (recovered) *recovered = true;
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_build_branch(uint64_t *tables, unsigned pages, uint64_t physical,
    uint32_t base, unsigned attribute, struct munch_sde_buffer buffers[3], unsigned *used)
{
	if (!tables || !buffers || !used || pages < 2 || !physical || (physical & 4095) || physical >= (1ULL << 48) ||
	    pages > 66 || physical + (uint64_t)pages * 4096 > (1ULL << 48) ||
	    (base & 0x3fffffff) || attribute > 7) return MUNCH_SDE_MAPPING;
	if (!buffers[0].length || buffers[0].length > 64 * 1024 * 1024 ||
	    buffers[0].physical >= (1ULL << 48) || buffers[0].physical + buffers[0].length > (1ULL << 48) ||
	    (uint64_t)buffers[0].iova + buffers[0].length > (1ULL << 32)) return MUNCH_SDE_MAPPING;
	uint64_t next = (uint64_t)base + 0x20000;
	for (unsigned index = 1; index < 3; index++) {
		if (!buffers[index].physical || !buffers[index].length || buffers[index].length > 64 * 1024 * 1024 ||
		    ((buffers[index].physical | buffers[index].length) & 4095) ||
		    buffers[index].physical + buffers[index].length < buffers[index].physical ||
		    buffers[index].physical + buffers[index].length > (1ULL << 48) ||
		    next + buffers[index].length > (uint64_t)base + 0x40000000)
			return MUNCH_SDE_MAPPING;
		if (munch_sde_overlaps(buffers[index].physical, buffers[index].length,
		    buffers[0].physical, buffers[0].length) ||
		    munch_sde_overlaps(buffers[index].physical, buffers[index].length, physical, pages * 4096))
			return MUNCH_SDE_MAPPING;
		next += buffers[index].length;
	}
	if (munch_sde_overlaps(base, 0x40000000, buffers[0].iova, buffers[0].length) ||
	    munch_sde_overlaps(physical, pages * 4096, buffers[0].physical, buffers[0].length) ||
	    1 + ((next - base + 0x1fffff) >> 21) > pages ||
	    munch_sde_overlaps(buffers[1].physical, buffers[1].length, buffers[2].physical, buffers[2].length))
		return MUNCH_SDE_MAPPING;
	buffers[1].iova = base + 0x20000;
	buffers[2].iova = buffers[1].iova + buffers[1].length;
	for (unsigned index = 0; index < pages * 512; index++) tables[index] = 0;
	*used = 1;
	uint64_t flags = 3ULL | (3ULL << 6) | (2ULL << 8) | (1ULL << 10) | (1ULL << 11) |
	    (3ULL << 53) | ((uint64_t)attribute << 2);
	for (unsigned index = 1; index < 3; index++) {
		for (uint32_t offset = 0; offset < buffers[index].length; offset += 4096) {
			uint32_t iova = buffers[index].iova + offset;
			unsigned block = (iova >> 21) & 511;
			if (!tables[block]) {
				if (*used == pages) return MUNCH_SDE_MAPPING;
				tables[block] = (physical + (uint64_t)(*used)++ * 4096) | 3;
			}
			unsigned page = (unsigned)(((tables[block] & 0x0000fffffffff000ULL) - physical) / 4096);
			tables[page * 512 + ((iova >> 12) & 511)] = (buffers[index].physical + offset) | flags;
		}
	}
	return MUNCH_SDE_OK;
}

struct munch_sde_presenter {
	struct munch_sde_io io;
	struct munch_sde_route route;
	struct munch_sde_buffer buffers[3];
	enum munch_sde_result fault;
	unsigned front, pending, drawing;
	uint64_t submitted, completed, deadline_us;
	bool armed, started;
};

static inline unsigned
munch_sde_latest_buffer(const struct munch_sde_presenter *presenter)
{
	return presenter->pending != 3 ? presenter->pending : presenter->front;
}

static inline enum munch_sde_result
munch_sde_prepare(struct munch_sde_presenter *presenter, const struct munch_sde_io *io,
    const struct munch_sde_snapshot *snapshot, const struct munch_smmu_domain *domain,
    munch_smmu_read_pte read_pte, const struct munch_sde_buffer buffers[3])
{
	if (presenter->armed) return MUNCH_SDE_BUSY;
	if (!io->read || !io->write || !io->barrier || !io->clean || !io->guard || !io->dsi_status)
		return MUNCH_SDE_DISABLED;
	struct munch_sde_route route;
	enum munch_sde_result result = munch_sde_find_route(snapshot, &route);
	if (result != MUNCH_SDE_OK) return result;
	result = io->guard(io->cookie);
	if (result != MUNCH_SDE_OK) return result;
	result = munch_sde_check_buffers(domain, read_pte, io->cookie, &route, buffers);
	if (result != MUNCH_SDE_OK) return result;
	result = io->guard(io->cookie);
	if (result != MUNCH_SDE_OK) return result;
	presenter->io = *io;
	presenter->route = route;
	for (unsigned index = 0; index < 3; index++) presenter->buffers[index] = buffers[index];
	presenter->front = 0;
	presenter->pending = presenter->drawing = 3;
	presenter->submitted = presenter->completed = 0;
	presenter->fault = MUNCH_SDE_OK;
	presenter->started = false;
	presenter->armed = true;
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_acquire(struct munch_sde_presenter *presenter, unsigned *index)
{
	if (!presenter->armed) return MUNCH_SDE_DISABLED;
	if (presenter->fault != MUNCH_SDE_OK) return presenter->fault;
	if (presenter->drawing != 3) return MUNCH_SDE_BUSY;
	if (presenter->pending != 3 && presenter->front >= 1 && presenter->front <= 2 &&
	    presenter->front != presenter->pending) {
		*index = presenter->front;
	} else {
		*index = presenter->front == 1 ? 2 : 1;
		if (*index == presenter->pending) *index = presenter->pending == 1 ? 2 : 1;
	}
	if (*index == 3) {
		for (unsigned candidate = 1; candidate < 3; candidate++) {
			if (candidate != presenter->pending && candidate != presenter->front) {
				*index = candidate;
				break;
			}
		}
	}
	if (*index == 3) return MUNCH_SDE_BUSY;
	presenter->drawing = *index;
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_submit_internal(struct munch_sde_presenter *presenter, uint64_t now_us, uint32_t timeout_us,
    bool validate)
{
	if (!presenter->armed) return MUNCH_SDE_DISABLED;
	if (presenter->fault != MUNCH_SDE_OK) return presenter->fault;
	if (presenter->drawing == 3 || presenter->pending != 3) return MUNCH_SDE_BUSY;
	if (!timeout_us || timeout_us > 100000 || now_us > UINT64_MAX - timeout_us ||
	    presenter->submitted == UINT64_MAX) return MUNCH_SDE_UNSUPPORTED;
	const struct munch_sde_io *io = &presenter->io;
	uint32_t ctl = munch_sde_ctl_offsets[presenter->route.ctl];
	enum munch_sde_result result = MUNCH_SDE_OK;
	if (validate) {
		result = io->guard(io->cookie);
		if (result != MUNCH_SDE_OK) return presenter->fault = result;
	}
	if (io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) & MUNCH_SDE_AUTOREFRESH_ENABLE)
		return MUNCH_SDE_BUSY;
	if (io->read(io->cookie, ctl + MUNCH_SDE_CTL_START) ||
	    io->read(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH) || (io->dsi_status(io->cookie) & (1u << 2)))
		return MUNCH_SDE_BUSY;
	if (io->read(io->cookie, MUNCH_SDE_INTR_STATUS) & MUNCH_SDE_INTF1_UNDERRUN)
		return presenter->fault = MUNCH_SDE_FAULT;
	if (!io->clean(io->cookie, presenter->drawing, presenter->buffers[presenter->drawing].length))
		return presenter->fault = MUNCH_SDE_MAPPING;
	/* The native XNU wrapper validates the SMMU domain at initialization and
	 * on the first handoff.  Repeating the full guard here costs more than the
	 * command submission itself. */
	io->barrier(io->cookie);
	io->write(io->cookie, MUNCH_SDE_INTR_CLEAR, presenter->route.done_mask);
	io->barrier(io->cookie);
	if (io->read(io->cookie, MUNCH_SDE_INTR_STATUS) & presenter->route.done_mask)
		return presenter->fault = MUNCH_SDE_STALE;
	presenter->pending = presenter->drawing;
	presenter->drawing = 3;
	presenter->started = false;
	presenter->deadline_us = now_us + timeout_us;
	presenter->submitted++;
	io->barrier(io->cookie);
	io->write(io->cookie, munch_sde_sspps[presenter->route.pipe].offset + MUNCH_SDE_SSPP_SRC_ADDR,
	    presenter->buffers[presenter->pending].iova);
	io->barrier(io->cookie);
	io->write(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH, presenter->route.flush_mask);
	io->barrier(io->cookie);
	if (io->kickoff) io->kickoff(io->cookie);
	io->write(io->cookie, ctl + MUNCH_SDE_CTL_START, 1);
	io->barrier(io->cookie);
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_submit(struct munch_sde_presenter *presenter, uint64_t now_us, uint32_t timeout_us)
{
	return munch_sde_submit_internal(presenter, now_us, timeout_us, true);
}

static inline enum munch_sde_result
munch_sde_submit_fast(struct munch_sde_presenter *presenter, uint64_t now_us, uint32_t timeout_us)
{
	return munch_sde_submit_internal(presenter, now_us, timeout_us, false);
}

static inline enum munch_sde_result
munch_sde_poll_internal(struct munch_sde_presenter *presenter, uint64_t now_us, bool full_guard,
    bool completion_hint)
{
	if (!presenter->armed) return MUNCH_SDE_DISABLED;
	if (presenter->fault != MUNCH_SDE_OK) return presenter->fault;
	if (presenter->pending == 3) return MUNCH_SDE_OK;
	const struct munch_sde_io *io = &presenter->io;
	if (full_guard) {
		enum munch_sde_result result = io->guard(io->cookie);
		if (result != MUNCH_SDE_OK) return presenter->fault = result;
	}
	uint32_t status = io->read(io->cookie, MUNCH_SDE_INTR_STATUS);
	uint32_t ctl = munch_sde_ctl_offsets[presenter->route.ctl];
	if (status & MUNCH_SDE_INTF1_UNDERRUN) return presenter->fault = MUNCH_SDE_FAULT;
	if (now_us >= presenter->deadline_us) return presenter->fault = MUNCH_SDE_TIMEOUT;
	if (!presenter->started && !completion_hint) {
		uint32_t line = io->read(io->cookie, MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) & 0xffff;
		if (line < 1) return MUNCH_SDE_BUSY;
		presenter->started = true;
	}
	if (!completion_hint && (!(status & presenter->route.done_mask) ||
	    io->read(io->cookie, ctl + MUNCH_SDE_CTL_START) ||
	    io->read(io->cookie, ctl + MUNCH_SDE_CTL_FLUSH))) return MUNCH_SDE_BUSY;
	if (full_guard) {
		enum munch_sde_result result = io->guard(io->cookie);
		if (result != MUNCH_SDE_OK) return presenter->fault = result;
	}
	io->barrier(io->cookie);
	io->write(io->cookie, MUNCH_SDE_INTR_CLEAR, presenter->route.done_mask);
	io->barrier(io->cookie);
	presenter->front = presenter->pending;
	presenter->pending = 3;
	presenter->started = false;
	presenter->completed = presenter->submitted;
	return MUNCH_SDE_OK;
}

static inline enum munch_sde_result
munch_sde_poll(struct munch_sde_presenter *presenter, uint64_t now_us)
{
	return munch_sde_poll_internal(presenter, now_us, true, false);
}

static inline enum munch_sde_result
munch_sde_poll_fast(struct munch_sde_presenter *presenter, uint64_t now_us)
{
	return munch_sde_poll_internal(presenter, now_us, false, false);
}

static inline enum munch_sde_result
munch_sde_poll_fast_hint(struct munch_sde_presenter *presenter, uint64_t now_us, bool completion_hint)
{
	return munch_sde_poll_internal(presenter, now_us, false, completion_hint);
}

#endif
