#!/usr/bin/env python3
import argparse
from pathlib import Path


QCA_SOURCES = (
    "hakios.wifi",
    "hakios.lx",
    "hakios.qmi",
    "hakios.pcie",
    "hakios.pcie.kona",
    "hakios.pcie.kona.platform",
    "hakios.pcie.kona.mmio",
    "hakios.pcie.kona.sid",
    "hakios.dma.pagetable",
    "hakios.dma.space",
    "hakios.dma.endpoint",
    "hakios.dma.cache",
    "hakios.dma.memory",
    "hakios.dma.arena",
    "hakios.msi.pool",
    "hakios.msi",
    "hakios.msi.kona",
    "hakios.mhi",
    "hakios.mhi.control",
    "hakios.mhi.boot",
    "hakios.mhi.runtime",
    "hakios.mhi.worker",
    "hakios.ipcr",
    "hakios.qrtr.wire",
    "hakios.qrtr.mhi",
    "hakios.qca6390",
)


def register_sources(root):
    manifest = root / "bsd/conf/files"
    if not manifest.is_file():
        raise ValueError(f"missing XNU source manifest: {manifest}")
    entries = [f"bsd/dev/{name}.c" for name in QCA_SOURCES]
    absent = [entry for entry in entries if not (root / entry).is_file()]
    if absent:
        raise ValueError(f"missing staged QCA source: {absent[0]}")

    text = manifest.read_text()
    present = {
        line.split()[0]
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    missing = [entry for entry in entries if entry not in present]
    if missing:
        separator = "" if not text or text.endswith("\n") else "\n"
        text += separator + "".join(f"{entry}\tstandard\n" for entry in missing)
        manifest.write_text(text)
    return tuple(missing)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("xnu_source", type=Path)
    arguments = parser.parse_args()
    try:
        added = register_sources(arguments.xnu_source)
    except ValueError as error:
        parser.error(str(error))
    print(f"registered {len(added)} QCA XNU source files")


if __name__ == "__main__":
    main()
