#!/usr/bin/env python3
"""Execute local Linux SCTLR setup and compare SDE domain/fault classification."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux-tree', required=True, type=Path)
args = parser.parse_args()
directory = args.linux_tree / 'drivers/iommu'
source = (directory / 'arm-smmu.c').read_text()
assert re.search(r'if\s*\(!\(fsr\s*&\s*FSR_FAULT\)\)', source)
writer = source[source.index('static void arm_smmu_write_context_bank('):source.index('static int arm_smmu_init_asid(')]
control = writer[writer.index('\t/* SCTLR */'):writer.rindex('\n}')]
assert control.count('writel_relaxed') == 1
attributes = (args.linux_tree / 'include/linux/iommu.h').read_text()
attributes = re.search(r'enum iommu_attr\s*\{.*?\};', attributes, re.S).group()
program = r'''
#include <assert.h>
#include <stdio.h>
#include "arm-smmu-regs.h"
#include "munch-sde.h"
__ATTRIBUTES__
struct linux_device { bool of_node; };
struct arm_smmu_domain { struct linux_device *dev; };
static struct linux_device linux_device;
static struct arm_smmu_domain linux_domain = { &linux_device };
static bool linux_big_endian;
#define CONFIG_CPU_BIG_ENDIAN linux_big_endian
#define IS_ENABLED(option) (option)
static struct arm_smmu_domain *cb_cfg_to_smmu_domain(void *cfg)
{
    assert(cfg == &linux_domain);
    return &linux_domain;
}
static bool of_dma_is_coherent(bool node) { return node; }
static void writel_relaxed(uint32_t value, void *address) { *(uint32_t *)address = value; }
static uint32_t linux_context_control(bool stage1, uint32_t attributes, bool coherent)
{
    uint32_t reg, output = 0;
    void *cb_base = &output, *cfg = &linux_domain;
    struct arm_smmu_domain *smmu_domain;
    linux_device.of_node = coherent;
__CONTROL__
    return output;
}
static void test_context_bypass(void)
{
    _Static_assert(SCTLR_M == MUNCH_SMMU_SCTLR_M, "translation enable bit");
    _Static_assert(CBAR_TYPE_S1_TRANS_S2_BYPASS == (1u << 16), "stage1 context type");
    _Static_assert(S2CR_TYPE_TRANS == 0 && CBA2R_RW64_64BIT == 1, "context format");
    for (unsigned settings = 0; settings < 256; settings++) {
        bool stage1 = settings & 1, bypass = settings & 2, early = settings & 4, coherent = settings & 8;
        linux_big_endian = settings & 16;
        uint32_t attributes = (bypass ? 1u << DOMAIN_ATTR_S1_BYPASS : 0) |
            (early ? 1u << DOMAIN_ATTR_EARLY_MAP : 0) |
            (settings & 32 ? 1u << DOMAIN_ATTR_FAULT_MODEL_NO_CFRE : 0) |
            (settings & 64 ? 1u << DOMAIN_ATTR_FAULT_MODEL_NO_STALL : 0) |
            (settings & 128 ? 1u << DOMAIN_ATTR_FAULT_MODEL_HUPCF : 0);
        struct munch_smmu_domain domain = {0};
        domain.cbar = stage1 ? CBAR_TYPE_S1_TRANS_S2_BYPASS : CBAR_TYPE_S2_TRANS;
        domain.cbar |= CBAR_S1_MEMATTR_WB << CBAR_S1_MEMATTR_SHIFT;
        domain.cba2r = CBA2R_RW64_64BIT;
        domain.sctlr = linux_context_control(stage1, attributes, coherent);
        assert(!!(domain.sctlr & SCTLR_M) == (!stage1 || (!bypass && !early)));
        assert(!!(domain.sctlr & SCTLR_MTCFG) == coherent);
        uint64_t physical = UINT64_MAX;
        struct munch_smmu_walk walk;
        bool supported = stage1 && (bypass || early) && !coherent && !linux_big_endian;
        assert(munch_smmu_translate_checked(&domain, NULL, NULL, 0x9c000000, &physical, &walk) ==
            (supported ? MUNCH_SDE_OK : MUNCH_SDE_DOMAIN));
        assert(!walk.read && walk.level == 4);
        assert(physical == (supported ? 0x9c000000 : UINT64_MAX));
    }
    puts("Actual local Linux SCTLR setup: 256 bypass/early-map/stage/policy combinations passed");
}
__attribute__((no_sanitize("undefined")))
static uint32_t linux_fault_mask(void)
{
    return FSR_FAULT;
}
int main(void)
{
    test_context_bypass();
    uint32_t mask = linux_fault_mask();
    assert(mask == MUNCH_SMMU_FSR_FAULT);
    _Static_assert(ARM_SMMU_CB_FSR == MUNCH_SMMU_CB_FSR, "FSR offset");
    _Static_assert(ARM_SMMU_CB_FAR == MUNCH_SMMU_CB_FAR, "FAR offset");
    _Static_assert(ARM_SMMU_CB_FSYNR0 == MUNCH_SMMU_CB_FSYNR0, "FSYNR0 offset");
    _Static_assert(ARM_SMMU_CB_FSYNR1 == MUNCH_SMMU_CB_FSYNR1, "FSYNR1 offset");
    _Static_assert(ARM_SMMU_CB_SCTLR == MUNCH_SMMU_CB_SCTLR, "SCTLR offset");
    _Static_assert(ARM_SMMU_CB_TTBCR == MUNCH_SMMU_CB_TCR, "TCR offset");
    _Static_assert(ARM_SMMU_CB_TTBCR2 == MUNCH_SMMU_CB_TCR2, "TCR2 offset");
    _Static_assert(ARM_SMMU_CB_TTBR0 == MUNCH_SMMU_CB_TTBR0, "TTBR offset");
    _Static_assert(ARM_SMMU_CB_S1_MAIR0 == MUNCH_SMMU_CB_MAIR0, "MAIR offset");
    for (unsigned format = 0; format < 4; format++) {
        assert(!munch_smmu_fault_bits(format << 9));
        for (unsigned bit = 0; bit < 32; bit++) {
            uint32_t fsr = (format << 9) | (1u << bit);
            assert(munch_smmu_fault_bits(fsr) == (fsr & mask));
        }
        for (unsigned faults = 0; faults < 1024; faults++) {
            uint32_t fsr = (format << 9) | ((faults & 255) << 1) | ((faults >> 8) << 30);
            assert(munch_smmu_fault_bits(fsr) == (fsr & mask));
            assert((munch_smmu_fault_bits(fsr) != 0) == (faults != 0));
        }
    }
    puts("Local Linux FSR_FAULT and context offsets match; 4096 fault/format combinations passed");
    return 0;
}
'''
program = program.replace('__ATTRIBUTES__', attributes).replace('__CONTROL__', control)
with tempfile.TemporaryDirectory(prefix='munch-sde-smmu-linux-') as temporary:
    work = Path(temporary)
    test = work / 'check.c'
    test.write_text(program)
    include = Path(__file__).resolve().parents[3] / 'xnu/files/iokit/Kernel'
    binary = work / 'check'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-const-variable', '-fsanitize=address,undefined', '-g',
                    '-I' + str(directory), '-I' + str(include), str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
