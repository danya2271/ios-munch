#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/hakios-pcie-test.XXXXXXXX")
trap 'rm -rf "$BUILD"' EXIT
mkdir -p "$BUILD/include/sys"
mkdir -p "$BUILD/osfmk/arm64"
ln -s "$ROOT/xnu/files/osfmk/arm64/gic_v3_spi.h" "$BUILD/osfmk/arm64/gic_v3_spi.h"
for header in hakios.pcie.h hakios.pcie.kona.h hakios.pcie.kona.platform.h hakios.pcie.kona.mmio.h hakios.pcie.kona.sid.h hakios.gic.h hakios.mmio.h hakios.dma.pagetable.h hakios.dma.space.h hakios.dma.endpoint.h hakios.dma.memory.h hakios.dma.arena.h hakios.msi.h hakios.msi.pool.h hakios.msi.kona.h hakios.qca6390.h hakios.mhi.h hakios.mhi.boot.h hakios.mhi.runtime.h hakios.mhi.worker.h hakios.ipcr.h hakios.qrtr.wire.h hakios.qrtr.mhi.h hakios.qrtr.h hakios.qmi.h hakios.wire.h; do
	ln -s "$ROOT/xnu/files/bsd/sys/$header" "$BUILD/include/sys/$header"
done
ln -s "$ROOT/wifi/lx/include/lx_qca_pci.h" "$BUILD/include/lx_qca_pci.h"
ln -s "$ROOT/wifi/lx/include/lx_qca_hif.h" "$BUILD/include/lx_qca_hif.h"
ln -s "$ROOT/wifi/lx/include/lx_qca_runtime.h" "$BUILD/include/lx_qca_runtime.h"
mkdir -p "$BUILD/include/linux"
ln -s "$ROOT/wifi/lx/include/linux/dma-mapping.h" "$BUILD/include/linux/dma-mapping.h"
python3 "$HERE/test_qca_manifest.py"
FLAGS=(-std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -pthread
    -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$ROOT/xnu/files/osfmk/arm64" -I"$BUILD/include" \
    "$HERE/test_gic_spi.c" -o "$BUILD/test-gic-spi"
"$BUILD/test-gic-spi"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_pcie.c" "$ROOT/xnu/files/bsd/dev/hakios.pcie.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.pcie.kona.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.pcie.kona.platform.c" -o "$BUILD/test-pcie"
"$BUILD/test-pcie"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_kona_mmio.c" "$ROOT/xnu/files/bsd/dev/hakios.pcie.kona.mmio.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.pcie.kona.sid.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.pcie.kona.c" -o "$BUILD/test-kona-mmio"
"$BUILD/test-kona-mmio"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_dma_pagetable.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.pagetable.c" \
    -o "$BUILD/test-dma-pagetable"
"$BUILD/test-dma-pagetable"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_dma_space.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.space.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.dma.pagetable.c" -o "$BUILD/test-dma-space"
"$BUILD/test-dma-space"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_endpoint_resources.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.endpoint.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.dma.cache.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.space.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.dma.pagetable.c" "$ROOT/xnu/files/bsd/dev/hakios.msi.pool.c" \
    -o "$BUILD/test-endpoint-resources"
"$BUILD/test-endpoint-resources"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" -I"$ROOT/xnu/files/osfmk" \
    "$HERE/test_msi_kona.c" "$ROOT/xnu/files/bsd/dev/hakios.msi.kona.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.msi.c" "$ROOT/xnu/files/bsd/dev/hakios.msi.pool.c" \
    -o "$BUILD/test-msi-kona"
"$BUILD/test-msi-kona"
for PAGE_SHIFT in 12 14; do
	"${CC:-cc}" "${FLAGS[@]}" -DPCIE_TEST_NATIVE_PAGE_SHIFT="$PAGE_SHIFT" -I"$HERE/shim" -I"$BUILD/include" \
	    "$HERE/test_dma_memory.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.memory.c" \
	    "$ROOT/xnu/files/bsd/dev/hakios.dma.endpoint.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.space.c" \
	    "$ROOT/xnu/files/bsd/dev/hakios.dma.pagetable.c" -o "$BUILD/test-dma-memory-$PAGE_SHIFT"
	"$BUILD/test-dma-memory-$PAGE_SHIFT"
done
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_transport.c" "$ROOT/xnu/files/bsd/dev/hakios.msi.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.mhi.c" "$ROOT/xnu/files/bsd/dev/hakios.qrtr.wire.c" \
    -o "$BUILD/test-transport"
"$BUILD/test-transport"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_mhi_control.c" "$ROOT/xnu/files/bsd/dev/hakios.mhi.control.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.mhi.c" -o "$BUILD/test-mhi-control"
"$BUILD/test-mhi-control"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_mhi_boot.c" "$ROOT/xnu/files/bsd/dev/hakios.mhi.boot.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.mhi.runtime.c" -o "$BUILD/test-mhi-boot"
"$BUILD/test-mhi-boot"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_qrtr_mhi.c" "$ROOT/xnu/files/bsd/dev/hakios.qrtr.mhi.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.qrtr.wire.c" "$ROOT/xnu/files/bsd/dev/hakios.qmi.c" -o "$BUILD/test-qrtr-mhi"
"$BUILD/test-qrtr-mhi"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_ipcr.c" "$HERE/test_pci_qmi_header.c" "$ROOT/xnu/files/bsd/dev/hakios.ipcr.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.mhi.c" "$ROOT/xnu/files/bsd/dev/hakios.qrtr.mhi.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.qrtr.wire.c" "$ROOT/xnu/files/bsd/dev/hakios.qmi.c" -o "$BUILD/test-ipcr"
"$BUILD/test-ipcr"
"${CC:-cc}" "${FLAGS[@]}" -I"$HERE/shim" -I"$BUILD/include" \
    "$HERE/test_mhi_worker.c" "$HERE/thread_call_shim.c" "$ROOT/xnu/files/bsd/dev/hakios.mhi.worker.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.ipcr.c" "$ROOT/xnu/files/bsd/dev/hakios.mhi.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.mhi.control.c" "$ROOT/xnu/files/bsd/dev/hakios.qrtr.mhi.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.qrtr.wire.c" "$ROOT/xnu/files/bsd/dev/hakios.qmi.c" -o "$BUILD/test-mhi-worker"
"$BUILD/test-mhi-worker"
"${CC:-cc}" "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -I"$HERE/ath11k" -I"$BUILD/include" \
    "$HERE/test_ath11k_pci.c" "$ROOT/wifi/lx/src/lx_qca_pci.c" -o "$BUILD/test-ath11k-pci"
"$BUILD/test-ath11k-pci"
"${CC:-cc}" "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -I"$HERE/ath11k" -I"$BUILD/include" \
    "$HERE/test_qca_hif.c" "$ROOT/wifi/lx/src/lx_qca_hif.c" -o "$BUILD/test-qca-hif"
"$BUILD/test-qca-hif"
"${CC:-cc}" "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -I"$BUILD/include" -I"$HERE/ath11k" -I"$HERE/shim" \
    "$HERE/test_dma_arena.c" "$ROOT/xnu/files/bsd/dev/hakios.dma.arena.c" \
    "$ROOT/wifi/lx/src/lx_qca_dma.c" -o "$BUILD/test-dma-arena"
"$BUILD/test-dma-arena"
"${CC:-cc}" "${FLAGS[@]}" -DCONFIG_ATH11K_PCI -I"$BUILD/include" -I"$HERE/ath11k" -I"$HERE/shim" \
    "$HERE/test_qca_runtime.c" "$ROOT/wifi/lx/src/lx_qca_runtime.c" "$ROOT/wifi/lx/src/lx_qca_pci.c" \
    "$ROOT/wifi/lx/src/lx_qca_hif.c" "$ROOT/wifi/lx/src/lx_qca_dma.c" \
    "$ROOT/xnu/files/bsd/dev/hakios.dma.arena.c" -o "$BUILD/test-qca-runtime"
"$BUILD/test-qca-runtime"
"${CC:-cc}" "${FLAGS[@]}" -std=gnu11 -DNO_ATH11K -I"$BUILD/include" \
    "$ROOT/tools/qmi-test/test_qmi.c" "$ROOT/xnu/files/bsd/dev/hakios.qmi.c" -o "$BUILD/test-qmi-regression"
"$BUILD/test-qmi-regression"
