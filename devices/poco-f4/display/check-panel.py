#!/usr/bin/env python3
"""Exercise L11r packet decoding and DT comparison using the local Linux source."""
import argparse
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location('inspect_panel', Path(__file__).with_name('inspect-panel.py'))
panel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(panel)
packer_spec = importlib.util.spec_from_file_location('pack_iosroot',
                                                   Path(__file__).resolve().parents[1] / 'loader/pack-iosroot.py')
packer = importlib.util.module_from_spec(packer_spec)
packer_spec.loader.exec_module(packer)
BACKEND_KERNEL = b'MUNCH SDE: hardware presenter v1\0unsafe loader handoff refused'
NATIVE_KERNEL = BACKEND_KERNEL + b'\0MUNCH SDE NATIVE r15\0loader return refused before CPU shutdown'


class PacketTests(unittest.TestCase):
    def test_valid(self):
        packets = panel.decode_commands(bytes.fromhex('05 01 00 00 78 00 02 11 00'))
        self.assertEqual(packets[0]['delay_ms'], 120)
        self.assertEqual(packets[0]['payload'], '1100')

    def test_invalid(self):
        for raw in ('', '05', '05 01 00 00 00 00 02 11',
                    '05 01 00 00 00 00 00', 'ff 01 00 00 00 00 02 11 00',
                    '05 02 00 00 00 00 02 11 00', '05 01 04 00 00 00 02 11 00',
                    '05 01 00 02 00 00 02 11 00', '05 01 00 00 00 00 02 11 00 00'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                panel.decode_commands(bytes.fromhex(raw))


class ProfileTests(unittest.TestCase):
    linux_tree = None

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='munch-panel-test-')
        self.addCleanup(self.directory.cleanup)
        self.dtb = Path(self.directory.name) / 'fixture.dtb'
        stub = ('/dts-v1/; / { model = "Qualcomm Technologies, Inc. xiaomi munch"; '
                'chosen { bootargs = "msm_drm.dsi_display0=' + panel.PANEL + ':"; }; '
                'soc { tlmm: gpio {}; mdss_mdp: qcom,mdss_mdp@ae00000 {}; }; };\n')
        suffix = '\n&dsi_l11r_38_08_0a_dsc_cmd { qcom,platform-te-gpio = <&tlmm 66 0>; };'
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(self.dtb), '-'],
                       input=stub.encode() + (self.linux_tree / panel.SOURCE).read_bytes() + suffix.encode(),
                       check=True)

    def put(self, kind, path, prop, *values):
        subprocess.run(['fdtput', '-t', kind, str(self.dtb), path, prop, *map(str, values)], check=True)

    def test_matching(self):
        result = panel.compare(self.dtb, self.linux_tree)
        self.assertTrue(result['matches_linux'])
        self.assertFalse(result['active_bootloader_mode_verified'])
        self.assertFalse(result['scanout_takeover_allowed'])
        self.assertEqual(result['profile']['controller'], [0])

    def test_wrong_selection(self):
        self.put('s', '/chosen', 'bootargs', 'msm_drm.dsi_display0=dsi_sim_vid:')
        with self.assertRaises(ValueError):
            panel.compare(self.dtb, self.linux_tree)

    def test_ambiguous_selection(self):
        self.put('s', '/chosen', 'bootargs', ('msm_drm.dsi_display0=' + panel.PANEL + ': ') * 2)
        with self.assertRaises(ValueError):
            panel.compare(self.dtb, self.linux_tree)

    def test_phy_difference(self):
        path = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings/timing@0'
        self.put('bx', path, 'qcom,mdss-dsi-panel-phy-timings', 'ff')
        result = panel.compare(self.dtb, self.linux_tree)
        self.assertFalse(result['matches_linux'])
        self.assertIn('60Hz/qcom,mdss-dsi-panel-phy-timings', result['differences'])

    def test_pps_geometry_mismatch(self):
        path = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings/timing@0'
        self.put('i', path, 'qcom,mdss-dsc-slice-height', 12)
        with self.assertRaisesRegex(ValueError, 'PPS disagrees'):
            panel.compare(self.dtb, self.linux_tree)

    def test_te_mismatch(self):
        self.put('i', panel.MDP + '/' + panel.PANEL, 'qcom,platform-te-gpio', 1, 67, 0)
        result = panel.compare(self.dtb, self.linux_tree)
        self.assertIn('live-te-gpio', result['differences'])

    def backend_profile(self):
        timings = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings'
        for child in panel.Fdt(self.dtb).children(timings):
            self.put('i', timings + '/' + child, 'qcom,display-topology', 1, 1, 1)

    def test_backend_profile(self):
        self.backend_profile()
        self.assertEqual(packer.validate_sde_backend(self.dtb, BACKEND_KERNEL), panel.PANEL)

    def native_profile(self):
        self.backend_profile()
        path = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings/timing@0'
        observed = json.loads(Path(__file__).with_name('l11r-observed.json').read_text())
        timings = observed['profile']['modes'][0]['qcom,mdss-dsi-panel-phy-timings']
        self.put('bx', path, 'qcom,mdss-dsi-panel-phy-timings',
                 *(f'{value:x}' for value in bytes.fromhex(timings)))
        self.put('i', path, 'qcom,default-topology-index', 0)
        return path

    def test_native_profile(self):
        self.native_profile()
        self.assertEqual(packer.validate_sde_backend(self.dtb, NATIVE_KERNEL, True), panel.PANEL)

    def test_native_stale_kernel(self):
        self.native_profile()
        with self.assertRaisesRegex(ValueError, 'native modesetting'):
            packer.validate_sde_backend(self.dtb, BACKEND_KERNEL, True)

    def test_native_wrong_rate(self):
        path = self.native_profile()
        self.put('i', path, 'qcom,mdss-dsi-panel-clockrate', 1000000000)
        with self.assertRaisesRegex(ValueError, 'exact captured'):
            packer.validate_sde_backend(self.dtb, NATIVE_KERNEL, True)

    def test_native_wrong_phy(self):
        self.backend_profile()
        with self.assertRaisesRegex(ValueError, 'exact captured'):
            packer.validate_sde_backend(self.dtb, NATIVE_KERNEL, True)

    def test_backend_stale_kernel(self):
        for kernel in (b'', b'MUNCH SDE: hardware presenter v1', b'unsafe loader handoff refused'):
            with self.subTest(kernel=kernel), self.assertRaisesRegex(ValueError, 'lacks'):
                packer.validate_sde_backend(self.dtb, kernel)

    def test_backend_wrong_selection(self):
        self.put('s', '/chosen', 'bootargs', 'msm_drm.dsi_display0=dsi_sim_vid:')
        with self.assertRaisesRegex(ValueError, 'selection'):
            packer.validate_sde_backend(self.dtb, BACKEND_KERNEL)

    def test_backend_te_gpio(self):
        self.backend_profile()
        self.put('i', panel.MDP + '/' + panel.PANEL, 'qcom,platform-te-gpio', 1, 67, 0)
        with self.assertRaisesRegex(ValueError, 'link'):
            packer.validate_sde_backend(self.dtb, BACKEND_KERNEL)

    def test_backend_topology(self):
        self.backend_profile()
        timing = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings/timing@0'
        self.put('i', timing, 'qcom,display-topology', 2, 2, 2)
        with self.assertRaisesRegex(ValueError, 'topology'):
            packer.validate_sde_backend(self.dtb, BACKEND_KERNEL)

    def test_backend_phy_inherited(self):
        self.backend_profile()
        timing = panel.MDP + '/' + panel.PANEL + '/qcom,mdss-dsi-display-timings/timing@0'
        self.put('bx', timing, 'qcom,mdss-dsi-panel-phy-timings', 'ff')
        self.assertEqual(packer.validate_sde_backend(self.dtb, BACKEND_KERNEL), panel.PANEL)

    def test_backend_arguments(self):
        arguments = [None] * 5 + [8, True, True, False, False, False, '']
        for extra in ('munch_sde=1', 'munch_sde_probe=1', 'other=1 munch_sde=0'):
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, '--sde-backend'):
                packer.pack(*arguments[:-1], extra)
        arguments[7] = False
        with self.assertRaisesRegex(ValueError, 'GPU profile'):
            packer.pack(*arguments, sde_backend=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-tree', type=Path, required=True)
    args = parser.parse_args()
    ProfileTests.linux_tree = args.linux_tree
    unittest.main(argv=[__file__])
