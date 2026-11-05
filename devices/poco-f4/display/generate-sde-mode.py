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
    observed_modes = {mode['qcom,mdss-dsi-panel-framerate']: mode
                      for mode in observed['profile']['modes']}
    timing = bytes.fromhex(observed_modes[60]['qcom,mdss-dsi-panel-phy-timings'])
    dts = (tree / 'arch/arm64/boot/dts/vendor/qcom/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi').read_text()
    def mode_commands(mode_name):
        mode = block(dts, mode_name)

        def command(name):
            raw = re.search(rf'qcom,mdss-dsi-{name}-command\s*=\s*\[([^]]+)\]', mode)[1]
            return bytes.fromhex(re.sub(r'/\*.*?\*/', '', raw, flags=re.S))

        return command('on'), command('off'), command('timing-switch')

    def normalize_dcs_commands(commands):
        normalized = bytearray(commands)
        offset = 0
        while offset < len(normalized):
            length = (normalized[offset + 5] << 8) | normalized[offset + 6]
            payload = normalized[offset + 7:offset + 7 + length]
            if normalized[offset] == 0x39 and length == 2:
                if payload == b'\x35\x00':
                    normalized[offset] = 0x05
                elif payload == b'\x53\x20':
                    normalized[offset] = 0x15
            offset += 7 + length
        return bytes(normalized)

    on_raw, off_raw, timing_switch = mode_commands('timing@0')
    on_120_raw, off_120_raw, timing_switch_120 = mode_commands('timing@1')
    on = normalize_dcs_commands(on_raw)
    on_120 = normalize_dcs_commands(on_120_raw)
    off = normalize_dcs_commands(off_raw)
    off_120 = normalize_dcs_commands(off_120_raw)

    def find_pps(commands):
        offset = 0
        while offset < len(commands):
            length = int.from_bytes(commands[offset + 5:offset + 7], 'big')
            payload = commands[offset + 7:offset + 7 + length]
            if len(payload) == 129 and payload[0] == 0x9e:
                return payload[1:]
            offset += 7 + length
        return None

    pps = find_pps(on)
    if pps is None:
        raise ValueError('No complete L11r DSC PPS')

    def parse_switch(commands):
        result = []
        offset = 0
        while offset < len(commands):
            if offset + 7 > len(commands):
                raise ValueError('Truncated L11r timing-switch command header')
            command_type = commands[offset]
            delay = commands[offset + 4]
            length = int.from_bytes(commands[offset + 5:offset + 7], 'big')
            payload = commands[offset + 7:offset + 7 + length]
            if len(payload) != length:
                raise ValueError('Truncated L11r timing-switch command payload')
            if command_type != 0x39 or not payload or len(payload) > 3:
                raise ValueError('Unsupported L11r timing-switch packet')
            result.append((command_type, delay, commands[offset + 1] != 0, payload))
            offset += 7 + length
        return result

    switch_commands = parse_switch(timing_switch)
    switch_commands_120 = parse_switch(timing_switch_120)
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
    expected = observed_modes[60]['qcom,mdss-dsi-on-command']['sha256']
    if hashlib.sha256(on_raw).hexdigest() != expected:
        raise ValueError('Local on-command differs from the captured live L11r profile')
    expected_120 = observed_modes[120]['qcom,mdss-dsi-on-command']['sha256']
    if hashlib.sha256(on_120_raw).hexdigest() != expected_120:
        raise ValueError('Local 120-Hz on-command differs from the captured live L11r profile')
    expected_off = observed_modes[60]['qcom,mdss-dsi-off-command']['sha256']
    if hashlib.sha256(off_raw).hexdigest() != expected_off:
        raise ValueError('Local off-command differs from the captured live L11r profile')
    expected_off_120 = observed_modes[120]['qcom,mdss-dsi-off-command']['sha256']
    if hashlib.sha256(off_120_raw).hexdigest() != expected_off_120:
        raise ValueError('Local 120-Hz off-command differs from the captured live L11r profile')

    def timing_registers(rate):
        mode = observed_modes[rate]
        h_active = mode['qcom,mdss-dsi-panel-width']
        h_sync = mode['qcom,mdss-dsi-h-pulse-width']
        h_back = mode['qcom,mdss-dsi-h-back-porch']
        h_front = mode['qcom,mdss-dsi-h-front-porch']
        v_active = mode['qcom,mdss-dsi-panel-height']
        v_sync = mode['qcom,mdss-dsi-v-pulse-width']
        v_back = mode['qcom,mdss-dsi-v-back-porch']
        v_front = mode['qcom,mdss-dsi-v-front-porch']
        h_period = h_sync + h_back + h_active + h_front
        v_period = v_sync + v_back + v_active + v_front
        dsi_width = 360
        dsi_h_period = h_sync + h_back + dsi_width + h_front
        dsi = [
            (0x024, ((h_sync + h_back + dsi_width) << 16) | (h_sync + h_back)),
            (0x028, ((v_sync + v_back + v_active) << 16) | (v_sync + v_back)),
            (0x02c, ((v_period - 1) << 16) | (dsi_h_period - 1)),
            (0x030, h_sync << 16),
            (0x034, v_sync << 16),
            (0x038, v_sync << 16),
            (0x0bc, 0x3fd08),
            (0x0a4, 0x10100),
            (0x1e4, 1),
        ]
        intf = [
            (0x008, (h_period << 16) | h_sync),
            (0x00c, v_period * h_period),
            (0x014, v_sync * h_period),
            (0x01c, (v_sync + v_back) * h_period),
            (0x024, ((v_period - v_front) * h_period) - 1),
            (0x03c, ((h_period - h_front - 1) << 16) | (h_sync + h_back)),
            (0x0a8, 3),
        ]
        tear_vsync = 19200000 // (v_period * rate)
        return dsi, intf, tear_vsync

    dsi_timing_60, intf_timing_60, tear_vsync_60 = timing_registers(60)
    dsi_timing_120, intf_timing_120, tear_vsync_120 = timing_registers(120)
    result = '#ifndef _IOKIT_MUNCH_SDE_MODE_DATA_H\n#define _IOKIT_MUNCH_SDE_MODE_DATA_H\n\n'
    result += 'struct munch_mode_register { uint32_t offset, value; };\n\n' + registers
    for name, values in (
        ('munch_l11r_dsi_timing_60', dsi_timing_60),
        ('munch_l11r_dsi_timing_120', dsi_timing_120),
        ('munch_l11r_intf_timing_60', intf_timing_60),
        ('munch_l11r_intf_timing_120', intf_timing_120),
    ):
        result += f'\nstatic const struct munch_mode_register {name}[] = {{\n'
        for offset, value in values:
            result += f'\t{{ 0x{offset:03x}, 0x{value:08x} }},\n'
        result += '};\n'
    def emit_bytes(name, data):
        nonlocal result
        result += f'\nstatic const uint8_t {name}[] = {{\n'
        for offset in range(0, len(data), 16):
            result += '\t' + ', '.join(f'0x{value:02x}' for value in data[offset:offset + 16]) + ',\n'
        result += '};\n'

    emit_bytes('munch_l11r_on', on)
    emit_bytes('munch_l11r_on_120hz', on_120)
    emit_bytes('munch_l11r_off', off)
    emit_bytes('munch_l11r_off_120hz', off_120)
    result += '\n'
    result += ('struct munch_mode_command { uint8_t type, length; uint16_t delay_ms; '
               'bool last; const uint8_t *payload; };\n\n')
    def emit_switch(name, commands):
        nonlocal result
        result += f'static const uint8_t {name}_payloads[][3] = {{\n'
        for _, _, _, payload in commands:
            result += '\t{' + ', '.join(f'0x{value:02x}' for value in payload)
            result += ', 0' * (3 - len(payload)) + '},\n'
        result += '};\n\n'
        result += f'static const struct munch_mode_command {name}[] = {{\n'
        for index, (command_type, delay, last, payload) in enumerate(commands):
            result += f'\t{{ 0x{command_type:02x}, {len(payload)}, {delay}, {str(last).lower()}, {name}_payloads[{index}] }},\n'
        result += '};\n\n'

    emit_switch('munch_l11r_60hz', switch_commands)
    emit_switch('munch_l11r_120hz', switch_commands_120)
    result += 'static const struct munch_mode_register munch_l11r_tear_120hz[] = {\n'
    result += f'\t{{ 0x288, 0x000800{tear_vsync_120:02x} }},\n'
    result += '\t{ 0x28c, 0x0000fff0 },\n'
    result += '\t{ 0x294, 0x00000960 },\n'
    result += '\t{ 0x2a4, 0x00000961 },\n'
    result += '\t{ 0x2a8, 0x00000001 },\n'
    result += '\t{ 0x2a0, 0x00000960 },\n'
    result += '\t{ 0x29c, 0x00040004 },\n'
    result += '\t{ 0x290, 0x00000965 },\n'
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
