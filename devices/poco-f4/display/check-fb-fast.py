#!/usr/bin/env python3
"""Compile and verify the optimized 1125x2436 -> 1080x2400 nearest path."""
import os
import subprocess
import tempfile
from pathlib import Path


root = Path(__file__).resolve().parents[3]
source = (root / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
start = source.index('static bool\nmunchFramebufferDownscaleRows(')
end = source.index('\nstatic bool\nmunchSdeReplacesFrame(', start)
helper = source[start:end]
harness = f'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
{helper}
int main(void) {{
    const uint32_t source_stride = 1125 * 4 + 32;
    const uint32_t destination_stride = 1080 * 4 + 28;
    uint8_t *source = new uint8_t[(size_t)source_stride * 2436];
    uint8_t *destination = new uint8_t[(size_t)destination_stride * 2400];
    memset(destination, 0xcd, (size_t)destination_stride * 2400);
    for (uint32_t y = 0; y < 2436; y++) {{
        uint32_t *row = (uint32_t *)(source + (size_t)y * source_stride);
        for (uint32_t x = 0; x < 1125; x++) row[x] = y * 100000 + x;
    }}
    assert(munchFramebufferDownscaleRows(source, source_stride, destination, destination_stride, 0, 2400));
    for (uint32_t y = 0; y < 2400; y++) {{
        const uint32_t source_y = (uint64_t)y * 2436 / 2400;
        const uint32_t *input = (const uint32_t *)(source + (size_t)source_y * source_stride);
        const uint32_t *output = (const uint32_t *)(destination + (size_t)y * destination_stride);
        for (uint32_t x = 0; x < 1080; x++)
            assert(output[x] == input[(uint64_t)x * 1125 / 1080]);
        for (uint32_t byte = 1080 * 4; byte < destination_stride; byte++)
            assert(destination[(size_t)y * destination_stride + byte] == 0xcd);
    }}
    assert(!munchFramebufferDownscaleRows(NULL, source_stride, destination, destination_stride, 0, 1));
    puts("Optimized framebuffer nearest path matches exact mapping and preserves row padding");
}}
'''

with tempfile.TemporaryDirectory(prefix='munch-fb-fast-') as directory:
    work = Path(directory)
    test = work / 'check.cpp'
    binary = work / 'check'
    test.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
