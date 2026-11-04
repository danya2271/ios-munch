#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD=$ROOT/xnu-deps/build/ath11k
SHIMS=$ROOT/wifi/lx
FREEBSD=$BUILD/freebsd/sys
for INPUT in "$BUILD/src/.release" "$BUILD/freebsd/.release" "$BUILD/gen/opt_global.h" "$BUILD/src/ath11k/qmi.c"; do
	if [ ! -f "$INPUT" ]; then
		printf 'ath11k-pci-check: prepare pinned ath11k/LinuxKPI inputs with tools/mk-ath11k.sh first; missing %s\n' "$INPUT" >&2
		exit 1
	fi
done
OUTPUT=$(mktemp -d "$ROOT/xnu-deps/build/ath11k-pci-check.XXXXXXXX")
printf 'ath11k-pci-check: isolated object output: %s\n' "$OUTPUT"
cp "$BUILD/src/.release" "$OUTPUT/linux-release.txt"
cp "$BUILD/freebsd/.release" "$OUTPUT/freebsd-release.txt"
FLAGS=(-target arm64-apple-ios16.5 -mkernel -mgeneral-regs-only -fno-builtin -fno-common
    -ffreestanding -fno-strict-aliasing -funsigned-char -nostdinc -std=gnu17 -O2 -g -w
    -D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
    -I"$SHIMS/include" -I"$BUILD/gen" -I"$FREEBSD" -I"$FREEBSD/contrib/ck/include"
    -I"$FREEBSD/compat/linuxkpi/common/include" -I"$FREEBSD/compat/linuxkpi/dummy/include"
    -include "$FREEBSD/compat/linuxkpi/common/include/linux/kconfig.h" -DLINUXKPI_VERSION=70000
    -I"$BUILD/src/ath11k" -I"$BUILD/src" -I"$ROOT/xnu/files/bsd" -include "$SHIMS/include/lx_prelude.h"
    -DCONFIG_ATH11K_DEBUG '-DKBUILD_MODNAME="ath11k"'
    "-DLX_HAKIOS_LX_H=\"$ROOT/xnu/files/bsd/sys/hakios.lx.h\""
    "-DLX_HAKIOS_QMI_H=\"$ROOT/xnu/files/bsd/sys/hakios.qmi.h\"")
compile() {
	local PROFILE=$1 NAME=$2 SOURCE=$3
	printf '%q ' clang "${FLAGS[@]}" -D"CONFIG_ATH11K_$PROFILE" -c "$SOURCE" -o "$OUTPUT/$NAME.o" >> "$OUTPUT/commands.txt"
	printf '\n' >> "$OUTPUT/commands.txt"
	clang "${FLAGS[@]}" -D"CONFIG_ATH11K_$PROFILE" -c "$SOURCE" -o "$OUTPUT/$NAME.o" > "$OUTPUT/$NAME.log" 2>&1 || {
		cat "$OUTPUT/$NAME.log"
		exit 1
	}
	llvm-nm -u "$OUTPUT/$NAME.o" > "$OUTPUT/$NAME.imports"
}
compile PCI qca-pci "$SHIMS/src/lx_qca_pci.c"
compile PCI qca-hif "$SHIMS/src/lx_qca_hif.c"
compile PCI qca-dma "$SHIMS/src/lx_qca_dma.c"
compile PCI qca-entry "$SHIMS/src/lx_qca_entry.c"
compile PCI qca-runtime "$SHIMS/src/lx_qca_runtime.c"
compile AHB qca-hif-ahb "$SHIMS/src/lx_qca_hif.c"
compile AHB qca-ahb "$SHIMS/src/lx_qca_pci.c"
llvm-nm "$OUTPUT/qca-ahb.o" > "$OUTPUT/qca-ahb.symbols"
! rg -q '_lx_qca_pci_' "$OUTPUT/qca-ahb.symbols"
compile PCI qmi-pci "$BUILD/src/ath11k/qmi.c"
compile AHB qmi-ahb "$BUILD/src/ath11k/qmi.c"
compile AHB base-ahb "$SHIMS/src/lx_base.c"
FLAGS+=(-DLX_QCA_NATIVE_DMA)
compile PCI base-pci "$SHIMS/src/lx_base.c"
for unit in hal ce dp dp_rx dp_tx; do
	compile PCI "$unit-pci" "$BUILD/src/ath11k/$unit.c"
done
! rg -q '_(pmap_kextract|lkpi_dma_map_phys|lx_os_radio_memory)$' "$OUTPUT/hal-pci.imports" "$OUTPUT/ce-pci.imports" "$OUTPUT/dp-pci.imports" "$OUTPUT/dp_rx-pci.imports" "$OUTPUT/dp_tx-pci.imports" "$OUTPUT/base-pci.imports"
llvm-nm --defined-only "$OUTPUT/base-pci.o" > "$OUTPUT/base-pci.symbols"
! rg -q '_(pmap_kextract|lkpi_dma_map_phys|linux_dma_alloc_coherent|linuxkpi_dev_alloc_skb|linuxkpi_kfree_skb)$' "$OUTPUT/base-pci.symbols"
FLAGS=("${FLAGS[@]:0:${#FLAGS[@]}-1}")
rg -q '^_hakios_ipcr_qmi_handle_init$' "$OUTPUT/qmi-pci.imports"
rg -q '^_qmi_send_request$' "$OUTPUT/qmi-pci.imports"
! rg -q '^_qmi_handle_init$|^_lx_qmi_send_request$' "$OUTPUT/qmi-pci.imports"
rg -q '^_qmi_handle_init$' "$OUTPUT/qmi-ahb.imports"
rg -q '^_lx_qmi_send_request$' "$OUTPUT/qmi-ahb.imports"
if clang "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -fsyntax-only "$SHIMS/src/lx_bus.c" > "$OUTPUT/refused-ahb.log" 2>&1; then
	printf 'ath11k-pci-check: AHB bus incorrectly compiled as PCI\n' >&2
	exit 1
fi
rg -q 'not a native QCA6390 PCI backend' "$OUTPUT/refused-ahb.log"
if clang "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -fsyntax-only "$SHIMS/src/lx_base.c" > "$OUTPUT/refused-ahb-dma.log" 2>&1; then
	printf 'ath11k-pci-check: AHB DMA pool incorrectly compiled as PCI\n' >&2
	exit 1
fi
rg -q 'native endpoint DMA binding is required' "$OUTPUT/refused-ahb-dma.log"
printf 'ath11k-pci-check: PASS; native PCI entry/DMA/SKB/runtime/HIF and HAL/CE/DP objects; upstream PCI/PCIC/MHI ownership API remains excluded\n'
