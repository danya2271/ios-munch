#ifndef _IOKIT_MUNCH_KONA_DISPLAY_H
#define _IOKIT_MUNCH_KONA_DISPLAY_H

#define MUNCH_MDP_PHYS                    0x0ae00000UL
#define MUNCH_MDP_SIZE                    0x084208UL
#define MUNCH_MDP_VERSION                 0x000

#define MUNCH_DSI_ENGINE_ENABLED          (1u << 0)
#define MUNCH_DSI_CMD_MODE_ENABLED        (1u << 2)
#define MUNCH_DSI_DATA_LANES_ENABLED      (0xfu << 4)
#define MUNCH_DSI_CLOCK_LANE_ENABLED      (1u << 8)
#define MUNCH_DSI_RX_ECC_ENABLED          (1u << 20)
#define MUNCH_DSI_RX_CRC_ENABLED          (1u << 24)

#define MUNCH_SDE_CTL_BASE                0x02000
#define MUNCH_SDE_CTL_STRIDE              0x00200
#define MUNCH_SDE_CTL_COUNT               6
#define MUNCH_SDE_CTL_TOP                 0x014
#define MUNCH_SDE_CTL_FLUSH               0x018
#define MUNCH_SDE_CTL_START               0x01c
#define MUNCH_SDE_CTL_LAYER0              0x000
#define MUNCH_SDE_CTL_LAYER_EXT           0x040
#define MUNCH_SDE_CTL_LAYER_EXT2          0x070
#define MUNCH_SDE_CTL_LAYER_EXT3          0x0a0
#define MUNCH_SDE_CTL_INTF_ACTIVE         0x0f4
#define MUNCH_SDE_CTL_INTF_FLUSH          0x110
#define MUNCH_SDE_CTL_MERGE_ACTIVE        0x0e4
#define MUNCH_SDE_CTL_DSC_ACTIVE          0x0e8
#define MUNCH_SDE_CTL_WB_ACTIVE           0x0ec
#define MUNCH_SDE_CTL_CWB_ACTIVE          0x0f0
#define MUNCH_SDE_CTL_CDM_ACTIVE          0x0f8
#define MUNCH_SDE_LM_BASE                 0x45000
#define MUNCH_SDE_LM_STRIDE               0x01000
#define MUNCH_SDE_LM_OP_MODE              0x000
#define MUNCH_SDE_LM_OUT_SIZE             0x004
#define MUNCH_SDE_LM_BLEND0_OP            0x020
#define MUNCH_SDE_LM_BLEND0_ALPHA         0x024
#define MUNCH_SDE_LM_BLEND_COUNT          11
#define MUNCH_SDE_ROUTE_MAX_STAGE         6
#define MUNCH_SDE_LM_OPAQUE_BLEND         (1u << 8)
#define MUNCH_SDE_LM_INVERSE_BG_BLEND     (1u << 10)
#define MUNCH_SDE_LM_OPAQUE_ALPHA         (0xffu << 16)
#define MUNCH_SDE_LM_DIM_LAYER            (1u << 16)
#define MUNCH_SDE_CTL_BORDER_OUT          (1u << 24)
#define MUNCH_SDE_CTL_CMD_MODE            (1u << 17)
#define MUNCH_SDE_INTF1_ACTIVE            (1u << 1)
#define MUNCH_SDE_INTF1_FLUSH             (1u << 1)

#define MUNCH_SDE_INTF1_OFFSET            0x6b800
#define MUNCH_SDE_INTF_ENABLE             0x000
#define MUNCH_SDE_INTF_FRAME_LINE_COUNT_EN 0x0a8
#define MUNCH_SDE_INTF_FRAME_COUNT        0x0ac
#define MUNCH_SDE_INTF_LINE_COUNT         0x0b0
#define MUNCH_SDE_INTF_MUX                0x25c
#define MUNCH_SDE_INTF_STATUS             0x26c
#define MUNCH_SDE_INTF_TEAR_MDP_VSYNC     0x280
#define MUNCH_SDE_INTF_TEAR_ENABLE        0x284
#define MUNCH_SDE_INTF_TEAR_SYNC_VSYNC    0x288
#define MUNCH_SDE_INTF_TEAR_HEIGHT        0x28c
#define MUNCH_SDE_INTF_TEAR_SYNC_WRCOUNT  0x290
#define MUNCH_SDE_INTF_TEAR_VSYNC_INIT    0x294
#define MUNCH_SDE_INTF_TEAR_INT_COUNT     0x298
#define MUNCH_SDE_INTF_TEAR_SYNC_THRESH   0x29c
#define MUNCH_SDE_INTF_TEAR_START_POS     0x2a0
#define MUNCH_SDE_INTF_TEAR_RD_PTR_IRQ    0x2a4
#define MUNCH_SDE_INTF_TEAR_WR_PTR_IRQ    0x2a8
#define MUNCH_SDE_INTF_TEAR_OUT_LINE      0x2ac
#define MUNCH_SDE_INTF_TEAR_LINE          0x2b0
#define MUNCH_SDE_INTF_TEAR_AUTOREFRESH   0x2b4
#define MUNCH_SDE_AUTOREFRESH_ENABLE     (1u << 31)
#define MUNCH_SDE_AUTOREFRESH_FRAMES     0xffffu
#define MUNCH_SDE_EXTERNAL_TE            (1u << 20)
#define MUNCH_SDE_AUTOREFRESH_BUSY       (1u << 7)
#define MUNCH_SDE_DEBUG_CTRL             0x1348
#define MUNCH_SDE_DEBUG_STATUS           0x134c
#define MUNCH_SDE_DEBUG_PERIPH           0x1418
#define MUNCH_SDE_INTF1_AUTOREFRESH_TEST  0x405
#define MUNCH_SDE_INTF_TEAR_DETECT        0x2b8

#define MUNCH_SDE_FLUSH_VIG0              (1u << 0)
#define MUNCH_SDE_FLUSH_VIG1              (1u << 1)
#define MUNCH_SDE_FLUSH_VIG2              (1u << 2)
#define MUNCH_SDE_FLUSH_VIG3              (1u << 18)
#define MUNCH_SDE_FLUSH_DMA0              (1u << 3)
#define MUNCH_SDE_FLUSH_DMA1              (1u << 4)
#define MUNCH_SDE_FLUSH_DMA2              (1u << 5)
#define MUNCH_SDE_FLUSH_DMA3              (1u << 19)
#define MUNCH_SDE_FLUSH_INTF1             (1u << 31)

#define MUNCH_SDE_SSPP_SRC_SIZE           0x000
#define MUNCH_SDE_SSPP_SRC_XY             0x008
#define MUNCH_SDE_SSPP_OUT_SIZE           0x00c
#define MUNCH_SDE_SSPP_OUT_XY             0x010
#define MUNCH_SDE_SSPP_SRC_ADDR           0x014
#define MUNCH_SDE_SSPP_SRC_YSTRIDE        0x024
#define MUNCH_SDE_SSPP_SRC_FORMAT         0x030
#define MUNCH_SDE_SSPP_SRC_UNPACK         0x034
#define MUNCH_SDE_SSPP_SRC_OP_MODE        0x038
#define MUNCH_SDE_SSPP_PE_OVERRIDE        (1u << 31)
#define MUNCH_SDE_SSPP_PRE_DOWNSCALE      0x050
#define MUNCH_SDE_SSPP_SECURE             0x070
#define MUNCH_SDE_SSPP_DECIMATION         0x0b4
#define MUNCH_SDE_SSPP_PE_BASE            0x100
#define MUNCH_SDE_SSPP_PE_STRIDE          0x010
#define MUNCH_SDE_SSPP_PE_COUNT           3
#define MUNCH_SDE_SSPP_MULTIRECT          0x170
#define MUNCH_SDE_VIG_SCALER_OP_MODE      0x0a04
#define MUNCH_SDE_VIG_CSC_OP_MODE         0x1a00
#define MUNCH_SDE_VIG_CSC_SRC_DATAFMT     (1u << 1)
#define MUNCH_SDE_VIG_SCALER_BYPASS_MASK  ((1u << 10) | (1u << 14) | (3u << 29))
#define MUNCH_SDE_INTR_STATUS             0x1014
#define MUNCH_SDE_INTR_CLEAR              0x1018
#define MUNCH_SDE_INTR_ENABLE             0x1010
#define MUNCH_SDE_GLOBAL_INTR_STATUS      0x0010
#define MUNCH_SDE_GLOBAL_INTR_MDP         (1u << 0)
#define MUNCH_SDE_INTF1_UNDERRUN           (1u << 26)

#define MUNCH_SDE_MAJOR_KONA              6
#define MUNCH_SDE_IOMMU_UNSECURE_SID      0x820
#define MUNCH_SDE_IOMMU_UNSECURE_MASK     0x402
#define MUNCH_SDE_IOMMU_SECURE_SID        0x821
#define MUNCH_SDE_IOMMU_IOVA_BASE         0x00020000UL
#define MUNCH_SDE_IOMMU_IOVA_SIZE         0xfffe0000UL
#define MUNCH_APPS_SMMU_PHYS              0x15000000UL
#define MUNCH_APPS_SMMU_WINDOW_SIZE       0x000d0000UL
#define MUNCH_SMMU_IDR0                   0x020
#define MUNCH_SMMU_IDR1                   0x024
#define MUNCH_SMMU_SCR0                   0x000
#define MUNCH_SMMU_SMR(n)                 (0x800 + 4 * (n))
#define MUNCH_SMMU_S2CR(n)                (0xc00 + 4 * (n))
#define MUNCH_SMMU_CBAR(n)                (4 * (n))
#define MUNCH_SMMU_CBA2R(n)               (0x800 + 4 * (n))
#define MUNCH_SMMU_SMR_VALID              (1u << 31)
#define MUNCH_SMMU_S2CR_EXIDVALID         (1u << 10)
#define MUNCH_SMMU_S2CR_TYPE_SHIFT        16
#define MUNCH_SMMU_CBNDX_MASK             0xff
#define MUNCH_SMMU_SCR0_EXIDENABLE        (1u << 3)
#define MUNCH_SMMU_IDR0_EXIDS             (1u << 8)
#define MUNCH_SMMU_CB_SCTLR               0x00
#define MUNCH_SMMU_CB_TCR2                0x10
#define MUNCH_SMMU_CB_TTBR0               0x20
#define MUNCH_SMMU_CB_TCR                 0x30
#define MUNCH_SMMU_CB_MAIR0               0x38
#define MUNCH_SMMU_CB_MAIR1               0x3c
#define MUNCH_SMMU_CB_FSR                 0x58
#define MUNCH_SMMU_CB_FAR                 0x60
#define MUNCH_SMMU_CB_FSYNR0              0x68
#define MUNCH_SMMU_CB_FSYNR1              0x6c
#define MUNCH_SMMU_FSR_FAULT              0xc00001feu
#define MUNCH_SMMU_FSR_FORMAT             (3u << 9)
#define MUNCH_SMMU_SCTLR_M               (1u << 0)

struct munch_sde_sspp {
	const char *name;
	unsigned offset;
};

static const char *const munch_sde_pe_names[MUNCH_SDE_SSPP_PE_COUNT] = { "C0", "C1C2", "C3" };

static const struct munch_sde_sspp munch_sde_sspps[] = {
	{ "VIG0", 0x05000 }, { "VIG1", 0x07000 }, { "VIG2", 0x09000 }, { "VIG3", 0x0b000 },
	{ "DMA0", 0x25000 }, { "DMA1", 0x27000 }, { "DMA2", 0x29000 }, { "DMA3", 0x2b000 },
};

static const unsigned munch_sde_ctl_offsets[MUNCH_SDE_CTL_COUNT] = {
	0x02000, 0x02200, 0x02400, 0x02600, 0x02800, 0x02a00,
};

static const unsigned munch_sde_blend_offsets[MUNCH_SDE_LM_BLEND_COUNT] = {
	0x020, 0x038, 0x050, 0x068, 0x080, 0x098, 0x0b0, 0x0c8, 0x0e0, 0x0f8, 0x110,
};

static const unsigned munch_sde_ctl_layer_offsets[4][6] = {
	{ 0x000, 0x004, 0x008, 0x00c, 0x010, 0x024 },
	{ 0x040, 0x044, 0x048, 0x04c, 0x050, 0x054 },
	{ 0x070, 0x074, 0x078, 0x07c, 0x080, 0x084 },
	{ 0x0a0, 0x0a4, 0x0a8, 0x0ac, 0x0b0, 0x0b4 },
};

#define MUNCH_PANEL_WIDTH                  1080
#define MUNCH_PANEL_HEIGHT                 2400
#define MUNCH_PANEL_DSI_CONTROLLER         0
#define MUNCH_PANEL_DSI_PHY                0
#define MUNCH_PANEL_TE_GPIO                66
#define MUNCH_PANEL_DSC_SLICE_WIDTH        540
#define MUNCH_PANEL_DSC_SLICE_HEIGHT       8
#define MUNCH_PANEL_DSC_BITS_PER_COMPONENT 8
#define MUNCH_PANEL_DSC_BITS_PER_PIXEL     8

struct munch_panel_mode {
	unsigned refresh_hz;
	unsigned h_front_porch;
	unsigned h_back_porch;
	unsigned h_sync_width;
	unsigned v_front_porch;
	unsigned v_back_porch;
	unsigned v_sync_width;
	unsigned mdp_transfer_time_us;
};

static const struct munch_panel_mode munch_panel_modes[] = {
	{ 60, 16, 8, 8, 600, 560, 32, 14000 },
	{ 120, 16, 8, 8, 4, 8, 4, 7000 },
	{ 144, 16, 8, 8, 580, 580, 32, 7000 },
};

#endif
