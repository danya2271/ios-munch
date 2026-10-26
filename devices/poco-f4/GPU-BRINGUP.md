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

## Native A650 drawing image r1

The device capture `munch-gpu-info-20261026-141054` contains all six requested
A650 files. Their SHA256 digests match the capture manifest. The split ELF
header, hashes/certificates blob and load payload agree with the MDT program
headers. This validates local file structure, not the vendor signature; only
TrustZone on the device can authenticate that. Android reports Adreno650v3 and
305/400/441.6/490/525/587/670 MHz. The drawing profile explicitly requests
`adreno_speed=1`, the port's 305-MHz entry.

`gpu/stage-kernel.py` applies `gpu/umi-a650.patch` to a prepared upstream XNU
8796.121.2 tree and stages FT3658U as a native kernel driver behind
`munch_touch=1`. It also removes the same premature PE bootcpu query previously
bypassed in the binary prototype: this profile runs on the current boot CPU.
The prebuilt kernel and original source overlays remain available unchanged.
The UMI GPU patch adapts symbols to HakiosGPU and hakios_* so it uses the
existing upstream service ABI rather than mixing it with HKGPU userland.

The `build` kernel target completed successfully with `BUILD_DSYM=0`. A first
plain `all` attempt failed because DSYMUTIL was unset; disabling dSYM then
exposed failures in optional exported symbol sets/debugger macro installation.
Those packaging targets are not part of the successful kernel `build` target.
Both GPU and native touch compile and link into the resulting Mach-O.
`XNU_HOST_CFLAGS` and `XNU_HOST_CLANG_DIR` allow local host headers and compiler
selection without installing system packages. Build logs remain in artifacts.

The separate helper APFS includes this phone's firmware and the original helper
files. apfsck reports clean; apfs-tool read back all 465 entries byte-for-byte.
The installed system APFS is unchanged. The existing iOS/touch/eight-CPU images
are retained.

`munch-a650-draw-r1.img` uses the source-built kernel and runs the UMI A650
initialization/drawing sequence through step 22, watches for 20 seconds, shuts
GPU state down and returns to UEFI. It requests all eight available CPUs and
keeps the verified UEFI touch power handoff. It does not start a GPU-accelerated
iOS session: the helper still has no Mesa library. Its exact arguments are:

```
-v cpus=8 small_cores_after=0 munch_touch=1 adreno=22 adreno_speed=1 adreno_pause=0 adreno_watch=20 adreno_return=1 loader_return_after=120
```

The image is 7,860,224 bytes. Host verification checked header, FD, captured
DTB, payload hashes, helper APFS, full compiled kernel and source manifest;
Windows copy hashes agree. Device drawing, authentication and clean shutdown
remain untested. Photograph the last output if no bands appear. A successful
test should show orange/white/blue bands before returning to the loader.

## Repository synchronization audit

The user fork `olzhas0986/hackios-xnu-kernel` was inspected at c80f1fad21.
It contains the POCO F4 commit 0ed9ad25fa, followed by upstream merges
81ec922ecf and c80f1fad21. Relative to the local F4 base, the merges change
20 files, chiefly SoC/modem parameter generation, modem code, tests and docs;
the upstream prebuilt kernel changes too. There are no merged changes to the
A650 port, F4 device directory or loader source. Both remote histories were
fetched. The active working branch was not moved because changing the pinned
prebuilt during a native GPU test would invalidate the existing binary-overlay
profiles. No user commits were reset or rewritten.

## Drawing r1 device result and SCM/DMA experiment r2

The photograph `5402504418520932994.jpg` shows no colour bands. Enlarged log
inspection places the failure at step 12 (zap metadata submission), after step
11. The PAS metadata call returns -1. GMU firmware startup and a drawn frame
are not established; the earlier interpretation of a GMU-start failure was
incorrect. The kernel remains alive through the observation loop and returns.

R2 tests two differences from the stock munch SCM path:

- The availability probe includes the SMC64 convention bit in the queried
  function ID, as `__qcom_scm_init` does. PIL/PAS calls use the convention
  negotiated by that probe rather than unconditionally using SMC64. Call
  type and owner are retained. The selected width is printed.
- Zap metadata uses a physically contiguous, page-aligned allocation constrained
  below 4 GiB. Its bounds and every 4-KiB physical offset are checked. The
  initial cached zero fill is cleaned/invalidated, and metadata is copied only
  through a normal noncacheable alias before the SMC. The allocation stays
  alive for the boot. A missing low-memory allocation stops the test rather
  than truncating the address. This follows the stock driver's requirement for
  contiguous/aligned/noncacheable PAS metadata; the below-4-GiB constraint also
  supports the SMC32 case. It is an experiment, not a confirmed explanation of
  the device refusal.

Primary references are Xiaomi munch-s-oss
[drivers/firmware/qcom_scm-64.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/munch-s-oss/drivers/firmware/qcom_scm-64.c)
and [qcom_scm.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/munch-s-oss/drivers/firmware/qcom_scm.c).
The first uses the negotiated convention for standard PIL calls; the second
allocates PAS metadata with dma_alloc_coherent and documents the memory rules.
No vendor firmware has been modified. Frequency remains 305 MHz and the
helper/system images are unchanged from drawing r1.

`python3 gpu/check-scm.py` compiles the actual width-normalization helper from
the patch and checks SMC32/64, call-type preservation, availability calls and
PSCI isolation with ASan/UBSan. The native kernel build target passed. Device
SCM acceptance and graphics execution remain pending for r2.

## Drawing r2 device result and loader reservation r3

`IMG_5583.HEIC` resolves the r2 stop: SCM negotiation selected 64-bit, but step
12 failed with `no contiguous low-DMA metadata page`. No PAS metadata call was
made at that step. The log shows the physical allocator's unsuccessful bounded
search. The RAM-root layout sets topOfKernelData after the multi-gigabyte
resident APFS image, making all earlier RAM static/wired; free VM pages are
above 4 GiB. The bounded allocator therefore cannot provide the requested page.
This does not establish whether TrustZone will accept a correct low buffer.

R3 reserves 64 KiB in the loader immediately after the final kernel segment,
and moves the small helper RAM disk forward by that size. Loader guards require
the reservation below 4 GiB and before the resident image; the helper overlap
check uses its shifted start. The reservation is below topOfKernelData and
covered by the existing pre-handoff cache clean/invalidate. It is published as
two u64 values (physical base and size) in `/chosen/munch-scm-metadata`.

The driver validates exact size, alignment and low-address bounds, maps each
16-KiB slot as normal noncacheable memory, and uses a fresh slot for each PAS
metadata submission. At most four submissions fit; an exhausted or malformed
reservation fails without reusing a potentially protected page. There is no
new request to the VM low-memory allocator on this path and no raw fixed-address
memory write in the kernel. The r2 allocation remains the fallback if there is
no reservation property.

For the r3 kernel and observed F4 window, host layout checks place the reservation
at `[0xf8260000, 0xf8270000)` and the helper ends at 0xf8670000, below the resident
root's 0xfc000000 start. The address is computed from the loaded kernel extent,
not hardcoded. Source tests cover malformed, high, overflowing and misaligned
reservations. Native kernel build completed; boot-image integrity and loader/
kernel property presence are checked before publication. GPU clock remains
305 MHz. Device authentication and drawing remain pending for r3.

## Drawing r3 device result and bounded SCM resumption r4

`IMG_5584.HEIC` confirms that step 12 consumes the loader reservation at
0xf8260000 (one 0x4000-byte slot) and issues PAS_INIT_IMAGE. The next message
is `TrustZone keeps saying the call was interrupted`. The reported -1 was
returned locally by the inherited driver's 100-attempt limit, not a final
signature verdict from TrustZone. This also corrects the earlier interpretation
of r1's -1 as a secure-firmware refusal. R3 establishes that the reservation
path runs; it does not establish successful authentication.

The stock munch qcom_scm-64.c resumes QCOM_SCM_INTERRUPTED until completion,
passing the A6 continuation state back each time. R4 preserves that convention
and replaces the inherited 100-attempt limit with at most 1,048,576 calls and
five seconds measured between calls. This bounds repeated returns, not a
single SMC that never returns. It prints counts 1/2/3/16/256/4096/65536 and the
returned x1/A6 state, then the actual completion result and call count. A local
timeout is explicitly marked `LOCAL SCM RESUME TIMEOUT` and uses a distinct
local status (-10001); it is not a vendor authentication error.

Host tests cover continued resumption beyond 100 calls and both time/call
limits. Native assembly inspection confirms x6 is saved into a callee-saved
register across logging/time helpers and supplied to the next SMC, with x0=1
on resumption. The kernel build completed. The low reservation, firmware,
305-MHz selection, helper APFS and drawing sequence remain unchanged.
Authentication/drawing are still pending for r4; a persistent local timeout
would require investigating the continuation state and secure call environment.

## Device-confirmed hardware drawing and first iOS GL image

The user reports orange, white and blue bands with drawing r4. This confirms
hardware command execution and the diagnostic frame on munch. It does not yet
confirm sustained CoreAnimation rendering or the final return/shutdown outcome.

Mesa 25.0.7 was fetched with the SHA256 pinned by tools/mk-mesa.sh, patched with
the upstream Hakios Darwin/freedreno integration, and built for ARM64 iOS with
softpipe and freedreno. The build completed all 1,134 targets. The resulting
libOSMesa.8.dylib is 14,838,712 bytes and exports 1,296 GL functions. Private
freedreno symbols are hidden by Mesa's export policy, so exported private
symbol names are not used as a driver-presence test; linked HakiosGPU/freedreno
markers and the required exported OSMesa/context API are checked instead.

`gpu/prepare-gl-helper.py` compiles the existing upstream EAGL bridge and gl_test
against that library. It copies the known helper and device-local firmware into
a separate staging tree, adds the two graphics dylibs and test binary, and makes
a separate APFS helper. It checks ARM64 Mach-O identity, install names and
runtime dependencies, including the staged Mesa dependency of the GL bridge.
The generated EAGL table has 409 entries: 356 forwarded, 30 implemented by hand,
and 23 without a Mesa function (the existing upstream bridge's limitations).
The resulting 6,074,368-byte helper passed apfsck and byte-for-byte readback of
all 468 entries. The original helper and installed system APFS remain unchanged.

`munch-ios165-gpu-r1.img` is 11,872,256 bytes and reuses the exact native kernel
that drew the r4 bands. Its strict profile is `munch-a650-ios-gpu`, with:

```
cpus=8 small_cores_after=0 munch_touch=1 gpu=1 adreno_speed=1 loader_return_after=600
```

The diagnostic stop arguments and verbose framebuffer console are absent.
The existing A650 `gpu=1` path initializes and retains the GPU, publishes
HakiosGPU and proceeds into iOS. The existing libhakios.dylib injection detects
that service and loads /redwood/lib/libhakios.gl.dylib; Mesa uses the matching
HakiosGPU DRM adapter. Without a started GPU the existing fallback keeps the
software renderer. The first image stays at the device-tested 305-MHz entry;
frequency scaling, Metal and other drivers are not added by this integration.

The native config pins the graphics report and helper hash as well as the
kernel/source report. Independent image inspection verifies both profiles,
including the exact GL helper payload and dependencies' build manifest. The
Windows copy hash agrees. Real FD650 context creation, CoreAnimation frames,
UI responsiveness and sustained reliability still require the new device boot.
No system APFS reinstallation is needed.

## First iOS GPU boot: pre-XNU UEFI exception and shim experiment r2

The user photograph `5402504418520933074.jpg` shows an AArch64 UEFI exception
with a firmware SCM/shared-memory-bridge call stack. The user confirms it
happens before XNU. This is not evidence of a Mesa shader or XNU GPU panic.
The FD is byte-identical to the storage FD used by the working drawing test.
The actual early runtime source address is not available from the photograph.

Review found a concrete independent bug in the inherited ARM64 shim: it copies
the FD first with forward-only copying, then copies the payload. An FD output
range overlapping the not-yet-copied payload input corrupts that input. The
larger GL helper increases the input extent. Such overlap is a hypothesis for
this particular device exception, not a confirmed runtime diagnosis.

The revised shim decides transfer order from the actual runtime ranges and
copies payload first when FD output overlaps its input. Each aligned transfer
uses memmove direction rules. It preserves incoming x0/x1, rejects oversized
payloads and rejects layouts in which the running code or still-needed FD
would be overwritten. Firmware and payload destinations are unchanged.
`uefi/check-bootshim.py` executes the real ARM64 machine code with Unicorn:
seven separated/overlapping layouts, both move directions, FD-in-place, input
register preservation and invalid-layout rejection pass. The same test with
the preserved old source reproduces payload corruption on a synthetic overlap.
No unapplied AArch64 relocations are allowed in the raw shim.

`munch-ios165-gpu-r2.img` replaces the 208-byte shim with the checked 416-byte
shim. Its entire original FD and XNUP payload are retained byte-for-byte,
including the EFI loader's original PE timestamp, compiled kernel, GL helper
and command line. Independent boot-image verification passed. The manifest
records shim size and source digest; older images retain the default size 208.
This isolates the bootstrap change without changing GPU or firmware modules.
Device UEFI startup and accelerated UI still require testing this image.

## First accelerated UI result and 587-MHz comparison

The user reports that iOS GPU image r2 reaches the UI and is dramatically
smoother. Touch and GPU-backed UI operation are device-confirmed by that report.
Intermittent visual glitches appear while swiping the home screen; their shape
and cause are not established. This does not establish sustained stability or
artifact-free rendering.

A controlled comparison image `munch-ios165-gpu-587-r1.img` selects
`adreno_speed=5`, the existing A650 table entry 587000 kHz / nominal level 256.
587 MHz is present in this phone's Android frequency list and captured power
levels. The GMU path derives its GFX/MX rail dependency votes from CMD DB.
The kernel table is not extended to 670 MHz. The checked kernel, Mesa/bridge,
GL helper, touch, eight-CPU request and overlap-safe shim are retained. Only
the requested speed changes; frame waiting/extension settings remain as before.
The 305-MHz image is retained for comparison. The new profile accepts only
indices 1 and 5, with 1 as default.

Low frequency can affect frame pacing but does not by itself explain corrupted
pixels or icons. Existing EAGL code already flushes/waits for frame completion
and has optional diagnostic flags for framebuffer fetch and waiting. Those
flags are not changed in the frequency comparison, so a timing-sensitive issue
is not prematurely declared fixed by a clock increase. The new operating point
and any effect on the reported artifacts require a device test.

The user further describes the glitch as a VSync-disabled-looking tear.
MiniIOMobileFramebuffer explicitly uses CPU copies into the boot framebuffer
and a timer for VSync, not a hardware display-controller page flip/vblank.
Its GPU-completion wait is not equivalent to synchronizing with physical panel
scanout. This makes display tearing plausible; it is not a proven diagnosis
without capture/timing evidence. `ca_vbl=1` changes CoreAnimation's notification
source but still uses that synthetic timer, so it is not advertised as hardware
VSync. The 587-MHz image keeps presentation settings unchanged for comparison.

## Session timer, presentation experiment and 120-Hz prerequisites

The user describes classic-looking horizontal frame discontinuities when
moving cards, with more discontinuities at higher motion speed. A log window
over a frozen UI appeared after roughly 10–20 minutes. The prior profile's
loader_return_after=600 schedules a deliberate return after ten minutes, so
this timing matches the planned debugger return; without its text, a separate
fault cannot be ruled out. The new session profile sets loader_return_after=0,
which the source explicitly handles by not scheduling a timer. Panic returns
remain available.

`munch-ios165-gpu-session-r1.img` retains 587 MHz, kernel, helper and safe shim,
sets ca_vbl=1 and glwait=1, and leaves the compositor at 60 Hz. This ties
CoreAnimation to the existing synthetic display notifications and explicitly
waits for GL completion before handing a frame to presentation. These are
frame-pacing/completion experiments, not hardware VSync or a tearing fix.
The previous 305/587-MHz comparison images remain available.

The selected Android panel is qcom,mdss_dsi_l11r_38_08_0a_dsc_cmd: a command-mode
OLED/DSC panel with timing@0 = 60 Hz and timing@1 = 120 Hz. Its platform TE input
is local GPIO66. Xiaomi pinctrl-kona.c places GPIO66 in the NORTH tile with
mdp_vsync function 1, giving config 0x0f942000 and input 0x0f942004.

`munch-panel-te-r1.img` is a standalone UEFI-only passive timing probe. It
requires that pad to already be mdp_vsync input, never changes pinmux, and polls
rising edges in three half-second windows using the 19.2-MHz architected timer.
It prints edge counts and median periods as cadence in Hz, then stays visible
for the usual diagnostic pause. No DSI, display clock, power, firmware or disk
writes are performed. An unconfigured pad, absent pulses or a bounded polling
failure is reported rather than interpreted as a panel frequency. TE cadence
is evidence about the panel timing signal, not proof of full frame delivery.

`uefi/check-panel-te.py` tests the actual estimator with synthetic 60/120-Hz
signals, absent edges, wrong mux and invalid timer under ASan/UBSan. Both image
containers, hashes and payloads were checked and Windows copies verified.
Physical 120-Hz operation is not advertised from fb_hz=120 alone. That tunable
would only change software mode reporting and timers; no such unmeasured
physical panel switch is included in these images. The new bounded profile
fields allow compositor_hz 60/120, return_after_seconds 0/600 and an explicit
sync_frame_clock Boolean for subsequent experiments.
