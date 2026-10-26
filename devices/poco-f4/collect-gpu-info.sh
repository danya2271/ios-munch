#!/system/bin/sh
# Read-only munch GPU inventory and device-local A650 firmware collection.
set -eu
umask 077
[ "$(id -u)" = 0 ] || { echo 'Run as root.' >&2; exit 1; }
[ "$(getprop ro.product.device)" = munch ] || { echo 'Expected munch.' >&2; exit 1; }
out="/data/local/tmp/munch-gpu-info-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/firmware"
uname -a > "$out/kernel.txt"
getprop ro.build.version.incremental > "$out/build.txt"
for name in gpu_model chipid gpu_available_frequencies gpuclk min_gpuclk max_gpuclk thermal_pwrlevel num_pwrlevels; do
    path="/sys/class/kgsl/kgsl-3d0/$name"
    [ ! -r "$path" ] || cat "$path" > "$out/$name.txt" 2> "$out/$name.err" || true
done
cat /proc/iomem > "$out/iomem.txt" 2>/dev/null || true
dmesg | grep -iE 'adreno|kgsl|gmu|gpucc|a650|zap|smmu' > "$out/gpu-dmesg.txt" || true
: > "$out/firmware-map.txt"
: > "$out/missing-firmware.txt"
for name in a650_gmu.bin a650_sqe.fw a650_zap.mdt a650_zap.b00 a650_zap.b01 a650_zap.b02; do
    found=0
    for dir in /vendor/firmware /vendor/firmware_mnt/image /odm/firmware /lib/firmware/qcom; do
        path="$dir/$name"
        [ -f "$path" ] && [ -r "$path" ] || continue
        cp "$path" "$out/firmware/$name"
        printf '%s\t%s\n' "$name" "$path" >> "$out/firmware-map.txt"
        found=1
        break
    done
    [ "$found" = 1 ] || echo "$name" >> "$out/missing-firmware.txt"
done
(cd "$out/firmware" && for name in *; do [ ! -f "$name" ] || sha256sum "$name"; done) > "$out/firmware-sha256.txt"
for path in /sys/kernel/debug/clk/clk_summary /sys/kernel/debug/regulator/regulator_summary; do
    [ ! -r "$path" ] || cat "$path" > "$out/$(basename "$path").txt" 2>/dev/null || true
done
archive="/sdcard/Download/$(basename "$out").tar.gz"
tar -czf "$archive" -C "$(dirname "$out")" "$(basename "$out")"
echo "Created: $archive"
if [ -s "$out/missing-firmware.txt" ]; then
    echo 'Some firmware files were not found; see missing-firmware.txt in the archive.'
fi
echo 'Only GPU information and local A650 firmware were read; no device controls or partitions were written.'
