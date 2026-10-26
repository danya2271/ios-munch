#!/usr/bin/env python3
"""Build a fixed, one-shot XNU entry-checkpoint probe for the inspected F4."""
import argparse
import gzip
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

LOADER_SHA = '7104a1403bb0007a8fc7a7cf4dfb327bc7136a09ba9479f17e5eca449499557b'
KERNEL_SHA = '04263e76d6cc8b13e8f9e89229afa8f4d3039d8f3714ca7fd1d6e8603091d28b'
CMDLINE = '-v cpus=1 loader_return_after=8'



def intermediate_kernel(original, checkpoint):
    """Insert a diagnostic return before or during the early arm_init prologue.

    Addresses and expected instructions apply only to the SHA-pinned baseline.
    Reuse the existing assembly checkpoint ABI without calling early C helpers.
    """
    segments = []
    offset = 32
    for _ in range(struct.unpack_from('<I', original, 16)[0]):
        command, size = struct.unpack_from('<II', original, offset)
        if command == 0x19:
            va, length, file_offset, file_size = struct.unpack_from('<QQQQ', original, offset + 24)
            segments.append((va, file_size, file_offset))
        offset += size
    def file_at(va, length):
        for base, size, start in segments:
            if base <= va and va + length <= base + size:
                return start + va - base
        raise ValueError('Diagnostic address is outside file-backed segments')
    cave = 0xfffffff0074424ac
    hook = {6: 0xfffffff0074454a8, 7: 0xfffffff00743952c,
            8: 0xfffffff007439568, 9: 0xfffffff0074396ec}[checkpoint]
    source = 0xfffffff007445430
    patched = bytearray(original)
    block = bytearray(original[file_at(source, 60):file_at(source, 60) + 60])
    expected = [0xd0ffffe9, 0x911fc129, 0xf940092a, 0xf100155f, 0x54000161,
                0xd2800000, 0xd2800001, 0xd2800042, 0xd28000a3, 0xd538100b,
                0x927ff96b, 0xd518100b, 0xd5033fdf, 0xf9400529, 0xd61f0120]
    if list(struct.unpack('<15I', block)) != expected:
        raise ValueError('Unexpected baseline assembly checkpoint instructions')
    stub_size = {6: 60, 7: 76, 8: 76, 9: 84}[checkpoint]
    if original[file_at(cave, stub_size):file_at(cave, stub_size) + stub_size] != bytes(stub_size):
        raise ValueError('Expected unused zero-filled entry padding')
    hook_bytes = (struct.pack('<III', 0x8b160280, 0xcb170000, 0xd65f03c0)
                  if checkpoint == 6 else struct.pack('<I',
                      {7: 0xd10303ff, 8: 0x94000695, 9: 0x940072f7}[checkpoint]))
    if original[file_at(hook, len(hook_bytes)):file_at(hook, len(hook_bytes)) + len(hook_bytes)] != hook_bytes:
        raise ValueError('Unexpected boot CPU arm_init handoff')
    # ADRP x9, _LowExceptionVectorBase: source page 0x7442 -> 0x7443.
    # No conditional fall-through: this image always stops at this location.
    words = expected[:]
    words[0] = 0xb0000009
    words[3] = words[4] = 0xd503201f
    words[8] = 0xd2800003 | (checkpoint << 5)  # mov x3, checkpoint
    if checkpoint in (7, 8, 9):
        # Switch from the high virtual alias to its identity-mapped physical
        # alias before using the existing MMU-off return sequence. x22/x23
        # retain the bootstrap VA/PA bases before the first arm_init helper call.
        words = [0x1000008b, 0xcb16016b, 0x8b17016b, 0xd61f0160] + words
        # adr x11, .+16; sub x11,x11,x22; add x11,x11,x23; br x11
    if checkpoint == 9:
        # Recover the bootstrap bases from the unchanged arm_init save slots.
        words = [0xf9404bf6, 0xf94047f7] + words  # ldr x22,[sp,#0x90]; ldr x23,[sp,#0x88]
    patched[file_at(cave, stub_size):file_at(cave, stub_size) + stub_size] = struct.pack('<' + 'I' * len(words), *words)
    branch = 0x14000000 | (((cave - hook) // 4) & 0x03ffffff)
    struct.pack_into('<I', patched, file_at(hook, 4), branch)
    return bytes(patched)


def no_early_bootargs_kernel(original):
    """Skip only loader_jump's PE bootcpu query before PE initialization."""
    address = 0xfffffff007456160
    offset = 32
    for _ in range(struct.unpack_from('<I', original, 16)[0]):
        command, size = struct.unpack_from('<II', original, offset)
        if command == 0x19:
            base, _, start, length = struct.unpack_from('<QQQQ', original, offset + 24)
            if base <= address and address + 4 <= base + length:
                at = start + address - base
                if original[at:at + 4] != struct.pack('<I', 0x9416f833):
                    raise ValueError('Unexpected loader_jump PE_parse_boot_argn call')
                patched = bytearray(original)
                # Return false from the skipped query, retaining current-core mode.
                struct.pack_into('<I', patched, at, 0x52800000)  # mov w0, #0
                return bytes(patched)
        offset += size
    raise ValueError('Early boot-argument call is outside file-backed segments')


def startup_checkpoint_kernel(original, checkpoint):
    """Stop at a reviewed machine/kernel bootstrap boundary."""
    segments = []
    offset = 32
    for _ in range(struct.unpack_from('<I', original, 16)[0]):
        command, size = struct.unpack_from('<II', original, offset)
        if command == 0x19:
            base, _, start, length = struct.unpack_from('<QQQQ', original, offset + 24)
            segments.append((base, start, length))
        offset += size
    def at(address, size):
        for base, start, length in segments:
            if base <= address and address + size <= base + length:
                return start + address - base
        raise ValueError('Startup checkpoint patch is outside file-backed segments')
    cave, target = 0xfffffff0074424ac, 0xfffffff0074562c8
    hook, expected = {50: (0xfffffff007446d14, 0x17fb2369),
                      51: (0xfffffff00730fb28, 0xb0004668),
                      52: (0xfffffff0073101cc, 0x9400005d),
                      53: (0xfffffff007a9f68c, 0x97fddcb7),
                      54: (0xfffffff007a9f844, 0x97f37ddf),
                      55: (0xfffffff00777f190, 0xb0ffcc20),
                      56: (0xfffffff00777f6a0, 0x9409c738)}[checkpoint]
    if original[at(cave, 8):at(cave, 8) + 8] != bytes(8):
        raise ValueError('Startup checkpoint requires unused entry padding')
    if struct.unpack_from('<I', original, at(hook, 4))[0] != expected:
        raise ValueError('Unexpected startup checkpoint hook instruction')
    def branch(source, destination):
        return 0x14000000 | (((destination - source) // 4) & 0x03ffffff)
    patched = bytearray(original)
    struct.pack_into('<II', patched, at(cave, 8), 0x52800000 | (checkpoint << 5), branch(cave + 4, target))
    struct.pack_into('<I', patched, at(hook, 4), branch(hook, cave))
    return bytes(patched)


def pack(fd_path, dtb_path, output, checkpoint=1, no_early_bootargs=False, ios_config=None, touch=False, cpus=1):
    if cpus not in (1, 8) or (cpus != 1 and checkpoint != 58):
        raise ValueError('Eight-core mode is supported only for the iOS profile')
    if touch and (checkpoint != 58 or not no_early_bootargs):
        raise ValueError('Touch prototype requires iOS mode and the tested return overlay')
    if checkpoint not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 58):
        raise ValueError('Only reviewed checkpoints 1 through 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57 and 58 are supported')
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    uefi = here.parent / 'uefi'
    source = root / 'xnuloader/loader.c'
    kernel = (root / 'prebuilt/mach.development.bcm2837').read_bytes()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    if sha(source.read_bytes()) != LOADER_SHA or sha(kernel) != KERNEL_SHA:
        raise ValueError('Loader or kernel revision differs from the reviewed checkpoint baseline')
    if kernel[:4] != b'\xcf\xfa\xed\xfe' or b'XNULDRST' not in kernel:
        raise ValueError('Expected the patched ARM64 Mach-O kernel with loader return slot')
    touch_report = None
    if touch:
        import importlib.util
        spec = importlib.util.spec_from_file_location('munch_touch_overlay', here.parent / 'touch/kernel-overlay.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix='munch-touch-code-') as directory:
            kernel, touch_report = module.build(kernel, Path(directory))
    if checkpoint in (6, 7, 8, 9):
        kernel = intermediate_kernel(kernel, checkpoint)
    if no_early_bootargs:
        if checkpoint not in (10, 11, 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 58):
            raise ValueError('Early boot-argument bypass is reviewed only for checkpoints 10, 11, 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57 and 58')
        kernel = no_early_bootargs_kernel(kernel)
    if checkpoint in (50, 51, 52, 53, 54, 55, 56):
        kernel = startup_checkpoint_kernel(kernel, checkpoint)
    hello = None
    if checkpoint == 57:
        if not no_early_bootargs:
            raise ValueError('Hello probe requires the tested current-core return overlay')
        hello = (root / 'prebuilt/userland/hello').read_bytes()
        if sha(hello) != '48a910940230506813aa0b75692e58d1c6698feccf7c6f7771c8790b6c97522d':
            raise ValueError('Hello prebuilt differs from the reviewed raw-syscall program')
        if len(hello) > 65536 or hello[:4] != b'\xcf\xfa\xed\xfe':
            raise ValueError('Expected a small Mach-O RAM root')
    ios = None
    cmdline = CMDLINE
    if checkpoint == 58:
        if not no_early_bootargs or ios_config is None:
            raise ValueError('iOS mode requires a verified config and tested return overlay')
        ios = json.loads(ios_config.read_text())
        if ios['version'] != '16.5' or ios['build'] != '20F66':
            raise ValueError('Unreviewed iOS version')
        for key in ('system', 'root', 'identity'):
            info = ios[key]
            path = Path(info['path'])
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if path.stat().st_size != info['size'] or digest != info['sha256']:
                raise ValueError('iOS config hash/size mismatch: ' + key)
        hello = Path(ios['root']['path']).read_bytes()
        if len(hello) > 0x03e00000 or hello[32:36] != b'NXSB':
            raise ValueError('Expected a small APFS helper RAM root')
        cmdline = ('cpus=8 small_cores_after=0 loader_return_after=600' if cpus == 8
                   else '-v cpus=1 loader_return_after=600')
    fd = fd_path.read_bytes()
    metadata = json.loads(fd_path.with_suffix(fd_path.suffix + '.munch.json').read_text())
    if (metadata.get('target') != 'munch' or metadata.get('diagnostic_revision') != (3 if checkpoint == 58 else 2) or
            metadata.get('fd_sha256') != sha(fd) or len(fd) != 0x300000):
        raise ValueError('Expected the reviewed revision-2 F4 FD')
    model = subprocess.check_output(['fdtget', str(dtb_path), '/', 'model'], text=True).strip()
    if model != 'Qualcomm Technologies, Inc. xiaomi munch':
        raise ValueError('Expected the captured F4 live DTB')
    with tempfile.TemporaryDirectory(prefix='munch-checkpoint-') as temporary:
        work = Path(temporary)
        shutil.copy2(source, work / 'loader.c')
        shutil.copy2(root / 'xnuloader/efi.h', work / 'efi.h')
        subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(here / 'checkpoint-loader.patch')],
                       cwd=work, check=True)
        if cpus == 8:
            loader_source = (work / 'loader.c').read_text()
            old = '\t\t/* This probe publishes only the current CPU, even with cpus=1 present. */\n\t\tmore_cores = 0;'
            new = '\t\tmore_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == 32);'
            guard = '"-v cpus=1 loader_return_after=600"'
            if loader_source.count(old) != 1 or loader_source.count(guard) != 1:
                raise ValueError('Unexpected single-core publication or payload guard')
            loader_source = loader_source.replace(old, new, 1).replace(guard, '"' + cmdline + '"', 1)
            (work / 'loader.c').write_text(loader_source)
        if ios is not None:
            identity = Path(ios['identity']['path']).read_bytes()
            system = ios['system']
            include = (f"#define MUNCH_SYSTEM_BYTES {system['size']}ULL\n"
                       f"#define MUNCH_SYSTEM_CRC32 0x{system['crc32']}u\n")
            (work / 'ios-root.inc').write_text(include + (here / 'ios-root.inc').read_text())
            loader_source = (work / 'loader.c').read_text()
            loader_source = loader_source.replace('static int\nrun_kernel(', '#include "ios-root.inc"\n\nstatic int\nrun_kernel(', 1)
            needle = '\t\trun_kernel(1, pi.checkpoint);'
            if loader_source.count(needle) != 1:
                raise ValueError('Unexpected one-shot entry for iOS load hook')
            loader_source = loader_source.replace(needle,
                '\t\tif (!munch_load_iosroot(&pi)) { reboot(); return 0; }\n' + needle)
            if touch:
                adapter = ('#define system_table ST\n'
                           'static void text(const char *s) { print("%s", s); }\n'
                           'static void hex(uint64_t x) { print("%x", x); }\n'
                           'static void field(const char *s, uint64_t x) { print("%s%x\\n", s, x); }\n'
                           '#define MUNCH_TOUCH_RELEASE_RESET\n#define MUNCH_TOUCH_POWER_ENABLE\n'
                           '#define MUNCH_TOUCH_HANDOFF\nstatic unsigned touch_handoff_ready;\n')
                for name in ('touch-power-probe.inc', 'touch-power-enable.inc', 'touch-probe.inc'):
                    adapter += '#include "' + str(uefi / name) + '"\n'
                loader_source = loader_source.replace('#include "ios-root.inc"', adapter + '\n#include "ios-root.inc"', 1)
                hook = '\t\trun_kernel(1, pi.checkpoint);'
                replacement = ('\t\tif (!touch_power_enable_begin()) { touch_power_enable_end(); reboot(); return 0; }\n'
                               '\t\ttouch_probe(ST, NULL);\n'
                               '\t\tif (!touch_handoff_ready) { touch_power_enable_end(); reboot(); return 0; }\n'
                               + hook + '\n\t\ttouch_restore();\n\t\ttouch_power_enable_end();')
                if loader_source.count(hook) != 1:
                    raise ValueError('Unexpected touch handoff hook')
                loader_source = loader_source.replace(hook, replacement, 1)
            (work / 'loader.c').write_text(loader_source)
        else:
            identity = subprocess.check_output([sys.executable, str(root / 'xnuloader/mkidentity.py'), '--placeholder'])
        (work / 'identity.h').write_bytes(identity)
        subprocess.run(['clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
                        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-const-variable', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-c', 'loader.c', '-o', 'loader.obj'], cwd=work, check=True)
        subprocess.run(['lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                        '/base:0xA1000000', '/out:loader.efi', 'loader.obj'], cwd=work, check=True)
        loader = (work / 'loader.efi').read_bytes()
        body = bytearray()
        entries = [('loader.efi', loader), ('kernel', kernel),
                           ('cmdline', cmdline.encode() + b'\0'),
                           ('checkpoint', str(checkpoint).encode() + b'\0')]
        if hello is not None:
            entries.append(('ramdisk', hello))
        for name, data in entries:
            body += struct.pack('<16sII', name.encode(), len(data), 0)
            body += data + b'\0' * (-len(data) % 16)
        body += b'\0' * 24
        payload = struct.pack('<II', 0x50554e58, len(body)) + body
        if len(payload) > 0x03fff000:
            raise ValueError('Payload exceeds the reserved RAM capacity')
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c', str(uefi / 'bootshim.S'),
                        '-DFD_BASE=0x9fc00000', '-DFD_SIZE=0x300000', '-DPAYLOAD_BASE=0xf0000000',
                        '-o', str(work / 'shim.o')], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work / 'shim.o'),
                        str(work / 'shim.bin')], check=True)
        blob = (work / 'shim.bin').read_bytes() + fd + payload
        blob += b'\0' * (-len(blob) % 16)
        image_kernel = work / 'kernel'
        image_kernel.write_bytes(gzip.compress(blob, mtime=0) + dtb_path.read_bytes())
        name = b'TRAILER!!!\0'
        cpio = b'070701' + b''.join(f'{n:08x}'.encode() for n in
                                  [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]) + name
        cpio += b'\0' * (-len(cpio) % 4)
        (work / 'ramdisk').write_bytes(gzip.compress(cpio, mtime=0))
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(root / 'devices/poco-x5-pro/prebuilt/mkbootimg.py'),
                        '--header_version', '1', '--board', f'MUNCHCP{checkpoint}', '--kernel', str(image_kernel),
                        '--ramdisk', str(work / 'ramdisk'), '-o', str(output)], check=True)
        report = {**metadata, 'mode': 'ios165-read-only-ramboot' if ios is not None else ('xnu-hello-ramroot' if hello is not None else 'xnu-entry-checkpoint'), 'checkpoint': checkpoint, 'cmdline': cmdline,
                  'loader_overlay_sha256': sha((here / 'checkpoint-loader.patch').read_bytes()),
                  'published_cpu_count': 1 if cpus == 1 else None, 'requested_cpu_count': cpus,
                  'cpu_publication': 'current-core-only' if cpus == 1 else 'PSCI-affinity-filtered-up-to-eight',
                  'verbose_console': cpus == 1, 'skip_early_bootcpu_query': no_early_bootargs,
                  'loader_source_sha256': LOADER_SHA, 'kernel_baseline_sha256': KERNEL_SHA, 'kernel_sha256': sha(kernel),
                  'loader_sha256': sha(loader), 'image_sha256': sha(output.read_bytes()),
                  'image_size': output.stat().st_size, 'dtb_sha256': sha(dtb_path.read_bytes()),
                  'ramroot_sha256': sha(hello) if hello is not None else None,
                  'contains_xnu': True, 'contains_ios': ios is not None, 'identity': 'local-ipsw' if ios is not None else 'placeholder',
                  'device_tested': False, 'touch_prototype': touch_report}
        output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'Built XNU checkpoint-{checkpoint} probe: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--checkpoint', type=int, choices=[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 58], default=1)
    parser.add_argument('--no-early-bootargs', action='store_true',
                        help='Checkpoints 10, 11, 12, 21, 31, 32, 34, 35, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57 and 58: bypass the pre-PE bootcpu query')
    parser.add_argument('--ios-config', type=Path)
    parser.add_argument('--cpus', type=int, choices=[1, 8], default=1, help='iOS only: request all eight CPUs through the existing PSCI path; disable verbose framebuffer console')
    parser.add_argument('--touch', action='store_true', help='Experimental ABI-pinned FT3658U driver with verified UEFI power handoff')
    args = parser.parse_args()
    pack(args.fd.resolve(), args.dtb.resolve(), args.output.resolve(), args.checkpoint, args.no_early_bootargs, args.ios_config, args.touch, args.cpus)
