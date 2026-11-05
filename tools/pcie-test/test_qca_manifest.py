#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/register-qca-sources.py"
SPEC = importlib.util.spec_from_file_location("register_qca_sources", SOURCE)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
QCA_DRIVER = ROOT / "xnu/files/bsd/dev/hakios.qca6390.c"
QCA_HEADER = ROOT / "xnu/files/bsd/sys/hakios.qca6390.h"


def run():
    qca_source = QCA_DRIVER.read_text()
    qca_header = QCA_HEADER.read_text()
    assert "hakios_qca6390_dma_register" in qca_source
    for legacy in ("SMMU_", "qca_smmu", "ml_io_map_unmappable", "kvtophys", "hakios_smmu_lock",
                   "hakios_qca6390_dma_map", "hakios_qca6390_dma_stop"):
        assert legacy not in qca_source and legacy not in qca_header

    with tempfile.TemporaryDirectory(prefix="qca-manifest-test-") as temporary:
        root = Path(temporary)
        manifest = root / "bsd/conf/files"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("# test manifest\n")
        for name in MODULE.QCA_SOURCES:
            source = root / "bsd/dev" / f"{name}.c"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("")

        first = MODULE.register_sources(root)
        second = MODULE.register_sources(root)
        text = manifest.read_text()
        assert len(first) == len(MODULE.QCA_SOURCES)
        assert second == ()
        for name in MODULE.QCA_SOURCES:
            entry = f"bsd/dev/{name}.c"
            assert text.splitlines().count(f"{entry}\tstandard") == 1

    with tempfile.TemporaryDirectory(prefix="qca-manifest-missing-") as temporary:
        root = Path(temporary)
        manifest = root / "bsd/conf/files"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("base.c standard\n")
        try:
            MODULE.register_sources(root)
        except ValueError as error:
            assert "missing staged QCA source" in str(error)
        else:
            raise AssertionError("missing QCA sources must be rejected")
        assert manifest.read_text() == "base.c standard\n"

    print("qca-manifest-test: PASS (provider-only QCA DMA, source check, idempotent registration)")


if __name__ == "__main__":
    run()
