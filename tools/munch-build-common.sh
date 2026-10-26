#!/usr/bin/env bash
# Shared setup for the source-built POCO F4 pipeline.

set -euo pipefail

MUNCH_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MUNCH_BUILD_DIR=${MUNCH_BUILD_DIR:-$MUNCH_ROOT/artifacts/munch-build}
MUNCH_MU_ROOT=${MU_ROOT:-$MUNCH_ROOT/Mu-Silicium}
MUNCH_MU_REPO=${MUNCH_MU_REPO:-https://github.com/Project-Silicium/Mu-Silicium.git}
MUNCH_MU_REF=${MUNCH_MU_REF:-$(cat "$MUNCH_ROOT/devices/poco-f4/uefi/BASE")}
MUNCH_XNU_DIR=${XNU_SRC:-$MUNCH_ROOT/xnu-src/xnu-8796.121.2}
MUNCH_XNU_BASE=${MUNCH_XNU_BASE:-c54f35ca767986246321eb901baf8f5ff7923f6a}
MUNCH_JOBS=${MUNCH_JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

export MUNCH_ROOT MUNCH_BUILD_DIR MUNCH_MU_ROOT MUNCH_MU_REPO MUNCH_MU_REF MUNCH_XNU_DIR MUNCH_XNU_BASE MUNCH_JOBS

munch_die() {
    echo "munch-build: $*" >&2
    exit 1
}

munch_need() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || munch_die "missing host tool: $tool"
    done
}

munch_lock() {
    mkdir -p "$MUNCH_BUILD_DIR"
    if [ "${MUNCH_LOCK_HELD:-0}" = 1 ]; then
        return
    fi
    exec 9>"$MUNCH_BUILD_DIR/.lock"
    flock 9
    export MUNCH_LOCK_HELD=1
}

munch_bootstrap_theos() {
    if [ "$(uname -s)" = Darwin ]; then
        return
    fi

    export THEOS=${THEOS:-$MUNCH_ROOT/.cache/theos}
    local cctools="$THEOS/toolchain/linux/iphone/bin"
    if [ ! -x "$cctools/ld" ] || [ ! -x "$cctools/clang" ]; then
        [ "${MUNCH_BOOTSTRAP_THEOS:-1}" = 1 ] ||
            munch_die "Theos Linux toolchain is missing at $THEOS (set MUNCH_BOOTSTRAP_THEOS=1 to fetch it)"
        munch_need curl tar xz

        local arch url archive
        arch=$(uname -m)
        case "$arch" in
            x86_64|aarch64) ;;
            *) munch_die "automatic Theos bootstrap does not support host architecture: $arch" ;;
        esac
        url=${MUNCH_THEOS_TOOLCHAIN_URL:-https://github.com/L1ghtmann/llvm-project/releases/latest/download/iOSToolchain-$arch.tar.xz}
        archive="$MUNCH_ROOT/.cache/iOSToolchain-$arch.tar.xz"
        mkdir -p "$(dirname "$archive")" "$THEOS/toolchain"
        if [ ! -s "$archive" ] || ! tar -tJf "$archive" >/dev/null 2>&1; then
            rm -f "$archive" "$archive.part"
            echo "munch-build: downloading Theos Linux toolchain ($arch)"
            curl --fail --location --retry 5 --retry-delay 2 --output "$archive.part" "$url"
            mv -f "$archive.part" "$archive"
        fi
        tar -xJf "$archive" -C "$THEOS/toolchain"
        [ -x "$cctools/ld" ] && [ -x "$cctools/clang" ] ||
            munch_die "downloaded Theos toolchain has no usable Linux cctools"
    fi

    local sdk="$THEOS/sdks/iPhoneOS16.5.sdk"
    if [ ! -d "$sdk/usr/include" ]; then
        [ ! -e "$sdk" ] || munch_die "incomplete iPhoneOS16.5 SDK at $sdk"
        local sdk_repo="$THEOS/sdks-git"
        if [ ! -d "$sdk_repo/.git" ]; then
            echo 'munch-build: downloading the iPhoneOS 16.5 SDK metadata'
            git clone --depth 1 --filter=blob:none --sparse \
                https://github.com/theos/sdks.git "$sdk_repo"
        fi
        git -C "$sdk_repo" sparse-checkout set iPhoneOS16.5.sdk
        mkdir -p "$THEOS/sdks"
        ln -sfn ../sdks-git/iPhoneOS16.5.sdk "$sdk"
    fi
    [ -d "$sdk/usr/include" ] || munch_die "iPhoneOS16.5 SDK was not prepared at $sdk"
}

munch_prepare_xnu() {
    munch_need bash git curl tar xz make cc clang flex bison m4 perl python3 autoreconf automake 7z
    munch_bootstrap_theos

    if [ ! -d "$MUNCH_XNU_DIR" ]; then
        echo "munch-build: fetching Apple XNU and applying the Munch overlay"
        XNU_SRC="$MUNCH_XNU_DIR" bash "$MUNCH_ROOT/xnu/apply.sh"
    fi
    [ -f "$MUNCH_XNU_DIR/makedefs/MakeInc.def" ] ||
        munch_die "XNU checkout is incomplete: $MUNCH_XNU_DIR"
    [ "$(git -C "$MUNCH_XNU_DIR" rev-parse HEAD 2>/dev/null || true)" = "$MUNCH_XNU_BASE" ] ||
        munch_die "XNU checkout is not the pinned Munch base $MUNCH_XNU_BASE"

    export THEOS
    if [ ! -x "$MUNCH_ROOT/xnu-deps/host/bin/xnu-clang" ] ||
       [ ! -x "$MUNCH_ROOT/xnu-deps/host/bin/ld64" ] ||
       [ ! -d "$MUNCH_ROOT/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/include" ]; then
        echo "munch-build: preparing XNU host tools and iPhoneOS 16.5 SDK"
        JOBS="$MUNCH_JOBS" bash "$MUNCH_ROOT/xnu-deps/setup.sh" --stage1
    fi
    if [ ! -f "$MUNCH_ROOT/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/local/lib/kernel/libfirehose_kernel.a" ] ||
       [ ! -f "$MUNCH_ROOT/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/local/lib/kernel/libpthread_kext.a" ]; then
        echo "munch-build: building XNU support libraries"
        JOBS="$MUNCH_JOBS" bash "$MUNCH_ROOT/xnu-deps/setup.sh" --stage2
    fi
    [ -x "$MUNCH_ROOT/xnu-deps/host/bin/xnu-clang" ] || munch_die "xnu-clang was not built"
    [ -x "$MUNCH_ROOT/xnu-deps/host/bin/ld64" ] || munch_die "ld64 was not built"
}

munch_prepare_mu() {
    local mu="$MUNCH_MU_ROOT"
    if [ ! -d "$mu/.git" ]; then
        [ ! -e "$mu" ] || munch_die "$mu exists but is not a git checkout"
        mkdir -p "$(dirname "$mu")"
        echo "munch-build: cloning Mu-Silicium at $MUNCH_MU_REF"
        git clone --no-checkout "$MUNCH_MU_REPO" "$mu"
    fi
    if [ "$(git -C "$mu" rev-parse HEAD 2>/dev/null || true)" != "$MUNCH_MU_REF" ]; then
        git -C "$mu" fetch --depth=1 origin "$MUNCH_MU_REF"
        git -C "$mu" checkout --detach -q "$MUNCH_MU_REF"
    fi
    git -C "$mu" submodule sync --recursive
    local submodule_jobs=${MUNCH_SUBMODULE_JOBS:-4}
    local attempt
    local basecore_submodules=(
        BaseTools/Source/C/BrotliCompress/brotli
        MdeModulePkg/Library/BrotliCustomDecompressLib/brotli
        MdeModulePkg/Universal/RegularExpressionDxe/oniguruma
        MdePkg/Library/BaseFdtLib/libfdt
        MdePkg/Library/MipiSysTLib/mipisyst
        SecurityPkg/DeviceSecurity/SpdmLib/libspdm
    )
    for attempt in 1 2 3; do
        if git -C "$mu" -c protocol.version=2 submodule update \
            --init --depth=1 --filter=blob:none --jobs="$submodule_jobs" &&
           git -C "$mu/Mu_Basecore" -c protocol.version=2 submodule update \
            --init --depth=1 --filter=blob:none --jobs="$submodule_jobs" \
            -- "${basecore_submodules[@]}"; then
            break
        fi
        [ "$attempt" -lt 3 ] || munch_die "Mu-Silicium submodules failed after $attempt attempts"
        echo "munch-build: retrying Mu-Silicium submodules ($((attempt + 1))/3)" >&2
        sleep 3
    done
    [ "$(git -C "$mu" rev-parse HEAD)" = "$MUNCH_MU_REF" ] ||
        munch_die "Mu-Silicium checkout is not pinned to $MUNCH_MU_REF"
    export MU_ROOT="$mu"
}

munch_prepare_uefi_python() {
    if [ -n "${UEFI_PYTHON:-}" ]; then
        UEFI_PYTHON=$(command -v "$UEFI_PYTHON" 2>/dev/null || printf '%s' "$UEFI_PYTHON")
        [ -x "$UEFI_PYTHON" ] || munch_die "UEFI_PYTHON is not executable: $UEFI_PYTHON"
        "$UEFI_PYTHON" -c 'import edk2toolext, edk2toollib, uefi_firmware' >/dev/null 2>&1 ||
            munch_die "UEFI_PYTHON lacks Mu-Silicium Python modules: $UEFI_PYTHON"
        return
    fi
    local venv="$MUNCH_ROOT/.venv-munch-uefi"
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv "$venv" || munch_die "cannot create $venv; install python3-venv"
    fi
    if ! "$venv/bin/python" -c 'import edk2toolext, edk2toollib, uefi_firmware' >/dev/null 2>&1; then
        echo "munch-build: installing Mu-Silicium Python build requirements"
        "$venv/bin/python" -m pip install --disable-pip-version-check -r "$MUNCH_MU_ROOT/pip-requirements.txt" uefi_firmware==1.16
    fi
    export UEFI_PYTHON="$venv/bin/python"
}

munch_prepare_uefi_tools() {
    local basecore="$MUNCH_MU_ROOT/Mu_Basecore/BaseTools/Bin"
    local extdep="$basecore/mu_nasm_extdep"
    local host_dir="$extdep/Linux-x86"
    local nasm_path ndisasm_path

    nasm_path=$(command -v nasm 2>/dev/null || true)
    ndisasm_path=$(command -v ndisasm 2>/dev/null || true)
    if [ -n "$nasm_path" ] && [ -n "$ndisasm_path" ]; then
        mkdir -p "$host_dir"
        ln -sfn "$nasm_path" "$host_dir/nasm"
        ln -sfn "$ndisasm_path" "$host_dir/ndisasm"
        printf 'version: 20016.1.1\n' > "$extdep/extdep_state.yaml"
        export NASM_PREFIX=${NASM_PREFIX:-/usr/bin/}
        return
    fi
    munch_die 'missing host tools: nasm and ndisasm (or install Mono for Mu NuGet bootstrap)'
}

munch_prepare_xbl() {
    local out="$MUNCH_MU_ROOT/Binaries/munch"
    if [ -f "$out/manifest.json" ]; then
        return
    fi
    local xbl=${MUNCH_XBL:-$MUNCH_BUILD_DIR/xbl_a.img}
    [ -f "$xbl" ] || munch_die "MUNCH_XBL does not exist: $xbl"
    [ ! -e "$out" ] || munch_die "$out exists without a manifest; inspect or remove it before extraction"
    echo "munch-build: extracting reviewed XBL modules"
    "$UEFI_PYTHON" "$MUNCH_ROOT/devices/poco-f4/extract-xbl.py" "$xbl" \
        --templates "$MUNCH_MU_ROOT/Binaries/alioth" --output "$out"
}

munch_prepare_dtb() {
    local dtb=${MUNCH_DTB:-$MUNCH_BUILD_DIR/munch.dtb}
    [ -f "$dtb" ] || munch_die "live Munch DTB is required: set MUNCH_DTB=/path/to/merged-munch.dtb"
    command -v fdtget >/dev/null 2>&1 || munch_die "missing host tool: fdtget"
    MUNCH_DTB=$(cd "$(dirname "$dtb")" && pwd)/$(basename "$dtb")
    export MUNCH_DTB
}

munch_hash_config() {
    local system=$1 root_image=$2 identity=$3 output=$4
    python3 - "$system" "$root_image" "$identity" "$output" <<'PY'
import hashlib
import json
import pathlib
import sys
import zlib

system, helper, identity, output = map(pathlib.Path, sys.argv[1:])

def record(path, crc=False):
    sha = hashlib.sha256()
    size = 0
    value = 0
    with path.open('rb') as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            sha.update(chunk)
            size += len(chunk)
            if crc:
                value = zlib.crc32(chunk, value)
    item = {'path': str(path.resolve()), 'size': size, 'sha256': sha.hexdigest()}
    if crc:
        item['crc32'] = f'{value & 0xffffffff:08x}'
    return item

if not system.is_file() or not helper.is_file() or not identity.is_file():
    raise SystemExit('missing root build artifact')
config = {
    'version': '16.5',
    'build': '20F66',
    'system': record(system, crc=True),
    'root': record(helper),
    'identity': record(identity),
}
output.parent.mkdir(parents=True, exist_ok=True)
temporary = output.with_suffix(output.suffix + '.tmp')
temporary.write_text(json.dumps(config, indent=2) + '\n')
temporary.replace(output)
print(f'munch-build: wrote {output}')
PY
}
