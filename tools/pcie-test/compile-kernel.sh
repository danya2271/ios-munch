#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
source "$ROOT/xnu-env.sh"
OBJECTS="$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT"
if [ ! -f "$OBJECTS/Makefile" ]; then
	printf 'pcie-kernel: missing configured XNU object tree: %s\n' "$OBJECTS" >&2
	exit 1
fi
mkdir -p "$ROOT/xnu-deps/build"
OUTPUT=$(mktemp -d "$ROOT/xnu-deps/build/pcie-kernel.XXXXXXXX")
printf 'pcie-kernel: isolated output: %s\n' "$OUTPUT"
make -n -C "$OBJECTS" SRCROOT="$XNU_SRC" OBJROOT="$XNU_SRC/BUILD/obj" \
    SYMROOT="$XNU_SRC/BUILD/sym" SDKROOT=iphoneos \
    ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT \
    CURRENT_ARCH_CONFIG=ARM64 CURRENT_MACHINE_CONFIG=BCM2837 CURRENT_KERNEL_CONFIG=DEVELOPMENT \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
    CC=xnu-clang CXX=xnu-clang++ BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
    -W "$XNU_SRC/bsd/dev/hakios.wifi.c" hakios.wifi.o > "$OUTPUT/commands.txt"
python3 "$HERE/compile-kernel.py" "$ROOT" "$XNU_SRC" "$OBJECTS" "$OUTPUT"
