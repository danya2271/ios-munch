#!/usr/bin/env python3
"""Run the supplied Linux PE register writer against the XNU layout checker."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux-tree', required=True, type=Path)
args = parser.parse_args()
source = (args.linux_tree / 'techpack/display/msm/sde/sde_hw_sspp.c').read_text()
assert re.search(r'opmode\s*\|=\s*MDSS_MDP_OP_PE_OVERRIDE\s*;', source)
start = source.index('static void sde_hw_sspp_setup_pe_config(')
opening = source.index('{', start)
depth = 1
position = opening + 1
while depth:
    depth += (source[position] == '{') - (source[position] == '}')
    position += 1
function = source[start:position]
constants = '\n'.join(line for line in source.splitlines()
                      if line.startswith(('#define SSPP_SW_PIX_EXT_', '#define MDSS_MDP_OP_PE_OVERRIDE')))
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "munch-sde.h"
typedef uint32_t u32;
typedef uint8_t u8;
#define BIT(index) (1u << (index))
#define SDE_MAX_PLANES 4
#define SDE_SSPP_SRC 0
struct sde_hw_blk_reg_map { u32 registers[0x200 / 4]; unsigned writes; };
struct sde_hw_pipe { struct sde_hw_blk_reg_map hw; };
struct sde_hw_pixel_ext {
    u32 right_ftch[4], right_rpt[4], left_ftch[4], left_rpt[4];
    u32 btm_ftch[4], btm_rpt[4], top_ftch[4], top_rpt[4];
    u32 roi_h[4], roi_w[4], num_ext_pxls_top[4], num_ext_pxls_btm[4];
    u32 num_ext_pxls_left[4], num_ext_pxls_right[4];
};
static int _sspp_subblk_offset(struct sde_hw_pipe *context, unsigned kind, u32 *offset)
{
    assert(context && kind == SDE_SSPP_SRC);
    *offset = 0;
    return 0;
}
static void record_register(struct sde_hw_blk_reg_map *map, unsigned offset, u32 value)
{
    assert(offset >= 0x100 && offset <= 0x128 && !(offset & 3));
    map->registers[offset / 4] = value;
    map->writes++;
}
#define SDE_REG_WRITE(map, offset, value) record_register(map, offset, value)
'''
suffix = r'''
static void set_identity(struct sde_hw_pixel_ext *pixel)
{
    memset(pixel, 0, sizeof(*pixel));
    for (unsigned component = 0; component < 4; component++) {
        pixel->num_ext_pxls_left[component] = 1080;
        pixel->num_ext_pxls_top[component] = 2400;
    }
}
static void read_pixel(struct sde_hw_pipe *context, struct munch_sde_pipe_state *pipe)
{
    for (unsigned component = 0; component < 3; component++) {
        unsigned offset = (0x100 + 0x10 * component) / 4;
        pipe->pixel_ext[component].lr = context->hw.registers[offset];
        pipe->pixel_ext[component].tb = context->hw.registers[offset + 1];
        pipe->pixel_ext[component].required = context->hw.registers[offset + 2];
    }
}
int main(void)
{
    _Static_assert(MDSS_MDP_OP_PE_OVERRIDE == MUNCH_SDE_SSPP_PE_OVERRIDE, "PE override bit");
    _Static_assert(SSPP_SW_PIX_EXT_C0_LR == MUNCH_SDE_SSPP_PE_BASE, "PE base");
    _Static_assert(SSPP_SW_PIX_EXT_C3_REQ_PIXELS == MUNCH_SDE_SSPP_PE_BASE + 2 * MUNCH_SDE_SSPP_PE_STRIDE + 8, "PE end");
    struct sde_hw_pipe context = {0};
    struct sde_hw_pixel_ext pixel;
    struct munch_sde_pipe_state pipe = {0};
    pipe.size = pipe.out_size = 0x09600438;
    pipe.stride = 4320;
    pipe.source = 0x9c000000;
    pipe.format = 0x237ff;
    pipe.unpack = 0x03020001;
    pipe.opmode = MDSS_MDP_OP_PE_OVERRIDE;
    set_identity(&pixel);
    sde_hw_sspp_setup_pe_config(&context, &pixel);
    assert(context.hw.writes == 9);
    read_pixel(&context, &pipe);
    assert(!munch_sde_pipe_layout_failures(&pipe, pipe.size, NULL));
    const unsigned colors[] = { 0, 1, 3 };
    for (unsigned component = 0; component < 3; component++) {
        assert(!pipe.pixel_ext[component].lr && !pipe.pixel_ext[component].tb);
        assert(pipe.pixel_ext[component].required == 0x09600438);
        for (unsigned field = 0; field < 3; field++) {
            set_identity(&pixel);
            if (field == 0) pixel.left_ftch[colors[component]] = 1;
            if (field == 1) pixel.btm_rpt[colors[component]] = 1;
            if (field == 2) pixel.num_ext_pxls_right[colors[component]] = 1;
            sde_hw_sspp_setup_pe_config(&context, &pixel);
            read_pixel(&context, &pipe);
            uint32_t failures = munch_sde_pipe_layout_failures(&pipe, pipe.size, NULL);
            uint32_t expected = 1u << (15 + 3 * component + field);
            if (component == 2 && field == 0) expected |= 1u << 22;
            if (component == 2 && field == 1) expected = 0;
            if (failures != expected)
                fprintf(stderr, "component=%u field=%u mask=%08x\n", component, field, failures);
            assert(failures == expected);
        }
    }
    puts("Local Linux PE writer: identity accepted; nonidentity registers rejected; C3 TB mirrors LR in this tree");
    return 0;
}
'''
headers = Path(__file__).resolve().parents[3] / 'xnu/files/iokit/Kernel'
with tempfile.TemporaryDirectory(prefix='munch-sde-linux-') as directory:
    work = Path(directory)
    generated = work / 'check-linux-pe.c'
    generated.write_text(prefix + constants + '\n' + function + '\n' + suffix)
    binary = work / 'check-linux-pe'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I' + str(headers),
                    str(generated), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
