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
    ok_userdata = validate_userdata_safety()
    ok_live_fdt = validate_live_fdt_helper()
    ok_shim_storage = validate_ram_only_shim()
    ok_python = validate_python_scripts(a.verbose)
    ok_shell = validate_shell_scripts(a.verbose)

    if ok_patches and ok_munch and ok_userdata and ok_live_fdt and ok_shim_storage and ok_python and ok_shell:
        print(bold(green("\nAll repository integrity checks PASSED successfully!")))
        return 0
    else:
        print(bold(red("\nRepository integrity check FAILED.")))
        return 1


if __name__ == '__main__':
    sys.exit(main())
