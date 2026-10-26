#!/usr/bin/env python3
"""Stage the UMI A650 adaptation and native munch touch in a prepared XNU tree."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('tree', type=Path)
p.add_argument('--report', required=True, type=Path)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
sha = lambda b: hashlib.sha256(b).hexdigest()
original = (root / 'xnu/files/bsd/dev/hakios.adreno.c').read_bytes()
if sha(original) != '6dfa464cf9be005f6520257721ab468fc7c33189be11d57078d36dc5faa33e77':
    raise ValueError('Upstream GPU source differs from the reviewed port base')
base = subprocess.check_output(['git', '-C', str(a.tree), 'rev-parse', 'HEAD'], text=True).strip()
if base != 'c54f35ca767986246321eb901baf8f5ff7923f6a':
    raise ValueError('Expected prepared XNU 8796.121.2')
with tempfile.TemporaryDirectory(prefix='munch-native-stage-') as directory:
    work = Path(directory)
    path = work / 'bsd/dev/hakios.adreno.c'
    path.parent.mkdir(parents=True)
    path.write_bytes(original)
    subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(here / 'umi-a650.patch')], cwd=work, check=True)
    gpu = path.read_bytes()
existing = a.tree / 'bsd/dev/hakios.adreno.c'
if existing.read_bytes() not in (original, gpu):
    raise ValueError('Refusing to overwrite independently modified GPU source')
touch = (here.parent / 'touch/xnu-ft3658u.c').read_text()
touch = touch.replace('extern struct state munch_state;', 'static struct state munch_state;')
touch = touch.replace('__attribute__((section(".text.entry")))', '')
touch = touch.replace('void munch_touch_init(void)', 'void hakios_goodix_init(void)')
needle = '    void *thread = 0;'
if touch.count(needle) != 1:
    raise ValueError('Unexpected touch entry')
touch = touch.replace(needle, '    extern int PE_parse_boot_argn(const char *, void *, int);\n'
                      '    unsigned enabled = 0;\n'
                      '    if (!PE_parse_boot_argn("munch_touch", &enabled, sizeof(enabled)) || !enabled) return;\n' + needle, 1)
touch = '/* Native munch build: enabled only with munch_touch=1 and verified loader power handoff. */\n' + touch
existing.write_bytes(gpu)
(a.tree / 'bsd/dev/hakios.goodix.c').write_text(touch)
header = (here.parent / 'touch/ft3658u-protocol.h').read_bytes()
(a.tree / 'bsd/dev/ft3658u-protocol.h').write_bytes(header)
report = {'xnu_base': base, 'umi_reference': '6b6fb2b0c5a1720496ec71e88c95d2a45a0046a4',
          'gpu_source_sha256': sha(gpu), 'touch_source_sha256': sha(touch.encode()),
          'touch_header_sha256': sha(header), 'gpu_patch_sha256': sha((here / 'umi-a650.patch').read_bytes()),
          'native_touch_bootarg': 'munch_touch=1', 'hardware_tested': False}
a.report.parent.mkdir(parents=True, exist_ok=True)
a.report.write_text(json.dumps(report, indent=2) + '\n')
print('Staged A650 GPU and native munch touch; base prebuilt and installed APFS unchanged')
