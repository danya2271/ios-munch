#!/usr/bin/env bash
# Fetch iOS 16.5, build the source userland/APFS images, and make the local boot manifest.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$ROOT/tools/munch-build-common.sh"
cd "$ROOT"
munch_lock

munch_prepare_xnu

echo 'munch-build: fetching and unpacking iOS 16.5 (20F66)'
KEEP=${KEEP:-} bash "$ROOT/sysimage/fetch.sh"

echo 'munch-build: building source userland and APFS images'
GL=${GL:-1} REST=${REST:-1} SYSTEM=1 CHECK=${CHECK:-1} PREBUILT='' \
    bash "$ROOT/userland/build.sh"

DT_IM4P="$MUNCH_BUILD_DIR/ios165-device-tree.im4p"
if [ ! -s "$DT_IM4P" ]; then
    echo 'munch-build: fetching the iOS identity device tree'
    python3 "$ROOT/sysimage/fetch-from-ipsw.py" \
        Firmware/all_flash/DeviceTree.d22ap.im4p "$DT_IM4P.part"
    mv -f "$DT_IM4P.part" "$DT_IM4P"
fi

echo 'munch-build: generating xnuloader identity'
bash "$ROOT/xnuloader/mkidentity.sh" "$DT_IM4P"
IDENTITY="$MUNCH_BUILD_DIR/identity.h"
cp -f "$ROOT/xnuloader/identity.h" "$IDENTITY"

munch_hash_config "$ROOT/userland/system.apfs" "$ROOT/userland/root.apfs" "$IDENTITY" \
    "$MUNCH_BUILD_DIR/ios-config.json"

echo 'munch-build: root artifacts ready'
printf '  system: %s\n' "$ROOT/userland/system.apfs"
printf '  helper: %s\n' "$ROOT/userland/root.apfs"
printf '  config: %s\n' "$MUNCH_BUILD_DIR/ios-config.json"
