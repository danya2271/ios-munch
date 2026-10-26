#!/usr/bin/env python3
"""Summarize bring-up data from a captured sysfs device-tree tar archive.

No archive members are extracted. Boot arguments are used only to identify the
selected panel; device identifiers and other boot arguments are not emitted.
"""
import argparse
import json
import re
import struct
import tarfile
from pathlib import PurePosixPath


def inspect(archive):
    nodes = {}
    with tarfile.open(archive) as tar:
        for member in tar:
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe archive path')
            if not member.isfile():
                continue
            if member.size > 4 * 1024 * 1024:
                raise ValueError('Unexpectedly large device tree property')
            name = '/' + '/'.join(path.parts[:-1])
            nodes.setdefault(name, {})[path.name] = tar.extractfile(member).read()

    nodes = dict(sorted(nodes.items()))

    def cells(raw):
        if len(raw) % 4:
            raise ValueError('Invalid cell property length')
        return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))

    def number(raw):
        return int.from_bytes(raw, 'big')

    def strings(raw):
        return raw.rstrip(b'\0').decode('ascii').split('\0')

    def scalar(props, key, default):
        return number(props[key]) if key in props else default

    def ranges(path):
        parent = nodes.get(str(PurePosixPath(path).parent), {})
        ac = scalar(parent, '#address-cells', 2)
        sc = scalar(parent, '#size-cells', 1)
        raw = cells(nodes[path].get('reg', b''))
        step = ac + sc
        if step == 0 or len(raw) % step:
            raise ValueError('Invalid reg property at ' + path)
        result = []
        for offset in range(0, len(raw), step):
            def combine(values):
                value = 0
                for cell in values:
                    value = (value << 32) | cell
                return value
            base = combine(raw[offset:offset + ac])
            size = combine(raw[offset + ac:offset + step])
            result.append({'base': hex(base), 'size': hex(size),
                           'end_exclusive': hex(base + size)})
        return result

    handles = {number(props['phandle']): path for path, props in nodes.items()
               if 'phandle' in props}
    result = {'schema_version': 1, 'status': 'observed; not boot validated',
              'model': strings(nodes['/']['model'])[0]}
    result['memory'] = [entry for path, props in nodes.items()
                        if props.get('device_type') == b'memory\0'
                        for entry in ranges(path)]
    result['reserved_memory'] = [
        {'path': path, 'ranges': ranges(path),
         'dynamic_size': hex(number(props['size'])) if 'size' in props else None,
         'no_map': 'no-map' in props, 'reusable': 'reusable' in props}
        for path, props in nodes.items()
        if str(PurePosixPath(path).parent) == '/reserved-memory']
    result['cpus'] = [
        {'path': path, 'mpidr': hex(number(props['reg'])),
         'capacity_dmips_mhz': scalar(props, 'capacity-dmips-mhz', None)}
        for path, props in nodes.items() if props.get('device_type') == b'cpu\0']
    result['psci'] = {key: strings(value) for key, value in nodes['/psci'].items()
                      if key in ('compatible', 'method')}
    for path, props in nodes.items():
        compatible = props.get('compatible', b'').split(b'\0')
        if b'arm,gic-v3' in compatible:
            result['gic'] = {'path': path, 'ranges': ranges(path),
                             'redistributor_stride': hex(number(props['redistributor-stride']))}
        if b'arm,armv8-timer' in compatible:
            result['timer'] = {'clock_hz': scalar(props, 'clock-frequency', None),
                               'interrupt_cells': cells(props['interrupts'])}
        if b'focaltech,fts' in compatible and props.get('status', b'okay\0') in (b'ok\0', b'okay\0'):
            touch = {'path': path, 'bus': str(PurePosixPath(path).parent),
                     'spi_max_hz': scalar(props, 'spi-max-frequency', None),
                     'display_coords_raw': cells(props['focaltech,display-coords']),
                     'max_touches': scalar(props, 'focaltech,max-touch-number', None)}
            for key in ('focaltech,reset-gpio', 'focaltech,irq-gpio'):
                values = cells(props[key])
                touch[key] = {'controller': handles[values[0]], 'specifier': values[1:]}
            for key in ('vdd-supply', 'vcc_i2c-supply'):
                supply_path = handles[number(props[key])]
                supply = nodes[supply_path]
                touch[key] = {'path': supply_path,
                              'name': strings(supply['regulator-name'])[0],
                              'min_uv': scalar(supply, 'regulator-min-microvolt', None),
                              'max_uv': scalar(supply, 'regulator-max-microvolt', None)}
            result['touch'] = touch
    bootargs = nodes.get('/chosen', {}).get('bootargs', b'').decode('ascii')
    panel = re.search(r'(?:^|\s)msm_drm\.dsi_display0=([^\s:]+)', bootargs)
    result['selected_panel'] = panel[1] if panel else None
    result['limitations'] = [
        'Android DT does not describe all resident UEFI allocations.',
        'Reserved splash memory does not establish the active scanout address or stride.',
        'Payload and FD addresses require the matching XBL configuration and EFI memory map.',
        'Touch protocol and coordinate scaling require matching driver source.']
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', help='device-tree.tar from the device collector')
    args = parser.parse_args()
    print(json.dumps(inspect(args.archive), indent=2))
