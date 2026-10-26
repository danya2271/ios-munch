#!/system/bin/sh
# Run as root on Android. Read boot inputs; never write a block device.
set -eu
umask 077

[ "$(id -u)" = 0 ] || { echo 'Run this script as root.' >&2; exit 1; }
[ "$(getprop ro.product.device)" = munch ] || {
    echo 'This collector requires a munch device.' >&2
    exit 1
}
slot=$(getprop ro.boot.slot_suffix)
case "$slot" in
    _a|_b) ;;
    *) echo 'Cannot determine the active slot.' >&2; exit 1 ;;
esac

out="/data/local/tmp/munch-boot-inputs-$(date +%Y%m%d-%H%M%S)"
mkdir "$out"
printf 'active_slot=%s\nfirmware=%s\n' "$slot" \
    "$(getprop ro.build.version.incremental)" > "$out/source.txt"

for name in xbl boot vendor_boot dtbo; do
    device="/dev/block/by-name/${name}${slot}"
    [ -b "$device" ] || { echo "Missing block device: $device" >&2; exit 1; }
    # The input is a partition; the output is a regular file in a new directory.
    dd if="$device" of="$out/${name}${slot}.img" bs=1048576
done

if [ -r /sys/firmware/fdt ]; then
    cat /sys/firmware/fdt > "$out/live.dtb"
fi
(cd "$out" && sha256sum ./*.img > SHA256SUMS)
archive="/sdcard/Download/$(basename "$out").tar.gz"
tar -C /data/local/tmp -czf "$archive" "$(basename "$out")"
echo "Saved: $archive"
echo 'Images remain local; do not commit firmware dumps.'
