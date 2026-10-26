#!/usr/bin/env bash
# Install only the F4 source overlay into a pinned, local Mu-Silicium checkout.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mu=${MU_ROOT:-$here/../../../Mu-Silicium}
expected=$(cat "$here/BASE")
[ "$(git -C "$mu" rev-parse HEAD)" = "$expected" ] || {
    echo "Mu-Silicium must be at $expected" >&2
    exit 1
}
cp -a "$here/files/." "$mu/"
echo "Installed munch source overlay into $mu"
