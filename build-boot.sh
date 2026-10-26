#!/usr/bin/env bash
# Build the pinned Munch UEFI, source XNU, and a temporary fastboot boot image.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$ROOT/tools/munch-build-common.sh"
cd "$ROOT"
munch_lock

CONFIG=${MUNCH_IOS_CONFIG:-$MUNCH_BUILD_DIR/ios-config.json}
munch_prepare_dtb
munch_prepare_requested_gpu_firmware
if [ "${MUNCH_SKIP_ROOT:-0}" != 1 ] && [ -z "${MUNCH_IOS_CONFIG:-}" ] &&
   { [ ! -f "$CONFIG" ] || [ "${MUNCH_REBUILD_ROOT:-0}" = 1 ]; }; then
    MUNCH_LOCK_HELD=1 bash "$ROOT/build-root.sh"
fi
[ -f "$CONFIG" ] || munch_die "missing $CONFIG; run ./build-root.sh first"

munch_prepare_xnu
munch_prepare_mu
munch_prepare_uefi_python
munch_prepare_uefi_tools
munch_prepare_xbl
munch_need clang lld-link aarch64-linux-gnu-objcopy

TARGET=${BUILD_TARGET:-RELEASE}
STORAGE=${MUNCH_STORAGE:-1}
case "$TARGET" in RELEASE|DEBUG) ;; *) munch_die 'BUILD_TARGET must be RELEASE or DEBUG' ;; esac
case "$STORAGE" in 0|1) ;; *) munch_die 'MUNCH_STORAGE must be 0 or 1' ;; esac

echo "munch-build: building Mu-Silicium Munch UEFI ($TARGET, storage=$STORAGE)"
MU_ROOT="$MUNCH_MU_ROOT" UEFI_PYTHON="$UEFI_PYTHON" BUILD_TARGET="$TARGET" \
    MUNCH_STORAGE="$STORAGE" PREPARE=1 bash "$ROOT/devices/poco-f4/uefi/build.sh"

KERNEL="$MUNCH_XNU_DIR/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837"
echo "munch-build: building Munch XNU ($MUNCH_JOBS jobs)"
XNU_LOG="$MUNCH_BUILD_DIR/xnu-build.log"
if ! MUNCH_STAGE=1 MUNCH_STAGE_REPORT="$MUNCH_BUILD_DIR/munch-stage.json" \
    bash "$ROOT/xnu-build.sh" -j"$MUNCH_JOBS" -k 2>&1 | tee "$XNU_LOG"; then
    # XNU 8796.121.2 links this bring-up kernel and then rejects the upstream
    # export lists in check_all_exports. The resulting Mach-O is still the
    # artifact consumed by the loader; unrelated build failures must abort.
    if [ ! -s "$KERNEL" ] || ! grep -Eq \
        'exported name not in import list:|check_all_exports' "$XNU_LOG"; then
        munch_die "XNU build failed; see $XNU_LOG"
    fi
    echo "munch-build: XNU linked; ignoring expected symbol-set validation status"
fi
[ -s "$KERNEL" ] || munch_die "XNU build did not produce $KERNEL"

FD="$MUNCH_MU_ROOT/Build/munchPkg/${TARGET}_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$FD" ] || munch_die "UEFI build did not produce $FD"

OUTPUT=${MUNCH_BOOT_IMAGE:-$MUNCH_BUILD_DIR/munch-iosroot.img}
GPU_ARGS=()
case "${MUNCH_GPU:-1}" in
    1)
        munch_prepare_gpu_firmware
        GPU_ARGS=()
        ;;
    0) GPU_ARGS=(--no-gpu) ;;
    auto)
        if munch_prepare_gpu_firmware; then
            GPU_ARGS=()
        else
            GPU_ARGS=(--no-gpu)
        fi
        ;;
    *) munch_die 'MUNCH_GPU must be auto, 0, or 1' ;;
esac

PACK_ARGS=(
    --fd "$FD"
    --dtb "$MUNCH_DTB"
    --kernel "$KERNEL"
    --ios-config "$CONFIG"
    --output "$OUTPUT"
    --cpus "${MUNCH_CPUS:-8}"
    "${GPU_ARGS[@]}"
)
[ "${MUNCH_TOUCH:-1}" = 1 ] || PACK_ARGS+=(--no-touch)
[ "${MUNCH_GPU_DIAGNOSTICS:-0}" = 1 ] && PACK_ARGS+=(--gpu-diagnostics)
[ "${MUNCH_USB:-0}" = 1 ] && PACK_ARGS+=(--usb-debug)
[ "${MUNCH_VERBOSE:-0}" = 1 ] && PACK_ARGS+=(--verbose)
[ -n "${MUNCH_EXTRA_CMDLINE:-}" ] && PACK_ARGS+=(--extra-cmdline "$MUNCH_EXTRA_CMDLINE")

echo 'munch-build: packaging Android boot image'
python3 "$ROOT/devices/poco-f4/loader/pack-iosroot.py" "${PACK_ARGS[@]}"

printf '\nBuilt image: %s\n' "$OUTPUT"
printf 'Test without flashing: fastboot boot %q\n' "$OUTPUT"
