#!/bin/sh
# Pull the QCA6390 firmware used by the Munch Wi-Fi port.
# Run from any directory with a rooted Munch connected over adb.
set -eu
umask 077

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

[ "$(adb get-state 2>/dev/null || true)" = device ] || {
	echo 'adb device is not connected' >&2
	exit 1
}
[ "$(adb shell getprop ro.product.device | tr -d '\r')" = munch ] || {
	echo 'Connected device is not Xiaomi munch' >&2
	exit 1
}

out="$root/fw-wifi/qca6390"
tmp="$out/.tmp.$$"
rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

names='amss20.bin m3.bin bd_l11rgl.elf'
for name in $names; do
	found=
	for dir in /vendor/firmware_mnt/image /vendor/firmware /odm/firmware /lib/firmware; do
		for path in "$dir/qca6390/$name" "$dir/$name"; do
			if adb shell "su -c 'test -r $path'" >/dev/null 2>&1; then
				found=$path
				break 2
			fi
		done
	done
	[ -n "$found" ] || { echo "Missing qca6390/$name on device" >&2; exit 1; }
	adb exec-out "su -c 'cat $found'" > "$tmp/$name"
	test -s "$tmp/$name"
	echo "Pulled qca6390/$name from $found"
done

mkdir -p "$out"
for name in $names; do
	install -m 0644 "$tmp/$name" "$out/$name"
done
(cd "$out" && sha256sum $names)
echo "Firmware saved in $out (ignored by git)."
