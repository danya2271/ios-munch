#!/usr/bin/env python3
"""Compare the selected live L11r panel with the local Linux DTS, without MMIO."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path


PANEL = 'qcom,mdss_dsi_l11r_38_08_0a_dsc_cmd'
MDP = '/soc/qcom,mdss_mdp@ae00000'
SOURCE = 'arch/arm64/boot/dts/vendor/qcom/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi'
NUMBERS = (
    'qcom,mdss-dsi-panel-framerate', 'qcom,mdss-dsi-panel-width',
    'qcom,mdss-dsi-panel-height', 'qcom,mdss-dsi-panel-clockrate',
    'qcom,mdss-dsi-h-front-porch', 'qcom,mdss-dsi-h-back-porch',
    'qcom,mdss-dsi-h-pulse-width', 'qcom,mdss-dsi-v-front-porch',
    'qcom,mdss-dsi-v-back-porch', 'qcom,mdss-dsi-v-pulse-width',
    'qcom,mdss-mdp-transfer-time-us', 'qcom,mdss-dsc-slice-width',
    'qcom,mdss-dsc-slice-height', 'qcom,mdss-dsc-slice-per-pkt',
    'qcom,mdss-dsc-bit-per-component', 'qcom,mdss-dsc-bit-per-pixel',
)
COMMANDS = ('qcom,mdss-dsi-on-command', 'qcom,mdss-dsi-off-command',
            'qcom,mdss-dsi-timing-switch-command')


class Fdt:
    def __init__(self, path):
        self.path = path

    def call(self, *args):
        return subprocess.check_output(['fdtget', *args[:1], str(self.path), *args[1:]],
                                       text=True).strip()

    def children(self, path):
        return self.call('-l', path).splitlines()

    def properties(self, path):
        return set(self.call('-p', path).splitlines())

    def raw(self, path, prop):
        output = subprocess.check_output(['fdtget', '-t', 'bx', str(self.path), path, prop], text=True)
        return bytes(int(value, 16) for value in output.split())

    def string(self, path, prop):
        raw = self.raw(path, prop)
        if not raw.endswith(b'\0') or b'\0' in raw[:-1]:
            raise ValueError(f'Invalid DT string: {path}/{prop}')
        return raw[:-1].decode('ascii')

    def cells(self, path, prop):
        raw = self.raw(path, prop)
        if not raw or len(raw) % 4:
            raise ValueError(f'Invalid DT cells: {path}/{prop}')
        return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))

    def scalar(self, path, prop):
        values = self.cells(path, prop)
        if len(values) != 1:
            raise ValueError(f'Expected one DT cell: {path}/{prop}')
        return values[0]


def decode_commands(raw):
    offset = 0
    packets = []
    while offset < len(raw):
        if len(raw) - offset < 7:
            raise ValueError('Truncated DSI command header')
        dtype, last, channel, ack, delay, high, low = raw[offset:offset + 7]
        length = (high << 8) | low
        offset += 7
        if (dtype not in (0x05, 0x15, 0x39) or last not in (0, 1) or
                channel > 3 or ack not in (0, 1) or not length or length > len(raw) - offset):
            raise ValueError('Unsupported or truncated DSI command packet')
        payload = raw[offset:offset + length]
        packets.append({'type': dtype, 'last': last, 'channel': channel, 'ack': ack,
                        'delay_ms': delay, 'payload': payload.hex()})
        offset += length
    if not packets:
        raise ValueError('Empty DSI command sequence')
    return packets


def profile(tree):
    panel = MDP + '/' + PANEL
    properties = tree.properties(panel)
    result = {
        'panel': PANEL,
        'type': tree.string(panel, 'qcom,mdss-dsi-panel-type'),
        'controller': tree.cells(panel, 'qcom,dsi-ctrl-num'),
        'phy': tree.cells(panel, 'qcom,dsi-phy-num'),
        'bpp': tree.scalar(panel, 'qcom,mdss-dsi-bpp'),
        'lanes': [lane for lane in range(4) if f'qcom,mdss-dsi-lane-{lane}-state' in properties],
        'te_pin': tree.scalar(panel, 'qcom,mdss-dsi-te-pin-select'),
        'te_check': 'qcom,mdss-dsi-te-check-enable' in properties,
        'te_external': 'qcom,mdss-dsi-te-using-te-pin' in properties,
        'modes': [],
    }
    timing_path = panel + '/qcom,mdss-dsi-display-timings'
    for child in tree.children(timing_path):
        path = timing_path + '/' + child
        mode_properties = tree.properties(path)
        mode = {prop: tree.scalar(path, prop) for prop in NUMBERS}
        mode['qcom,compression-mode'] = tree.string(path, 'qcom,compression-mode')
        mode['qcom,mdss-dsc-block-prediction-enable'] = 'qcom,mdss-dsc-block-prediction-enable' in mode_properties
        for prop in ('qcom,display-topology', 'qcom,default-topology-index'):
            mode[prop] = tree.cells(path, prop) if prop in mode_properties else None
        phy = 'qcom,mdss-dsi-panel-phy-timings'
        mode[phy] = tree.raw(path, phy).hex() if phy in mode_properties else None
        for prop in COMMANDS:
            raw = tree.raw(path, prop)
            packets = decode_commands(raw)
            mode[prop] = {'sha256': hashlib.sha256(raw).hexdigest(), 'packets': len(packets),
                          'state': tree.string(path, prop + '-state')}
            if prop == COMMANDS[0]:
                pps = [packet['payload'] for packet in packets if packet['payload'].startswith('9e')]
                if len(pps) != 1 or len(bytes.fromhex(pps[0])) != 129:
                    raise ValueError('Expected one 128-byte L11r DSC PPS payload')
                mode['pps'] = pps[0][2:]
                payload = bytes.fromhex(mode['pps'])
                geometry = struct.unpack('>4H', payload[6:14])
                expected = (mode['qcom,mdss-dsi-panel-height'], mode['qcom,mdss-dsi-panel-width'],
                            mode['qcom,mdss-dsc-slice-height'], mode['qcom,mdss-dsc-slice-width'])
                if (geometry != expected or payload[3] >> 4 != mode['qcom,mdss-dsc-bit-per-component'] or
                        ((payload[4] & 3) << 8 | payload[5]) != 16 * mode['qcom,mdss-dsc-bit-per-pixel']):
                    raise ValueError('DSC PPS disagrees with mode geometry or bit depth')
        result['modes'].append(mode)
    result['modes'].sort(key=lambda mode: mode[NUMBERS[0]])
    return result


def compare(dtb, linux_tree):
    live = Fdt(dtb)
    if live.string('/', 'model') != 'Qualcomm Technologies, Inc. xiaomi munch':
        raise ValueError('Expected a live Munch DTB')
    selected = re.findall(r'(?:^|\s)msm_drm\.dsi_display0=([^:\s]+)', live.string('/chosen', 'bootargs'))
    if selected != [PANEL]:
        raise ValueError('Live DT does not uniquely select the L11r primary panel')
    source = linux_tree / SOURCE
    source_bytes = source.read_bytes()
    with tempfile.TemporaryDirectory(prefix='munch-l11r-') as directory:
        reference = Path(directory) / 'panel.dtb'
        stub = '/dts-v1/; / { soc { tlmm: gpio {}; mdss_mdp: qcom,mdss_mdp@ae00000 {}; }; };\n'
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(reference), '-'],
                       input=stub.encode() + source_bytes, check=True)
        expected = profile(Fdt(reference))
    observed = profile(live)
    differences = []
    for key in observed:
        if key == 'modes':
            expected_modes = {mode[NUMBERS[0]]: mode for mode in expected['modes']}
            observed_rates = {mode[NUMBERS[0]] for mode in observed['modes']}
            if len(observed_rates) != len(observed['modes']) or observed_rates != set(expected_modes):
                differences.append('mode-list')
            for mode in observed['modes']:
                rate = mode[NUMBERS[0]]
                for prop, value in mode.items():
                    if value != expected_modes.get(rate, {}).get(prop):
                        differences.append(f'{rate}Hz/{prop}')
        elif observed[key] != expected[key]:
            differences.append(key)
    te = live.cells(MDP + '/' + PANEL, 'qcom,platform-te-gpio')
    if len(te) != 3 or te[1:] != [66, 0]:
        differences.append('live-te-gpio')
    return {'schema_version': 1, 'selected_panel': selected[0], 'profile': observed,
            'te_gpio': te[1:], 'linux_source': SOURCE,
            'linux_source_sha256': hashlib.sha256(source_bytes).hexdigest(),
            'live_dtb_sha256': hashlib.sha256(dtb.read_bytes()).hexdigest(),
            'matches_linux': not differences, 'differences': differences,
            'active_bootloader_mode_verified': False, 'scanout_takeover_allowed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--linux-tree', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = compare(args.dtb, args.linux_tree)
    rendered = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end='')
    return 0 if result['matches_linux'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
