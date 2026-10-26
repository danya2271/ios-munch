#!/usr/bin/env python3
"""Build a UEFI-only F4 RAM boot probe; never invokes adb or fastboot."""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def pack(fd_path, dtb_path, output, usb_probe=False, root_probe=False, touch_probe=False, touch_reset=False, touch_power=False, touch_enable=False, gpu_probe=False, panel_te_probe=False, display_scan_probe=False):
    if touch_enable:
        touch_probe = touch_reset = True
    if touch_reset and not touch_probe:
        raise ValueError('Reset release requires the touch diagnostic')
    if sum((usb_probe, root_probe, touch_probe, touch_power, gpu_probe, panel_te_probe, display_scan_probe)) > 1:
        raise ValueError('Select one probe mode')
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    if usb_probe:
        subprocess.run([sys.executable, str(here / 'check-usb-descriptors.py')], check=True)
    fd = fd_path.read_bytes()
    metadata = json.loads(fd_path.with_suffix(fd_path.suffix + '.munch.json').read_text())
    if metadata.get('target') != 'munch' or metadata.get('fd_sha256') != hashlib.sha256(fd).hexdigest():
        raise ValueError('FD does not match the F4 build manifest')
    if len(fd) != 0x300000:
        raise ValueError('Expected a 3 MiB F4 FD')
    model = subprocess.check_output(['fdtget', str(dtb_path), '/', 'model'], text=True).strip()
    if model != 'Qualcomm Technologies, Inc. xiaomi munch':
        raise ValueError('Expected the captured live munch DTB')
    with tempfile.TemporaryDirectory(prefix='munch-diagnostic-') as temp:
        work = Path(temp)
        obj, efi = work / 'diagnostic.obj', work / 'diagnostic.efi'
        subprocess.run(['clang', *(['-DMUNCH_DISPLAY_SCAN_PROBE=1'] if display_scan_probe else []), *(['-DMUNCH_PANEL_TE_PROBE=1'] if panel_te_probe else []), *(['-DMUNCH_GPU_PROBE=1'] if gpu_probe else []), *(['-DMUNCH_USB_PROBE=1'] if usb_probe else []), *(['-DMUNCH_ROOT_PROBE=1'] if root_probe else []), *(['-DMUNCH_TOUCH_PROBE=1'] if touch_probe else []), *(['-DMUNCH_TOUCH_RELEASE_RESET=1'] if touch_reset else []), *(['-DMUNCH_TOUCH_POWER_PROBE=1'] if touch_power or touch_enable else []), *(['-DMUNCH_TOUCH_POWER_ENABLE=1'] if touch_enable else []), '--target=aarch64-unknown-windows', '-ffreestanding',
                        '-fno-builtin', '-fno-stack-protector', '-fshort-wchar',
                        '-mgeneral-regs-only', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'xnuloader'), '-c', str(here / 'diagnostic.c'),
                        '-o', str(obj)], check=True)
        subprocess.run(['lld-link', '/subsystem:efi_application', '/entry:efi_main',
                        '/nodefaultlib', '/base:0xA1000000', '/out:' + str(efi), str(obj)], check=True)
        probe = efi.read_bytes()
        body = struct.pack('<16sII', b'loader.efi', len(probe), 0)
        body += probe + b'\0' * (-len(probe) % 16) + b'\0' * 24
        payload = struct.pack('<II', 0x50554e58, len(body)) + body
        shim_obj, shim_raw = work / 'shim.o', work / 'shim.bin'
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c', str(here / 'bootshim.S'),
                        '-DFD_BASE=0x9fc00000', '-DFD_SIZE=0x300000',
                        '-DPAYLOAD_BASE=0xf0000000', '-o', str(shim_obj)], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(shim_obj), str(shim_raw)], check=True)
        blob = shim_raw.read_bytes() + fd + payload
        blob += b'\0' * (-len(blob) % 16)
        kernel = work / 'kernel'
        kernel.write_bytes(gzip.compress(blob, mtime=0) + dtb_path.read_bytes())
        # An empty newc archive supplies a valid ramdisk; no Android or iOS init.
        name = b'TRAILER!!!\0'
        cpio = b'070701' + b''.join(f'{v:08x}'.encode() for v in
                                  [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]) + name
        cpio += b'\0' * (-len(cpio) % 4)
        ramdisk = work / 'ramdisk'
        ramdisk.write_bytes(gzip.compress(cpio, mtime=0))
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(root / 'devices/poco-x5-pro/prebuilt/mkbootimg.py'),
                        '--kernel', str(kernel), '--ramdisk', str(ramdisk),
                        '--header_version', '1', '--board', 'MUNCHDIAG', '-o', str(output)], check=True)
        report = {**metadata, 'image_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                  'image_size': output.stat().st_size, 'shim_size': len(shim_raw.read_bytes()),
                  'bootshim_revision': 2, 'probe_sha256': hashlib.sha256(probe).hexdigest(),
                  'dtb_sha256': hashlib.sha256(dtb_path.read_bytes()).hexdigest(),
                  'boot_header_version': 1, 'contains_xnu': False, 'contains_ios': False,
                  'display_register_writes': False if display_scan_probe else None,
                  'temporary_display_bus_clocks': display_scan_probe,
                  'probe_mode': 'display-scan-observe' if display_scan_probe else 'panel-te-read-only' if panel_te_probe else 'a650-read-only' if gpu_probe else 'ft3658u-power-enable' if touch_enable else 'ft3658u-power-read' if touch_power else 'ft3658u-runtime-read' if touch_probe else ('iosroot-read-only' if root_probe else ('usb' if usb_probe else 'display')),
                  'usb_probe_revision': 2 if usb_probe else None,
                  'touch_probe_revision': (5 if touch_enable else 3 if touch_reset else 2) if touch_probe else None,
                  'touch_reset_release': touch_reset, 'touch_temporary_power_enable': touch_enable,
                  'device_tested': False}
        output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'Built UEFI diagnostic image: {output}')
        print('Header v1 acceptance and all runtime behavior remain unverified on F4.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', required=True, type=Path)
    parser.add_argument('--dtb', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--usb-probe', action='store_true')
    parser.add_argument('--root-probe', action='store_true')
    parser.add_argument('--touch-probe', action='store_true')
    parser.add_argument('--touch-reset-probe', action='store_true')
    parser.add_argument('--touch-power-probe', action='store_true')
    parser.add_argument('--touch-enable-probe', action='store_true')
    parser.add_argument('--gpu-probe', action='store_true')
    parser.add_argument('--panel-te-probe', action='store_true')
    parser.add_argument('--display-scan-probe', action='store_true')
    args = parser.parse_args()
    pack(args.fd, args.dtb, args.output, args.usb_probe, args.root_probe, args.touch_probe or args.touch_reset_probe, args.touch_reset_probe, args.touch_power_probe, args.touch_enable_probe, args.gpu_probe, args.panel_te_probe, args.display_scan_probe)
