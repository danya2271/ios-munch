#!/usr/bin/env python3
"""Summarize hardware facts from a flattened device tree without dumping IDs.

  inspect-munch-dtb.py DTB [--expect-munch]

Use this on a stock boot/vendor_boot DTB or a captured live FDT before
creating the Munch Apple Device Tree. This is static inspection, not proof that
the board or boot chain works.
"""
import argparse
import struct
import sys


FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


class FdtNode:
    def __init__(self, name, parent=None):
        self.name = name
        self.parent = parent
        self.children = []
        self.props = {}

    @property
    def path(self):
        if self.parent is None:
            return '/'
        parent_path = self.parent.path.rstrip('/')
        return parent_path + '/' + self.name


def be32(blob, offset):
    if offset < 0 or offset + 4 > len(blob):
        raise ValueError('truncated 32-bit field')
    return struct.unpack_from('>I', blob, offset)[0]


def cstring(blob, offset, end):
    if offset >= end:
        raise ValueError('string offset outside block')
    nul = blob.find(b'\0', offset, end)
    if nul < 0:
        raise ValueError('unterminated string')
    return blob[offset:nul].decode('utf-8', errors='replace'), nul + 1


def parse_fdt(blob):
    if len(blob) < 40 or be32(blob, 0) != FDT_MAGIC:
        raise ValueError('not a flattened device tree (bad magic/header)')

    (total_size, struct_off, strings_off, _reserve_off, version,
     _last_compatible, _boot_cpu, strings_size, struct_size) = struct.unpack_from('>9I', blob, 4)
    if total_size > len(blob) or total_size < 40:
        raise ValueError('invalid FDT total size')
    if version < 16:
        raise ValueError(f'unsupported FDT version {version}; expected version 16 or newer')
    struct_end = struct_off + struct_size
    strings_end = strings_off + strings_size
    if struct_off < 40 or struct_end > total_size or strings_off < 40 or strings_end > total_size:
        raise ValueError('FDT structure/string block lies outside the declared blob')

    root = None
    stack = []
    offset = struct_off
    while offset + 4 <= struct_end:
        token = be32(blob, offset)
        offset += 4
        if token == FDT_BEGIN_NODE:
            name, offset = cstring(blob, offset, struct_end)
            offset = (offset + 3) & ~3
            parent = stack[-1] if stack else None
            node = FdtNode(name, parent)
            if parent:
                parent.children.append(node)
            elif root is None:
                root = node
            else:
                raise ValueError('multiple root nodes')
            stack.append(node)
        elif token == FDT_END_NODE:
            if not stack:
                raise ValueError('unbalanced FDT_END_NODE')
            stack.pop()
        elif token == FDT_PROP:
            if not stack or offset + 8 > struct_end:
                raise ValueError('property outside a node or truncated property header')
            length = be32(blob, offset)
            name_offset = be32(blob, offset + 4)
            offset += 8
            if name_offset >= strings_size or offset + length > struct_end:
                raise ValueError('property name or value lies outside its block')
            prop_name, _ = cstring(blob, strings_off + name_offset, strings_end)
            stack[-1].props[prop_name] = blob[offset:offset + length]
            offset = (offset + length + 3) & ~3
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            if stack:
                raise ValueError('FDT ended before all nodes were closed')
            break
        else:
            raise ValueError(f'unknown FDT token {token} at offset 0x{offset - 4:x}')
    else:
        raise ValueError('FDT structure block has no FDT_END token')

    if root is None:
        raise ValueError('FDT has no root node')
    return root, total_size, version


def strings(prop):
    if prop is None:
        return []
    return [part.decode('utf-8', errors='replace') for part in prop.split(b'\0') if part]


def cell_words(prop):
    if prop is None or len(prop) % 4:
        raise ValueError('invalid cell array')
    return struct.unpack('>%dI' % (len(prop) // 4), prop) if prop else ()


def decode_group(words):
    if len(words) > 2:
        raise ValueError('addresses wider than 64 bits are unsupported')
    value = 0
    for word in words:
        value = (value << 32) | word
    return value


def cell_tuples(prop, widths):
    if any(width < 0 or width > 2 for width in widths):
        raise ValueError('cell widths must be between zero and two')
    words = cell_words(prop)
    tuple_width = sum(widths)
    if tuple_width == 0 or len(words) % tuple_width:
        raise ValueError('cell array has a partial tuple')
    values = []
    for offset in range(0, len(words), tuple_width):
        entry = []
        cursor = offset
        for width in widths:
            entry.append(decode_group(words[cursor:cursor + width]))
            cursor += width
        values.append(tuple(entry))
    return values


def cell_count(node, name, default):
    raw = node.props.get(name)
    if raw is None:
        return default
    values = cell_tuples(raw, (1,))
    return values[0][0] if values else default


def reg_entries(node):
    parent = node.parent
    if parent is None:
        return []
    addr_cells = cell_count(parent, '#address-cells', 2)
    size_cells = cell_count(parent, '#size-cells', 1)
    if addr_cells > 2 or size_cells > 2:
        return []
    raw = node.props.get('reg')
    if raw is None:
        return []
    return cell_tuples(raw, (addr_cells, size_cells))


def translate_address(node, address, size):
    bus = node.parent
    while bus is not None and bus.parent is not None:
        ranges = bus.props.get('ranges')
        if ranges is None:
            return None
        if not ranges:
            bus = bus.parent
            continue

        child_cells = cell_count(bus, '#address-cells', 2)
        parent_cells = cell_count(bus.parent, '#address-cells', 2)
        range_size_cells = cell_count(bus, '#size-cells', 1)
        tuple_cells = child_cells + parent_cells + range_size_cells
        values = cell_tuples(ranges, (child_cells, parent_cells, range_size_cells))
        for child, parent, length in values:
            if child <= address and address + size <= child + length:
                address = parent + address - child
                break
        else:
            return None
        bus = bus.parent
    return address


def walk(node):
    yield node
    for child in node.children:
        yield from walk(child)


def is_enabled(node):
    status = strings(node.props.get('status'))
    return not status or status[0] in ('okay', 'ok')


def describe_regs(node):
    output = []
    names = strings(node.props.get('reg-names'))
    for index, (address, size) in enumerate(reg_entries(node)):
        physical = translate_address(node, address, size)
        label = names[index] if index < len(names) else str(index)
        shown = f'{physical:#x}' if physical is not None else f'bus:{address:#x}'
        output.append(f'{label}={shown}+{size:#x}')
    return ', '.join(output)


def describe_reserved(node):
    entries = describe_regs(node)
    if entries:
        return entries

    size_prop = node.props.get('size')
    if size_prop is None:
        return ''

    parent = node.parent
    size_cells = cell_count(parent, '#size-cells', 1)
    size = decode_group(cell_words(size_prop)[-size_cells:])
    output = [f'dynamic-size={size:#x}']
    alignment_prop = node.props.get('alignment')
    if alignment_prop is not None:
        alignment = decode_group(cell_words(alignment_prop)[-size_cells:])
        output.append(f'alignment={alignment:#x}')

    ranges_prop = node.props.get('alloc-ranges')
    if ranges_prop is not None:
        address_cells = cell_count(parent, '#address-cells', 2)
        ranges = cell_tuples(ranges_prop, (address_cells, size_cells))
        shown_ranges = ','.join(f'{address:#x}+{length:#x}'
                                for address, length in ranges)
        output.append(f'alloc-ranges={shown_ranges}')

    flags = [name for name in ('no-map', 'reusable') if name in node.props]
    if flags:
        output.append(','.join(flags))
    return ', '.join(output)


def report(path, root, total_size, version, expect_munch):
    model = strings(root.props.get('model'))
    compatible = strings(root.props.get('compatible'))
    all_nodes = list(walk(root))
    cpus = [node for node in all_nodes
            if strings(node.props.get('device_type')) == ['cpu'] and is_enabled(node)]
    memory = [node for node in all_nodes
              if strings(node.props.get('device_type')) == ['memory']]
    reserved = [node for node in all_nodes if node.path.startswith('/reserved-memory/')]

    print(f'DTB: {path} (v{version}, {total_size} bytes)')
    print('model: ' + ('; '.join(model) if model else '(not supplied)'))
    print('compatible: ' + ('; '.join(compatible) if compatible else '(not supplied)'))
    print(f'CPU nodes: {len(cpus)}')
    identity_matches = any('munch' in value.lower() for value in model + compatible)
    if expect_munch and not identity_matches:
        print('ERROR: root identity does not contain "munch"; refusing to treat this as a Munch DTB')

    print('memory:')
    for node in memory:
        print(f'  {node.path}: {describe_regs(node) or "reg missing"}')
    print('reserved-memory:')
    for node in reserved:
        entries = describe_reserved(node)
        if entries:
            print(f'  {node.path}: {entries}')

    groups = {
        'interrupt controllers': {
            'name_prefixes': ('interrupt-controller@',),
            'name_exact': (),
            'compatible_prefixes': ('arm,gic-v3', 'arm,gic-400', 'qcom,msm-qgic'),
        },
        'timers': {
            'name_prefixes': ('timer@',),
            'name_exact': ('timer',),
            'compatible_prefixes': ('arm,armv8-timer', 'arm,armv7-timer'),
        },
        'UFS': {
            'name_prefixes': ('ufshc@', 'ufs@', 'ufsphy'),
            'name_exact': (),
            'compatible_prefixes': ('qcom,ufshc', 'qcom,ufs-phy'),
        },
        'display': {
            'name_prefixes': ('mdss@', 'qcom,mdss_mdp@', 'sde@', 'dsi@', 'dsi-phy@', 'dsi_phy@',
                              'display@', 'framebuffer@'),
            'name_exact': ('display', 'framebuffer'),
            'compatible_prefixes': ('qcom,sde-kms', 'qcom,mdss_mdp', 'qcom,dsi-panel',
                                    'simple-framebuffer'),
        },
        'touch': {
            'name_prefixes': ('touch@', 'goodix@', 'focaltech@'),
            'name_exact': ('touchscreen',),
            'compatible_prefixes': ('goodix,gt', 'focaltech,fts', 'focaltech,ft'),
        },
        'power/PMIC': {
            'name_prefixes': ('spmi@', 'qcom,spmi@', 'pmic@', 'rpmh@', 'rpmh_rsc@'),
            'name_exact': ('rpmh', 'rpmh-rsc'),
            'compatible_prefixes': ('qcom,spmi-pmic-arb', 'qcom,spmi-pmic', 'qcom,rpmh-rsc'),
        },
    }
    for title, selectors in groups.items():
        matches = []
        for node in all_nodes:
            if not is_enabled(node):
                continue
            name = node.name.lower()
            compatibles = [value.lower() for value in strings(node.props.get('compatible'))]
            if (name in selectors['name_exact'] or
                    any(name.startswith(prefix) for prefix in selectors['name_prefixes']) or
                    any(value.startswith(prefix) for value in compatibles
                        for prefix in selectors['compatible_prefixes'])):
                matches.append(node)
        print(title + ':')
        if not matches:
            print('  (no enabled matching nodes)')
        for node in matches[:25]:
            compatibles = ','.join(strings(node.props.get('compatible'))) or '(no compatible)'
            regs = describe_regs(node)
            suffix = f' [{regs}]' if regs else ''
            print(f'  {node.path}: {compatibles}{suffix}')
        if len(matches) > 25:
            print(f'  ... {len(matches) - 25} additional matching nodes omitted')
    return identity_matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', help='flattened device tree binary')
    parser.add_argument('--expect-munch', action='store_true',
                        help='fail if root model/compatible lacks the Munch codename')
    args = parser.parse_args()
    try:
        with open(args.dtb, 'rb') as source:
            root, total_size, version = parse_fdt(source.read())
        identity_matches = report(args.dtb, root, total_size, version, args.expect_munch)
        if args.expect_munch and not identity_matches:
            return 2
    except (OSError, ValueError, struct.error) as error:
        print(f'{args.dtb}: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
