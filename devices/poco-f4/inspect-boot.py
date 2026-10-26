#!/usr/bin/env python3
"""Inspect a local F4 active-slot image set without emitting kernel command lines."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(directory, slot):
    paths = {name: directory / f'{name}_{slot}.img'
             for name in ('boot', 'vendor_boot', 'dtbo', 'xbl')}
    result = {'schema_version': 1, 'slot': slot, 'images': {}}
    for name, path in paths.items():
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        result['images'][name] = {'size': path.stat().st_size, 'sha256': digest}
    boot = paths['boot'].read_bytes()
    if boot[:8] != b'ANDROID!' or struct.unpack_from('<I', boot, 40)[0] != 3:
        raise ValueError('Expected Android boot header v3')
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', boot, 8)
    if header_size != 1580 or 4096 + kernel_size > len(boot):
        raise ValueError('Invalid boot v3 sizes')
    result['boot'] = {'header_version': 3, 'header_size': header_size,
                      'page_size': 4096, 'kernel_size': kernel_size,
                      'ramdisk_size': ramdisk_size, 'os_version_raw': hex(os_version),
                      'kernel_arm64_image_magic': boot[4096 + 56:4096 + 60] == b'ARMd'}
    vendor = paths['vendor_boot'].read_bytes()
    if vendor[:8] != b'VNDRBOOT':
        raise ValueError('Invalid vendor_boot magic')
    version, page, kernel_address, ramdisk_address, vendor_ramdisk_size = struct.unpack_from('<5I', vendor, 8)
    if version != 3 or page != 4096:
        raise ValueError('Expected vendor_boot header v3 with 4096-byte pages')
    vendor_header_size, dtb_size, dtb_address = struct.unpack_from('<IIQ', vendor, 2096)
    align = lambda value: (value + page - 1) // page * page
    dtb_offset = align(vendor_header_size) + align(vendor_ramdisk_size)
    if vendor_header_size != 2112 or dtb_offset + dtb_size > len(vendor):
        raise ValueError('Invalid vendor_boot sizes')
    if vendor[dtb_offset:dtb_offset + 4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('Vendor DTB not found at calculated offset')
    result['vendor_boot'] = {'header_version': version, 'page_size': page,
                             'header_size': vendor_header_size,
                             'vendor_ramdisk_size': vendor_ramdisk_size,
                             'dtb_offset': dtb_offset, 'dtb_size': dtb_size,
                             'kernel_address_raw': hex(kernel_address),
                             'ramdisk_address_raw': hex(ramdisk_address),
                             'dtb_address_raw': hex(dtb_address)}
    dtbo = paths['dtbo'].read_bytes()
    magic, total, header, entry_size, count, entries_offset, _, version = struct.unpack_from('>8I', dtbo)
    if magic != 0xd7b7ab1e or header < 32 or entry_size < 32 or total > len(dtbo):
        raise ValueError('Invalid DTBO table')
    if entries_offset + count * entry_size > total:
        raise ValueError('DTBO table exceeds declared image size')
    entries = []
    for index in range(count):
        size, offset, board_id, revision = struct.unpack_from('>4I', dtbo, entries_offset + index * entry_size)
        if offset + size > total or dtbo[offset:offset + 4] != b'\xd0\x0d\xfe\xed':
            raise ValueError('Invalid DTBO entry')
        entries.append({'size': size, 'offset': offset, 'id': board_id, 'revision': revision})
    result['dtbo'] = {'version': version, 'total_size': total, 'entries': entries}
    result['limitations'] = ['Installed images may include previous root or ios-sweet modifications.',
                             'Header v3 on Android does not prove which temporary UEFI image formats ABL accepts.']
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--slot', choices=['a', 'b'], required=True)
    args = parser.parse_args()
    print(json.dumps(inspect(args.directory, args.slot), indent=2))
