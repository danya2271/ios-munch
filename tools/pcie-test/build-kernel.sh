#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
source "$ROOT/xnu-env.sh"
SOURCE=$XNU_SRC
if [ ! -f "$SOURCE/bsd/conf/files" ]; then
	printf 'pcie-build: missing staged XNU source: %s\n' "$SOURCE" >&2
	exit 1
fi
mkdir -p "$ROOT/xnu-deps/build"
OUTPUT=$(mktemp -d "$ROOT/xnu-deps/build/pcie-full-kernel.XXXXXXXX")
printf 'pcie-build: isolated source and output: %s\n' "$OUTPUT"
rsync -a --exclude=/.git --exclude=/BUILD "$SOURCE/" "$OUTPUT/src/"
for name in gic_v3_irq.c gic_v3_spi.inc gic_v3_spi.h; do
	cp "$ROOT/xnu/files/osfmk/arm64/$name" "$OUTPUT/src/osfmk/arm64/$name"
done
for name in hakios.pcie hakios.pcie.kona hakios.pcie.kona.platform hakios.pcie.kona.mmio hakios.pcie.kona.sid hakios.dma.pagetable hakios.dma.space hakios.dma.endpoint hakios.dma.cache hakios.dma.memory hakios.dma.arena hakios.msi.pool hakios.msi hakios.msi.kona hakios.mhi hakios.mhi.control hakios.mhi.boot hakios.mhi.runtime hakios.mhi.worker hakios.ipcr hakios.qrtr.wire hakios.qrtr.mhi hakios.qmi hakios.qca6390 hakios.wifi hakios.lx; do
	cp "$ROOT/xnu/files/bsd/dev/$name.c" "$OUTPUT/src/bsd/dev/$name.c"
done
for name in hakios.pcie hakios.pcie.kona hakios.pcie.kona.platform hakios.pcie.kona.mmio hakios.pcie.kona.sid hakios.gic hakios.dma.pagetable hakios.dma.space hakios.dma.endpoint hakios.dma.memory hakios.dma.arena hakios.msi.pool hakios.msi hakios.msi.kona hakios.mhi hakios.mhi.boot hakios.mhi.runtime hakios.mhi.worker hakios.ipcr hakios.qrtr.wire hakios.qrtr.mhi hakios.qrtr hakios.qmi hakios.wire hakios.qca6390; do
	cp "$ROOT/xnu/files/bsd/sys/$name.h" "$OUTPUT/src/bsd/sys/$name.h"
done
python3 - "$OUTPUT/src/bsd/conf/files" <<'PY'
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1])
text = manifest.read_text()
entries = {line.split()[0] for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")}
for name in ("hakios.pcie", "hakios.pcie.kona", "hakios.pcie.kona.platform", "hakios.pcie.kona.mmio", "hakios.pcie.kona.sid", "hakios.dma.pagetable", "hakios.dma.space", "hakios.dma.endpoint", "hakios.dma.cache", "hakios.dma.memory", "hakios.dma.arena", "hakios.msi.pool", "hakios.msi", "hakios.msi.kona", "hakios.mhi", "hakios.mhi.control", "hakios.mhi.boot", "hakios.mhi.runtime", "hakios.mhi.worker", "hakios.ipcr", "hakios.qrtr.wire", "hakios.qrtr.mhi", "hakios.qca6390"):
    entry = f"bsd/dev/{name}.c"
    if entry not in entries:
        text += f"\n{entry}\tstandard\n"
manifest.write_text(text)
PY
XNU_SRC="$OUTPUT/src" MUNCH_STAGE=0 bash "$ROOT/xnu-build.sh" MAKEJOBS="--jobs=${JOBS:-4}" "$@" > "$OUTPUT/build.log" 2>&1 || {
	tail -70 "$OUTPUT/build.log"
	printf 'pcie-build: FAIL; full log: %s/build.log\n' "$OUTPUT" >&2
	exit 1
}
printf 'pcie-build: PASS; full log: %s/build.log\n' "$OUTPUT"
