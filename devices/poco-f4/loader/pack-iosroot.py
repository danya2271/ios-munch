#!/usr/bin/env python3
"""Pack a source-built Munch XNU kernel with the verified iosroot handoff."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


FD_BASE = 0x9FC00000
FD_SIZE = 0x300000
PAYLOAD_BASE = 0xF0000000
PAYLOAD_MAX = 0x03FFF000
LPM_PATH = '/soc/qcom,lpm-levels'
LPM_STRING_PROPERTIES = {'compatible', 'label'}
LPM_CELL_PROPERTIES = {
    'qcom,clstr-tmr-add', 'qcom,psci-mode-shift', 'qcom,psci-mode-mask',
    'qcom,psci-mode', 'qcom,entry-latency-us', 'qcom,exit-latency-us',
    'qcom,min-residency-us', 'qcom,min-child-idx', 'qcom,tmr-add',
    'qcom,psci-cpu-mode',
}
LPM_FLAG_PROPERTIES = {'qcom,is-reset', 'qcom,use-broadcast-timer', 'qcom,notify-rpm'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_crc32(path):
    value = 0
    with path.open('rb') as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            value = zlib.crc32(chunk, value)
    return f'{value & 0xffffffff:08x}'


def validate_ios_config(path):
    config = json.loads(path.read_text())
    if (config.get('version'), config.get('build')) != ('16.5', '20F66'):
        raise ValueError('Expected the verified iOS 16.5 (20F66) configuration')
    for key in ('system', 'root', 'identity'):
        info = config.get(key)
        if not isinstance(info, dict) or not {'path', 'size', 'sha256'} <= info.keys():
            raise ValueError(f'Missing {key} integrity record')
        artifact = Path(info['path'])
        if not artifact.is_file() or artifact.stat().st_size != info['size'] or file_digest(artifact) != info['sha256']:
            raise ValueError(f'iOS config hash/size mismatch: {key}')
    system = config['system']
    helper = Path(config['root']['path'])
    if system['size'] % 4096 or helper.stat().st_size > 0x03E00000:
        raise ValueError('Unsupported iosroot/helper geometry')
    if helper.read_bytes()[32:36] != b'NXSB':
        raise ValueError('The helper image is not APFS')
    if 'crc32' not in system:
        raise ValueError('The iosroot system record has no CRC32')
    return config


def validate_device_inputs(fd_path, dtb_path):
    fd = fd_path.read_bytes()
    metadata = json.loads(fd_path.with_suffix(fd_path.suffix + '.munch.json').read_text())
    if len(fd) != FD_SIZE or metadata.get('target') != 'munch' or metadata.get('fd_sha256') != digest(fd):
        raise ValueError('FD does not match the reviewed Munch build manifest')
    model = subprocess.check_output(['fdtget', str(dtb_path), '/', 'model'], text=True).strip()
    if model != 'Qualcomm Technologies, Inc. xiaomi munch':
        raise ValueError('Expected the captured live Munch DTB')
    return fd, metadata


def load_lpm_tree(dtb_path):
    inspector = Path(__file__).resolve().parents[1] / 'display/inspect-panel.py'
    spec = importlib.util.spec_from_file_location('munch_lpm_fdt', inspector)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    tree = module.Fdt(dtb_path)

    if tree.string(LPM_PATH, 'compatible') != 'qcom,lpm-levels':
        raise ValueError('The captured Munch DTB has no qcom,lpm-levels node')

    def node(path, depth=0):
        if depth > 3:
            raise ValueError(f'Unexpectedly deep Munch LPM node: {path}')
        present = tree.properties(path)
        properties = []
        for name in sorted(LPM_STRING_PROPERTIES | LPM_CELL_PROPERTIES | LPM_FLAG_PROPERTIES):
            if name not in present:
                continue
            if name in LPM_STRING_PROPERTIES:
                value = tree.string(path, name)
                kind = 'string'
            elif name in LPM_FLAG_PROPERTIES:
                value = None
                kind = 'flag'
            else:
                values = tree.cells(path, name)
                if len(values) != 1:
                    raise ValueError(f'Expected one cell for {path}/{name}')
                value = values[0]
                kind = 'cell'
            properties.append((name, kind, value))
        children = []
        for child_name in tree.children(path):
            if child_name.startswith(('qcom,pm-cluster', 'qcom,pm-cpu')):
                children.append(node(f'{path}/{child_name}', depth + 1))
        return {'name': path.rsplit('/', 1)[-1], 'properties': properties, 'children': children}

    result = node(LPM_PATH)
    cluster = next((child for child in result['children'] if child['name'].startswith('qcom,pm-cluster@')), None)
    if cluster is None:
        raise ValueError('The captured Munch DTB has no qcom,pm-cluster node')
    cpu_groups = [child for child in cluster['children'] if child['name'].startswith('qcom,pm-cpu@')]
    if len(cpu_groups) != 2:
        raise ValueError('Expected both Munch CPU LPM groups in the captured DTB')
    return result


def render_lpm_node(entry, indent=2):
    prefix = '\t' * indent
    lines = [f'{prefix}dt_begin(&w, {json.dumps(entry["name"])});']
    for name, kind, value in entry['properties']:
        quoted_name = json.dumps(name)
        if kind == 'string':
            lines.append(f'{prefix}dt_prop_str(&w, {quoted_name}, {json.dumps(value)});')
        elif kind == 'cell':
            lines.append(f'{prefix}dt_prop_u32(&w, {quoted_name}, 0x{value:x});')
        else:
            lines.append(f'{prefix}dt_prop(&w, {quoted_name}, "", 0);')
    for child in entry['children']:
        lines.extend(render_lpm_node(child, indent + 1))
    lines.append(f'{prefix}dt_end(&w);')
    return lines


def patch_loader(source, work, config, touch, gpu, sde_backend=False, usb_autoboot=False, lpm_tree=None):
    """Make the generic loader select Munch and load the iosroot system image."""
    loader = source.read_text()
    platform = '''
/* Verified POCO F4 (munch) platform: Kona MPIDRs 0x000..0x700. */
static const struct platform platform_munch = {
\t.name = "munch", .io_base = 0x00400000, .io_size = 0x3FC00000,
\t.gic_dist = 0x17A00000, .gic_redist = 0x17A60000,
\t.cores = 8, .core_shift = 8, .small_cores = 0x0f,
};
'''
    marker = '\n/* ------------------------------------------------------------------------ */\n\nstatic void\nreboot(void)'
    if loader.count(marker) != 1:
        raise ValueError('Unexpected loader platform section')
    loader = loader.replace(marker, '\n' + platform + marker, 1)

    old_display = '''\tPLAT = &platform_qemu_virt;
\tif (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
\t    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
\t\tGOP = NULL;                     /* no display (QEMU without -device ramfb) */
\t} else {
\t\tSCREEN.base = GOP->mode->fb_base;
\t\tSCREEN.width = GOP->mode->info->width;
\t\tSCREEN.height = GOP->mode->info->height;
\t\tSCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
\t\tif (SCREEN.base == 0xE1000000ULL) {
\t\t\tPLAT = &platform_redwood;
\t\t} else {
\t\t\tramfb_phone_screen();
\t\t}
\t\tprint("display %d x %d at %x\\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
\t}
'''
    new_display = '''\tPLAT = &platform_qemu_virt;
\tif (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
\t    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
\t\tGOP = NULL;                     /* no display (QEMU without -device ramfb) */
\t} else {
\t\tSCREEN.base = GOP->mode->fb_base;
\t\tSCREEN.width = GOP->mode->info->width;
\t\tSCREEN.height = GOP->mode->info->height;
\t\tSCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
\t\tif (SCREEN.base == 0x9C000000ULL && SCREEN.width == 1080 && SCREEN.height == 2400 &&
\t\t    GOP->mode->info->pixels_per_scanline == 1080 && GOP->mode->info->pixel_format == 1) {
\t\t\tPLAT = &platform_munch;
\t\t} else if (SCREEN.base == 0xE1000000ULL) {
\t\t\tPLAT = &platform_redwood;
\t\t} else {
\t\t\tramfb_phone_screen();
\t\t}
\t\tprint("display %d x %d at %x\\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
\t}
'''
    if loader.count(old_display) != 1:
        raise ValueError('Unexpected loader display detection block')
    loader = loader.replace(old_display, new_display, 1)

    system = config['system']
    include = (f'#define MUNCH_SYSTEM_BYTES {system["size"]}ULL\n'
               f'#define MUNCH_SYSTEM_CRC32 0x{system["crc32"]}u\n')
    (work / 'ios-root.inc').write_text(include + (Path(__file__).with_name('ios-root.inc')).read_text())
    loader = loader.replace('static int\nrun_kernel(', '#include "ios-root.inc"\n\nstatic int\nrun_kernel(', 1)

    if touch:
        uefi = Path(__file__).parent.parent / 'uefi'
        adapter = ('#define system_table ST\n'
                   'static void text(const char *s) { print("%s", s); }\n'
                   'static void hex(uint64_t x) { print("%x", x); }\n'
                   'static void field(const char *s, uint64_t x) { print("%s%x\\n", s, x); }\n'
                   '#define MUNCH_TOUCH_RELEASE_RESET\n'
                   '#define MUNCH_TOUCH_POWER_ENABLE\n'
                   '#define MUNCH_TOUCH_HANDOFF\n'
                   'static unsigned touch_handoff_ready;\n')
        for name in ('touch-power-probe.inc', 'touch-power-enable.inc', 'touch-probe.inc'):
            adapter += f'#include "{(uefi / name).as_posix()}"\n'
        loader = loader.replace('#include "ios-root.inc"', adapter + '\n#include "ios-root.inc"', 1)

    if usb_autoboot:
        one_shot_usb = '''\tif (have_payload) {
'''
        if touch:
            one_shot_usb += '''\t\ttouch_handoff_ready = 0;
\t\tif (!touch_power_enable_begin()) {
\t\t\ttouch_power_enable_end();
\t\t\treboot();
\t\t}
\t\ttouch_probe(ST, NULL);
\t\tif (!touch_handoff_ready) {
\t\t\ttouch_restore();
\t\t\ttouch_power_enable_end();
\t\t\treboot();
\t\t}
'''
        one_shot_usb += '''\t\tif (!usb_start()) {
\t\t\tprint("usb: could not start Qualcomm USB protocol for one-shot handoff\\n");
'''
        if touch:
            one_shot_usb += '''\t\t\ttouch_restore();
\t\t\ttouch_power_enable_end();
'''
        one_shot_usb += '''\t\t\treboot();
\t\t}
\t\tif (!usb_stop_for_kernel()) {
\t\t\tprint("usb: could not release the Qualcomm USB protocol; kernel not started\\n");
'''
        if touch:
            one_shot_usb += '''\t\t\ttouch_restore();
\t\t\ttouch_power_enable_end();
'''
        one_shot_usb += '''\t\t\treboot();
\t\t}
'''
        one_shot_usb += '''\t\trun_kernel(0, 0);
'''
        if touch:
            one_shot_usb += '''\t\ttouch_restore();
\t\ttouch_power_enable_end();
'''
        one_shot_usb += '''\t\treboot();
\t}
'''
        hook = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
        replacement = one_shot_usb
    elif touch:
        hook = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
        replacement = '''\tif (have_payload) {
\t\tif (!touch_power_enable_begin()) { touch_power_enable_end(); reboot(); return 0; }
\t\ttouch_probe(ST, NULL);
\t\tif (!touch_handoff_ready) { touch_power_enable_end(); reboot(); return 0; }
\t\trun_kernel(0, 0);
\t\ttouch_restore();
\t\ttouch_power_enable_end();
\t}
'''
    else:
        hook = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
        replacement = '''\tif (have_payload) {
\t\trun_kernel(0, 0);
\t}
'''
    if loader.count(hook) != 1:
        raise ValueError('Unexpected loader one-shot entry')
    loader = loader.replace(hook, replacement, 1)
    usb_run = '\t\t\tint kernel_started = run_kernel(have_cp, cp);\n'
    if loader.count(usb_run) != 1:
        raise ValueError('Unexpected USB kernel-run hook')
    usb_run_replacement = usb_run
    if touch:
        touch_prepare = '''\t\t\ttouch_handoff_ready = 0;
\t\t\tif (!touch_power_enable_begin()) {
\t\t\t\ttouch_power_enable_end();
\t\t\t\tfb_reply("FAIL", "touch power handoff failed");
\t\t\t\treturn;
\t\t\t}
\t\t\ttouch_probe(ST, NULL);
\t\t\tif (!touch_handoff_ready) {
\t\t\t\ttouch_restore();
\t\t\t\ttouch_power_enable_end();
\t\t\t\tfb_reply("FAIL", "touch controller handoff failed");
\t\t\t\treturn;
\t\t\t}
'''
        command_start = '\t\t\tset_result("running", cp, 0);\n'
        if loader.count(command_start) != 1:
            raise ValueError('Unexpected USB command start hook')
        loader = loader.replace(command_start, touch_prepare + command_start, 1)
        usb_run_replacement += '\t\t\ttouch_restore();\n\t\t\ttouch_power_enable_end();\n'
    usb_run_replacement += '\t\t\tif (!usb_restart_after_kernel()) {\n'
    usb_run_replacement += '\t\t\t\tprint("usb: fastboot restart failed after XNU returned\\n");\n'
    usb_run_replacement += '\t\t\t}\n'
    loader = loader.replace(usb_run, usb_run_replacement, 1)
    if touch:
        usb_stop_error = '''\t\t\tif (!usb_stop_for_kernel()) {
\t\t\t\tprint("usb: could not release the Qualcomm USB protocol; kernel not started\\n");'''
        usb_stop_error_touch = '''\t\t\tif (!usb_stop_for_kernel()) {
\t\t\t\ttouch_restore();
\t\t\t\ttouch_power_enable_end();
\t\t\t\tprint("usb: could not release the Qualcomm USB protocol; kernel not started\\n");'''
        if loader.count(usb_stop_error) != 1:
            raise ValueError('Unexpected USB stop failure hook')
        loader = loader.replace(usb_stop_error, usb_stop_error_touch, 1)
    marker = '\tint have_payload = parse_payload(&pi, 0);\n'
    preload = marker + '\tif (have_payload && !munch_load_iosroot(&pi)) { reboot(); return 0; }\n'
    if loader.count(marker) != 1:
        raise ValueError('Unexpected loader payload entry point')
    loader = loader.replace(marker, preload, 1)
    preload_call = loader.find('if (have_payload && !munch_load_iosroot(&pi))')
    usb_start_call = loader.find('if (have_payload && pi.usb_mode && usb_start())')
    if preload_call < 0 or usb_start_call < 0 or preload_call > usb_start_call:
        raise ValueError('iosroot must be loaded and verified before the USB command loop')

    if gpu:
        allocation = '\tuint64_t rd_pa = kern_hi;'
        reservation = ('\tuint64_t scm_pa = kern_hi, scm_size = 4 * PAGE_SIZE_MAX;\n'
                       '\tif (scm_pa == 0 || scm_pa + scm_size < scm_pa ||\n'
                       '\t    scm_pa + scm_size > 0x100000000ULL ||\n'
                       '\t    (resident_rd_size != 0 && scm_pa + scm_size > phys_base + RESIDENT_RD_OFFSET)) {\n'
                       '\t\tprint("MUNCH: no low SCM reservation fits\\n"); return 0;\n\t}\n'
                       '\tmemset((void *)scm_pa, 0, scm_size);\n'
                       '\tprint("MUNCH SCM reserved page: %x size %x\\n", scm_pa, scm_size);\n'
                       '\tuint64_t rd_pa = kern_hi + scm_size;')
        if loader.count(allocation) != 1:
            raise ValueError('Unexpected kernel/ramdisk allocation hook')
        loader = loader.replace(allocation, reservation, 1)
        overlap = '\t\tif (kern_hi + rd_size > res_pa) {'
        if loader.count(overlap) != 1:
            raise ValueError('Unexpected resident-image overlap check')
        loader = loader.replace(overlap, '\t\tif (rd_pa + rd_size > res_pa) {', 1)
        chosen = '\tdt_begin(&w, "chosen");'
        metadata = ('\tdt_begin(&w, "chosen");\n'
                    '\tuint64_t scm_metadata[2] = {scm_pa, scm_size};\n'
                    '\tdt_prop(&w, "munch-scm-metadata", scm_metadata, sizeof(scm_metadata));')
        if loader.count(chosen) != 1:
            raise ValueError('Unexpected chosen device-tree hook')
        loader = loader.replace(chosen, metadata, 1)

    if sde_backend:
        memory_header = Path(__file__).resolve().parents[3] / 'xnu/files/iokit/Kernel/munch-sde-memory.h'
        loader = loader.replace('#include "efi.h"', '#include "efi.h"\n#include "munch-sde-memory.h"', 1)
        (work / 'munch-sde-memory.h').write_bytes(memory_header.read_bytes())
        allocation = '\tuint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);'
        reservation = allocation + '''
	uint64_t sde_pa = 0;
	if (pi.resident_rd != 0) resident_rd_size = pi.resident_rd;
	if (phys_base > UINT64_MAX - RESIDENT_RD_OFFSET ||
	    !munch_sde_pool_plan(rd_pa, win_base, win_size,
	    resident_rd_size ? phys_base + RESIDENT_RD_OFFSET : 0, rd_size, &sde_pa, &rd_pa)) {
		print("MUNCH: no low SDE reservation fits\\n"); return 0;
	}
	print("MUNCH SDE reserved buffers: %x size %x\\n", sde_pa, (uint64_t)MUNCH_SDE_POOL_SIZE);'''
        if loader.count(allocation) != 1:
            raise ValueError('Unexpected SDE reservation layout hook')
        loader = loader.replace(allocation, reservation, 1)
        if not gpu:
            overlap = '\t\tif (kern_hi + rd_size > res_pa) {'
            if loader.count(overlap) != 1:
                raise ValueError('Unexpected SDE resident-image overlap hook')
            loader = loader.replace(overlap, '\t\tif (rd_pa + rd_size > res_pa) {', 1)
        chosen = '\tdt_begin(&w, "chosen");'
        metadata = chosen + '''
	uint64_t sde_buffers[2] = {sde_pa, MUNCH_SDE_POOL_SIZE};
	dt_prop(&w, "munch-sde-buffers", sde_buffers, sizeof(sde_buffers));'''
        if loader.count(chosen) != 1:
            raise ValueError('Unexpected SDE chosen device-tree hook')
        loader = loader.replace(chosen, metadata, 1)

    if lpm_tree is not None:
        marker = '\n\tdt_end(&w);\n\n\tuint32_t dt_len = (uint32_t)(w.cur - w.base);'
        if loader.count(marker) != 1:
            raise ValueError('Cannot insert the captured Munch LPM levels into the Apple device tree')
        lpm_block = ('\n\tif (plat == &platform_munch) {\n' +
                     '\n'.join(render_lpm_node(lpm_tree, 2)) +
                     '\n\t}\n')
        loader = loader.replace(marker, lpm_block + marker, 1)
    (work / 'loader.c').write_text(loader)


def make_cpio(work):
    name = b'TRAILER!!!\0'
    cpio = b'070701' + b''.join(f'{value:08x}'.encode() for value in
                                 [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]) + name
    cpio += b'\0' * (-len(cpio) % 4)
    path = work / 'ramdisk'
    path.write_bytes(gzip.compress(cpio, mtime=0))
    return path


def validate_sde_backend(dtb_path, kernel, native=False):
    if b'MUNCH SDE: hardware presenter v1' not in kernel or b'unsafe loader handoff refused' not in kernel:
        raise ValueError('The selected kernel lacks the SDE presenter or safe loader-return hook')
    inspector = Path(__file__).resolve().parents[1] / 'display/inspect-panel.py'
    spec = importlib.util.spec_from_file_location('munch_panel', inspector)
    panel = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(panel)
    tree = panel.Fdt(dtb_path)
    selected = re.findall(r'(?:^|\s)msm_drm\.dsi_display0=([^:\s]+)', tree.string('/chosen', 'bootargs'))
    if selected != [panel.PANEL]:
        raise ValueError('SDE presentation requires the captured live L11r panel selection')
    profile = panel.profile(tree)
    te = tree.cells(panel.MDP + '/' + panel.PANEL, 'qcom,platform-te-gpio')
    if (profile['type'] != 'dsi_cmd_mode' or profile['controller'] != [0] or profile['phy'] != [0] or
            profile['lanes'] != [0, 1, 2, 3] or profile['bpp'] != 24 or
            profile['te_pin'] != 1 or len(te) != 3 or te[1:] != [66, 0] or
            not profile['te_external'] or not profile['te_check'] or not profile['modes']):
        raise ValueError('Unsupported L11r command-mode link')
    geometry = {'qcom,mdss-dsi-panel-width': 1080, 'qcom,mdss-dsi-panel-height': 2400,
                'qcom,mdss-dsc-slice-width': 540, 'qcom,mdss-dsc-slice-height': 8,
                'qcom,mdss-dsc-bit-per-component': 8, 'qcom,mdss-dsc-bit-per-pixel': 8,
                'qcom,compression-mode': 'dsc', 'qcom,display-topology': [1, 1, 1]}
    if any(any(mode[key] != value for key, value in geometry.items()) for mode in profile['modes']):
        raise ValueError('Unsupported L11r DSC geometry/topology')
    if native:
        if b'MUNCH SDE NATIVE r15' not in kernel or b'loader return refused before CPU shutdown' not in kernel:
            raise ValueError('The selected kernel lacks native modesetting or native return protection')
        observed = json.loads((inspector.parent / 'l11r-observed.json').read_text())['profile']['modes'][0]
        if profile['modes'][0] != observed:
            raise ValueError('Native mode requires the exact captured L11r 60-Hz timing/PHY/command profile')
    return profile['panel']


def pack(fd_path, dtb_path, kernel_path, ios_config_path, output, cpus, touch, gpu,
         gpu_diagnostics, usb_debug, usb_autoboot, verbose, extra,
         sde_probe=False, sde_backend=False, sde_native=False):
    if cpus not in (1, 8):
        raise ValueError('Only 1 or 8 CPUs are supported')
    if type(sde_probe) is not bool or (sde_probe and not gpu):
        raise ValueError('The SDE probe requires the GPU profile')
    if type(sde_backend) is not bool or (sde_backend and not gpu):
        raise ValueError('The SDE backend requires the GPU profile')
    if type(sde_native) is not bool or (sde_native and not sde_backend):
        raise ValueError('Native L11r mode requires --sde-backend')
    if usb_autoboot and usb_debug:
        raise ValueError('--usb-autoboot and --usb-debug are mutually exclusive')
    if sde_native and any(argument.startswith(('fb_hz=', 'loader_return_after=')) for argument in extra.split()):
        raise ValueError('Native mode fixes 60 Hz and uses the guarded timed loader return')
    if any(argument.startswith('munch_sde') for argument in extra.split()):
        raise ValueError('Use --sde-probe, --sde-backend or --sde-native to select the SDE path')
    config = validate_ios_config(ios_config_path)
    fd, metadata = validate_device_inputs(fd_path, dtb_path)
    lpm_tree = load_lpm_tree(dtb_path) if cpus == 8 else None
    kernel = kernel_path.read_bytes()
    if kernel[:4] != b'\xcf\xfa\xed\xfe' or b'XNULDRST' not in kernel:
        raise ValueError('Expected a source-built ARM64 Mach-O with the loader return slot')
    if sde_probe and b'MUNCH SDE: read-only Kona snapshot' not in kernel:
        raise ValueError('The selected kernel does not contain the read-only SDE probe')
    sde_panel = validate_sde_backend(dtb_path, kernel, sde_native) if sde_backend else None
    system_path = Path(config['system']['path'])
    if config['system']['crc32'] != file_crc32(system_path):
        raise ValueError('The iosroot CRC32 record does not match the system image')
    root = Path(__file__).resolve().parents[3]
    source = root / 'xnuloader/loader.c'
    identity = Path(config['identity']['path']).read_bytes()
    if cpus == 8:
        cmdline = ('bootcpu=7 cpus=8 cpufreq=2 lpm=1 small_cores=2 small_cores_after=0 touchscreen=1 '
                   'buttons=1')
    else:
        cmdline = '-v cpus=1 touchscreen=1 buttons=1'
    if not any(argument.startswith('loader_return_after=') for argument in extra.split()):
        cmdline += ' loader_return_after=600'
    if gpu:
        # Start above the floor to avoid first-frame ramp latency; the worker still scales both ways.
        # gpu_va is required by the Mesa/freedreno bridge before it can hand IOSurface buffers
        # to the kernel.
        # adreno_speed selects the startup OPP; gpu_dvfs keeps runtime scaling enabled.
        cmdline += ' gpu=1 adreno_speed=2 gpu_dvfs=1 gpu_va=1'
        if gpu_diagnostics:
            cmdline += ' gpu_log=1 gpu_exact=1'
    if sde_probe:
        cmdline += ' munch_sde_probe=1'
    if sde_backend:
        cmdline += ' munch_sde=1'
    if sde_native:
        cmdline += ' munch_sde_native=1'
    if (usb_debug or usb_autoboot) and not any(argument.startswith('munchusb=') for argument in extra.split()):
        cmdline += ' munchusb=2 usb_handoff=1 munchusblog=1'
    if verbose and '-v' not in cmdline.split():
        cmdline = '-v ' + cmdline
    if extra:
        cmdline += ' ' + extra
    helper = Path(config['root']['path']).read_bytes()

    with tempfile.TemporaryDirectory(prefix='munch-iosroot-') as directory:
        work = Path(directory)
        shutil.copy2(root / 'xnuloader/efi.h', work / 'efi.h')
        patch_loader(source, work, config, touch, gpu, sde_backend, usb_autoboot, lpm_tree)
        (work / 'identity.h').write_bytes(identity)
        subprocess.run(['clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
                        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-const-variable', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                        '-c', 'loader.c', '-o', 'loader.obj'], cwd=work, check=True)
        subprocess.run(['lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                        '/base:0xA1000000', '/out:loader.efi', 'loader.obj'], cwd=work, check=True)
        loader_bytes = (work / 'loader.efi').read_bytes()
        if gpu and (b'munch-scm-metadata' not in loader_bytes or b'MUNCH SCM reserved page' not in loader_bytes):
            raise ValueError('GPU image loader is missing the low SCM metadata reservation')
        if lpm_tree is not None and any(marker not in loader_bytes for marker in (
                b'qcom,lpm-levels', b'qcom,pm-cluster@0', b'rail-pc', b'llcc-off')):
            raise ValueError('Eight-CPU image loader is missing the captured Munch LPM levels')
        if sde_backend and (b'munch-sde-buffers' not in loader_bytes or b'munch-sde-buffers' not in kernel):
            raise ValueError('SDE image is missing the loader/kernel reserved-memory contract')
        entries = [('loader.efi', loader_bytes), ('kernel', kernel),
                   ('cmdline', cmdline.encode() + b'\0'), ('ramdisk', helper)]
        if usb_debug:
            entries.append(('usb', b'1'))
        body = bytearray()
        for name, data in entries:
            body += struct.pack('<16sII', name.encode(), len(data), 0)
            body += data + b'\0' * (-len(data) % 16)
        body += b'\0' * 24
        payload = struct.pack('<II', 0x50554E58, len(body)) + body
        if len(payload) > PAYLOAD_MAX:
            raise ValueError('Loader payload exceeds the reserved RAM capacity')
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c', str(root / 'devices/poco-f4/uefi/bootshim.S'),
                        f'-DFD_BASE=0x{FD_BASE:x}', f'-DFD_SIZE=0x{FD_SIZE:x}',
                        f'-DPAYLOAD_BASE=0x{PAYLOAD_BASE:x}', '-o', str(work / 'shim.o')], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work / 'shim.o'),
                        str(work / 'shim.bin')], check=True)
        blob = (work / 'shim.bin').read_bytes() + fd + payload
        blob += b'\0' * (-len(blob) % 16)
        image_kernel = work / 'kernel'
        image_kernel.write_bytes(gzip.compress(blob, mtime=0) + dtb_path.read_bytes())
        ramdisk = make_cpio(work)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(root / 'devices/poco-x5-pro/prebuilt/mkbootimg.py'),
                        '--header_version', '1', '--board', 'MUNCHIOS', '--kernel', str(image_kernel),
                        '--ramdisk', str(ramdisk), '-o', str(output)], check=True)
        report = {**metadata, 'mode': 'iosroot-source-kernel', 'cmdline': cmdline,
                  'requested_cpu_count': cpus, 'touchscreen': touch, 'gpu_requested': gpu,
                  'gpu_diagnostics': gpu_diagnostics, 'usb_debug': usb_debug,
                  'usb_autoboot': usb_autoboot,
                  'lpm_dt_embedded': lpm_tree is not None,
                  'sde_probe': sde_probe,
                  'sde_backend': sde_backend, 'sde_panel': sde_panel, 'sde_native': sde_native,
                  'loader_scm_reservation_bytes': 65536 if gpu else 0,
                  'loader_sde_reservation_bytes': 0x1444000 if sde_backend else 0,
                  'gpu_firmware_external': True, 'ios_version': config['version'], 'ios_build': config['build'],
                  'iosroot_system_sha256': config['system']['sha256'], 'iosroot_system_size': config['system']['size'],
                  'iosroot_helper_sha256': config['root']['sha256'], 'kernel_sha256': digest(kernel),
                  'loader_sha256': digest((work / 'loader.efi').read_bytes()),
                  'dtb_sha256': digest(dtb_path.read_bytes()), 'image_sha256': digest(output.read_bytes()),
                  'image_size': output.stat().st_size, 'contains_iosroot_reader': True,
                  'device_tested': False}
        output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Built Munch iosroot image: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--ios-config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=[1, 8], default=8)
    parser.add_argument('--no-touch', dest='touch', action='store_false')
    parser.add_argument('--no-gpu', dest='gpu', action='store_false')
    parser.add_argument('--gpu-diagnostics', action='store_true')
    parser.add_argument('--sde-probe', action='store_true', help='Enable the read-only Kona SDE snapshot')
    parser.add_argument('--sde-backend', action='store_true', help='Opt in to qualified L11r hardware presentation; no live-scanout fallback')
    parser.add_argument('--sde-native', action='store_true', help='Opt in to native L11r 60-Hz SDE handoff; keeps the inherited panel mode')
    parser.add_argument('--usb-debug', action='store_true')
    parser.add_argument('--usb-autoboot', action='store_true',
                        help='Release UEFI USB ownership in one-shot boot before starting XNU')
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--extra-cmdline', default='')
    parser.set_defaults(touch=True, gpu=True)
    args = parser.parse_args()
    pack(args.fd.resolve(), args.dtb.resolve(), args.kernel.resolve(), args.ios_config.resolve(),
         args.output.resolve(), args.cpus, args.touch, args.gpu, args.gpu_diagnostics,
         args.usb_debug, args.usb_autoboot, args.verbose, args.extra_cmdline,
         args.sde_probe, args.sde_backend, args.sde_native)
