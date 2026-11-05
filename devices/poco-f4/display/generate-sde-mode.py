#!/usr/bin/env python3
"""Reproduce fixed L11r register programs with the local Linux implementations."""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path


def block(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def defines(source):
    result = []
    lines = iter(source.splitlines())
    for line in lines:
        if line.startswith('#define'):
            while line.endswith('\\'):
                line = line[:-1] + next(lines)
            result.append(line)
    return '\n'.join(result)


def generate(tree):
    here = Path(__file__).resolve().parent
    pll = (tree / 'techpack/display/pll/dsi_pll_7nm.c').read_text()
    phy = (tree / 'techpack/display/msm/dsi/dsi_phy_hw_v4_0.c').read_text()
    panel = (tree / 'techpack/display/msm/dsi/dsi_panel.c').read_text()
    dsc = (tree / 'techpack/display/msm/sde/sde_hw_dsc.c').read_text()
    encoder = (tree / 'techpack/display/msm/sde/sde_encoder.c').read_text()
    drv = (tree / 'techpack/display/msm/msm_drv.h').read_text()
    controller = (tree / 'techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c').read_text()
    intf = (tree / 'techpack/display/msm/sde/sde_hw_intf.c').read_text()
    mdss = (tree / 'techpack/display/msm/sde/sde_hw_mdss.h').read_text()
    functions = [defines(pll), defines(phy), defines(dsc), defines(intf),
                 defines((tree / 'techpack/display/msm/dsi/dsi_ctrl_reg.h').read_text())]
    functions += [block(pll, 'struct ' + name + ' {') + ';'
                  for name in ('dsi_pll_regs', 'dsi_pll_config', 'dsi_pll_7nm')]
    functions += [block(pll, signature) for signature in (
        'static inline bool dsi_pll_7nm_is_hw_revision_v1(',
        'static inline bool dsi_pll_7nm_is_hw_revision_v4_1(',
        'static void dsi_pll_setup_config(', 'static void dsi_pll_calc_dec_frac(',
        'static void dsi_pll_calc_ssc(', 'static void dsi_pll_ssc_commit(',
        'static void dsi_pll_config_hzindep_reg(', 'static void dsi_pll_init_val(',
        'static void dsi_pll_commit(')]
    functions += [block(phy, signature) for signature in (
        'static void dsi_phy_hw_v4_0_config_lpcdrx(',
        'static void dsi_phy_hw_v4_0_lane_swap_config(',
        'static void dsi_phy_hw_v4_0_lane_settings(',
        'void dsi_phy_hw_v4_0_commit_phy_timing(',
        'static inline int dsi_phy_hw_calc_cmn_lane_ctrl0(',
        'static void dsi_phy_hw_dphy_enable(')]
    functions += [block(drv, 'struct msm_display_dsc_info {') + ';']
    functions += [panel[panel.index('enum dsi_dsc_ratio_type {'):
                        panel.index('int dsi_dsc_create_pps_buf_cmd(')]]
    functions += [block(panel, signature) for signature in (
        'int dsi_dsc_populate_static_param(', 'void dsi_dsc_pclk_param_calc(',
        'int dsi_dsc_create_pps_buf_cmd(')]
    functions += [block(encoder, 'static int _sde_encoder_dsc_initial_line_calc(')]
    functions += [block(dsc, signature) for signature in (
        'static void sde_hw_dsc_config(', 'static void sde_hw_dsc_config_thresh(')]
    functions += [block(controller, 'static const u8 cmd_mode_format_map[') + ';']
    functions += [block(controller, signature) for signature in (
        'void dsi_ctrl_hw_cmn_cmd_engine_setup(', 'void dsi_ctrl_hw_cmn_setup_cmd_stream(')]
    functions += [block(mdss, 'struct sde_hw_tear_check {') + ';',
                  block(intf, 'static int sde_hw_intf_setup_te_config(')]
    observed = json.loads((here / 'l11r-observed.json').read_text())
    timing = bytes.fromhex(observed['profile']['modes'][0]['qcom,mdss-dsi-panel-phy-timings'])
    dts = (tree / 'arch/arm64/boot/dts/vendor/qcom/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi').read_text()
    mode = block(dts, 'timing@0')
    on = re.search(r'qcom,mdss-dsi-on-command\s*=\s*\[([^]]+)\]', mode)[1]
    on = bytes.fromhex(re.sub(r'/\*.*?\*/', '', on, flags=re.S))
    timing_switch = re.search(r'qcom,mdss-dsi-timing-switch-command\s*=\s*\[([^]]+)\]', mode)[1]
    timing_switch = bytes.fromhex(re.sub(r'/\*.*?\*/', '', timing_switch, flags=re.S))
    offset = 0
    pps = None
    while offset < len(on):
        length = int.from_bytes(on[offset + 5:offset + 7], 'big')
        payload = on[offset + 7:offset + 7 + length]
        if len(payload) == 129 and payload[0] == 0x9e:
            pps = payload[1:]
        offset += 7 + length
    if pps is None:
        raise ValueError('No complete L11r DSC PPS')
    switch_commands = []
    offset = 0
    while offset < len(timing_switch):
        if offset + 7 > len(timing_switch):
            raise ValueError('Truncated L11r timing-switch command header')
        command_type = timing_switch[offset]
        delay = timing_switch[offset + 4]
        length = int.from_bytes(timing_switch[offset + 5:offset + 7], 'big')
        payload = timing_switch[offset + 7:offset + 7 + length]
        if len(payload) != length:
            raise ValueError('Truncated L11r timing-switch command payload')
        if command_type != 0x39 or not payload or len(payload) > 3:
            raise ValueError('Unsupported L11r timing-switch packet')
        switch_commands.append((command_type, delay, payload))
        offset += 7 + length
    template = (here / 'sde-mode-reference.c').read_text()
    template = template.replace('MODE_LINUX_FUNCTIONS', '\n'.join(functions))
    template = template.replace('MODE_PHY_TIMINGS', ', '.join(str(value) for value in timing))
    template = template.replace('MODE_PANEL_PPS', ', '.join(str(value) for value in pps))
    with tempfile.TemporaryDirectory(prefix='munch-native-reference-') as directory:
        source = Path(directory) / 'reference.c'
        binary = Path(directory) / 'reference'
        source.write_text(template)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                        '-fsanitize=undefined', str(source), '-o', str(binary)], check=True)
        registers = subprocess.check_output([str(binary)], text=True)
    import hashlib
    expected = observed['profile']['modes'][0]['qcom,mdss-dsi-on-command']['sha256']
    if hashlib.sha256(on).hexdigest() != expected:
        raise ValueError('Local on-command differs from the captured live L11r profile')
    result = '#ifndef _IOKIT_MUNCH_SDE_MODE_DATA_H\n#define _IOKIT_MUNCH_SDE_MODE_DATA_H\n\n'
    result += 'struct munch_mode_register { uint32_t offset, value; };\n\n' + registers
    result += '\nstatic const uint8_t munch_l11r_on[] = {\n'
    for offset in range(0, len(on), 16):
        result += '\t' + ', '.join(f'0x{value:02x}' for value in on[offset:offset + 16]) + ',\n'
    result += '};\n\n'
    result += 'struct munch_mode_command { uint8_t type, length; uint16_t delay_ms; const uint8_t *payload; };\n\n'
    result += 'static const uint8_t munch_l11r_60hz_payloads[][3] = {\n'
    for _, _, payload in switch_commands:
        result += '\t{' + ', '.join(f'0x{value:02x}' for value in payload)
        result += ', 0' * (3 - len(payload)) + '},\n'
    result += '};\n\n'
    result += 'static const struct munch_mode_command munch_l11r_60hz[] = {\n'
    for index, (command_type, delay, payload) in enumerate(switch_commands):
        result += f'\t{{ 0x{command_type:02x}, {len(payload)}, {delay}, munch_l11r_60hz_payloads[{index}] }},\n'
    result += '};\n\n#endif\n'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-tree', type=Path, required=True)
    parser.add_argument('--check', type=Path)
    args = parser.parse_args()
    generated = generate(args.linux_tree)
    if args.check:
        if args.check.read_text() != generated:
            raise SystemExit('Native mode register programs differ from local Linux')
        print('Native PLL/PHY/DSC and live-panel command reference: PASS')
    else:
        print(generated, end='')
