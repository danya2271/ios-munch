# Munch Adreno 650 bring-up

Status: hardware profile inspection complete; GPU execution on munch is not
established. iOS UI and touch input are device-confirmed. Eight CPUs were
requested by the current image; the photograph alone does not establish the
number of running CPUs or hardware graphics acceleration.

## Candidate implementation

The local UMI kernel checkout at commit
6b6fb2b0c5a1720496ec71e88c95d2a45a0046a4 contains an Adreno 650/Kona path in
`xnu/files/bsd/dev/hk_adreno.c`. Its `docs/umi-gpu.md` records GMU startup,
command execution, a three-band drawing test, and Mesa FD650 CoreAnimation.
The matching userland reference is commit
1ce76c12717e2926651afa847a7e8e5307ab2156. These are reference-port results,
not evidence that GPU initialization has succeeded on munch.

The captured munch live DT matches these UMI prerequisites:

| Property | Munch observation |
| --- | --- |
| GPU chip ID | 0x06050002, Adreno 650 |
| GPUCC | qcom,gpucc-kona at 0x03d90000 |
| GPU register base | 0x03d00000 |
| GMU register base | 0x03d6a000 |
| GMU PDC config / sequencer | 0x0b290000 / 0x0b490000 |
| GMU kernel stream / mask | 5 / 0x400 |
| Graphics user stream / mask | 0 / 0x401 |
| Zap reserved region | 0x8681a000, size 0x2000 |

Reproduce selected checks with `python3 gpu/inspect-profile.py captured-live.dtb`.
The generated `gpu/observed.json` records these properties. Matching addresses
alone do not establish identical power constraints or firmware compatibility.
UMI's default 587 MHz operating point must not be assumed to be a verified
munch setting. Start with a supported low operating point after inspecting the
munch table and Android clock inventory.

## Device inputs

`collect-gpu-info.sh` reads Android KGSL identity/frequency attributes, selected
GPU kernel messages, optional existing debugfs summaries, and these device-local
firmware files: a650_gmu.bin, a650_sqe.fw, a650_zap.mdt, a650_zap.b00, .b01 and
.b02. It records origins and SHA256 digests in a small archive. It never mounts
debugfs, writes controls, or modifies device partitions. Missing files are
listed explicitly; this is not a complete firmware-validity check.

## Integration requirements

The currently working prebuilt contains the Redwood A642L implementation.
Adding `gpu=1` to that image would select the wrong hardware path. The touch
binary overlay is intentionally bounded to dormant touch code and provides no
GPU conversion. A650 integration needs a built kernel with the adapted driver,
correct trusted-firmware inputs, matching GPU service/user-client ABI, and the
Mesa-enabled helper image. The current 4-MiB helper was built without GL.
UMI uses HKGPU and its matching userland bridge; upstream uses HakiosGPU.
Do not mix these interfaces by changing only the boot argument.

Apple's pinned XNU base c54f35ca767986246321eb901baf8f5ff7923f6a has been
fetched under the local references directory for the source build. Firmware and
build outputs remain outside git. The working touch/eight-CPU image is retained.

The first device GPU image should exercise the bounded initialization/drawing
sequence and clean shutdown before GPU-backed iOS rendering is advertised.
Maintain RAM boot below the user's 120-MB transfer limit, and retain the
existing APFS installation unless userland integration actually requires a
separate reviewed update.
