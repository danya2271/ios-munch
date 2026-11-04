#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
source "$ROOT/xnu-env.sh"
OBJECTS="$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT"
if [ ! -f "$OBJECTS/Makefile" ]; then
	printf 'gic-check: missing configured osfmk object tree: %s\n' "$OBJECTS" >&2
	exit 1
fi
OUTPUT=$(mktemp -d "$ROOT/xnu-deps/build/gic-objects.XXXXXXXX")
printf 'gic-check: isolated output: %s\n' "$OUTPUT"
make -n -C "$OBJECTS" SRCROOT="$XNU_SRC" SOURCE="$XNU_SRC/osfmk/" OBJROOT="$XNU_SRC/BUILD/obj" \
    SYMROOT="$XNU_SRC/BUILD/sym" SDKROOT=iphoneos \
    ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT \
    CURRENT_ARCH_CONFIG=ARM64 CURRENT_MACHINE_CONFIG=BCM2837 CURRENT_KERNEL_CONFIG=DEVELOPMENT \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
    CC=xnu-clang CXX=xnu-clang++ BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
    -W "$XNU_SRC/osfmk/arm64/gic_v3_irq.c" gic_v3_irq.o > "$OUTPUT/commands.txt"
python3 - "$ROOT" "$XNU_SRC" "$OBJECTS" "$OUTPUT" <<'PY'
import json
import pathlib
import shlex
import subprocess
import sys

root, source, objects, output = map(pathlib.Path, sys.argv[1:])
commands = [shlex.split(line) for line in (output / 'commands.txt').read_text().splitlines()
            if line.startswith('xnu-clang -c ')]
if len(commands) != 1:
    raise SystemExit('gic-check: expected exactly one compiler command')
original = str(source / 'osfmk/arm64/gic_v3_irq.c')
if commands[0].count(original) != 1:
    raise SystemExit('gic-check: unexpected template source')
flags = []
arguments = iter(commands[0][1:])
for argument in arguments:
    if argument in ('-MF', '-MT', '-MQ', '-o'):
        next(arguments)
    elif argument in ('-MD', '-MMD', '-MP', original):
        continue
    elif argument.startswith('-I') and len(argument) > 2 and not pathlib.Path(argument[2:]).is_absolute():
        flags.append(f'-I{objects / argument[2:]}')
    else:
        flags.append(argument)
command = [commands[0][0], f'-I{root / "xnu/files/osfmk"}', f'-I{source / "osfmk"}', *flags,
           str(root / 'xnu/files/osfmk/arm64/gic_v3_irq.c'),
           '-o', str(output / 'gic_v3_irq.o')]
(output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=output, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(output / 'compile.log').write_text(result.stdout)
print(result.stdout, end='')
print(f'gic-check: {"PASS" if result.returncode == 0 else "FAIL"}')
sys.exit(result.returncode)
PY
