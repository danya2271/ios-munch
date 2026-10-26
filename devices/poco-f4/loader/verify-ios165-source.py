#!/usr/bin/env python3
"""Check selected IPSW members against the Apple-CDN ZIP directory and build manifest.

CRC verifies archive integrity, not an Apple code-signature trust chain.
"""
import argparse
import hashlib
import importlib.util
import json
import plistlib
import struct
import zlib
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('bounded_fetch', here / 'fetch-ios165-member.py')
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)
upstream = fetcher.upstream


def verify(root, artifacts, metadata_only=False):
    members = {
        'BuildManifest.plist': artifacts / 'ios165-BuildManifest.plist',
        'Firmware/all_flash/DeviceTree.d22ap.im4p': artifacts / 'ios165-device-tree.im4p',
    }
    if not metadata_only:
        members.update({'096-04088-077.dmg': root / 'rootfs.dmg',
                        '096-04666-078.dmg': root / 'cryptex-os.dmg',
                        '096-03536-083.dmg': root / 'cryptex-app.dmg'})
    manifest = plistlib.loads(members['BuildManifest.plist'].read_bytes())
    if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != ('16.5', '20F66'):
        raise ValueError('Unexpected iOS build')
    tail = fetcher.checked_fetch(upstream.SIZE - 65536, upstream.SIZE - 1).read()
    at = tail.rfind(b'PK\x06\x06')
    if at < 0:
        raise ValueError('Missing ZIP64 end directory')
    size, offset = struct.unpack_from('<QQ', tail, at + 40)
    if not 0 < size < 16 * 1024 * 1024:
        raise ValueError('Unexpected central-directory size')
    cd = fetcher.checked_fetch(offset, offset + size - 1).read()
    crcs = {}
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        n, e, c = struct.unpack_from('<HHH', cd, p + 28)
        name = cd[p + 46:p + 46 + n].decode()
        crcs[name] = struct.unpack_from('<I', cd, p + 16)[0]
        p += 46 + n + e + c
    sizes = {name: usize for name, method, csize, usize, offset in upstream.directory()}
    report = {'source': upstream.URL, 'product_version': '16.5', 'build': '20F66',
              'integrity': 'Apple-CDN ZIP member size and CRC32, plus local SHA256', 'members': {}}
    for member, path in members.items():
        sha = hashlib.sha256()
        crc, size = 0, 0
        with path.open('rb') as stream:
            while chunk := stream.read(8 * 1024 * 1024):
                sha.update(chunk)
                crc = zlib.crc32(chunk, crc)
                size += len(chunk)
        if size != sizes[member] or crc != crcs[member]:
            raise ValueError(f'IPSW member size/CRC mismatch: {member}')
        report['members'][member] = {'size': size, 'crc32': f'{crc:08x}', 'sha256': sha.hexdigest()}
        print(f'PASS {member}: {size} bytes, CRC32 {crc:08x}', flush=True)
    output = artifacts / ('ios165-source-metadata.json' if metadata_only else 'ios165-source-verified.json')
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wrote {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysimage', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--metadata-only', action='store_true')
    args = parser.parse_args()
    verify(args.sysimage, args.artifacts, args.metadata_only)
