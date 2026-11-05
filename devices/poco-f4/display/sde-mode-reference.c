#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(bit) (1u << (bit))
#define DIV_ROUND_UP(value, divisor) (((value) + (divisor) - 1) / (divisor))
#define DIV_ROUND_CLOSEST(value, divisor) (((value) + (divisor) / 2) / (divisor))
#define DIV_ROUND_CLOSEST_ULL(value, divisor) DIV_ROUND_CLOSEST(value, divisor)
#define div_u64(value, divisor) ((value) / (divisor))
#define __iomem
#define pr_debug(...) ((void)0)
#define pr_info(...) ((void)0)
#define DSI_PHY_DBG(...) ((void)0)
#define DSI_CTRL_HW_DBG(...) ((void)0)
#define DSI_ERR(...) ((void)0)
#define MDSS_DSI_PLL_7NM 1
#define MDSS_DSI_PLL_7NM_V2 2
#define MDSS_DSI_PLL_7NM_V4_1 3
#define DSI_PHY_VERSION_4_1 41
#define DSI_LOGICAL_LANE_0 0
#define DSI_LOGICAL_LANE_1 1
#define DSI_LOGICAL_LANE_2 2
#define DSI_LOGICAL_LANE_3 3
#define DSI_LANE_MAX 5
#define DSI_DATA_LANE_0 1
#define DSI_DATA_LANE_1 2
#define DSI_DATA_LANE_2 4
#define DSI_DATA_LANE_3 8
#define DSI_PLL_SOURCE_STANDALONE 0
#define DSI_PLL_SOURCE_NATIVE 1
#define DSI_PLL_SOURCE_NON_NATIVE 2
#define DSI_PIXEL_FORMAT_MAX 7

struct mdss_pll_resources {
	u64 vco_current_rate, vco_ref_clk_rate;
	u32 pll_interface_type, ssc_freq, ssc_ppm;
	bool ssc_en, ssc_center;
	void *pll_base;
	struct mdss_pll_resources *slave;
};
struct dsi_lane_map { u8 lane_map_v2[4]; };
struct dsi_phy_per_lane_cfgs { u32 lane_v4[14], lane[5][4]; };
struct dsi_phy_cfg {
	struct dsi_phy_per_lane_cfgs timing, lanecfg, strength;
	struct dsi_lane_map lane_map;
	u64 bit_clk_rate_hz;
	u32 phy_voltage, clk_strength, data_lanes, pll_source;
};
struct dsi_phy_hw { unsigned version; };
struct sde_hw_blk_reg_map { unsigned unused; };
struct sde_hw_dsc { struct sde_hw_blk_reg_map hw; };
struct sde_hw_intf { struct sde_hw_blk_reg_map hw; };
struct dsi_ctrl_hw { bool null_insertion_enabled; };
struct dsi_mode_info { bool dsc_enabled; struct msm_display_dsc_info *dsc; u32 h_active, v_active; };
struct dsi_rect { u32 w, h; };
struct dsi_host_common_cfg { bool bit_swap_red, bit_swap_green, bit_swap_blue; unsigned dst_format; };
struct dsi_cmd_engine_cfg { u32 max_cmd_packets_interleave, wr_mem_start, wr_mem_continue; bool insert_dcs_command; };

static u64 div_u64_rem(u64 value, u32 divisor, u32 *remainder)
{
	*remainder = value % divisor;
	return value / divisor;
}

static void dsi_pll_config_slave(struct mdss_pll_resources *resources)
{
	assert(!resources->slave);
}

static int dsi_phy_conv_logical_to_phy_lane(struct dsi_lane_map *map, unsigned lane)
{
	return map->lane_map_v2[lane];
}

static void record_write(unsigned offset, unsigned value)
{
	printf("\t{ 0x%03x, 0x%08x },\n", offset, value);
}

#define MDSS_PLL_REG_W(base, offset, value) record_write(offset, value)
#define DSI_W32(base, offset, value) record_write(offset, value)
#define DSI_R32(base, offset) 0u
#define SDE_REG_WRITE(base, offset, value) record_write(offset, value)

MODE_LINUX_FUNCTIONS

int main(void)
{
	struct mdss_pll_resources resources = {
		.vco_current_rate = 1100000000, .vco_ref_clk_rate = 19200000,
		.pll_interface_type = MDSS_DSI_PLL_7NM_V4_1, .ssc_en = true,
	};
	struct dsi_pll_7nm pll = {0};
	puts("static const struct munch_mode_register munch_l11r_pll[] = {");
	dsi_pll_init_val(&resources);
	dsi_pll_setup_config(&pll, &resources);
	dsi_pll_calc_dec_frac(&pll, &resources);
	dsi_pll_calc_ssc(&pll, &resources);
	dsi_pll_commit(&pll, &resources);
	dsi_pll_config_hzindep_reg(&pll, &resources);
	dsi_pll_ssc_commit(&pll, &resources);
	puts("};\n");
	struct dsi_phy_cfg cfg = {
		.timing = { .lane_v4 = { MODE_PHY_TIMINGS } },
		.lanecfg = { .lane = {{0, 0, 10, 10}, {0, 0, 10, 10}, {0, 0, 10, 10},
			{0, 0, 10, 10}, {0, 0, 138, 138}} },
		.strength = { .lane = {{85, 3}, {85, 3}, {85, 3}, {85, 3}, {85, 0}} },
		.lane_map = {{0, 1, 2, 3}}, .bit_clk_rate_hz = 1100000000, .data_lanes = 15,
	};
	struct dsi_phy_hw phy = { .version = DSI_PHY_VERSION_4_1 };
	puts("static const struct munch_mode_register munch_l11r_phy[] = {");
	dsi_phy_hw_dphy_enable(&phy, &cfg);
	puts("};\n");
	struct msm_display_dsc_info dsc = {
		.version = 0x11, .pic_height = 2400, .pic_width = 1080, .slice_height = 8,
		.slice_width = 540, .slice_per_pkt = 2, .bpc = 8, .bpp = 8, .block_pred_enable = true,
	};
	dsi_dsc_populate_static_param(&dsc);
	dsi_dsc_pclk_param_calc(&dsc, 1080);
	_sde_encoder_dsc_initial_line_calc(&dsc, 1080);
	char pps[135] = {0};
	static const uint8_t panel_pps[] = { MODE_PANEL_PPS };
	assert(dsi_dsc_create_pps_buf_cmd(&dsc, pps, 0) == 128);
	assert(!memcmp(pps + 7, panel_pps, sizeof(panel_pps)));
	assert(dsc.pclk_per_line == 360 && dsc.bytes_per_pkt == 1080 && dsc.initial_lines == 3);
	struct sde_hw_dsc hardware = {0};
	puts("static const struct munch_mode_register munch_l11r_dsc[] = {");
	sde_hw_dsc_config(&hardware, &dsc, 0, false);
	sde_hw_dsc_config_thresh(&hardware, &dsc);
	puts("};\n");
	struct dsi_ctrl_hw controller = {0};
	struct dsi_host_common_cfg common = { .dst_format = 3 };
	struct dsi_cmd_engine_cfg command = { .wr_mem_start = 0x2c, .wr_mem_continue = 0x3c, .insert_dcs_command = true };
	struct dsi_mode_info mode = { .dsc_enabled = true, .dsc = &dsc, .h_active = 1080, .v_active = 2400 };
	puts("static const struct munch_mode_register munch_l11r_stream[] = {");
	dsi_ctrl_hw_cmn_cmd_engine_setup(&controller, &common, &command);
	dsi_ctrl_hw_cmn_setup_cmd_stream(&controller, &mode, 1080 * 3, 0, NULL);
	puts("};\n");
	struct sde_hw_intf intf = {0};
	struct sde_hw_tear_check tear = {
		.vsync_count = 19200000 / (3592 * 60), .sync_cfg_height = 0xfff0,
		.vsync_init_val = 2400, .sync_threshold_start = 4, .sync_threshold_continue = 4,
		.start_pos = 2400, .rd_ptr_irq = 2401, .wr_ptr_irq = 1, .hw_vsync_mode = 0,
	};
	puts("static const struct munch_mode_register munch_l11r_tear[] = {");
	assert(!sde_hw_intf_setup_te_config(&intf, &tear));
	puts("};");
	return 0;
}
