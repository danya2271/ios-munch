#!/usr/bin/env python3
"""Stage the Munch A650, input, display, GIC, and QCA sources in prepared XNU."""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path


def stage_native_return(early):
    declaration = 'extern int munch_sde_native_owned(void);\n'
    hook = ('\tif (munch_sde_native_owned()) {\n'
            '\t\tprintf("MUNCH SDE: native mode owns the link; loader return refused before CPU shutdown\\n");\n'
            '\t\treturn;\n\t}\n')
    if 'munch_sde_native_owned' in early:
        if early.count(declaration) != 1 or early.count(hook) != 1 or early.count('munch_sde_native_owned') != 4:
            raise ValueError('Refusing to replace independently modified native return hook')
        if 'loader_return(void)\n{\n' + hook not in early:
            raise ValueError('Native return hook must precede CPU shutdown')
        return early
    declaration_marker = 'extern int munch_sde_quiesce(void);\n'
    function_marker = 'loader_return(void)\n{\n'
    if early.count(declaration_marker) != 1 or early.count(function_marker) != 1:
        raise ValueError('Unexpected native loader return sequence')
    return early.replace(declaration_marker, declaration_marker + declaration, 1).replace(
        function_marker, function_marker + hook, 1)


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('tree', type=Path)
p.add_argument('--touch-poll-ms', type=int, choices=(4, 16), default=16,
               help='Delay between native touch reads; 4 ms is experimental')
p.add_argument('--report', required=True, type=Path)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
sha = lambda b: hashlib.sha256(b).hexdigest()
previous_report = json.loads(a.report.read_text()) if a.report.is_file() else {}
known_staged = {
    'bsd/dev/hakios.input.c': 'd7fd79a65f78e614521a45e9b6fe4d49e38461c18148ebd2d277cb2a55726088',
    'bsd/dev/hakios.cpufreq.c': '6ef61704e86aeabda1687a8ebb4e1364086e8228d1d3eff0ac503078572c5751',
    'bsd/dev/hakios.lpm.c': 'ad2df86e5c696870105d3c741328c445537216481b436c05498dbf8864523fdd',
    'bsd/sys/hakios.input.h': '0171a7f2d5c1c2c711d0253550f345680909fbfffb531418ba038402c0b5dd4c',
    'osfmk/arm64/psci_smp.c': '66589d92f7dad754b780d88af3c60d60b7f50b675adeece34b691a4443cbfd47',
    'osfmk/arm64/psci_smp.h': 'a808ab5a44c6a55dae9754d4b65959bac27972715de8bdb3221c4197d17943fe',
    'bsd/dev/hakios.lx.c': '7ca560f380fc5ad9e6ae5fb1fac400a41fd703a5c26d7983ac8063abda203600',
    'bsd/dev/hakios.qmi.c': '5feb3ddc9c4244701e2f462e77500a732a3fe1f55d43127a2341ca9a55c1c0a1',
    'bsd/sys/hakios.lx.h': 'da95675e330be185291f2ecc9e23dc0db1a7c1f83a368438566ff7c6240bdcbf',
    'bsd/sys/hakios.qmi.h': 'bc1e9da6ad96eb93069d20774768c3a003e21e4eb60998920958d90fabd798f0',
    'bsd/sys/hakios.qrtr.h': 'd9658ba4ddbc941cf201edfadbfdb38248c4939641d394eb7fecb50be1180e66',
    'bsd/dev/hakios.wifi.c': '986ac197da8f2a1bce3546540826a2c9a73a4bfa42abb471289ca74953409c0e',
}


def stage_canonical(relative, report_key):
    source = root / 'xnu/files' / relative
    target = a.tree / relative
    contents = source.read_bytes()
    target.parent.mkdir(parents=True, exist_ok=True)
    if (target.exists() and target.read_bytes() != contents and
            sha(target.read_bytes()) not in (previous_report.get(report_key), known_staged.get(relative))):
        raise ValueError(f'Refusing to overwrite independently modified staged source: {relative}')
    target.write_bytes(contents)
    return sha(contents)


wifi_dev = ('hakios.wifi', 'hakios.lx', 'hakios.pcie', 'hakios.pcie.kona', 'hakios.pcie.kona.platform', 'hakios.pcie.kona.mmio', 'hakios.pcie.kona.sid', 'hakios.dma.pagetable', 'hakios.dma.space',
            'hakios.dma.endpoint', 'hakios.dma.cache', 'hakios.dma.memory', 'hakios.dma.arena',
            'hakios.msi.pool', 'hakios.msi', 'hakios.mhi', 'hakios.mhi.control', 'hakios.mhi.boot',
            'hakios.mhi.runtime', 'hakios.mhi.worker', 'hakios.ipcr', 'hakios.qmi', 'hakios.qrtr.wire',
            'hakios.qrtr.mhi', 'hakios.qca6390.diag', 'hakios.qca6390')
wifi_sys = ('hakios.lx', 'hakios.pcie', 'hakios.pcie.kona', 'hakios.pcie.kona.platform', 'hakios.pcie.kona.mmio', 'hakios.pcie.kona.sid', 'hakios.mmio', 'hakios.dma.pagetable', 'hakios.dma.space',
            'hakios.dma.endpoint', 'hakios.dma.memory', 'hakios.dma.arena', 'hakios.msi.pool',
            'hakios.msi', 'hakios.mhi', 'hakios.mhi.boot', 'hakios.mhi.runtime', 'hakios.mhi.worker',
            'hakios.ipcr', 'hakios.qmi', 'hakios.qrtr', 'hakios.qrtr.wire', 'hakios.qrtr.mhi', 'hakios.wire',
            'hakios.qca6390')
staged_wifi = {}
for name in wifi_dev:
    staged_wifi[f'dev_{name}'] = stage_canonical(f'bsd/dev/{name}.c', f'wifi_dev_{name}_sha256')
for name in wifi_sys:
    staged_wifi[f'sys_{name}'] = stage_canonical(f'bsd/sys/{name}.h', f'wifi_sys_{name}_sha256')
for name in ('gic_v3_spi.h', 'gic_v3_spi.inc'):
    staged_wifi[f'arm64_{name}'] = stage_canonical(f'osfmk/arm64/{name}', f'wifi_arm64_{name}_sha256')
staged_input = stage_canonical('bsd/dev/hakios.input.c', 'input_source_sha256')
staged_power = {
    relative: stage_canonical(relative, key)
    for relative, key in (
        ('bsd/dev/hakios.cpufreq.c', 'cpufreq_source_sha256'),
        ('bsd/dev/hakios.lpm.c', 'lpm_source_sha256'),
        ('bsd/sys/hakios.input.h', 'input_header_sha256'),
        ('osfmk/arm64/psci_smp.c', 'psci_source_sha256'),
        ('osfmk/arm64/psci_smp.h', 'psci_header_sha256'),
    )
}

cpu_target = a.tree / 'osfmk/arm64/cpu.c'
cpu_source = cpu_target.read_text()
if 'hakios_lpm_idle' not in cpu_source:
    idle_marker = '\n\tif (wfi) {'
    idle_start = cpu_source.index('cpu_idle(void)')
    idle_at = cpu_source.index(idle_marker, idle_start)
    cpu_source = (cpu_source[:idle_at] +
                  '\n\textern bool hakios_lpm_idle(uint64_t);\n'
                  '\tbool lpm_slept = wfi && hakios_lpm_idle(new_idle_timeout_ticks);\n' +
                  cpu_source[idle_at:])
    cpu_source = cpu_source.replace('\n\tif (wfi) {',
                                    '\n\tif (wfi && !lpm_slept) {', 1)
if 'hakios_lpm_cpu_wake' not in cpu_source:
    exit_marker = '\n\tassert(exception_stack_pointer() != 0);'
    exit_start = cpu_source.index('cpu_idle_exit(boolean_t from_reset)')
    exit_at = cpu_source.index(exit_marker, exit_start)
    cpu_source = (cpu_source[:exit_at] +
                  '\n\textern void hakios_lpm_cpu_wake(void);\n'
                  '\thakios_lpm_cpu_wake();\n' +
                  cpu_source[exit_at:])
cpu_target.write_text(cpu_source)

gic_target = a.tree / 'osfmk/arm64/gic_v3_irq.c'
gic_source = gic_target.read_text()
gic_resume = ('void\n'
              'gic_v3_irq_cpu_resume(void)\n'
              '{\n'
              '\tint cpu = cpu_number();\n'
              '\n'
              '\tif (cpu < 0 || cpu >= MAX_CPUS || gicr_base == 0) return;\n'
              '\tgicr_cpu_base[cpu] = 0;\n'
              '\tgic_active_iar[cpu] = 0;\n'
              '\tgic_v3_irq_cpu_init();\n'
              '}\n')
if 'gic_v3_irq_cpu_resume' not in gic_source:
    gic_marker = '\n#endif /* GENERIC_GIC_V3_IRQ */'
    if gic_source.count(gic_marker) != 1:
        raise ValueError('Cannot stage GIC redistributor reset-resume hook')
    gic_source = gic_source.replace(gic_marker, '\n' + gic_resume + gic_marker, 1)
gic_target.write_text(gic_source)

arm_init_target = a.tree / 'osfmk/arm/arm_init.c'
arm_init_source = arm_init_target.read_text()
if 'gic_v3_irq_cpu_resume' not in arm_init_source:
    marker = '\tfiq_context_init(FALSE);\n'
    if arm_init_source.count(marker) != 1:
        raise ValueError('Cannot stage GIC resume hook in arm_init.c')
    arm_init_source = arm_init_source.replace(
        marker,
        marker + '\n#if defined(GENERIC_GIC_V3_IRQ)\n'
        '\textern void gic_v3_irq_cpu_resume(void);\n'
        '\tgic_v3_irq_cpu_resume();\n'
        '#endif\n',
        1)
arm_init_target.write_text(arm_init_source)

mmio_paths = ('osfmk/arm64/sleh.c', 'osfmk/arm64/machine_routines_asm.s')
mmio_report_keys = ('mmio_sleh_sha256', 'mmio_machine_asm_sha256')
mmio_current = [a.tree / path for path in mmio_paths]
mmio_current_hashes = [sha(path.read_bytes()) for path in mmio_current]
mmio_previous_hashes = [previous_report.get(key) for key in mmio_report_keys]
patch = root / 'xnu/hakios-mmio.patch'
mmio_patch_hash = sha(patch.read_bytes())
if (mmio_current_hashes == mmio_previous_hashes and
        previous_report.get('mmio_patch_sha256') != mmio_patch_hash):
    for path in mmio_paths:
        original = subprocess.check_output(['git', '-C', str(a.tree), 'show', f'HEAD:{path}'])
        (a.tree / path).write_bytes(original)
    subprocess.run(['git', 'apply', '--whitespace=nowarn', str(patch)], cwd=a.tree, check=True)
elif mmio_current_hashes != mmio_previous_hashes:
    base_hashes = []
    for path in mmio_paths:
        original = subprocess.check_output(['git', '-C', str(a.tree), 'show', f'HEAD:{path}'])
        base_hashes.append(sha(original))
    if mmio_current_hashes == base_hashes:
        subprocess.run(['git', 'apply', '--whitespace=nowarn', str(patch)], cwd=a.tree, check=True)
    else:
        result = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=a.tree,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            raise ValueError('Refusing to patch independently modified ARM64 abort sources')
mmio_current_hashes = [sha(path.read_bytes()) for path in mmio_current]

files_target = a.tree / 'bsd/conf/files'
files = files_target.read_text()
if 'bsd/dev/hakios.lpm.c' not in files:
    marker = 'bsd/dev/hakios.cpufreq.c\tstandard\n'
    if files.count(marker) != 1:
        raise ValueError('Cannot register the Munch LPM source in bsd/conf/files')
    files_target.write_text(files.replace(marker, marker + 'bsd/dev/hakios.lpm.c\tstandard\n', 1))
    files = files_target.read_text()
for name in wifi_dev:
    path = f'bsd/dev/{name}.c'
    entry = f'{path}\tstandard\n'
    if not re.search(rf'^{re.escape(path)}\s+', files, re.MULTILINE):
        files += f'\n{entry}'
files_target.write_text(files)

original = (root / 'xnu/files/bsd/dev/hakios.adreno.c').read_bytes()
if sha(original) != 'ad65456275966b3e026880e5eb9161ad2c8868e23bfc8d9540e370061229a5ab':
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
    subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(here / 'adreno-dvfs.patch')], cwd=work, check=True)
    gpu = path.read_bytes()
existing = a.tree / 'bsd/dev/hakios.adreno.c'
old_a650 = '5d5c303f422f2e18badd91b1b46da7dae51fcd5417e14658a9c4bc9b15b59799'
existing_gpu = existing.read_bytes()
previously_staged = False
if a.report.is_file():
    previous_report = json.loads(a.report.read_text())
    previously_staged = (sha(existing_gpu) == previous_report.get('gpu_source_sha256') and
                         previous_report.get('gpu_patch_sha256') == sha((here / 'umi-a650.patch').read_bytes()))
if existing_gpu not in (original, gpu) and sha(existing_gpu) != old_a650 and not previously_staged:
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
                  '    if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {\n'
                  '        enabled = 0;\n'
                  '        if (!PE_parse_boot_argn("munch_touch", &enabled, sizeof(enabled)) || !enabled) return;\n'
                  '    }\n' + needle, 1)
touch = '/* Native munch build: enabled by touchscreen=1 (munch_touch=1 is retained for probes). */\n' + touch
if touch.count('IOSleep(16);') != 1:
    raise ValueError('Unexpected touch polling loop')
touch = touch.replace('IOSleep(16);', f'IOSleep({a.touch_poll_ms});')
early_path = a.tree / 'osfmk/arm64/loader_return.c'
loader_source = (root / 'xnu/files/osfmk/arm64/loader_return.c').read_text()
early = early_path.read_text()
loaded_canonical = early == loader_source
previous_early = previous_report.get('early_return_source_sha256')
if (early != loader_source and previous_early is not None and
        sha(early.encode()) == previous_early):
    early = loader_source
    loaded_canonical = True
if 'loader_log_copy(' not in early:
    if ('if (PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core))) {' not in early and
            'if (0) { /* munch current-core handoff: PE is not initialized here. */' not in early):
        raise ValueError('Refusing to replace independently modified loader_return.c')
    early = loader_source
old_query = 'if (PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core))) {'
new_query = 'if (0) { /* munch current-core handoff: PE is not initialized here. */'
if early.count(old_query) == 1:
    early = early.replace(old_query, new_query, 1)
elif early.count(new_query) != 1:
    raise ValueError('Unexpected early bootcpu query')
sde_declaration = 'extern int munch_sde_quiesce(void);\n'
sde_hook = ('\tif (!munch_sde_quiesce()) {\n'
            '\t\tprintf("MUNCH SDE: unsafe loader handoff refused; scanout memory remains pinned\\n");\n'
            '\t\treturn;\n\t}\n')
if 'munch_sde_quiesce' in early:
    if early.count(sde_declaration) != 1 or early.count(sde_hook) != 1 or early.count('munch_sde_quiesce') != 2:
        raise ValueError('Refusing to replace independently modified SDE quiesce hook')
else:
    declaration_marker = 'extern void hakios_adreno_quiesce(void);\n'
    hook_marker = '\tpsci_smp_stop_others();\n#endif\n'
    if early.count(declaration_marker) != 1 or early.count(hook_marker) != 1:
        raise ValueError('Unexpected loader quiesce sequence')
    early = early.replace(declaration_marker, declaration_marker + sde_declaration, 1)
    early = early.replace(hook_marker, hook_marker + sde_hook, 1)
if not loaded_canonical:
    early = stage_native_return(early)
early_path.write_text(early)
existing.write_bytes(gpu)

usb = (root / 'xnu/files/bsd/dev/hakios.usb.c').read_bytes()
usb_target = a.tree / 'bsd/dev/hakios.usb.c'
previous_usb = None
if a.report.is_file():
    previous_usb = json.loads(a.report.read_text()).get('usb_source_sha256')
if (usb_target.exists() and usb_target.read_bytes() != usb and
        sha(usb_target.read_bytes()) != previous_usb):
    raise ValueError('Refusing to overwrite independently modified Munch USB source')
usb_target.write_bytes(usb)
files_target = a.tree / 'bsd/conf/files'
files = files_target.read_text()
usb_entry = 'bsd/dev/hakios.usb.c\t\tstandard\n'
if usb_entry not in files:
    marker = 'bsd/dev/hakios.net.c\t\tstandard\n'
    if files.count(marker) != 1:
        raise ValueError('Cannot register the Munch USB source in bsd/conf/files')
    files_target.write_text(files.replace(marker, marker + usb_entry, 1))

buttons = (root / 'xnu/files/bsd/dev/hakios.buttons.c').read_bytes()
buttons_target = a.tree / 'bsd/dev/hakios.buttons.c'
previous_buttons_sha = None
if a.report.is_file():
    previous_buttons_sha = json.loads(a.report.read_text()).get('buttons_source_sha256')
old_buttons = {
    '13d98aa814c0edfcffa414e594011f25b7d579f2468b310d2e43f5cf57ba53ba',
    '63bd20a4ae9fc8f93f7cedecdfd05087fd2a4b50b21d554899baa8b0dc880197',
    'cdd15554746045282d19c283028bb2b16230515541898ace8a40f85f43f60080',
    'e2e3af9820415675d806965ff066a3f1b23c44b6ab4474ce1d0bfbef899433d6',
}
if (buttons_target.read_bytes() not in (buttons,) and sha(buttons_target.read_bytes()) not in old_buttons and
        sha(buttons_target.read_bytes()) != previous_buttons_sha):
    raise ValueError('Refusing to overwrite independently modified Munch button source')
buttons_target.write_bytes(buttons)

(a.tree / 'bsd/dev/hakios.goodix.c').write_text(touch)
header = (here.parent / 'touch/ft3658u-protocol.h').read_bytes()
(a.tree / 'bsd/dev/ft3658u-protocol.h').write_bytes(header)
gpu_cpp = (root / 'xnu/files/iokit/Kernel/hakios.gpu.cpp').read_bytes()
gpu_cpp_target = a.tree / 'iokit/Kernel/hakios.gpu.cpp'
old_gpu_cpp = 'fbadcedbd5dec62ac5ea73a83777135bff624ab105c68a5ac6aeb019256fc45b'
existing_gpu_cpp = gpu_cpp_target.read_bytes()
surface_header = (root / 'xnu/files/iokit/Kernel/MiniIOSurface.h').read_bytes()
surface_header_target = a.tree / 'iokit/Kernel/MiniIOSurface.h'
existing_surface_header = surface_header_target.read_bytes()
previously_staged_gpu_cpp = sha(existing_gpu_cpp) == previous_report.get('gpu_service_source_sha256')
previously_staged_surface_header = (sha(existing_surface_header) == previous_report.get('surface_header_sha256') or
                                    sha(existing_surface_header) == '0e0f0cf2f7ab8a0c6059c3cbe49312089747fcbc0bc32797da8926831f078d2b')
if (existing_gpu_cpp != gpu_cpp and sha(existing_gpu_cpp) != old_gpu_cpp and
        not previously_staged_gpu_cpp):
    raise ValueError('Refusing to overwrite independently modified HakiosGPU service')
old_surface_header = '2158f5512be4e939f995c7b9ab52f6df89eaa56756f4cc778ce3976c9fc82d16'
if (existing_surface_header != surface_header and sha(existing_surface_header) != old_surface_header and
        not previously_staged_surface_header):
    raise ValueError('Refusing to overwrite independently modified MiniIOSurface header')
gpu_cpp_target.write_bytes(gpu_cpp)
surface_header_target.write_bytes(surface_header)
surface_source = (root / 'xnu/files/iokit/Kernel/MiniIOSurface.cpp').read_bytes()
surface_source_target = a.tree / 'iokit/Kernel/MiniIOSurface.cpp'
existing_surface_source = surface_source_target.read_bytes()
old_surface_source = '88a59bb62df6d11a084b957154f6222747710f903b7475644074677c8347e3f7'
previously_staged_surface_source = sha(existing_surface_source) == previous_report.get('surface_source_sha256')
if (existing_surface_source != surface_source and sha(existing_surface_source) != old_surface_source and
        not previously_staged_surface_source):
    raise ValueError('Refusing to overwrite independently modified MiniIOSurface implementation')
surface_source_target.write_bytes(surface_source)
fb_base = (root / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_bytes()
fb_target = a.tree / 'iokit/Kernel/MiniIOMobileFramebuffer.cpp'
existing_fb = fb_target.read_bytes()
previously_staged_fb = sha(existing_fb) == previous_report.get('display_source_sha256')
known_te_staging_fb = sha(existing_fb) == 'ff3d012f911517db49e4385c675d365e9d79b4a0f49a4218a8beeff95f404ecc'
known_previous_fb = sha(existing_fb) == '93db7b90af073f891efd94eb85504b2c65fec44d2ba7ca73c23e0aeecdf99b1b'
known_sde_probe_base_fb = sha(existing_fb) == 'be6e30b09c8ee87a173a9935dbf02c03d731c7a2477041aae611c2e4862f70b0'
if (existing_fb != fb_base and
        sha(existing_fb) != 'c6aa9da0a649ae6adf784a00327a60b7bd21e10549c9094093301951971f6ebe' and
        not previously_staged_fb and not known_te_staging_fb and not known_previous_fb and not known_sde_probe_base_fb):
    raise ValueError('Refusing to overwrite independent framebuffer changes')
fb_target.write_bytes(fb_base)
display_header = (root / 'xnu/files/iokit/Kernel/munch-kona-display.h').read_bytes()
display_header_target = a.tree / 'iokit/Kernel/munch-kona-display.h'
existing_display_header = display_header_target.read_bytes() if display_header_target.exists() else display_header
previously_staged_display_header = sha(existing_display_header) == previous_report.get('display_header_sha256')
known_sde_probe_base_header = sha(existing_display_header) == 'e1ce6d89cce53c9bd3c61ee4e3dd3eaf1dfe553de66ec5d177d1bfd89fea4540'
if (existing_display_header != display_header and not previously_staged_display_header and not known_sde_probe_base_header):
    raise ValueError('Refusing to overwrite independent Kona display profile')
display_header_target.write_bytes(display_header)
display_core = (root / 'xnu/files/iokit/Kernel/munch-sde.h').read_bytes()
display_core_target = a.tree / 'iokit/Kernel/munch-sde.h'
if (display_core_target.exists() and display_core_target.read_bytes() != display_core and
        sha(display_core_target.read_bytes()) != previous_report.get('display_core_sha256')):
    raise ValueError('Refusing to overwrite independently modified SDE backend')
display_core_target.write_bytes(display_core)
display_runtime = (root / 'xnu/files/iokit/Kernel/munch-sde-xnu.h').read_bytes()
display_runtime_target = a.tree / 'iokit/Kernel/munch-sde-xnu.h'
if (display_runtime_target.exists() and display_runtime_target.read_bytes() != display_runtime and
        sha(display_runtime_target.read_bytes()) != previous_report.get('display_runtime_sha256')):
    raise ValueError('Refusing to overwrite independently modified SDE runtime')
display_runtime_target.write_bytes(display_runtime)
display_memory = (root / 'xnu/files/iokit/Kernel/munch-sde-memory.h').read_bytes()
display_memory_target = a.tree / 'iokit/Kernel/munch-sde-memory.h'
if (display_memory_target.exists() and display_memory_target.read_bytes() != display_memory and
        sha(display_memory_target.read_bytes()) != previous_report.get('display_memory_sha256')):
    raise ValueError('Refusing to overwrite independently modified SDE reservation contract')
display_memory_target.write_bytes(display_memory)
display_modes = {}
for name in ('mode', 'mode-data'):
    contents = (root / f'xnu/files/iokit/Kernel/munch-sde-{name}.h').read_bytes()
    target = a.tree / f'iokit/Kernel/munch-sde-{name}.h'
    key = f'display_{name.replace("-", "_")}_sha256'
    if target.exists() and target.read_bytes() != contents and sha(target.read_bytes()) != previous_report.get(key):
        raise ValueError('Refusing to overwrite independently modified native display code')
    target.write_bytes(contents)
    display_modes[key] = sha(contents)
report = {'xnu_base': base, 'umi_reference': '6b6fb2b0c5a1720496ec71e88c95d2a45a0046a4',
          'early_return_source_sha256': sha(early.encode()), 'gpu_source_sha256': sha(gpu),
          'buttons_source_sha256': sha(buttons),
          'usb_source_sha256': sha(usb),
          'input_source_sha256': staged_input,
          'gpu_service_source_sha256': sha(gpu_cpp),
          'surface_source_sha256': sha(surface_source),
          'surface_header_sha256': sha(surface_header),
          'touch_source_sha256': sha(touch.encode()),
          'touch_header_sha256': sha(header), 'gpu_patch_sha256': sha((here / 'umi-a650.patch').read_bytes()),
          'gpu_dvfs_patch_sha256': sha((here / 'adreno-dvfs.patch').read_bytes()),
          'native_touch_bootarg': 'touchscreen=1', 'legacy_touch_bootarg': 'munch_touch=1',
          'touch_poll_sleep_ms': a.touch_poll_ms, 'display_te_staging': False,
          'display_header_sha256': sha(display_header),
          'display_core_sha256': sha(display_core), 'display_sde_takeover': False,
          'display_runtime_sha256': sha(display_runtime), 'display_sde_backend_available': True,
          'display_memory_sha256': sha(display_memory),
          'display_sde_bootarg': 'munch_sde=1',
          'display_source_sha256': sha(fb_target.read_bytes()),
          'hardware_tested': False}
report.update(display_modes)
report.update({f'wifi_{key}_sha256': value for key, value in staged_wifi.items()})
report.update({
    'cpufreq_source_sha256': staged_power['bsd/dev/hakios.cpufreq.c'],
    'lpm_source_sha256': staged_power['bsd/dev/hakios.lpm.c'],
    'input_header_sha256': staged_power['bsd/sys/hakios.input.h'],
    'psci_source_sha256': staged_power['osfmk/arm64/psci_smp.c'],
    'psci_header_sha256': staged_power['osfmk/arm64/psci_smp.h'],
})
report['lpm_cpu_source_sha256'] = sha(cpu_target.read_bytes())
report['arm_init_source_sha256'] = sha(arm_init_target.read_bytes())
report['gic_resume_source_sha256'] = sha(gic_target.read_bytes())
report.update(dict(zip(mmio_report_keys, mmio_current_hashes)))
report['mmio_patch_sha256'] = mmio_patch_hash
a.report.parent.mkdir(parents=True, exist_ok=True)
a.report.write_text(json.dumps(report, indent=2) + '\n')
print('Staged A650 GPU DVFS, native munch touch, and GPU service; base prebuilt and installed APFS unchanged')
