#!/usr/bin/env python3
"""Extract local XBL modules by GUID into a Mu-Silicium munch binary directory.

Requires uefi_firmware==1.16. Preserves vendor executable bytes without patching.
The output directory must be outside the tracked source tree.
"""
import argparse
import hashlib
import json
import re
import uuid
from pathlib import Path

from uefi_firmware.uefi import FirmwareFile, FirmwareFileSystemSection, FirmwareVolume


def sections(obj):
    result = []
    for child in getattr(obj, 'objects', []):
        if not hasattr(child, '__dict__'):
            continue
        if isinstance(child, FirmwareFileSystemSection) and child.type in (0x10, 0x13, 0x15, 0x19):
            result.append(child)
        else:
            result.extend(sections(child))
    return result


def extract(xbl, templates, output):
    data = xbl.read_bytes()
    start = data.index(b'_FVH') - 40
    if start < 0:
        raise ValueError('Invalid firmware volume offset')
    volume = FirmwareVolume(data[start:])
    if not volume.process():
        raise ValueError('Cannot parse XBL firmware volume')
    files = {}
    file_types = {}

    def walk(obj):
        if isinstance(obj, FirmwareFile):
            guid = str(uuid.UUID(bytes_le=obj.guid))
            if guid == 'ffffffff-ffff-ffff-ffff-ffffffffffff':
                return  # FFS padding, not a module.
            if guid in files:
                raise ValueError('Duplicate firmware GUID: ' + guid)
            files[guid] = sections(obj)
            file_types[guid] = obj.type
        for child in getattr(obj, 'objects', []):
            if hasattr(child, '__dict__'):
                walk(child)

    walk(volume)
    plan = {}
    manifest = {'xbl_sha256': hashlib.sha256(data).hexdigest(), 'modules': []}
    for inf in sorted(templates.rglob('*.inf')):
        text = inf.read_text()
        match = re.search(r'FILE_GUID\s*=\s*([\w-]+)', text)
        if not match:
            continue
        guid = match[1].lower()
        if guid not in files:
            raise ValueError('Missing firmware GUID: ' + guid)
        # The upstream template names a patched executable. This extractor keeps
        # the original bytes and names it accordingly; no offset patch is applied.
        text = text.replace('.patched.efi', '.efi')
        relative = inf.relative_to(templates)
        plan[relative] = text.encode()
        entry = {'guid': guid, 'template': str(relative), 'sections': []}
        for kind, filename in re.findall(r'^\s*(PE32|DXE_DEPEX|RAW)\|([^|\r\n]+)', text, re.M):
            leaf = Path(filename.strip())
            if leaf.is_absolute() or len(leaf.parts) != 1:
                raise ValueError('Unsafe template filename')
            code = {'PE32': 0x10, 'DXE_DEPEX': 0x13, 'RAW': 0x19}[kind]
            matches = [section for section in files[guid] if section.type == code]
            if len(matches) != 1:
                raise ValueError(f'{guid}: expected one {kind}, got {len(matches)}')
            blob = matches[0].data
            destination = relative.parent / leaf
            if destination in plan and plan[destination] != blob:
                raise ValueError('Conflicting output: ' + str(destination))
            plan[destination] = blob
            entry['sections'].append({'file': str(destination), 'size': len(blob),
                                      'sha256': hashlib.sha256(blob).hexdigest()})
        manifest['modules'].append(entry)
    if not manifest['modules']:
        raise ValueError('No module templates found')
    for guid, items in files.items():
        if file_types[guid] != 0x02:  # FFS freeform resources only.
            continue
        name = next((s.name for s in items if s.type == 0x15), None)
        raw = [s.data for s in items if s.type == 0x19]
        if not name or not raw:
            continue
        if Path(name).name != name or name in ('.', '..') or len(raw) != 1:
            raise ValueError('Unsafe or ambiguous raw resource name')
        destination = Path('RawFiles') / name
        if destination in plan and plan[destination] != raw[0]:
            raise ValueError('Conflicting raw resource')
        plan[destination] = raw[0]
    output.mkdir(parents=True, exist_ok=False)
    for relative, blob in plan.items():
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Extracted {len(manifest["modules"])} modules to {output}; no executable patches applied.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xbl', type=Path)
    parser.add_argument('--templates', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    extract(args.xbl, args.templates, args.output)
