#!/system/bin/sh
# Read-only Android touchscreen inventory. No sysfs/proc control writes.
set -eu
umask 077
[ "$(id -u)" = 0 ] || { echo 'Run as root.' >&2; exit 1; }
[ "$(getprop ro.product.device)" = munch ] || { echo 'Expected munch.' >&2; exit 1; }
out="/data/local/tmp/munch-touch-info-$(date +%Y%m%d-%H%M%S)"
mkdir "$out"
uname -a > "$out/kernel.txt"
getprop ro.build.version.incremental > "$out/build.txt"
cat /proc/bus/input/devices > "$out/input-devices.txt"
ls -la /sys/bus/spi/devices > "$out/spi-devices.txt" 2>&1 || true
dev=/sys/bus/spi/devices/spi1.0
ls -la "$dev/" > "$out/spi-touch-attributes.txt" 2>&1 || true
readlink -f "$dev/driver" > "$out/spi-touch-driver.txt" 2>&1 || true
for name in modalias uevent fts_fw_version fts_tpfwver fts_driverinfo fts_driver_info fts_ic_info fts_tp_id tp_info touch_info; do
    [ -f "$dev/$name" ] || continue
    cat "$dev/$name" > "$out/attr-$name.txt" 2> "$out/attr-$name.err" || true
done
for name in /proc/tp_info /proc/touchpanel/tp_info /proc/touchpanel/vendor_id /proc/touchpanel/fw_version; do
    [ -f "$name" ] || continue
    base=$(echo "$name" | tr / _)
    cat "$name" > "$out/$base.txt" 2> "$out/$base.err" || true
done
dmesg | grep -iE 'focaltech|fts_ts|fts_spi|fts_core|\[FTS\]|990000[.]spi' > "$out/touch-dmesg.txt" || true
if [ -r /proc/config.gz ]; then
    gzip -dc /proc/config.gz | grep -iE 'CONFIG_.*(FOCAL|FTS|TOUCHSCREEN)' > "$out/touch-config.txt" || true
fi
for name in /sys/kernel/debug/gpio /sys/kernel/debug/regulator/regulator_summary /sys/kernel/debug/clk/clk_summary; do
    [ -r "$name" ] || continue
    base=$(echo "$name" | tr / _)
    cat "$name" > "$out/$base.txt" 2> "$out/$base.err" || true
done
for dir in /sys/kernel/debug/pinctrl/*; do
    [ -f "$dir/pinmux-pins" ] || continue
    base=$(basename "$dir")
    cat "$dir/pinmux-pins" > "$out/pinmux-$base.txt" 2>/dev/null || true
    [ ! -f "$dir/pinconf-pins" ] || cat "$dir/pinconf-pins" > "$out/pinconf-$base.txt" 2>/dev/null || true
done
find /vendor/firmware /vendor/firmware_mnt/image /vendor/lib/modules -type f 2>/dev/null |
    grep -iE '(focaltech|(^|/)[^/]*(fts|touch|tp_fw)[^/]*\.(bin|fw|img|ko)$)' > "$out/touch-firmware-paths.txt" || true
mkdir "$out/firmware"
n=0
while IFS= read -r name; do
    [ -r "$name" ] || continue
    n=$((n + 1))
    dest="$out/firmware/$n-$(basename "$name")"
    cp "$name" "$dest"
    printf '%s\t%s\n' "$dest" "$name" >> "$out/firmware-map.txt"
done < "$out/touch-firmware-paths.txt"
if command -v timeout >/dev/null && command -v getevent >/dev/null; then
    timeout 3 getevent -lp /dev/input/event2 > "$out/touch-axes.txt" 2>&1 || true
fi
archive="/sdcard/Download/$(basename "$out").tar.gz"
tar -czf "$archive" -C "$(dirname "$out")" "$(basename "$out")"
echo "Created: $archive"
echo 'Only Android touch information and matching local firmware files were read.'
