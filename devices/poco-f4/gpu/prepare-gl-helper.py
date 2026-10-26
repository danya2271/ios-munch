#!/usr/bin/env python3
"""Compile the matching GL bridge and build a separate Mesa-enabled RAM helper."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--base', required=True, type=Path)
p.add_argument('--mesa', required=True, type=Path)
p.add_argument('--sdk', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
for field in ('base', 'mesa', 'sdk', 'output'):
    setattr(a, field, getattr(a, field).resolve())
here = Path(__file__).resolve().parent
root = here.parents[2]
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
if a.output.exists():
    raise ValueError('Output directory already exists; use a new build directory')
for name in ('libOSMesa.8.dylib', 'gl.xml', 'exported-gl.txt', 'built-from'):
    if not (a.mesa / name).is_file(): raise ValueError('Missing Mesa output: ' + name)
for name in ('sbin/init', 'lib/libhakios.dylib', 'fw/a650_gmu.bin', 'fw/a650_sqe.fw', 'fw/a650_zap.mdt', 'fw/a650_zap.b02'):
    if not (a.base / name).is_file(): raise ValueError('Missing base helper file: ' + name)
a.output.mkdir(parents=True)
work = a.output / 'obj'
work.mkdir()
userland = root / 'userland'
with (work / 'eagl_gen.h').open('wb') as stream:
    subprocess.run(['python3', str(userland / 'mkeagl.py'), str(userland / 'src/eagl_slots.txt'),
                    str(a.mesa / 'gl.xml'), str(a.mesa / 'exported-gl.txt'), str(userland / 'src/eagl.c')], stdout=stream, check=True)
cc = ['xnu-clang', '-arch', 'arm64', '-isysroot', str(a.sdk), '-O1']
bridge = work / 'libhakios.gl.dylib'
subprocess.run(cc + ['-dynamiclib', '-install_name', '/redwood/lib/libhakios.gl.dylib', '-I' + str(work),
                    '-framework', 'CoreFoundation', '-framework', 'IOKit', '-framework', 'IOSurface', '-lobjc',
                    str(a.mesa / 'libOSMesa.8.dylib'), '-o', str(bridge),
                    str(userland / 'src/eagl.c'), str(userland / 'src/gl_osmesa.c')], check=True)
subprocess.run(cc + ['-framework', 'CoreFoundation', '-framework', 'IOKit', str(a.mesa / 'libOSMesa.8.dylib'),
                    '-o', str(work / 'gl_test'), str(userland / 'src/gl_test.c')], check=True)
def dependencies(path, install_name):
    b = path.read_bytes()
    if b[:4] != b'\xcf\xfa\xed\xfe' or struct.unpack_from('<I', b, 4)[0] != 0x100000c:
        raise ValueError('Expected ARM64 Mach-O: ' + str(path))
    result, identity, offset = [], None, 32
    for _ in range(struct.unpack_from('<I', b, 16)[0]):
        command, size = struct.unpack_from('<II', b, offset)
        if command in (0xc, 0xd, 0x80000018, 0x8000001f):
            start = struct.unpack_from('<I', b, offset + 8)[0]
            label = b[offset + start:offset + size].split(b'\0', 1)[0].decode()
            if command == 0xd: identity = label
            else: result.append(label)
        offset += size
    if identity != install_name: raise ValueError('Unexpected dylib install name: ' + str(path))
    for dep in result:
        if not dep.startswith(('/usr/lib/', '/System/Library/', '/redwood/lib/')):
            raise ValueError('Host/build dependency in dylib: ' + dep)
    return result
mesa_deps = dependencies(a.mesa / 'libOSMesa.8.dylib', '/redwood/lib/libOSMesa.8.dylib')
bridge_deps = dependencies(bridge, '/redwood/lib/libhakios.gl.dylib')
if '/redwood/lib/libOSMesa.8.dylib' not in bridge_deps:
    raise ValueError('GL bridge does not link the staged Mesa library')
helper = a.output / 'root'
shutil.copytree(a.base, helper, symlinks=True)
shutil.copy2(a.mesa / 'libOSMesa.8.dylib', helper / 'lib')
shutil.copy2(bridge, helper / 'lib')
shutil.copy2(work / 'gl_test', helper / 'bin')
for dep in mesa_deps + bridge_deps:
    if dep.startswith('/redwood/') and not (helper / dep.removeprefix('/redwood/')).is_file():
        raise ValueError('Missing staged runtime dependency: ' + dep)
symbols = subprocess.check_output(['nm', '-gU', str(a.mesa / 'libOSMesa.8.dylib')], text=True)
if '_OSMesaCreateContextAttribs' not in symbols or '_glGetString' not in symbols:
    raise ValueError('Mesa lacks the required exported context/GL API')
# Mesa hides private driver symbols; check the linked GPU-service bridge too.
mesa_bytes = (a.mesa / 'libOSMesa.8.dylib').read_bytes()
if b'HakiosGPU' not in mesa_bytes or b'freedreno' not in mesa_bytes:
    raise ValueError('Mesa lacks the freedreno/HakiosGPU integration')
image = a.output / 'root.apfs'
subprocess.run(['python3', str(root / 'tools/apfs/mkapfs.py'), '-z', '--plain', 'fw', '-o', str(image), 'redwood=' + str(helper)], check=True)
subprocess.run(['bash', str(root / 'tools/apfs/check-image.sh'), str(image), 'redwood=' + str(helper)], check=True)
report = {'mesa_version': '25.0.7', 'driver': 'freedreno', 'service': 'HakiosGPU', 'hardware_tested': False,
          'helper': {'path': str(image), 'size': image.stat().st_size, 'sha256': sha(image)},
          'mesa': {'sha256': sha(a.mesa / 'libOSMesa.8.dylib'), 'dependencies': mesa_deps},
          'bridge': {'sha256': sha(bridge), 'dependencies': bridge_deps},
          'mesa_built_from': (a.mesa / 'built-from').read_text().strip()}
(a.output / 'graphics.json').write_text(json.dumps(report, indent=2) + '\n')
print('Built and checked GL-enabled helper:', image)
