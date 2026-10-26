#!/usr/bin/env python3
"""Independently inspect the experimental touch boot image before publication."""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image', type=Path)
p.add_argument('--fd', required=True, type=Path)
p.add_argument('--dtb', required=True, type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
sha = lambda b: hashlib.sha256(b).hexdigest()
image = a.image.read_bytes()
report = json.loads(a.image.with_suffix('.img.json').read_text())
assert len(image) == report['image_size'] < 120 * 1024 * 1024
assert sha(image) == report['image_sha256']
assert image[:8] == b'ANDROID!' and struct.unpack_from('<I', image, 40)[0] == 1
page, kernel_size = struct.unpack_from('<I', image, 36)[0], struct.unpack_from('<I', image, 8)[0]
z = zlib.decompressobj(31)
blob = z.decompress(image[page:page + kernel_size]) + z.flush()
assert z.eof and z.unused_data == a.dtb.read_bytes()
assert sha(z.unused_data) == report['dtb_sha256']
assert blob[208:208 + 0x300000] == a.fd.read_bytes()
assert sha(a.fd.read_bytes()) == report['fd_sha256']
at = 208 + 0x300000
magic, length = struct.unpack_from('<II', blob, at)
assert magic == 0x50554e58
end, at = at + 8 + length, at + 8
entries = {}
while at + 24 <= end:
    name, size, reserved = struct.unpack_from('<16sII', blob, at); at += 24
    if name == bytes(16):
        assert not size and not reserved and at == end
        break
    name = name.rstrip(b'\0').decode()
    assert name not in entries and not reserved and at + size <= end
    entries[name] = blob[at:at + size]; at += (size + 15) & ~15
assert set(entries) == {'loader.efi', 'kernel', 'cmdline', 'checkpoint', 'ramdisk'}
assert report['cmdline'] in ('-v cpus=1 loader_return_after=600', 'cpus=8 small_cores_after=0 loader_return_after=600')
assert entries['cmdline'] == report['cmdline'].encode() + b'\0'
assert entries['checkpoint'] == b'58\0'
for name, key in [('loader.efi', 'loader_sha256'), ('kernel', 'kernel_sha256'), ('ramdisk', 'ramroot_sha256')]:
    assert sha(entries[name]) == report[key]
assert entries['loader.efi'][:2] == b'MZ' and entries['ramdisk'][32:36] == b'NXSB'
baseline = (root / 'prebuilt/mach.development.bcm2837').read_bytes()
assert sha(baseline) == report['kernel_baseline_sha256']
kernel = entries['kernel']; assert len(kernel) == len(baseline)
segments, at = [], 32
for _ in range(struct.unpack_from('<I', baseline, 16)[0]):
    cmd, size = struct.unpack_from('<II', baseline, at)
    if cmd == 0x19:
        va, _, off, n = struct.unpack_from('<QQQQ', baseline, at + 24)
        segments.append((va, off, n))
    at += size
def file_at(va, n):
    for base, off, size in segments:
        if base <= va and va + n <= base + size: return off + va - base
    raise ValueError('Unmapped VA')
touch = report['touch_prototype']
start, limit = int(touch['entry'], 16), int(touch['limit'], 16)
n = touch['code_bytes']; assert 0 < n <= limit - start
at = file_at(start, n)
assert sha(kernel[at:at + n]) == touch['code_sha256']
restored = bytearray(kernel); restored[at:at + n] = baseline[at:at + n]
at = file_at(0xfffffff007456160, 4)
assert kernel[at:at + 4] == struct.pack('<I', 0x52800000)
restored[at:at + 4] = baseline[at:at + 4]
assert bytes(restored) == baseline
print('PASS: boot header, size, FD, DTB, payload, APFS helper, hashes, bounded touch overlay and sole early-query patch')
