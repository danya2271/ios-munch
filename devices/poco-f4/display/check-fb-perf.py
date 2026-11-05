#!/usr/bin/env python3
"""Compile and check the framebuffer performance log formatter."""
import os
import subprocess
import tempfile
from pathlib import Path


root = Path(__file__).resolve().parents[3]
source = (root / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
start = source.index('\t\tchar liveLine[256];', source.index('void\nIOMobileFramebuffer::show('))
end = source.index('\n\t\tloader_log_line(liveLine);', start)
formatter = source[start:end]
harness = f'''#include <stdint.h>
#include <stdio.h>
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_USEC 1000ULL
int main(void) {{
\tuint32_t fStatFrames = 44, fStatOnBeat = 5, fStatHeld = 3, fStatLost = 0;
\tuint32_t fStatSwapsLate = 11, fStatGPUBudgetMisses = 7;
\tuint64_t nanos = 5000000000ULL, drawing = 13586ULL * 44000ULL;
\tuint64_t waiting = 411ULL * 44000ULL, beat = 7352ULL * 5000ULL;
{formatter}
\tputs(liveLine);
}}
'''

with tempfile.TemporaryDirectory(prefix='munch-fb-perf-') as directory:
    work = Path(directory)
    test = work / 'check.cpp'
    binary = work / 'check'
    test.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wformat=2', str(test), '-o', str(binary)], check=True)
    output = subprocess.check_output([str(binary)], text=True)

assert output.strip() == (
    'MUNCH FB PERF: frames=44 fps=8 draw_us=13586 gpu_wait_us=411 '
    'beat_wait_us=7352 beat_wait_frames=5 held=3 lost=0 late=11 gpu_budget_misses=7'
), output
print('Framebuffer performance fields and printf types verified')
