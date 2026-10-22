#!/usr/bin/env python3
"""Validate repository integrity, patch sequences, and script syntax.

  tools/validate-repo.py [--verbose]

Checks:
  1. Patch sequence in xnu/patches/:
     - Sequential numbering starting at 0001 with no duplicates or gaps.
     - Patch files are non-empty and have valid unified diff markers.
  2. Python script syntax:
     - All tools/*.py compile cleanly via py_compile.
  3. Shell script syntax:
     - All tools/*.sh, shim/*.sh, and xnu/patches/*.sh pass bash -n check.
"""
import argparse, glob, os, py_compile, re, subprocess, sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def green(text): return f"\033[92m{text}\033[0m"
def red(text): return f"\033[91m{text}\033[0m"
def bold(text): return f"\033[1m{text}\033[0m"


def validate_patches(verbose=False):
    patch_dir = os.path.join(REPO_ROOT, 'xnu/patches')
    patch_files = sorted(glob.glob(os.path.join(patch_dir, '*.patch')))
    if not patch_files:
        print(red("[-] Error: No patch files found in xnu/patches/"))
        return False

    errors = []
    seen_numbers = {}

    for path in patch_files:
        fname = os.path.basename(path)
        m = re.match(r'^(\d{4})-(.+)\.patch$', fname)
        if not m:
            errors.append(f"Invalid patch filename format: {fname}")
            continue

        num = int(m.group(1))
        if num in seen_numbers:
            errors.append(f"Duplicate patch number {num:04d}: {fname} and {seen_numbers[num]}")
        seen_numbers[num] = fname

        size = os.path.getsize(path)
        if size == 0:
            errors.append(f"Patch is empty: {fname}")
            continue

        with open(path, 'r', errors='ignore') as f:
            head = f.read(2048)
            if not ('--- ' in head or 'diff ' in head or 'Index:' in head):
                errors.append(f"Patch missing standard diff header: {fname}")

    # Check contiguous sequence
    if seen_numbers:
        sorted_nums = sorted(seen_numbers.keys())
        expected = 1
        for num in sorted_nums:
            if num != expected:
                errors.append(f"Gap in patch sequence: expected {expected:04d}, found {num:04d} ({seen_numbers[num]})")
                expected = num + 1
            else:
                expected += 1

    if errors:
        print(red(f"[-] Patch validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green(f"[✓] Validated {len(patch_files)} patches (0001-{len(patch_files):04d} contiguous, format OK)"))
    return True


def validate_python_scripts(verbose=False):
    tools_dir = os.path.join(REPO_ROOT, 'tools')
    py_files = sorted(glob.glob(os.path.join(tools_dir, '*.py')))
    errors = []

    for path in py_files:
        fname = os.path.basename(path)
        try:
            py_compile.compile(path, doraise=True)
            if verbose:
                print(f"    [OK] {fname}")
        except py_compile.PyCompileError as e:
            errors.append(f"{fname}: {e}")

    if errors:
        print(red(f"[-] Python syntax validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green(f"[✓] Validated {len(py_files)} Python tools (syntax OK)"))
    return True


def validate_shell_scripts(verbose=False):
    sh_files = sorted(
        glob.glob(os.path.join(REPO_ROOT, 'tools/*.sh')) +
        glob.glob(os.path.join(REPO_ROOT, 'shim/*.sh')) +
        glob.glob(os.path.join(REPO_ROOT, 'xnu/patches/*.sh'))
    )
    errors = []

    for path in sh_files:
        rel = os.path.relpath(path, REPO_ROOT)
        res = subprocess.run(['bash', '-n', path], capture_output=True, text=True)
        if res.returncode != 0:
            errors.append(f"{rel}: {res.stderr.strip()}")
        elif verbose:
            print(f"    [OK] {rel}")

    if errors:
        print(red(f"[-] Shell syntax validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green(f"[✓] Validated {len(sh_files)} Shell scripts (syntax OK)"))
    return True


def validate_munch_build_plumbing():
    patch_dir = os.path.join(REPO_ROOT, 'xnu/patches')
    required = {
        '0083-munch-machine-config.patch': [
            'ifeq ($(filter SWEET MUNCH,$(CURRENT_MACHINE_CONFIG)),)',
            "+    \"     'IONameMatch'     = 'D22AP';\"",
        ],
        '0086-munch-shared-exception-diagnostics.patch': [
            '+#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_MUNCH)',
        ],
        '0019-munch-builtin-pthread.patch': [
            'ifneq ($(filter SWEET MUNCH,$(CURRENT_MACHINE_CONFIG)),)',
        ],
        '0020-munch-portable-aes-cbc.patch': [
            'ifneq ($(filter SWEET MUNCH,$(CURRENT_MACHINE_CONFIG)),)',
        ],
    }
    errors = []
    for fname, needles in required.items():
        path = os.path.join(patch_dir, fname)
        try:
            text = open(path, errors='replace').read()
        except OSError as exc:
            errors.append(f'{fname}: cannot read ({exc})')
            continue
        for needle in needles:
            if needle not in text:
                errors.append(f'{fname}: missing MUNCH build condition {needle!r}')

    if errors:
        print(red(f"[-] MUNCH build-plumbing validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green("[✓] MUNCH arm64 and dependency staging guards present"))
    return True


def validate_munch_ci_fail_closed():
    path = os.path.join(REPO_ROOT, '.github/workflows/xnu.yml')
    try:
        text = open(path, errors='replace').read()
    except OSError as exc:
        print(red(f"[-] MUNCH CI patch guard validation failed: {exc}"))
        return False

    required = (
        'Refusing to skip MUNCH patch ${PATCH}: ${APPLY_CHECK}',
        'pinned DXB patch-skip handler changed; refusing a non-fail-closed build',
        'if [ "$MACHINE_CONFIG" = "MUNCH" ]; then',
    )
    errors = [f'missing fail-closed workflow guard: {needle!r}'
              for needle in required if needle not in text]
    if errors:
        print(red('[-] MUNCH CI patch guard validation failed:'))
        for error in errors:
            print(f'    - {error}')
        return False

    print(green('[✓] MUNCH CI refuses skipped XNU patches'))
    return True


def validate_munch_adt():
    import mkadt

    original_product = mkadt.d22ap_product
    mkadt.d22ap_product = lambda path=None: None
    try:
        root = mkadt.build(munch=True, dram_base=0x80000000,
                           dram_size=0x80000000, seed=bytes(64))
    finally:
        mkadt.d22ap_product = original_product

    def prop(node, name):
        for key, _flags, value in node.props:
            if key == name:
                return value
        return None

    def child(node, name):
        return next((entry for entry in node.children if entry.name == name), None)

    def number(node, name):
        value = prop(node, name)
        return int.from_bytes(value, 'little') if value is not None else None

    errors = []
    compatible = prop(root, 'compatible') or b''
    if b'xiaomi,munch\0' not in compatible or b'xiaomi,sweet\0' in compatible:
        errors.append('root compatible must expose only the Munch platform identity')
    fbcon = child(root, 'munch-fbcon')
    if fbcon is None or child(root, 'sweet-fbcon') is not None:
        errors.append('framebuffer console node must be named munch-fbcon')
    arm_io = child(root, 'arm-io')
    disp0 = child(arm_io, 'disp0') if arm_io else None
    display_compatible = prop(disp0, 'compatible') if disp0 else b''
    if b'munch,display\0' not in (display_compatible or b'') or b'sweet,display\0' in (display_compatible or b''):
        errors.append('display node must expose only the Munch compatible')

    cpus = child(root, 'cpus')
    cpu_nodes = cpus.children if cpus else []
    expected_types = [ord('E')] * 4 + [ord('P')] * 4
    expected_domains = [0] * 4 + [1] * 3 + [2]
    expected_capacities = [448] * 4 + [1023] * 3 + [1024]
    if len(cpu_nodes) != 8:
        errors.append(f'expected eight CPU nodes, found {len(cpu_nodes)}')
    else:
        for index, cpu in enumerate(cpu_nodes):
            if number(cpu, 'cluster-type') != expected_types[index]:
                errors.append(f'cpu{index} has the wrong E/P cluster type')
            if number(cpu, 'qcom,freq-domain') != expected_domains[index]:
                errors.append(f'cpu{index} has the wrong Qualcomm frequency domain')
            if number(cpu, 'capacity-dmips-mhz') != expected_capacities[index]:
                errors.append(f'cpu{index} has the wrong capacity')

    cpu_source = os.path.join(REPO_ROOT, 'xnu', 'patches',
                              '0088-munch-rebrand-amp-topology.patch')
    try:
        cpu_text = open(cpu_source, errors='replace').read()
    except OSError as exc:
        errors.append(f'cannot read Munch CPU patch ({exc})')
    else:
        added_cpu_text = '\n'.join(
            line[1:] for line in cpu_text.splitlines()
            if line.startswith('+') and not line.startswith('+++'))
        if 'Cortex-A77' not in added_cpu_text or 'Cortex-A76' in added_cpu_text:
            errors.append('active Munch CPU profile must identify A77, not A76')
        if 'Snapdragon 870' not in added_cpu_text or 'Kryo 585' not in added_cpu_text:
            errors.append('active Munch CPU profile must identify Snapdragon 870/Kryo 585')
        if '#define __ARM_AMP__' not in added_cpu_text:
            errors.append('Munch board profile must enable heterogeneous AMP topology')
        if '#define MAX_CPU_CLUSTERS               2' not in added_cpu_text:
            errors.append('Munch board profile must allow E/P logical clusters')
        if '+#define NO_ECORE' in cpu_text:
            errors.append('Munch board profile must not disable E-core topology')
        if re.search(r'^\+[^+].*SWEET|^\+[^+].*sweet', cpu_text, re.MULTILINE):
            errors.append('0088 must not add active SWEET identifiers')
        if 'MUNCH EARLY PANIC' not in added_cpu_text:
            errors.append('early panic diagnostics must use the Munch identity')

    if errors:
        print(red('[-] Munch ADT validation failed:'))
        for error in errors:
            print(f'    - {error}')
        return False

    print(green('[✓] Munch ADT exposes eight CPUs, E/P classes and Munch-only names'))
    return True


def validate_userdata_safety():
    path = os.path.join(REPO_ROOT, 'tools/munch-log-pull.sh')
    try:
        text = open(path, errors='replace').read()
    except OSError as exc:
        print(red(f"[-] userdata safety validation failed: {exc}"))
        return False

    forbidden = ('adb ', 'exec-out', 'blockdev', 'dd if=', '/dev/block')
    errors = [needle for needle in forbidden if needle in text]
    if errors:
        print(red("[-] userdata safety validation failed: pull helper is not fail-closed"))
        for needle in errors:
            print(f"    - forbidden device-access token: {needle!r}")
        return False

    print(green("[✓] Munch userdata pull helper is fail-closed"))
    return True


def validate_live_fdt_helper():
    path = os.path.join(REPO_ROOT, 'tools/munch-fdt-pull.sh')
    try:
        text = open(path, errors='replace').read()
    except OSError as exc:
        print(red(f"[-] live FDT helper validation failed: {exc}"))
        return False

    required = ('adb exec-out', '/sys/firmware/fdt', 'ro.product.device', 'munch')
    forbidden = ('adb pull', 'fastboot', '/dev/block', 'userdata', '/data/', 'dd ')
    errors = [f"missing required token: {needle!r}" for needle in required if needle not in text]
    errors.extend(f"forbidden device-access token: {needle!r}" for needle in forbidden if needle in text)
    if errors:
        print(red("[-] live FDT helper validation failed:"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green("[✓] Live Munch FDT helper is read-only and repository-safe"))
    return True


def validate_ram_only_shim():
    shim_dir = os.path.join(REPO_ROOT, 'shim')
    paths = {
        name: os.path.join(shim_dir, name)
        for name in ('build.sh', 'shim.c', 'loader.c', 'ufs.c')
    }
    contents = {}
    errors = []
    for name, path in paths.items():
        try:
            contents[name] = open(path, errors='replace').read()
        except OSError as exc:
            errors.append(f'{name}: cannot read ({exc})')

    build = contents.get('build.sh', '')
    source_list = re.search(r'for f in ([^;]+); do', build)
    if source_list is None:
        errors.append('build.sh: cannot identify compiled shim source list')
    elif re.search(r'\bufs\.c\b', source_list.group(1)):
        errors.append('build.sh: legacy UFS probe must not be compiled')
    if re.search(r'\bout/ufs\.o\b', build):
        errors.append('build.sh: legacy UFS probe must not be linked')

    for name in ('shim.c', 'loader.c'):
        if re.search(r'\btest_ufs\s*\(', contents.get(name, '')):
            errors.append(f'{name}: RAM-only boot path must not call test_ufs')

    if 'IOS_MUNCH_ALLOW_LEGACY_UFS_PROBE' not in contents.get('ufs.c', ''):
        errors.append('ufs.c: missing explicit compile-time opt-in guard')

    if errors:
        print(red("[-] RAM-only shim storage validation failed:"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green("[✓] RAM-only shim excludes and guards legacy UFS probe"))
    return True


def main():
    ap = argparse.ArgumentParser(description="Validate repository integrity, patches, and scripts.")
    ap.add_argument('-v', '--verbose', action='store_true', help='Show detailed per-file output')
    a = ap.parse_args()

    print(bold("=== Running ios-munch Repository Validation ==="))
    ok_patches = validate_patches(a.verbose)
    ok_munch = validate_munch_build_plumbing()
    ok_munch_ci = validate_munch_ci_fail_closed()
    ok_munch_adt = validate_munch_adt()
    ok_userdata = validate_userdata_safety()
    ok_live_fdt = validate_live_fdt_helper()
    ok_shim_storage = validate_ram_only_shim()
    ok_python = validate_python_scripts(a.verbose)
    ok_shell = validate_shell_scripts(a.verbose)

    if ok_patches and ok_munch and ok_munch_ci and ok_munch_adt and ok_userdata and ok_live_fdt and ok_shim_storage and ok_python and ok_shell:
        print(bold(green("\nAll repository integrity checks PASSED successfully!")))
        return 0
    else:
        print(bold(red("\nRepository integrity check FAILED.")))
        return 1


if __name__ == '__main__':
    sys.exit(main())
