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
shim_size = report.get('shim_size', 208)
assert shim_size % 16 == 0 and 64 <= shim_size <= 4096
assert blob[56:60] == b'ARMd'
assert blob[shim_size:shim_size + 0x300000] == a.fd.read_bytes()
assert sha(a.fd.read_bytes()) == report['fd_sha256']
at = shim_size + 0x300000
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
native_cmdline = ('-v cpus=8 small_cores_after=0 munch_touch=1 adreno=22 adreno_speed=1 '
                  'adreno_pause=0 adreno_watch=20 adreno_return=1 loader_return_after=120')
ios_gpu_cmdline = 'cpus=8 small_cores_after=0 munch_touch=1 gpu=1 adreno_speed=1 loader_return_after=600'
ios_gpu_fast_cmdline = ios_gpu_cmdline.replace('adreno_speed=1', 'adreno_speed=5')
allowed_gpu = {
    f'cpus=8 small_cores_after=0 munch_touch=1 gpu=1 adreno_speed={speed} '
    + ('fb_hz=120 ' if hz == 120 else '') + ('ca_vbl=1 glwait=1 ' if sync else '')
    + ('munch_te=1 ' if te else '') + f'loader_return_after={seconds}'
    for speed in (1, 5) for hz in (60, 120) for sync in (False, True) for seconds in (0, 600) for te in (False, True)
}
allowed_gpu |= {'-v ' + c for c in allowed_gpu}
assert report['cmdline'] in allowed_gpu | {'-v cpus=1 loader_return_after=600', 'cpus=8 small_cores_after=0 loader_return_after=600', native_cmdline}
assert entries['cmdline'] == report['cmdline'].encode() + b'\0'
assert entries['checkpoint'] == b'58\0'
for name, key in [('loader.efi', 'loader_sha256'), ('kernel', 'kernel_sha256'), ('ramdisk', 'ramroot_sha256')]:
    assert sha(entries[name]) == report[key]
assert entries['loader.efi'][:2] == b'MZ' and entries['ramdisk'][32:36] == b'NXSB'
if report.get('native_gpu') is not None:
    native = report['native_gpu']
    if native['profile'] == 'munch-a650-ios-gpu':
        assert native.get('gpu_speed_index', 1) in (1, 5)
        speed = native.get('gpu_speed_index', 1)
        hz = native.get('compositor_hz', 60)
        seconds = native.get('return_after_seconds', 600)
        sync = native.get('sync_frame_clock', False)
        assert hz in (60, 120) and seconds in (0, 600) and type(sync) is bool
        te = native.get('display_te', False)
        assert type(te) is bool
        expected = (f'cpus=8 small_cores_after=0 munch_touch=1 gpu=1 adreno_speed={speed} '
                    + ('fb_hz=120 ' if hz == 120 else '') + ('ca_vbl=1 glwait=1 ' if sync else '')
                    + ('munch_te=1 ' if te else '') + f'loader_return_after={seconds}')
        verbose = native.get('verbose_console', False)
        assert type(verbose) is bool
        if verbose: expected = '-v ' + expected
        assert report['verbose_console'] == verbose
        assert report['cmdline'] == expected and native['revision'] >= 4
        info = native['graphics_report']
        graphics_bytes = Path(info['path']).read_bytes()
        assert sha(graphics_bytes) == info['sha256']
        graphics = json.loads(graphics_bytes)
        assert graphics['driver'] == 'freedreno' and graphics['service'] == 'HakiosGPU'
        assert graphics['mesa_version'] == '25.0.7'
        assert sha(entries['ramdisk']) == graphics['helper']['sha256']
        assert entries['ramdisk'] == Path(graphics['helper']['path']).read_bytes()
    else:
        assert native['profile'] == 'munch-a650-diagnostic-22' and report['cmdline'] == native_cmdline
    assert entries['kernel'] == Path(native['kernel']['path']).read_bytes()
    assert sha(entries['kernel']) == native['kernel']['sha256']
    assert len(entries['kernel']) == native['kernel']['size']
    assert b'XNULDRST' in entries['kernel']
    if native.get('revision', 1) >= 3:
        assert native['loader_scm_reservation_bytes'] == 65536
        assert b'munch-scm-metadata' in entries['loader.efi']
        assert b'munch-scm-metadata' in entries['kernel']
        assert b'MUNCH SCM reserved page' in entries['loader.efi']
    if native.get('revision', 1) >= 4:
        assert native['scm_resume_timeout_ns'] == 5000000000
        assert native['scm_resume_max_calls'] == 1048576
        assert b'LOCAL SCM RESUME TIMEOUT' in entries['kernel']
        assert b'SCM INTERRUPTED count' in entries['kernel']
    source = Path(native['source_report']['path']).read_bytes()
    assert sha(source) == native['source_report']['sha256']
    provenance = json.loads(source)
    assert provenance['xnu_base'] == 'c54f35ca767986246321eb901baf8f5ff7923f6a'
    if native.get('display_te'):
        assert provenance['display_te_staging'] is True
        assert b'MUNCH TE' in entries['kernel']
    assert provenance['native_touch_bootarg'] == 'munch_touch=1'
    print('PASS: native A650 boot header, size, FD, DTB, payload, helper APFS, full kernel hash and source manifest')
    raise SystemExit(0)
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
