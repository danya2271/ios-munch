#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "xnu/register-qca-sources.py"
SPEC = importlib.util.spec_from_file_location("register_qca_sources", SOURCE)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def run():
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

    print("qca-manifest-test: PASS (complete source check, idempotent registration, no partial write)")


if __name__ == "__main__":
    run()
