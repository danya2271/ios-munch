#!/usr/bin/env python3
"""Exercise the actual ARM64 XNU runtime with mock IOKit/MMIO under user-mode QEMU."""
import subprocess
import sys
import tempfile
from pathlib import Path


here = Path(__file__).resolve().parent
source = (here.parents[2] / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
snapshot = source[source.index('static void\nmunchSdeReadSnapshot('):source.index('static void\nmunchSdeQualify(')]
constants = '\n'.join(line for line in source.splitlines()
                      if line.startswith(('#define MUNCH_DSI_', '#define MUNCH_DISPLAY_')))
with tempfile.TemporaryDirectory(prefix='munch-sde-runtime-') as directory:
    work = Path(directory)
    (work / 'sde-snapshot.inc').write_text(constants + '\n' + snapshot)
    binary = work / 'check-sde-runtime'
    subprocess.run(['aarch64-linux-gnu-g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-static', '-I' + str(work), str(here / 'check-sde-runtime.cpp'), '-o', str(binary)], check=True)
    if '--native-only' in sys.argv[1:]:
        subprocess.run(['qemu-aarch64', str(binary), '128'], check=True, timeout=30)
        for scenario in range(6):
            subprocess.run(['qemu-aarch64', str(binary), '--native', str(scenario)], check=True, timeout=30)
        raise SystemExit(0)
    for scenario in range(129):
        subprocess.run(['qemu-aarch64', str(binary), str(scenario)], check=True, timeout=30)
    for scenario in range(39):
        subprocess.run(['qemu-aarch64', str(binary), '--bypass', str(scenario)], check=True, timeout=30)
    for scenario in [*range(17), *range(21, 30), *range(33, 38), *range(39, 58)]:
        subprocess.run(['qemu-aarch64', str(binary), '--reserved', str(scenario)], check=True, timeout=30)
    for scenario in range(1, 7):
        subprocess.run(['qemu-aarch64', str(binary), '--pacing', str(scenario)], check=True, timeout=30)
    for scenario in range(4):
        subprocess.run(['qemu-aarch64', str(binary), '--panel', str(scenario)], check=True, timeout=30)
    for scenario in range(6):
        subprocess.run(['qemu-aarch64', str(binary), '--native', str(scenario)], check=True, timeout=30)
