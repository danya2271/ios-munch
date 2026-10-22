#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPO_ROOT=$(realpath "$REPO_ROOT")

KERNEL=""
DTB=""
CPUS=8
CP_MACHO=""
OUT=""
PREFIX="munch"
DRAM_BASE="0x100000000"
DRAM_SIZE="0xfcc00000"
CMDLINE="debug=0xc serial=3 cpus=${CPUS} keepsyms=1 rd=md0 -enable_kprintf_spam -noprogress"

usage() {
    cat <<EOF
Usage: tools/mkram-boot.sh --kernel KERNEL --dtb DTB --out OUTPUT_DIR
                            [--cpus 1|8] [--cp-macho MACHO] [--prefix NAME]
                            [--dram-base BASE] [--dram-size SIZE]
                            [--cmdline CMDLINE]

Builds a temporary Android boot image for the MUNCH XNU RAM-only shim.
The output directory receives an ADT, MUNCHXNU payload, and shim-boot.img.
The default root is a small diagnostic cpio that requires --cp-macho.

Examples:
  tools/mkram-boot.sh \\
    --kernel xnu/out-39853721117/kernel.development.munch \\
    --dtb /tmp/munch-live.dtb \\
    --out /tmp/munch-ram-8cpu \\
    --cp-macho /tmp/munch-init.macho

This script never runs fastboot, writes to storage, or accesses userdata.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --kernel) KERNEL=$2; shift 2 ;;
        --dtb) DTB=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;;
        --cp-macho) CP_MACHO=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --dram-base) DRAM_BASE=$2; shift 2 ;;
        --dram-size) DRAM_SIZE=$2; shift 2 ;;
        --cmdline) CMDLINE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() {
    echo "mkram-boot: $*" >&2
    exit 1
}

[[ $CPUS =~ ^[0-9]+$ ]] || die "--cpus must be a positive integer"
[[ $CPUS -gt 0 && $CPUS -le 8 ]] || die "--cpus must be 1..8"
[[ -n $KERNEL ]] || die "--kernel is required"
[[ -n $DTB ]] || die "--dtb is required"
[[ -n $OUT ]] || die "--out is required"
[[ -n $CP_MACHO ]] || die "--cp-macho is required for the RAM diagnostic root"

require_file() {
    [[ -f $1 && -r $1 ]] || die "missing unreadable/missing file: $1"
}
require_file "$KERNEL" "$DTB" "$CP_MACHO"

[[ $(file -b "$KERNEL") == *'Mach-O'* ]] || die "kernel is not Mach-O: $KERNEL"
[[ $(file -b "$CP_MACHO") == *'Mach-O'* ]] || die "cp-macho is not Mach-O: $CP_MACHO"

for tool in python3 mkbootimg unpack_bootimg dtc fdtdump cpio gzip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "missing required tool: $tool"
done

mkdir -p "$OUT" "$(dirname "$OUT")"
OUT=$(realpath "$OUT")

[[ ! -e $OUT/shim-boot.img ]] || die "refusing to overwrite $OUT/shim-boot.img"
[[ ! -e $OUT/payload.bin ]] || die "refusing to overwrite $OUT/payload.bin"
[[ ! -e $OUT/adt ]] || die "refusing to overwrite $OUT/adt"
[[ ! -e $OUT/root.cpio.gz ]] || die "refusing to overwrite $OUT/root.cpio.gz"

echo "mkram-boot: DTB=$DTB"
dtc -q -I dtb -O dts "$DTB" -o "$OUT/dtb.dts"
grep -q 'qcom,kona' "$OUT/dtb.dts" || grep -q 'xiaomi,munch' "$OUT/dtb.dts" || \
    grep -q 'qcom,sm8250' "$OUT/dtb.dts" || die "DTB does not look like MUNCH"

if grep -q 'linux,initrd-start' "$OUT/dtb.dts"; then
    echo "mkram-boot: warning: DTB contains old initrd chosen properties" >&2
fi

echo "mkram-boot: building diagnostic RAM root"
mkdir -p "$OUT/root/sbin" "$OUT/root/dev"
install -m 0755 "$CP_MACHO" "$OUT/root/sbin/launchd"
: > "$OUT/root/dev/console"
(cd "$OUT/root" && find . -print0 | sort -z | cpio --null -o --format=newc --quiet | gzip -9) > "$OUT/root.cpio.gz"

echo "mkram-boot: generating MUNCH ADT"
python3 "$REPO_ROOT/tools/mkadt.py" "$OUT/adt" --munch --ram-only \
    --dram-base "$DRAM_BASE" --dram-size "$DRAM_SIZE"

if [[ $CPUS -ne 1 && $CPUS -ne 8 ]]; then
    python3 - "$OUT/adt" "$CPUS" <<'PY'
import struct, sys
path = sys.argv[1]
cpus = int(sys.argv[2])
data = bytearray(open(path, 'rb').read())
print(f'warning: ADT helper cannot patch arbitrary CPU counts; use 1 or 8 (got {cpus})', file=sys.stderr)
if cpus != 1 and cpus != 8:
    raise SystemExit(2)
PY
fi

echo "mkram-boot: packing MUNCHXNU payload"
python3 "$REPO_ROOT/tools/mkpayload.py" "$KERNEL" "$OUT/adt" "$OUT/payload.bin" \
    --cmdline "$CMDLINE" --ramdisk "$OUT/root.cpio.gz"

echo "mkram-boot: packing Android boot image"
mkbootimg \
    --header_version 2 \
    --kernel "$REPO_ROOT/shim/out/Image" \
    --ramdisk "$OUT/payload.bin" \
    --dtb "$DTB" \
    --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
    --tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
    --os_version 17.0.0 --os_patch_level 2035-06 \
    --cmdline console=null \
    -o "$OUT/shim-boot.img"

echo "mkram-boot: verifying unpacked image"
rm -rf "$OUT/unpacked"
unpack_bootimg --boot_img "$OUT/shim-boot.img" --out "$OUT/unpacked" --format=info \
    > "$OUT/bootimg-info.txt"
cmp -s "$OUT/unpacked/ramdisk" "$OUT/payload.bin" || die "unpacked ramdisk differs from payload"
cmp -s "$OUT/unpacked/dtb" "$DTB" || die "unpacked DTB differs from input"

echo
echo "mkram-boot: outputs"
sha256sum "$OUT/shim-boot.img" "$OUT/payload.bin" "$OUT/adt" "$OUT/root.cpio.gz"
ls -lh "$OUT/shim-boot.img" "$OUT/payload.bin" "$OUT/adt" "$OUT/root.cpio.gz"
echo
echo "mkram-boot: test only with temporary fastboot boot:"
echo "  fastboot boot $OUT/shim-boot.img"
