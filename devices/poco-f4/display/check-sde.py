#!/usr/bin/env python3
"""Run the portable SDE/SMMU backend against mocked MMIO under ASan/UBSan."""
import os
import subprocess
import tempfile
from pathlib import Path


with tempfile.TemporaryDirectory(prefix='munch-sde-check-') as directory:
    for name in ('check-sde', 'check-sde-refresh'):
        binary = Path(directory) / name
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-const-variable', '-fsanitize=address,undefined', '-g',
                        str(Path(__file__).with_name(name + '.c')), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
