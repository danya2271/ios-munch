/* SPDX-License-Identifier: MIT
 * Munch A650 first-access probe. Only GCC and gated GPUCC reads.
 * Offsets: UMI Kona driver; matched against captured munch hardware profile.
 * No clocks, regulators, GPU/GMU/SMMU registers or firmware are written.
 */
static uint32_t gpu_probe_read(uint64_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}
static void gpu_probe(void)
{
    text("\r\nMUNCH A650 READ-ONLY PROBE 1\r\n");
    text("No iOS GPU acceleration in this image.\r\n");
    text("BEFORE GCC GPU CFG AHB read\r\n");
    uint32_t ahb = gpu_probe_read(0x00171004);
    field("GCC GPU CFG AHB: ", ahb);
    field("GCC GPU MEMNOC GFX: ", gpu_probe_read(0x0017100c));
    field("GCC GPU SNOC DVM GFX: ", gpu_probe_read(0x00171018));
    if ((ahb & 1) && !(ahb & 0x80000000u)) {
        text("BEFORE GPUCC PLL0 MODE read\r\n");
        field("GPUCC PLL0 MODE: ", gpu_probe_read(0x03d90000));
        text("BEFORE GPUCC PLL0 L read\r\n");
        field("GPUCC PLL0 L: ", gpu_probe_read(0x03d90004));
        text("GPUCC READ ACCESS COMPLETED\r\n");
    } else {
        text("GPUCC READ SKIPPED: CFG AHB clock is not confirmed running.\r\n");
    }
    text("MUNCH A650 PROBE FINISHED\r\n");
}
