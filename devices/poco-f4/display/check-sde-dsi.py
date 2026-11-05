#!/usr/bin/env python3
"""Run the actual DCS sender against W1C MMIO and compare the local Linux FIFO writes."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux-tree', type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (here.parents[2] / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
sender = function(source, 'IOReturn\nIOMobileFramebuffer::sendDcsCommand(')
sender = re.sub(r'regs\[(MUNCH_DSI_[A-Z_]+) / 4\]', r'dsiRegister(\1)', sender)
constants = '\n'.join(line for line in source.splitlines() if line.startswith('#define MUNCH_DSI_'))
linux = (args.linux_tree / 'techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c').read_text()
fifo = function(linux, 'void dsi_ctrl_hw_cmn_kickoff_fifo_command(')
reset = function(linux, 'void dsi_ctrl_hw_cmn_reset_cmd_fifo(')
with tempfile.TemporaryDirectory(prefix='munch-sde-dsi-') as directory:
    work = Path(directory)
    (work / 'sde-dsi-constants.inc').write_text(constants)
    (work / 'sde-dsi-sender.inc').write_text(sender)
    (work / 'sde-dsi-linux.inc').write_text(fifo + '\n' + reset)
    binary = work / 'check-sde-dsi'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I' + str(work),
                    str(here / 'check-sde-dsi.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
