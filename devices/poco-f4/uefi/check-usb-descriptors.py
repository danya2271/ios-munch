#!/usr/bin/env python3
"""Validate numeric USB descriptor initializers before building the probe.

Checks lengths and fields relevant to BOS enumeration, including the SSP
attribute count and reserved bytes that regressed in the first probe.
"""
import re
import struct
from pathlib import Path


def numeric_array(source, name):
    match = re.search(r'\b' + re.escape(name) + r'\[(\d*)\]\s*=\s*\{([^}]+)\}', source)
    if not match:
        raise ValueError('Missing numeric descriptor: ' + name)
    data = bytes(int(value.strip(), 0) for value in match[2].split(',') if value.strip())
    if match[1] and int(match[1]) != len(data):
        raise ValueError('Implicit initializer padding or excess bytes in ' + name)
    return data


def validate(source):
    bos = numeric_array(source, 'probe_bos')
    if bos[:2] != bytes([5, 15]) or struct.unpack_from('<H', bos, 2)[0] != len(bos):
        raise ValueError('Invalid BOS header or total length')
    offset, capabilities = 5, 0
    while offset < len(bos):
        length = bos[offset]
        if length < 3 or offset + length > len(bos) or bos[offset + 1] != 16:
            raise ValueError('Invalid BOS capability boundary')
        cap = bos[offset:offset + length]
        if cap[2] == 2 and length != 7:
            raise ValueError('Invalid USB 2 extension length')
        if cap[2] == 3 and length != 10:
            raise ValueError('Invalid SuperSpeed capability length')
        if cap[2] == 10:
            if length < 16 or cap[3] or cap[10:12] != b'\0\0':
                raise ValueError('Invalid SSP reserved fields')
            attributes = struct.unpack_from('<I', cap, 4)[0]
            count = (attributes & 31) + 1
            if length != 12 + 4 * count:
                raise ValueError('SSP sublink attribute count does not match length')
        offset += length
        capabilities += 1
    if offset != len(bos) or capabilities != bos[4]:
        raise ValueError('BOS capability count mismatch')
    for name in ('probe_config', 'probe_ss_config'):
        data = numeric_array(source, name)
        if data[:2] != bytes([9, 2]) or struct.unpack_from('<H', data, 2)[0] != len(data):
            raise ValueError('Invalid configuration descriptor: ' + name)
        offset = 0
        while offset < len(data):
            length = data[offset]
            if length < 2 or offset + length > len(data):
                raise ValueError('Invalid configuration boundary: ' + name)
            offset += length
    return bos


if __name__ == '__main__':
    validate(Path(__file__).with_name('usb-probe.inc').read_text())
    print('USB BOS and configuration descriptor checks passed.')
