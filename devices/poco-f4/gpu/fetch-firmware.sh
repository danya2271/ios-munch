#!/usr/bin/env bash
# Verify or fetch the device-local Adreno 650 firmware used by the Munch GPU path.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${MUNCH_GPU_FW_DIR:-$ROOT/fw-gpu}
MANIFEST="$ROOT/devices/poco-f4/gpu/firmware-manifest.sha256"
REPO=${MUNCH_GPU_FW_REPO:-TheMuppets/proprietary_vendor_xiaomi_sm8250-common}
COMMIT=${MUNCH_GPU_FW_COMMIT:-c48b43a4db777a1005f730ca1ff0c14d022055db}
REMOTE_DIR=${MUNCH_GPU_FW_REMOTE_DIR:-proprietary/vendor/firmware}
RAW_BASE=${MUNCH_GPU_FW_RAW_BASE:-https://raw.githubusercontent.com/$REPO/$COMMIT/$REMOTE_DIR}

die() {
    echo "munch-gpu-firmware: $*" >&2
    exit 1
}

[ -r "$MANIFEST" ] || die "missing manifest: $MANIFEST"

check_files() {
    local expected name path missing=0 bad=0
    while read -r expected name; do
        [ -n "$name" ] || continue
        path="$OUT/$name"
        if [ ! -s "$path" ]; then
            echo "munch-gpu-firmware: missing $path" >&2
            missing=1
            continue
        fi
        if ! printf '%s  %s\n' "$expected" "$path" | sha256sum -c - >/dev/null; then
            echo "munch-gpu-firmware: SHA-256 mismatch: $path" >&2
            bad=1
        fi
    done < "$MANIFEST"
    [ "$bad" = 0 ] || return 2
    [ "$missing" = 0 ] || return 1
}

if [ "${1:-}" = --check ]; then
    check_files
    echo "munch-gpu-firmware: verified $OUT"
    exit 0
fi

mkdir -p "$OUT"
status=0
if check_files; then
    echo "munch-gpu-firmware: local firmware is already verified: $OUT"
    exit 0
else
    status=$?
fi
[ "$status" = 1 ] || die "refusing to replace modified firmware in $OUT"

command -v curl >/dev/null 2>&1 || die 'curl is required for firmware download'
command -v sha256sum >/dev/null 2>&1 || die 'sha256sum is required for firmware verification'

tmp=$(mktemp -d "$OUT/.munch-gpu-download.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

echo "munch-gpu-firmware: downloading from $REPO@$COMMIT"
while read -r expected name; do
    [ -n "$name" ] || continue
    path="$OUT/$name"
    if [ -s "$path" ] && printf '%s  %s\n' "$expected" "$path" | sha256sum -c - >/dev/null; then
        echo "munch-gpu-firmware: keeping verified $name"
        continue
    fi
    curl --fail --location --retry 5 --retry-delay 2 \
        --output "$tmp/$name" "$RAW_BASE/$name"
    printf '%s  %s\n' "$expected" "$tmp/$name" | sha256sum -c - >/dev/null \
        || die "downloaded SHA-256 mismatch: $name"
    install -m 0644 "$tmp/$name" "$path"
    echo "munch-gpu-firmware: installed $name"
done < "$MANIFEST"

check_files || die 'downloaded firmware did not pass the manifest'
echo "munch-gpu-firmware: verified $OUT"
