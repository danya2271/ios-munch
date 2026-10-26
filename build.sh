#!/usr/bin/env bash
# Complete source build: iOS root, Mu-Silicium UEFI, XNU, and fastboot image.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$ROOT/tools/munch-build-common.sh"
cd "$ROOT"
munch_lock

bash "$ROOT/build-root.sh"
MUNCH_LOCK_HELD=1 MUNCH_SKIP_ROOT=1 bash "$ROOT/build-boot.sh"
