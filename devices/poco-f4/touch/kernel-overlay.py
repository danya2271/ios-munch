#!/usr/bin/env python3
"""Build a bounded, SHA/ABI-pinned FT3658U prototype in dormant driver space."""
import hashlib
import struct
import subprocess
from pathlib import Path

BASELINE = '04263e76d6cc8b13e8f9e89229afa8f4d3039d8f3714ca7fd1d6e8603091d28b'
ENTRY = 0xfffffff0074a4b80
LIMIT = 0xfffffff0074a5e2c  # chip_poll: never overwrite this or later functions
SYMBOLS = {
    'ml_io_map': 0xfffffff007447dbc,
    'kernel_thread_start': 0xfffffff0073246a8,
    'thread_deallocate': 0xfffffff007322a78,
    'current_thread': 0xfffffff007446eac,
    'thread_terminate': 0xfffffff007326c34,
    'IOSleep': 0xfffffff00794dac4,
    'delay_for_interval': 0xfffffff0072cbb18,
    'hakios_touch_post': 0xfffffff0074a3ed0,
    'memset': 0xfffffff007b26760,
    'printf': 0xfffffff0072f5ba0,
    'munch_state': 0xfffffff007ed6fd0,
}

def build(original, work):
    if hashlib.sha256(original).hexdigest() != BASELINE:
        raise ValueError('Touch overlay requires the exact reviewed prebuilt')
    here = Path(__file__).resolve().parent
    # Verify symbols from the Mach-O nlist, including static symbols.
    commands, cursor = struct.unpack_from('<I', original, 16)[0], 32
    segments, symbols = [], {}
    for _ in range(commands):
        cmd, size = struct.unpack_from('<II', original, cursor)
        if cmd == 0x19:
            va, _, off, length = struct.unpack_from('<QQQQ', original, cursor + 24)
            segments.append((va, off, length))
        elif cmd == 2:
            symoff, count, stroff, strsize = struct.unpack_from('<4I', original, cursor + 8)
            strings = original[stroff:stroff + strsize]
            for i in range(count):
                name, kind, sect, desc, address = struct.unpack_from('<IBBHQ', original, symoff + i * 16)
                label = strings[name:strings.index(b'\0', name)].decode()
                symbols.setdefault(label, []).append(address)
        cursor += size
    expected = {**{'_' + k: v for k, v in SYMBOLS.items() if k != 'munch_state'},
                '_hakios_goodix_init': ENTRY, '_chip_poll': LIMIT, '_spi_tx': SYMBOLS['munch_state'],
                '_spi_rx': SYMBOLS['munch_state'] + 1032}
    for name, address in expected.items():
        if address not in symbols.get(name, []):
            raise ValueError('Kernel ABI mismatch: ' + name)
    script = 'SECTIONS { . = 0x%x; .text : { *(.text.entry) *(.text*) *(.rodata*) . = ALIGN(4); } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n' % ENTRY
    script += '\n'.join('%s = 0x%x;' % item for item in SYMBOLS.items())
    (work / 'touch.ld').write_text(script)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin',
                    '-fno-stack-protector', '-fno-pic', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                    '-c', str(here / 'xnu-ft3658u.c'), '-o', str(work / 'touch.o')], check=True)
    subprocess.run(['ld.lld', '-e', 'munch_touch_init', '-T', str(work / 'touch.ld'), '--no-undefined',
                    '-o', str(work / 'touch.elf'), str(work / 'touch.o')], check=True)
    subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work / 'touch.elf'), str(work / 'touch.bin')], check=True)
    blob = (work / 'touch.bin').read_bytes()
    if not blob or len(blob) > LIMIT - ENTRY or len(blob) % 4:
        raise ValueError('Touch overlay exceeds dormant entry/thread region')
    for va, off, length in segments:
        if va <= ENTRY and LIMIT <= va + length:
            start = off + ENTRY - va
            patched = bytearray(original)
            patched[start:start + len(blob)] = blob
            return bytes(patched), {'code_bytes': len(blob), 'code_sha256': hashlib.sha256(blob).hexdigest(),
                                    'entry': hex(ENTRY), 'limit': hex(LIMIT), 'bss': hex(SYMBOLS['munch_state'])}
    raise ValueError('Overlay is outside file-backed kernel text')
