#!/usr/bin/env python3
"""Pack a source-built Munch XNU kernel with the verified iosroot handoff."""
import argparse
import gzip
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


FD_BASE = 0x9FC00000
FD_SIZE = 0x300000
PAYLOAD_BASE = 0xF0000000
PAYLOAD_MAX = 0x03FFF000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_crc32(path):
    value = 0
    with path.open('rb') as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            value = zlib.crc32(chunk, value)
    return f'{value & 0xffffffff:08x}'


def validate_ios_config(path):
    config = json.loads(path.read_text())
    if (config.get('version'), config.get('build')) != ('16.5', '20F66'):
        raise ValueError('Expected the verified iOS 16.5 (20F66) configuration')
    for key in ('system', 'root', 'identity'):
        info = config.get(key)
        if not isinstance(info, dict) or not {'path', 'size', 'sha256'} <= info.keys():
            raise ValueError(f'Missing {key} integrity record')
        artifact = Path(info['path'])
        if not artifact.is_file() or artifact.stat().st_size != info['size'] or file_digest(artifact) != info['sha256']:
            raise ValueError(f'iOS config hash/size mismatch: {key}')
    system = config['system']
    helper = Path(config['root']['path'])
    if system['size'] % 4096 or helper.stat().st_size > 0x03E00000:
        raise ValueError('Unsupported iosroot/helper geometry')
    if helper.read_bytes()[32:36] != b'NXSB':
        raise ValueError('The helper image is not APFS')
    if 'crc32' not in system:
        raise ValueError('The iosroot system record has no CRC32')
    return config


def validate_device_inputs(fd_path, dtb_path):
    fd = fd_path.read_bytes()
    metadata = json.loads(fd_path.with_suffix(fd_path.suffix + '.munch.json').read_text())
    if len(fd) != FD_SIZE or metadata.get('target') != 'munch' or metadata.get('fd_sha256') != digest(fd):
        raise ValueError('FD does not match the reviewed Munch build manifest')
    model = subprocess.check_output(['fdtget', str(dtb_path), '/', 'model'], text=True).strip()
    if model != 'Qualcomm Technologies, Inc. xiaomi munch':
        raise ValueError('Expected the captured live Munch DTB')
    return fd, metadata


def patch_loader(source, work, config, touch):
    """Make the generic loader select Munch and load the iosroot system image."""
    loader = source.read_text()
    platform = '''
/* Verified POCO F4 (munch) platform: Kona MPIDRs 0x000..0x700. */
static const struct platform platform_munch = {
\t.name = "munch", .io_base = 0x00400000, .io_size = 0x3FC00000,
\t.gic_dist = 0x17A00000, .gic_redist = 0x17A60000,
\t.cores = 8, .core_shift = 8, .small_cores = 0x0f,
};
'''
    marker = '\n/* ------------------------------------------------------------------------ */\n\nstatic void\nreboot(void)'
    if loader.count(marker) != 1:
        raise ValueError('Unexpected loader platform section')
    loader = loader.replace(marker, '\n' + platform + marker, 1)

    old_display = '''\tPLAT = &platform_qemu_virt;
\tif (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
\t    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
\t\tGOP = NULL;                     /* no display (QEMU without -device ramfb) */
\t} else {
\t\tSCREEN.base = GOP->mode->fb_base;
\t\tSCREEN.width = GOP->mode->info->width;
\t\tSCREEN.height = GOP->mode->info->height;
\t\tSCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
\t\tif (SCREEN.base == 0xE1000000ULL) {
\t\t\tPLAT = &platform_redwood;
\t\t} else {
\t\t\tramfb_phone_screen();
\t\t}
\t\tprint("display %d x %d at %x\\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
\t}
'''
    new_display = '''\tPLAT = &platform_qemu_virt;
\tif (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
\t    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
\t\tGOP = NULL;                     /* no display (QEMU without -device ramfb) */
\t} else {
\t\tSCREEN.base = GOP->mode->fb_base;
\t\tSCREEN.width = GOP->mode->info->width;
\t\tSCREEN.height = GOP->mode->info->height;
\t\tSCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
\t\tif (SCREEN.base == 0x9C000000ULL && SCREEN.width == 1080 && SCREEN.height == 2400 &&
\t\t    GOP->mode->info->pixels_per_scanline == 1080 && GOP->mode->info->pixel_format == 1) {
\t\t\tPLAT = &platform_munch;
\t\t} else if (SCREEN.base == 0xE1000000ULL) {
\t\t\tPLAT = &platform_redwood;
\t\t} else {
\t\t\tramfb_phone_screen();
\t\t}
\t\tprint("display %d x %d at %x\\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
\t}
'''
    if loader.count(old_display) != 1:
        raise ValueError('Unexpected loader display detection block')
    loader = loader.replace(old_display, new_display, 1)

    system = config['system']
    include = (f'#define MUNCH_SYSTEM_BYTES {system["size"]}ULL\n'
               f'#define MUNCH_SYSTEM_CRC32 0x{system["crc32"]}u\n')
    (work / 'ios-root.inc').write_text(include + (Path(__file__).with_name('ios-root.inc')).read_text())
    loader = loader.replace('static int\nrun_kernel(', '#include "ios-root.inc"\n\nstatic int\nrun_kernel(', 1)

    if touch:
        uefi = Path(__file__).parent.parent / 'uefi'
        adapter = ('#define system_table ST\n'
                   'static void text(const char *s) { print("%s", s); }\n'
                   'static void hex(uint64_t x) { print("%x", x); }\n'
                   'static void field(const char *s, uint64_t x) { print("%s%x\\n", s, x); }\n'
                   '#define MUNCH_TOUCH_RELEASE_RESET\n'
                   '#define MUNCH_TOUCH_POWER_ENABLE\n'
                   '#define MUNCH_TOUCH_HANDOFF\n'
                   'static unsigned touch_handoff_ready;\n')
        for name in ('touch-power-probe.inc', 'touch-power-enable.inc', 'touch-probe.inc'):
            adapter += f'#include "{(uefi / name).as_posix()}"\n'
        loader = loader.replace('#include "ios-root.inc"', adapter + '\n#include "ios-root.inc"', 1)
        hook = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
        replacement = '''\tif (have_payload) {
\t\tif (!munch_load_iosroot(&pi)) { reboot(); return 0; }
\t\tif (!touch_power_enable_begin()) { touch_power_enable_end(); reboot(); return 0; }
\t\ttouch_probe(ST, NULL);
\t\tif (!touch_handoff_ready) { touch_power_enable_end(); reboot(); return 0; }
\t\trun_kernel(0, 0);
\t\ttouch_restore();
\t\ttouch_power_enable_end();
\t}
'''
    else:
        hook = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
        replacement = '''\tif (have_payload) {
\t\tif (!munch_load_iosroot(&pi)) { reboot(); return 0; }
\t\trun_kernel(0, 0);
\t}
'''
    if loader.count(hook) != 1:
        raise ValueError('Unexpected loader one-shot entry')
    loader = loader.replace(hook, replacement, 1)
    (work / 'loader.c').write_text(loader)


def make_cpio(work):
    name = b'TRAILER!!!\0'
    cpio = b'070701' + b''.join(f'{value:08x}'.encode() for value in
                                 [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]) + name
    cpio += b'\0' * (-len(cpio) % 4)
    path = work / 'ramdisk'
    path.write_bytes(gzip.compress(cpio, mtime=0))
    return path


def pack(fd_path, dtb_path, kernel_path, ios_config_path, output, cpus, touch, gpu, verbose, extra):
    if cpus not in (1, 8):
        raise ValueError('Only 1 or 8 CPUs are supported')
    config = validate_ios_config(ios_config_path)
    fd, metadata = validate_device_inputs(fd_path, dtb_path)
    kernel = kernel_path.read_bytes()
    if kernel[:4] != b'\xcf\xfa\xed\xfe' or b'XNULDRST' not in kernel:
        raise ValueError('Expected a source-built ARM64 Mach-O with the loader return slot')
    system_path = Path(config['system']['path'])
    if config['system']['crc32'] != file_crc32(system_path):
        raise ValueError('The iosroot CRC32 record does not match the system image')
    root = Path(__file__).resolve().parents[3]
    source = root / 'xnuloader/loader.c'
    identity = Path(config['identity']['path']).read_bytes()
    if cpus == 8:
        cmdline = ('root_apfs=ufs_darwin ufs=2 apfs_volume=ufs_darwin bootcpu=7 '
                   'cpus=8 cpufreq=1 small_cores=2 small_cores_after=0 touchscreen=1 '
                   'buttons=1 loader_return_after=600')
    else:
        cmdline = '-v cpus=1 touchscreen=1 buttons=1 loader_return_after=600'
    if gpu:
        cmdline += ' gpu=1'
    if verbose and '-v' not in cmdline.split():
        cmdline = '-v ' + cmdline
    if extra:
        cmdline += ' ' + extra
    helper = Path(config['root']['path']).read_bytes()

    with tempfile.TemporaryDirectory(prefix='munch-iosroot-') as directory:
        work = Path(directory)
        shutil.copy2(root / 'xnuloader/efi.h', work / 'efi.h')
        patch_loader(source, work, config, touch)
        (work / 'identity.h').write_bytes(identity)
        subprocess.run(['clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
                        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-const-variable', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                        '-c', 'loader.c', '-o', 'loader.obj'], cwd=work, check=True)
        subprocess.run(['lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                        '/base:0xA1000000', '/out:loader.efi', 'loader.obj'], cwd=work, check=True)
        entries = [('loader.efi', (work / 'loader.efi').read_bytes()), ('kernel', kernel),
                   ('cmdline', cmdline.encode() + b'\0'), ('ramdisk', helper)]
        body = bytearray()
        for name, data in entries:
            body += struct.pack('<16sII', name.encode(), len(data), 0)
            body += data + b'\0' * (-len(data) % 16)
        body += b'\0' * 24
        payload = struct.pack('<II', 0x50554E58, len(body)) + body
        if len(payload) > PAYLOAD_MAX:
            raise ValueError('Loader payload exceeds the reserved RAM capacity')
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c', str(root / 'devices/poco-f4/uefi/bootshim.S'),
                        f'-DFD_BASE=0x{FD_BASE:x}', f'-DFD_SIZE=0x{FD_SIZE:x}',
                        f'-DPAYLOAD_BASE=0x{PAYLOAD_BASE:x}', '-o', str(work / 'shim.o')], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work / 'shim.o'),
                        str(work / 'shim.bin')], check=True)
        blob = (work / 'shim.bin').read_bytes() + fd + payload
        blob += b'\0' * (-len(blob) % 16)
        image_kernel = work / 'kernel'
        image_kernel.write_bytes(gzip.compress(blob, mtime=0) + dtb_path.read_bytes())
        ramdisk = make_cpio(work)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(root / 'devices/poco-x5-pro/prebuilt/mkbootimg.py'),
                        '--header_version', '1', '--board', 'MUNCHIOS', '--kernel', str(image_kernel),
                        '--ramdisk', str(ramdisk), '-o', str(output)], check=True)
        report = {**metadata, 'mode': 'iosroot-source-kernel', 'cmdline': cmdline,
                  'requested_cpu_count': cpus, 'touchscreen': touch, 'gpu_requested': gpu,
                  'gpu_firmware_external': True, 'ios_version': config['version'], 'ios_build': config['build'],
                  'iosroot_system_sha256': config['system']['sha256'], 'iosroot_system_size': config['system']['size'],
                  'iosroot_helper_sha256': config['root']['sha256'], 'kernel_sha256': digest(kernel),
                  'loader_sha256': digest((work / 'loader.efi').read_bytes()),
                  'dtb_sha256': digest(dtb_path.read_bytes()), 'image_sha256': digest(output.read_bytes()),
                  'image_size': output.stat().st_size, 'contains_iosroot_reader': True,
                  'device_tested': False}
        output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Built Munch iosroot image: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--ios-config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=[1, 8], default=8)
    parser.add_argument('--no-touch', dest='touch', action='store_false')
    parser.add_argument('--no-gpu', dest='gpu', action='store_false')
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--extra-cmdline', default='')
    parser.set_defaults(touch=True, gpu=True)
    args = parser.parse_args()
    pack(args.fd.resolve(), args.dtb.resolve(), args.kernel.resolve(), args.ios_config.resolve(),
         args.output.resolve(), args.cpus, args.touch, args.gpu, args.verbose, args.extra_cmdline)
