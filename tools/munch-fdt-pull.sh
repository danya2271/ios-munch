#!/bin/sh
# Capture only the live Android FDT exposed by the kernel.
#
# The default output is outside the repository because the FDT can contain
# boot arguments and other device-specific metadata. This helper never reads
# block devices, partitions, mounts, or writes to the phone.
set -eu
umask 077

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-/tmp/munch-live.dtb}
output_abs=$(realpath -m -- "$output")

case "$output_abs" in
	"$repo_root"|"$repo_root"/*)
		echo "refusing to place a device dump inside the repository: $output_abs" >&2
		exit 1
		;;
esac

command -v adb >/dev/null 2>&1 || {
	echo "adb is required" >&2
	exit 1
}

[ "$(adb get-state 2>/dev/null || true)" = device ] || {
	echo "no Android device is ready over adb" >&2
	exit 1
}

[ "$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r')" = munch ] || {
	echo "refusing a non-munch device" >&2
	exit 1
}

adb shell 'su -c "test -r /sys/firmware/fdt"' >/dev/null 2>&1 || {
	echo "root-readable /sys/firmware/fdt is unavailable" >&2
	exit 1
}

mkdir -p "$(dirname -- "$output_abs")"
temporary="$output_abs.tmp.$$"
trap 'rm -f "$temporary"' EXIT HUP INT TERM

adb exec-out su -c 'cat /sys/firmware/fdt' > "$temporary"
[ "$(wc -c < "$temporary")" -ge 40 ] || {
	echo "captured FDT is too small" >&2
	exit 1
}

if command -v dtc >/dev/null 2>&1; then
	dtc -q -I dtb -O dts -o /dev/null "$temporary" || {
		echo "captured file is not a valid device tree blob" >&2
		exit 1
	}
fi

mv -f "$temporary" "$output_abs"
trap - EXIT HUP INT TERM
chmod 600 "$output_abs"
printf 'captured live Munch FDT: %s (%s bytes)\n' "$output_abs" "$(wc -c < "$output_abs")"
