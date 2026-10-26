#!/usr/bin/env python3
"""Test the prototype's actual event dispatch with host-only bus stubs."""
from pathlib import Path
import subprocess
import tempfile
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='munch-touch-events-') as directory:
    work = Path(directory)
    for name in ('check-xnu-events.c', 'ft3658u-protocol.h', 'xnu-ft3658u.c'):
        source = (here / name).read_text().replace('__asm__ volatile("dsb sy" ::: "memory");', '')
        (work / name).write_text(source)
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(work / 'check-xnu-events.c'), '-o', str(work / 'check')], check=True)
    subprocess.run([str(work / 'check')], check=True)
print('PASS: down/move/up, duplicate suppression, invalid count, absent contact, coordinate endpoints')
