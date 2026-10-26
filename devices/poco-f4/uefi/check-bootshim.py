#!/usr/bin/env python3
"""Emulate the actual ARM64 shim, including payload/FD overlap and rejection."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).with_name('bootshim.S'))
p.add_argument('--expect-old-corruption', action='store_true')
a = p.parse_args()
fd_base, fd_size, payload_base = 0x90000000, 0x3000, 0xa0000000
with tempfile.TemporaryDirectory(prefix='munch-shim-test-') as directory:
    work = Path(directory)
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c', str(a.source),
                    f'-DFD_BASE={fd_base}', f'-DFD_SIZE={fd_size}', f'-DPAYLOAD_BASE={payload_base}',
                    '-o', str(work/'shim.o')], check=True)
    reloc = subprocess.check_output(['llvm-readelf', '-r', str(work/'shim.o')], text=True)
    assert 'R_AARCH64' not in reloc, 'Raw shim must have no unapplied relocations'
    subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work/'shim.o'), str(work/'shim.bin')], check=True)
    shim = (work/'shim.bin').read_bytes()
assert len(shim) % 16 == 0 and shim[56:60] == b'ARMd'
fd = bytes((i * 17 + 3) & 255 for i in range(fd_size))
body = bytes((i * 31 + 7) & 255 for i in range(0x3000 - 8))
payload = struct.pack('<II', 0x50554e58, len(body)) + body

def run(source_fd, invalid=False):
    start = source_fd - len(shim)
    data = shim + fd + (struct.pack('<II', 0x50554e58, 0x4000000) if invalid else payload)
    ranges = [(start & ~4095, (start+len(data)+4095)&~4095),
              (fd_base, fd_base+fd_size), (payload_base, payload_base+0x4000)]
    merged = []
    for lo, hi in sorted(ranges):
        if merged and lo <= merged[-1][1]: merged[-1][1] = max(hi, merged[-1][1])
        else: merged.append([lo,hi])
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for lo, hi in merged: uc.mem_map(lo, hi-lo)
    uc.mem_write(start, data)
    uc.reg_write(UC_ARM64_REG_X0, 0x12345678)
    uc.reg_write(UC_ARM64_REG_X1, 0x87654321)
    entered = []
    def entry(machine, address, size, user):
        entered.append(True); machine.emu_stop()
    uc.hook_add(UC_HOOK_CODE, entry, begin=fd_base, end=fd_base)
    uc.emu_start(start, 0, count=200000)
    return (bool(entered), bytes(uc.mem_read(fd_base,fd_size)), bytes(uc.mem_read(payload_base,len(payload))),
            uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_X1))

if a.expect_old_corruption:
    entered, got_fd, got_payload, _, _ = run(fd_base-fd_size-0x1000)
    assert entered and got_fd == fd and got_payload != payload
    print('REPRODUCED: old FD-first shim corrupts overlapping payload input')
else:
    cases = [0x80000000, fd_base-fd_size-0x1000, fd_base-0x1000,
             fd_base, fd_base+0x4000, payload_base-0x1000-fd_size,
             payload_base+0x1000-fd_size]
    for source_fd in cases:
        entered, got_fd, got_payload, x0, x1 = run(source_fd)
        assert entered and got_fd == fd and got_payload == payload, hex(source_fd)
        assert (x0,x1) == (0x12345678,0x87654321)
    assert not run(0x80000000, invalid=True)[0]
    assert not run(fd_base+0x1000)[0]
    print('PASS: seven relocation layouts, FD/payload overlap, memmove directions, input registers, oversized/self-overlap rejection')
