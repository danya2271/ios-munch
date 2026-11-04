#!/usr/bin/env python3
import json
import pathlib
import shlex
import subprocess
import sys


def main():
    root, source, objects, output = map(pathlib.Path, sys.argv[1:])
    candidates = [
        shlex.split(line)
        for line in (output / "commands.txt").read_text().splitlines()
        if line.startswith("xnu-clang -c ")
    ]
    if len(candidates) != 1:
        raise SystemExit("pcie-kernel: expected exactly one compiler command from make -n")
    arguments = candidates[0]
    original = str(source / "bsd/dev/hakios.wifi.c")
    if arguments.count(original) != 1:
        raise SystemExit("pcie-kernel: unexpected template source")
    flags = []
    remaining = iter(arguments[1:])
    for argument in remaining:
        if argument in ("-MF", "-MT", "-MQ", "-o"):
            next(remaining)
        elif argument in ("-MD", "-MMD", "-MP", original):
            continue
        elif argument == "-I.":
            flags.append(f"-I{objects}")
        else:
            flags.append(argument)
    commands = []
    failed = []
    names = ("hakios.pcie", "hakios.pcie.kona", "hakios.pcie.kona.platform", "hakios.pcie.kona.mmio", "hakios.pcie.kona.sid", "hakios.dma.pagetable", "hakios.dma.space",
             "hakios.dma.endpoint", "hakios.dma.cache", "hakios.dma.memory", "hakios.dma.arena", "hakios.msi.pool",
             "hakios.msi", "hakios.mhi", "hakios.mhi.control", "hakios.mhi.boot", "hakios.mhi.runtime", "hakios.mhi.worker", "hakios.ipcr",
             "hakios.qrtr.wire", "hakios.qrtr.mhi",
             "hakios.qmi", "hakios.qca6390", "hakios.wifi", "hakios.lx")
    for name in names:
        command = [arguments[0], f"-I{root / 'xnu/files/bsd'}", *flags,
                   str(root / "xnu/files/bsd/dev" / f"{name}.c"), "-o", str(output / f"{name}.o")]
        commands.append(command)
        result = subprocess.run(command, cwd=output, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (output / f"{name}.log").write_text(result.stdout)
        print(f"{name}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
        if result.returncode != 0:
            failed.append(name)
            print(result.stdout, end="", flush=True)
    (output / "compile-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
