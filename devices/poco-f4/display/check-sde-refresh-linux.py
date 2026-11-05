#!/usr/bin/env python3
"""Compile the supplied Linux autorefresh/TE/debug-bus accessors against mock MMIO."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


def extract(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    position = opening + 1
    while depth:
        depth += (source[position] == '{') - (source[position] == '}')
        position += 1
    return source[start:position]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux-tree', required=True, type=Path)
args = parser.parse_args()
tree = args.linux_tree
directory = tree / 'techpack/display/msm/sde'
intf = (directory / 'sde_hw_intf.c').read_text()
top = (directory / 'sde_hw_top.c').read_text()
encoder = (directory / 'sde_encoder_phys_cmd.c').read_text()
mdss = (directory / 'sde_hw_mdss.h').read_text()
assert re.search(r'qcom,sde-off\s*=\s*<0x1000>',
                 (tree / 'arch/arm64/boot/dts/vendor/qcom/kona-sde.dtsi').read_text())
assert 'INTF_0 = 1,\n\tINTF_1,\n\tINTF_2,' in mdss
prepare = extract(encoder, 'static void sde_encoder_phys_cmd_prepare_commit(')
steps = ('sde_encoder_phys_cmd_connect_te(phys_enc, false)',
         '_sde_encoder_autorefresh_disable_seq1(phys_enc)',
         '_sde_encoder_autorefresh_disable_seq2(phys_enc)',
         'sde_encoder_phys_cmd_connect_te(phys_enc, true)')
assert [prepare.index(step) for step in steps] == sorted(prepare.index(step) for step in steps)
constants = '\n'.join(line for line in (intf + '\n' + top).splitlines() if line.startswith((
    '#define INTF_TEAR_SYNC_CONFIG_VSYNC', '#define INTF_TEAR_AUTOREFRESH_CONFIG',
    '#define INTF_TEAR_LINE_COUNT', '#define MDP_PERIPH_DBGBUS_CTRL',
    '#define MDP_DSPP_DBGBUS_CTRL', '#define MDP_DSPP_DBGBUS_STATUS',
    '#define AUTOREFRESH_TEST_POINT', '#define TEST_MASK')))
functions = '\n'.join(extract(intf, signature) for signature in (
    'static int sde_hw_intf_setup_autorefresh_config(',
    'static int sde_hw_intf_get_autorefresh_config(',
    'static int sde_hw_intf_connect_external_te(',
    'static int sde_hw_intf_get_vsync_info('))
functions += '\n' + extract(top, 'static u32 sde_hw_get_autorefresh_status(')
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "munch-sde.h"
typedef uint32_t u32;
#define BIT(index) (1u << (index))
#define INTF_1 2
#define INTF_2 3
static u32 registers[MUNCH_MDP_SIZE / 4 + 1];
struct sde_hw_blk_reg_map { u32 base; };
struct sde_hw_intf { struct sde_hw_blk_reg_map hw; };
struct sde_hw_mdp { struct sde_hw_blk_reg_map hw; };
struct sde_hw_autorefresh { bool enable; u32 frame_count; };
struct sde_hw_pp_vsync_info { u32 wr_ptr_line_count; };
#define SDE_REG_READ(map, offset) registers[((map)->base + (offset)) / 4]
#define SDE_REG_WRITE(map, offset, value) (registers[((map)->base + (offset)) / 4] = (value))
static void wmb(void) { }
'''
suffix = r'''
int main(void)
{
    _Static_assert(INTF_TEAR_AUTOREFRESH_CONFIG == MUNCH_SDE_INTF_TEAR_AUTOREFRESH, "autorefresh offset");
    _Static_assert(INTF_TEAR_SYNC_CONFIG_VSYNC == MUNCH_SDE_INTF_TEAR_SYNC_VSYNC, "TE offset");
    _Static_assert(INTF_TEAR_LINE_COUNT == MUNCH_SDE_INTF_TEAR_LINE, "write pointer offset");
    _Static_assert(0x1000 + MDP_PERIPH_DBGBUS_CTRL == MUNCH_SDE_DEBUG_PERIPH, "MDP peripheral base");
    _Static_assert(0x1000 + MDP_DSPP_DBGBUS_CTRL == MUNCH_SDE_DEBUG_CTRL, "MDP debug base");
    _Static_assert(0x1000 + MDP_DSPP_DBGBUS_STATUS == MUNCH_SDE_DEBUG_STATUS, "MDP status base");
    _Static_assert(TEST_MASK(64, AUTOREFRESH_TEST_POINT) == MUNCH_SDE_INTF1_AUTOREFRESH_TEST, "DSI0 test point");
    struct sde_hw_intf intf = { { MUNCH_SDE_INTF1_OFFSET } };
    struct sde_hw_mdp mdp = { { 0x1000 } };
    struct sde_hw_autorefresh config = { true, 1 }, decoded = {0};
    struct sde_hw_pp_vsync_info info = {0};
    unsigned auto_index = (MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_AUTOREFRESH) / 4;
    unsigned vsync_index = (MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_SYNC_VSYNC) / 4;
    assert(!sde_hw_intf_setup_autorefresh_config(&intf, &config));
    assert(registers[auto_index] == (MUNCH_SDE_AUTOREFRESH_ENABLE | 1));
    assert(!sde_hw_intf_get_autorefresh_config(&intf, &decoded) && decoded.enable && decoded.frame_count == 1);
    registers[vsync_index] = 0x00181234;
    assert(sde_hw_intf_connect_external_te(&intf, false) == 1);
    assert(registers[vsync_index] == (0x00181234 & ~MUNCH_SDE_EXTERNAL_TE));
    config.enable = false;
    assert(!sde_hw_intf_setup_autorefresh_config(&intf, &config) && registers[auto_index] == 1);
    assert(!sde_hw_intf_get_autorefresh_config(&intf, &decoded) && !decoded.enable && decoded.frame_count == 1);
    registers[(MUNCH_SDE_INTF1_OFFSET + MUNCH_SDE_INTF_TEAR_LINE) / 4] = 0x12340960;
    assert(!sde_hw_intf_get_vsync_info(&intf, &info) && info.wr_ptr_line_count == 2400);
    registers[MUNCH_SDE_DEBUG_STATUS / 4] = 0x80;
    assert(sde_hw_get_autorefresh_status(&mdp, INTF_1) == MUNCH_SDE_AUTOREFRESH_BUSY);
    assert(registers[MUNCH_SDE_DEBUG_CTRL / 4] == 0x7001 && !registers[MUNCH_SDE_DEBUG_PERIPH / 4]);
    assert(sde_hw_intf_connect_external_te(&intf, true) == 0 && registers[vsync_index] == 0x00181234);
    config.enable = true;
    assert(!sde_hw_intf_setup_autorefresh_config(&intf, &config) && registers[auto_index] == 0x80000001);
    puts("Local Linux autorefresh enable/count, TE gating, write-pointer and Kona debug-bus offsets match");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='munch-sde-refresh-linux-') as temporary:
    work = Path(temporary)
    source = work / 'check.c'
    source.write_text(prefix + '\n' + constants + '\n' +
                      '#pragma GCC diagnostic push\n' +
                      '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n' + functions +
                      '\n#pragma GCC diagnostic pop\n' + suffix)
    include = Path(__file__).resolve().parents[3] / 'xnu/files/iokit/Kernel'
    binary = work / 'check'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-const-variable', '-fsanitize=address,undefined', '-g',
                    '-I' + str(include), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
