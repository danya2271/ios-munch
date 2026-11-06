#!/usr/bin/env python3
"""Test native mode MMIO, DMA, fault boundaries and local Linux reference programs."""
import argparse
import ast
import os
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux-tree', type=Path, required=True)
args = parser.parse_args()
root = here.parents[2]
runtime = (root / 'xnu/files/iokit/Kernel/munch-sde-xnu.h').read_text()
framebuffer = (root / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
initialize = runtime[runtime.index('munchSdeInitialize('):runtime.index('munchSdeReleaseUnused(')]
assert 'munch_mode_configure(' not in initialize
submit = runtime[runtime.index('munchSdeSubmitFrame('):runtime.index('munchSdeCompleteFrame(')]
complete = runtime[runtime.index('munchSdeCompleteFrame('):runtime.index('munchSdeEndFrame(')]
assert submit.index('munchSdeGuard(') < submit.index('munch_sde_pause_refresh(')
assert 'munch_sde_check_buffers(' not in submit
assert submit.index('munch_sde_pause_refresh(') < submit.index('munch_mode_adopt(') < submit.index('munch_sde_submit(')
assert 'munch_mode_adopt(&state->mode, state->nativeRefreshHz)' in submit
assert complete.index('munchSdePollFrame(') < complete.index('munch_mode_light(')
power = framebuffer[framebuffer.index('IOMobileFramebuffer::setDisplayPower('):
                    framebuffer.index('IOMobileFramebuffer::changeRefresh(')]
native_on = power[power.index('if (fSDERequested && fSDE->mode.configured) {'):
                   power.index('\n\t\t} else {', power.index('if (on) {'))]
native_off_branch = power[power.index(
    'if (fSDERequested && fSDE->nativeTakeover && fSDE->mode.configured) {'):]
native_off_branch = native_off_branch[:native_off_branch.index('\n\t\t} else {')]
assert 'munch_mode_light(' in native_on
assert 'sendDcsCommand(0x11' not in native_on
assert 'munch_mode_panel_on(' not in native_on
assert 'munch_mode_set_brightness(&fSDE->mode, 0)' in native_off_branch
assert 'sendDcsCommand(' not in native_off_branch
assert 'munchSdeSubmitFrame(state)' in runtime[runtime.index('munchSdeEndFrame('):runtime.index('munchSdeSampleVBlank(')]
assert 'ml_io_map(0x0af00000, 0x4000)' in initialize
mode = (root / 'xnu/files/iokit/Kernel/munch-sde-mode.h').read_text()
for phase in ('native-inherited', 'native-panel-sleep', 'native-phy', 'native-clocks',
              'native-host', 'native-dsc', 'native-route', 'native-panel-init', 'native-tearcheck'):
    assert phase in mode
assert mode.index('munch_mode_adopt(') < mode.index('munch_mode_configure(')
switch = mode[mode.index('munch_mode_switch_refresh('):mode.index('munch_mode_byte_sequence(')]
assert switch.index('munch_mode_program_dsi_timing_shadow(') < switch.index('munch_mode_command_sequence(')
assert switch.index('munch_mode_command_sequence(') < switch.index('munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, intf_timing')
assert switch.index('munch_mode_program(mode, MUNCH_MODE_MDP, 0x6b800, tear') < switch.index('0x1e8, 0')
assert mode.index('native-panel-sleep') < mode.index('native-phy') < mode.index('native-clocks')
assert mode.index('native-clocks') < mode.index('native-host') < mode.index('native-dsc')
assert mode.index('native-dsc') < mode.index('native-route') < mode.index('native-panel-init')
assert mode.index('native-panel-init') < mode.index('native-tearcheck')
assert 'munch_mode_command_sequence(mode, munch_l11r_60hz' not in mode[mode.index('munch_mode_configure('):mode.index('munch_mode_connect_te(')]
return_code = (root / 'xnu/files/osfmk/arm64/loader_return.c').read_text().split('loader_return(void)\n{', 1)[1]
assert return_code.index('munch_sde_native_owned()') < return_code.index('psci_smp_stop_others()')
stage_code = ast.parse((here.parent / 'gpu/stage-kernel.py').read_text())
stage_function = next(node for node in stage_code.body if isinstance(node, ast.FunctionDef) and node.name == 'stage_native_return')
namespace = {}
exec(compile(ast.Module(body=[stage_function], type_ignores=[]), '<native-staging-test>', 'exec'), namespace)
stage = namespace['stage_native_return']
original = 'extern int munch_sde_quiesce(void);\nvoid\nloader_return(void)\n{\n\tpsci_smp_stop_others();\n\tuser_owned_function();\n}\n'
staged = stage(original)
assert stage(staged) == staged and 'user_owned_function();' in staged
assert staged.index('if (munch_sde_native_owned())') < staged.index('psci_smp_stop_others()')
for corrupt in (staged.replace('before CPU shutdown', 'elsewhere'), staged.replace('extern int munch_sde_native_owned(void);', ''),
                staged.replace('loader_return(void)\n{\n', 'loader_return(void)\n{\n\tpsci_smp_stop_others();\n')):
    try:
        stage(corrupt)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed native return hook was silently accepted')
with tempfile.TemporaryDirectory(prefix='munch-native-reference-check-') as directory:
    generated = Path(directory) / 'munch-sde-mode-data.h'
    with generated.open('w') as output:
        subprocess.run(['python3', str(here / 'generate-sde-mode.py'), '--linux-tree', str(args.linux_tree)],
                       check=True, stdout=output)
    expected = generated.read_text()
    actual = (here.parents[2] / 'xnu/files/iokit/Kernel/munch-sde-mode-data.h').read_text()
    assert actual == expected
    print('Native PLL/PHY/DSC and live-panel command reference: PASS')
with tempfile.TemporaryDirectory(prefix='munch-native-test-') as directory:
    binary = Path(directory) / 'check-mode'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(here / 'check-sde-mode.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
