#!/bin/sh
# Pull only the device-local Adreno 650 firmware needed by the Munch GPU port.
# Run from the repository root with a rooted Android phone connected over adb.
set -eu
umask 077

[ "$(adb get-state 2>/dev/null || true)" = device ] || {
    echo 'adb device is not connected' >&2
    exit 1
}
[ "$(adb shell getprop ro.product.device | tr -d '\r')" = munch ] || {
    echo 'Connected device is not Xiaomi munch' >&2
    exit 1
}

out="$(pwd)/fw-gpu"
mkdir -p "$out"
for name in a650_gmu.bin a650_sqe.fw a650_zap.mdt a650_zap.b00 a650_zap.b01 a650_zap.b02; do
    found=
    for dir in /vendor/firmware /vendor/firmware_mnt/image /odm/firmware /lib/firmware/qcom; do
        path="$dir/$name"
        if adb shell "su -c 'test -r $path'" >/dev/null 2>&1; then
            found=$path
            break
        fi
    done
    [ -n "$found" ] || { echo "Missing $name on device" >&2; exit 1; }
    adb exec-out "su -c 'cat $found'" > "$out/$name"
    test -s "$out/$name"
    echo "Pulled $name from $found"
done
(cd "$out" && sha256sum ./*)
"$(dirname "$0")/gpu/fetch-firmware.sh" --check
echo "Firmware saved in $out (ignored by git)."
