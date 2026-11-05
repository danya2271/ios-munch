# Munch display path

## Native L11r handoff (current)

`--sde-backend --sde-native` selects a fixed **1080x2400@60** XNU presentation
path. The default handoff keeps the panel, DSI link, PHY and clocks inherited
from the bootloader, then takes ownership of guarded SDE buffer commits. This
avoids blanking the panel while XNU is still missing the complete Linux power
and reset sequence.

The inherited refresh is drained around each guarded commit and restored after
hardware completion. Native VSync samples the INTF1 hardware frame counter,
including during idle periods, while the CPU compositor remains unchanged.
The framework is advertised at 60 Hz to match the inherited physical panel
mode; `fb_hz` cannot raise the panel's scanout rate.

The r33 safe-refresh image was reported to show the Apple logo and then leave
the display off; it is not hardware-validated. The r34 candidate addresses two
specific lifecycle failures:

- Safe-native refresh restart can recover a missing autorefresh completion
  after the submitted frame has already completed. Recovery rechecks the
  SMMU/route guard, source address and idle state, rewrites inherited TE and
  autorefresh controls, and verifies readback before releasing ownership.
  Faults, drift, failed readback and unfinished frame DMA still fail closed.
- The inherited-panel power path sends Sleep Out (`0x11`), waits 120 ms,
  restores `0x53=0x20`, then sends Display On (`0x29`). Previously it sent
  only Display On after powering down with Sleep In (`0x10`), which cannot
  reliably wake a sleeping panel. Failed wake commands stop the sequence.

Host tests cover repeated frames and wake after missing refresh completion,
failed control readback, guard faults, pending-DMA timeouts, and each failing
wake command. They do not establish that r34 fixes the handset blackout or FPS.

The r37 candidate adds PP_DONE IRQ-assisted completion waits on the shared
MDSS GIC summary interrupt. It attaches only with an empty inherited MDP mask
and a successful SPI lease, acknowledges only PP_DONE/underrun, and falls back
to timed status checks if GIC ownership is unavailable or another MDSS source
appears. A received PP_DONE is treated as transfer completion without waiting
for redundant CTL status bits.

The r37 triple-buffer pipeline uses the two reserved back buffers as alternating
compose/transfer targets. While one buffer is being transferred, the presenter
may compose the next queued swap into the other; it waits for PP_DONE only
before submitting that prepared frame. Native `SwapWait` returns once the frame
is copied into a driver-owned back buffer; the IOSurface reference and hardware
error state remain held until PP_DONE. It never writes the buffer currently
owned by the transfer. `MUNCH SDE PERF r19` adds `overlap=N` to count frames
composed during a prior transfer. `MUNCH SDE PERF r20` adds `retired=N` for
early native SwapWait completions and `queued=N` for swaps the presenter finds
while a transfer is active. These counters distinguish producer pacing from
hardware completion. This remains unmeasured on the handset.

The r39 candidate adds `ca_vbl=1`, which asks CoreAnimation to use the driver's
sampled hardware VSync notifications instead of its internal timer. It does not
force GPU completion waits.

The r40 candidate removes `ca_vbl=1` and moves the first graphics-console
handoff to successful native submit/retire. This prevents kernel console output
from writing into a back buffer after it has become DMA-visible but before the
first PP_DONE.

The r41 helper disables per-call OpenGL `glGetError()` polling in normal boots.
That query can synchronize with the GL driver after every WindowServer call;
`glerrors=1` enables the old diagnostic behavior when needed.

The r42 candidate adds `glsync=0` only. `glFlush()` is still issued before the
frame is handed to the display; this removes the extra zero-timeout fence used
to force Mesa batch submission and is an A/B pacing test.

r42 was rejected by device telemetry: Mesa submissions dropped to 9--17 per
interval, confirming that the fence path is needed to push batches. The r43
diagnostic image restores `glsync=1` and adds `glframes=1 glslow=20000
touch_lat=1` to expose producer-side frame gaps and slow GL calls.

120 Hz is a separate modeset task. Although the panel description contains
120-Hz timing data, its captured profile does not match the live device tree and
the active bootloader mode has not been verified at 120 Hz. The safe handoff
therefore does not advertise or assume 120-Hz scanout.

The previous full panel takeover remains available only with the explicit
`munch_sde_takeover=1` boot argument. It sleeps the panel and rewrites DPHY,
PLL, clocks, DSI, DSC and panel commands; it is a diagnostic path until the
missing XNU panel power/reset sequence is implemented and tested on hardware.

Full takeover mode keeps external TE disconnected while programming the
tear-check registers and through the first CTL START. It reconnects TE after
that kickoff, matching the local Linux `handle_post_kickoff()` sequence. The
mode register value before START is `0x00080059`; bit 20 is added after START.

Tear-check is derived from the explicitly programmed 19.2-MHz vsync clock,
3592-line total and 60-Hz mode: **89 ticks/line**. The inherited `0x885` value
is not retained. Native VSync notifications sample the INTF1 tear-check
read-pointer frame counter (`INTF_TEAR_INT_COUNT[31:16]`) independently of
swaps, including idle periods. This is the same command-mode cadence used by
the local Linux driver; the INTF timing-engine frame counter is not used for
VSync. The polling timer is not a synthetic presentation-completion source.
The prior completion-only notification path could leave the producer without
a VSync when there was no new swap.

`generate-sde-mode.py` compiles extracted functions from the local Linux tree to
reproduce the PLL, PHY, DSC, command-stream and tear-check register programs.
It also verifies the computed 128-byte DSC PPS against the panel's PPS. The PHY
timings come from `l11r-observed.json`, since the standalone source differs from
the captured live DT. No network checkout or register values from another device
are used. The packer requires an exact match of the captured 60-Hz profile.

This is **not a complete Android display-stack port**: resource power/rails,
reset GPIO and bandwidth setup are still acquired from firmware, and the
existing CPU compositor remains. Native takeover requires the qualified
CTL0/VIG0/LM0/DSC0 route and reserved buffers. Adaptive refresh, full rail/PHY
		suspend and IRQ-driven VSync are not implemented. Read-pointer events are polled
at 1 ms and can coalesce while the display lock is busy. Runtime clock, stream
and SMMU drift fail closed rather than continuing to fetch unproven memory.

The safe handoff can return to the original ABL configuration through the
existing guarded path. Full takeover still refuses native-owned return requests
and can leave the panel blank after a partial modeset failure. Keep the existing
r13/r14 images for recovery. Host tests do not measure handset FPS.

```sh
python3 devices/poco-f4/display/check-sde-mode.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel
MUNCH_STAGE=1 MUNCH_STAGE_REPORT="$PWD/artifacts/munch-build/munch-stage.json" ./xnu-build.sh -j8 -k
```

The TE-order test image is
`artifacts/munch-build/munch-iosroot-l11r-sde-native-r30-te.img` (SHA-256
`c27521b40dcaaac3fa819c4b39fca981e3fb4b4b57324c97dc7799bab0373c6e`). The
known white stripe on the handset panel is a separate physical display defect.
Host runtime, mode, swap, and Linux-reference tests validate register sequencing
and fault handling; they do not prove that the display remains lit on hardware
or measure handset FPS. Hardware validation remains pending. Building does not
flash, reboot, or write installed APFS.

Latest packaged candidate:
`artifacts/munch-build/munch-iosroot-l11r-sde-native-r47-queue-wakeup.img`
(SHA-256 `66d938c67f9c860e934d49bebc78d8e164eb211b4ca5c503b0c1dd1a0c1a71d0`).
It rolls back r46's serial scaler after the handset measured 31.7 ms composition
and 3 FPS. Scaling again uses the existing parallel row workers. New swaps wake
the presenter during completion waits; partial overlap preserves the latest
pending frame. `MUNCH SDE PERF r22` identifies the candidate. Eight CPUs,
USB/GPU diagnostics and `wifi_qca_status=1` remain enabled; per-frame GL/touch
tracing is off. Host queue-wakeup, buffer-ownership, full runtime, memory,
refresh and independent payload checks pass. The image is not device-tested,
and the wallpaper issue remains unconfirmed. Test with `fastboot boot`; no
installed APFS or device partition was changed.

## Inherited path and history

The default XNU path inherits ABL's framebuffer and composites CPU-rendered
frames directly into the active scanout buffer. The panel is the command-mode
`qcom,mdss_dsi_l11r_38_08_0a_dsc_cmd` at approximately 60.68 Hz. INTF1
measurements show a roughly 12-ms write phase and a 4.5-ms idle gap per
16.48-ms frame.

The opt-in shadow-buffer/TE copy experiment has been removed: device testing
made tearing and frame pacing worse. Default images retain the original direct
framebuffer path, and image packaging rejects `display_te=true`. The scan
probe remains read-only. An optional `munch_sde_probe=1` boot argument logs
the Kona SDE revision, active SSPP source configuration, CTL routing, and
INTF1 mux, frame/line counters, and command-mode tear-check state without
writing display registers. It also snapshots the apps SMMU mapping matching
Kona's unsecure SID `0x820`/mask `0x402`: SMR/S2CR, the selected context-bank
registers, TTBR0, and translation-fault state. CTL flush bits, INTF counters,
tear-check fields, and block offsets follow
the Munch Kona DTS and the Linux `sde_hw_ctl.c`, `sde_hw_sspp.c`, and
`sde_hw_intf.c` register definitions. The XNU profile also carries the panel's
60/120/144-Hz timing values and DSC geometry. Default presentation uses the
inherited framebuffer; the old experimental CTL kickoff was removed because it
restarted the active source without switching buffers.
Enable the kernel snapshot only for a diagnostic boot with
`loader/pack-iosroot.py --sde-probe` alongside the source-built iOS GPU kernel;
normal images leave it disabled.

An opt-in hardware presenter is now wired to the compositor through
`munch-sde-xnu.h`. It uses the inherited SDE route and SMMU context, not a cold
DSI/SDE initialization. No framebuffer source address is written until all
three buffers and the live CTL/SSPP route are verified. The diagnostic probe
remains read-only and does not enable the presenter.

## Fixed-mode backend: opt-in integration

`xnu/files/iokit/Kernel/munch-sde.h` contains the first portable backend layer.
Its register layout and sequencing are based on the local Kona Linux
`sde_hw_ctl.c`, `sde_hw_sspp.c`, `sde_hw_lm.c`, `sde_hw_intf.c`,
`sde_hw_interrupts.c`, `drivers/iommu/arm-smmu-regs.h`, and
`drivers/iommu/io-pgtable-arm.c`. No DRM objects or Linux device model are
imported. With `munch_sde=1`, composition targets a verified back buffer and
Native `SwapWait` acknowledges driver-owned back-buffer admission; hardware
completion remains tied to PP_DONE and native VSync. Tearing and latency on the
actual phone have **not** yet been validated.

Implemented and host-tested:

- Decode the actual CTL layer registers, including DMA2/3 in EXT2. Require one
  source at the base stage (with or without inherited border color), or at
  a directly encoded blend stage 1–6 over border color with verified opaque
  constant blending at that stage's own register offset.
  Require one full-size mixer, one DSC, command-mode INTF1 and one ping-pong.
  DMA0-3 and bypassed VIG0-3 are supported. VIG requires disabled QSEED scaling,
  CSC, pre-downscale and decimation; no scaler/CSC programming is performed.
  Reject ambiguous routing, extra sources, extended stages, dim layers, split/merge, scaling,
  multirect, secure sources, non-linear formats and unsupported autorefresh
  configurations. Enabled autorefresh with frame count one is qualified for
  a bounded, deferred pause before the first commit; other busy transfers reject.
  Only inherited 1080x2400 ARGB/XRGB8888 with matching
  unpack order is accepted. SSPP opmode may be zero or PE_OVERRIDE with verified
  identity pixel extension. Blend/scaler/pixel-extension controls join the raw-state guard.
- Require exactly one active SMMU stream mapping covering **all** SID variants
  selected by `0x820/0x402`, not merely an overlapping SMR. Extended SID mode,
  duplicate/partial matches and context bounds are checked. S2CR bypass and
  fault streams remain rejected. A translation stream into a qualified
  stage-1 context with `SCTLR.M=0` and stage-2 bypass uses physical addresses
  without changing the inherited SMMU configuration (r12).
- Walk imported AArch64 stage-1 4-KiB tables, including 1-GiB and 2-MiB blocks.
  Reject unsupported granules, nested translation, faults, invalid descriptors,
  missing access permission, and unsupported memory attributes. Verify every
  page of the inherited buffer and two replacement buffers, with IOVA/physical
  overlap, alignment, size, and overflow checks. With translation enabled,
  build isolated L2/L3 tables for replacement buffers without changing any
  existing translation. In inherited context bypass, use two pinned contiguous
  physical back buffers below 4 GiB, with no PTE reads, table allocations,
  publication or TLBI. Reject physical aliasing before clearing new memory.
- Maintain one pending transfer and two replacement-buffer ownership slots.
  After cache cleaning, clear only the selected stale ping-pong completion,
  write the back-buffer source address, flush that SSPP, then issue CTL START
  with barriers. Complete only after the selected ping-pong event, idle DSI,
  and cleared CTL START/FLUSH. An underrun, changed hardware state, or deadline
  expiry latches an error and pins the pending buffer; it never becomes a
  successful software-vsync event or an automatically reusable buffer.

The XNU probe now uses the same route/domain qualification code and also logs
DSI0 state, unpack/security/multirect state, mixer sizes, DSC/merge/writeback
routing, and SMMU TCR2/MAIR. It does **not** walk firmware page tables, allocate
scanout memory, acknowledge interrupts, or call the submission functions.
The separate `munch_sde=1` argument enables the write path described below.

### XNU lifetime and completion

- Prefer the loader's `munch-sde-buffers` reservation: two 10-MiB slots plus
  a 272-KiB table workspace, aligned to 16 KiB and below 4 GiB. Import through
  XNU's existing static mapping after checking every physical page, static
  RAM bounds and exclusion of kernel/boot pixels/SCM/ramdisks. It remains wired
  for the kernel lifetime. Without the property, use host-physically-contiguous
  VM allocations with the system mapper explicitly disabled; malformed
  reservations do not fall back. Import translated identity mappings only after a complete walk;
  otherwise publish a private branch in a zero root entry of a supported
  32-bit-input, 4-KiB-granule stage-1 context. Preview every mapping before
  publication, clean table cache lines, invalidate the ASID, and wait for TLBSYNC.
  Existing SMR/S2CR, TTBR/TCR and valid PTEs are never replaced.
- Firmware table reads are restricted to Munch's Display Reserved RAM
  `[0x9c000000, 0x9e400000)`, excluding boot pixels. Tables outside that range,
  shared contexts, unavailable slots and unsupported routes are rejected,
  not probed or forcibly reconfigured.
- Retain one queued/in-flight swap until completion. Wait for GPU rendering;
  acquire an inactive buffer, copy the previous frame for partial updates,
  and run the existing CPU compositor there. No TE shadow-copy into live
  scanout and no fixed-delay success path are used.
- Serialize with the panel lock and shared apps-SMMU lock. Revalidate the
  route, domain and buffer translations, clean pixels, commit SSPP/CTL, and
  poll hardware for at most 50 ms. Only successful completion advances the
  counter, records a frame, wakes `SwapWait` successfully and sends a
  notification. Notifications describe completed frames, not a continuous
  idle vsync source; CoreAnimation behavior still needs device validation.
- `SwapWait` resolves an accepted swap ID (scalar 0, or the client's last
  accepted ID when omitted). Unknown/expired IDs and GPU/commit failures are
  errors. No ABI meaning is guessed for additional scalars. The last 64 results
  are retained; all waits are bounded. A backend error stops further swaps,
  retains DMA memory, and never falls back to writing ABL scanout.
- DCS display blank/unblank is serialized with frame submission; see r14 below.
  Full panel sleep, PHY programming and refresh modesets remain unimplemented.
  Live L11r PHY values remain inherited.
- After other CPUs stop, `munch_sde_quiesce()` restores the boot source through
  a completed hardware commit, then removes only its owned root entry and
  synchronizes TLBs. If that cannot be proved safe, loader handoff is refused;
  allocations remain pinned. This can prevent return to the loader after a
  hardware fault. There is no automatic reset/free/retry recovery.

### Enable a reversible test image

Build with `MUNCH_STAGE=1 ./xnu-build.sh -j8 -k`, then add `--sde-backend` to
the existing `loader/pack-iosroot.py` command. Keep a known-good image. The
packer requires the GPU profile, matching kernel presenter/quiesce signatures,
and live-DT L11r selection, DSI0/PHY0, GPIO66 TE and supported DSC geometry.
It rejects injecting `munch_sde*` through `--extra-cmdline`. The manifest records
the opt-in separately from the read-only `--sde-probe` option. Default is off.

Initialization logs `MUNCH SDE: hardware presenter v1: ready` or `rejected` and
publishes `MunchSDEBackend`. Rejection preserves the inherited source but does
not accept swaps in requested backend mode. Do not weaken qualification to
make a rejected boot appear successful; capture the probe and extend the
specific unsupported case with Linux sequencing and tests.

For on-screen boot diagnostics, pack with `--verbose --sde-backend --sde-probe`.
`--verbose` supplies `-v`, enabling the XNU text console. In requested SDE mode,
the legacy GPU test blit (`gl_test`'s triangle) is rejected instead of overwriting
the console or the inherited scanout. Console ownership changes only after the
first successfully completed compositor frame, not when a swap is queued or a
back buffer is acquired. Initialization/acquisition/commit failures include the
named SDE reason. This handoff indicates a displayed compositor frame, not proof
that the SpringBoard process has completed startup. A hardware failure after
scanout programming can still prevent fresh console pixels from being visible;
this diagnostic mode does not force an unsafe rollback or fallback.

The r13 kernel repeats a `MUNCH SDE STATUS r13` block every five
seconds while no frame has completed or presentation has failed. It reports
the saved initialization phase/result, the actual `SwapEnd` return code and
input size, accepted/completed counts, and the saved route snapshot. It runs
even when no `SwapEnd` reaches this driver and stops after healthy presentation
begins. Reporting reads saved state only; it does not retry initialization or
change hardware qualification. These rows distinguish backend errors from
WindowServer/CommCenter messages. In this kernel, `0xe00002d0` means
`kIOReturnNotAligned`, whereas rejected SDE initialization returns
`kIOReturnNotReady` (`0xe00002d8`). Do not treat those two codes as equivalent or
infer a hardware alignment fix from the WindowServer message alone.

The user's r3 status identified a false rejection at `inherited-route`:
`rev=60000000 mux=0 tear=1 auto=00000001 dsi=1f5`, with no accepted or completed
swaps. The old check incorrectly required the entire `DSI_CTRL` register to
equal `0x5`. In the local Linux `techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c`,
`dsi_ctrl_hw_cmn_host_setup()` enables data lanes at bits 4–7 and the clock
lane at bit 8, in addition to controller/command bits 0/2. The r4 check requires
all of these (`0x1f5`), permits only optional receive ECC/CRC bits 20/24, and
still rejects video mode, missing lanes and unknown bits. It does not write
DSI/PHY registers or mask the saved baseline: any later raw register change,
including ECC/CRC changes, still fails the runtime guard. Per
`sde_hw_intf_get_autorefresh_config()` in `sde_hw_intf.c`, autorefresh enable
is bit 31; `auto=1` is a disabled configuration retaining frame count 1.
The old synthetic fixture also used `0x5`, hiding this bug. Regression tests
now exercise the observed tuple, all ECC/CRC combinations, rejected bit
changes and preservation of the full-state guard. This fixes the evidenced
qualification error; it does not prove the remaining route/SMMU checks or
physical frame completion. Keep capturing the next phase/result if initialization fails.

The user's r4 log progresses to `ambiguous-or-unsupported-route` at
`inherited-route` with the same saved link values and zero accepted/completed
swaps. This confirms the link gate now passes, but the five saved link fields
cannot identify which CTL/layer predicate failed. Linux `sde_hw_ctl.c` confirms
the current register offsets and command-mode/interface encodings; there is
no evidence yet for choosing another CTL, accepting extra blend stages or
ignoring DSC/merge state. The r5 change preserves every qualification rule.
The shared checker additionally returns the precise failed predicate and
CTL/LM/SSPP indices (`-1` means not selected). Its periodic `MUNCH SDE ROUTE`
line is followed by all six `MUNCH SDE CTL<n>` register rows, layer maps for
candidate CTLs, and configured SSPP geometry/format/security rows. These are
decoded from the saved snapshot only, with no MMIO reads/writes or retries.
Capture the whole block, particularly `check=`, CTL, LM and named DMA/VIG rows;
the unchanged overall route error alone still cannot identify a safe fix.
This is a diagnostic image, not a claim that the route rejection is fixed.

The user's r5 photograph identifies `check=border-stage ctl=0 lm=0 pipe=-1`
and a configured VIG0 source. The border rejection occurs before the pipe is
selected; `pipe=-1` does not mean no pipe exists. The r6 path decodes bit 24
as inherited border color, not an additional memory source. It accepts a
single base-stage source with that bit preserved, or one first-blend-stage
source over border only when `LM_BLEND0_OP=0x100` and packed constant alpha
is `0x00ff0000` (foreground 255, background 0). Higher stages, extra sources,
dim layers and unsupported blending remain rejected. New VIG qualification
requires QSEED bypass, RGB CSC bypass and zero pre-downscale/decimation;
only the documented disabled-scaler depth/alpha/filter bits are permitted.
The screenshot is not a reliable transcript of every hex digit and did not
contain these extra LM/VIG controls, so host fixtures cover supported variants
without claiming they are a complete live register capture.

References in the supplied Linux tree: `techpack/display/msm/sde/sde_hw_ctl.c`
(`sde_hw_ctl_setup_blendstage`, border bit and stage+1 encoding),
`sde_hw_lm.c` (Kona packed constant-alpha layout), `sde_hw_util.c`
(`sde_hw_setup_scaler3`, disabled QSEED depth/alpha fields), `sde_hw_sspp.c`
(CSC10/pre-downscale/decimation), and
`arch/arm64/boot/dts/vendor/qcom/display/kona-sde.dtsi` (VIG QSEED/CSC offsets
and blend-stage offsets). The existing source-address/flush/start sequence
now applies to a qualified VIG pipe too; routing, blending, QSEED, CSC and
DSI/PHY registers remain untouched. The new fields are saved and checked for
later changes by the same raw-state guard. Status includes their values as
`MUNCH SDE LM<n>: op=... blend0=... alpha0=...` and
`MUNCH SDE VIG<n>: scaler=... csc=... pre=... decim=...`.
Physical frame completion and any following SMMU/idle gate remain unverified.

### r7: stage-specific opaque blending and disabled CSC metadata

The October 29 r6 transcription is internally inconsistent: `rev=0` would
reject at `revision`, and r6 did not print the saved block with a zero revision.
`blend-setup` was not a check name in that binary. Taken literally, `top=21f00`
and `fmt=23ff7` also fail before blend qualification. These gates remain intact;
the new image is not a promise that this exact tuple will initialize.

Linux `sde_hw_mdss.h` defines `0x400` as inverse foreground-constant background
alpha. With packed constants `0x00ff0000`, both this and `0x100` give foreground
255/background 0. `sde_hw_sspp.c` defines CSC10 enable as bit 0 and source format
as bit 1, so `csc=2` is now accepted while CSC remains disabled. All other CSC
bits still reject; the guard continues comparing the full raw value.

Single-source stages 1–6 now check their own blend registers. In particular,
`map=01000005` means logical stage 4 and requires LM+`0x68`/`0x6c`, not `blend0`.
The snapshot captures all eleven Kona blend pairs, rejects enabled dim layers
even in an otherwise unstaged slot, and guards every pair against later changes.
Extended-stage encodings and extra sources remain unsupported. No new register
writes, fallback, early console takeover or TE-copy path are introduced.

Status now prints `MUNCH SDE LM<n> B<index> @<offset>: op=... alpha=...` once per
configured mixer, including zero values. A separate snapshot-valid flag allows
revision-zero failures to report their actual saved state. Logs remain RAM-only
and rate-limited. Capture the original photo of the complete r7 block, including
revision, CTL0 top/map, VIG0 format/CSC and the selected blend pair.

The source-built r7 image is
`artifacts/munch-build/munch-iosroot-l11r-sde-blend-r7.img` (12,103,680 bytes),
SHA256 `d268814f69d04c6feb0b8b0e88f94ca62f16b594d3834f17b1ccb5f41d8e77e1`.
The adjacent verifier and log check payload identities and source-stage hashes.
Verbose output, the opt-in backend/probe and the 600-second safe-return timer
are retained. Use reversible `fastboot boot`, not a partition flash; the image
has not been booted by the build process and does not prove handset presentation.

### r8: exact layout rejection, unchanged qualification

Use only the supplied `/home/danya2271/xiaomi_sm8250_kernel` for further Linux
reference work, as requested by the user; do not substitute a GitHub branch.
Its `techpack/display/msm/sde/sde_hw_sspp.c` format/rectangle setup and
`sde_hw_ctl.c` layer encoding were checked against this backend.

The latest r7 transcription reports `pipe-layout`, but its exact `map=01800005`
selects both VIG0 and DMA1 and rejects earlier at `multiple-pipes`. With only
that map changed to `01000005`, the listed pipe/mixer/blend values pass route
qualification. Both cases are regression tests, not inferred hardware state.
There is no evidence for relaxing format, geometry or routing checks.

r8 replaces the combined layout predicate with the same fifteen conditions,
each with a distinct `pipe-*` name. Immediately after `MUNCH SDE ROUTE`, a
layout failure prints `MUNCH SDE LAYOUT: bad=... first=... got=...` and
`MUNCH SDE EXPECT: rule=... value=... alternative=...`. `bad` marks every failed
condition; `first`/`got` identify the earliest. `bits-clear` uses `value` as a
forbidden-bit mask, `one-of` allows either value, and `at-least`/`at-most` are
inclusive bounds. The other rule is exact equality. All data is saved RAM;
the reporting interval and console handoff rules are unchanged.

Acceptance is checked against the original r7 predicate with one-bit mutations
of every layout field and stride boundary cases. All fifteen named rejections
are also exercised in the real ARM64 snapshot/initialization path under QEMU,
with no display writes or mapping publication. This revision is diagnostic;
it neither constitutes a full Linux driver port nor proves phone presentation.

Artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-layout-r8.img`,
12,103,680 bytes, SHA256
`0e9f7d5bc37e4c7f025407e6417a38dae40c6918b9ff70116ab0fba847ecb4ed`.
Full DEVELOPMENT build and independent payload/source-stage verification pass;
the adjacent verifier, logs and checksum are retained. Boot options match r7.
After reversible boot, capture the `ROUTE`, `LAYOUT` and `EXPECT` lines together
with the original photo rather than only a reconstructed hexadecimal transcript.

### r9: Linux PE_OVERRIDE with identity pixel extension

The local `techpack/display/msm/sde/sde_hw_sspp.c` defines
`MDSS_MDP_OP_PE_OVERRIDE` as bit 31 and sets it unconditionally in
`sde_hw_sspp_setup_format()`. Thus requiring `SRC_OP_MODE == 0` rejects a normal
Linux SSPP configuration, not just scaling or flipping. The r8 transcript's
`pipe-opmode` with `got=0` remains internally inconsistent; it does not prove
bit 31 was actually observed on the handset.

r9 allows only that additional opmode bit, and only after qualifying all three
pixel-extension register groups (C0, C1C2, C3). Each must have zero LR/TB
fetch/repeat controls and required size equal to the source rectangle. This
matches the same-size RGB QSEED3 defaults in the local `sde_plane.c` and their
register encoding in `sde_hw_sspp_setup_pe_config()`. Enabled scaling/CSC,
flips, compression, unknown opmode bits and nonidentity pixel extension still
reject. Opmode zero retains its prior acceptance; inactive PE values are not
used to qualify it.

Nine PE registers at SSPP+`0x100` through `0x128` join the saved snapshot and
full-state guard, including when PE_OVERRIDE is off. No PE/opmode registers
are written or cleared during initialization, frame submission or return.
Status adds `MODE: pe-override=<0|1> forbidden=...` plus the three `PE` rows
when enabled, so the flag is readable without relying on one hexadecimal digit.

`check-sde-linux.py` extracts and executes the actual PE writer from the
explicitly supplied local tree against mock registers, then checks its outputs
with the XNU validator. It also records that this tree writes C3 TB from LR;
this does not affect the accepted all-zero fetch/repeat configuration. The
Linux source is not modified. Core tests cover 448 pipe/stage/blend/CSC/PE
combinations and individual PE/opmode bit mutations. ARM64 scenarios cover
accepted PE routes, repeated frames, imported/owned mappings, raw-state changes,
timeout pinning and safe loader return. Physical presentation remains unverified.

The full DEVELOPMENT build and image verification completed on October 29,
2026. Artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-pe-r9.img`,
12,103,680 bytes, SHA256
`850283560fce647283982c85258a45a6b48699bdd3a579569c28faf0aed118c0`.
The adjacent `verify-sde-pe-r9.py`/`.log`, build/pack logs and `.img.sha256`
record Android boot-header/payload integrity, the exact built kernel and four
staged display sources, live L11r DTB, helper, loader and unchanged FD/shim.
The r8 image remains unchanged. Boot arguments retain the verbose console,
opt-in SDE backend/probe and 600-second safe-return timer. Test reversibly with
`fastboot boot artifacts/munch-build/munch-iosroot-l11r-sde-pe-r9.img`;
this image has not yet been tested on the handset.

### r10: deferred autorefresh handoff

The r9 handset log reaches `check=autorefresh` after accepting the inherited
PE configuration. That gate tests bit 31; the transcribed `auto=00000001`
cannot itself trigger it. r10 prints separate decimal `enabled` and `frames`
fields, just as r9 did for PE, rather than trusting a reconstructed high bit.

Enabled autorefresh with frame count one is now supported. Initialization and
the read-only probe still do not modify display controls. Only after mapping
validation and composing the first back buffer, the adapter follows the local
Linux `sde_encoder_phys_cmd_prepare_commit()` sequence: disconnect external TE
by clearing only VSYNC bit 20, clear autorefresh bit 31 while retaining the
frame count, and wait for the write pointer and DSI/CTL transfer to become idle.
It then reads the Kona autorefresh debug-bus status twice, 25 ms apart, and
requires bit 7 clear before restoring the original TE configuration. Debug-bus
selectors are saved and restored. This is hardware handoff, not the removed
TE-triggered copy into active scanout. No timing, PHY or pixel-extension values
are reprogrammed.

The source address remains unchanged throughout the pause. Drain waits are
bounded to 50 ms; the subsequent commit keeps its separate 50-ms deadline.
The presenter independently refuses CTL START while autorefresh remains on.
The Linux fallback that rewrites tear write-count/start-position is deliberately
not used: failure to prove idle latches an error, pins mappings and refuses
loader return. In that failure case autorefresh may already be off, so the
physical boot-console picture can freeze even though console ownership has
not switched. There is no unsafe automatic restart or fallback scanout write.

After successful handoff, frames use the existing source/flush/CTL START path.
Loader return first completes a commit of the original boot source, restores
the exact autorefresh configuration, and requires a fresh hardware completion
before removing owned tables. This return path uses busy waits, not scheduler
sleep, because the other CPUs have already stopped. Raw autorefresh and TE
controls remain guarded against unexpected changes throughout.

The local-source test compiles the actual Linux INTF/MDP accessors and checks
the prepare-commit ordering, enable/count decoding, TE bit, write-pointer
register and Kona debug-bus base/selector. Portable fault-injection tests cover
every pause guard, failed control writes, both status samples, stuck transfer,
underrun, stale completion and restoration timeout. All 105 ARM64 runtime
scenarios, swap/console diagnostics, panel and export checks pass. Hardware
presentation still requires a handset test.

The full DEVELOPMENT build and independent boot-image verification pass.
Artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-autorefresh-r10.img`,
12,105,728 bytes, SHA256
`8835cf0318f0dbbc4d8c7566a92ab3397642d10a0cf8fdfc00f4e438db0d78fe`.
Adjacent build/pack logs, `verify-sde-autorefresh-r10.py`/`.log` and checksum
record the exact source-stage/kernel/helper/loader/DTB/FD/shim identities and
Android header/payload integrity. The r9 image is preserved. Verbose console,
SDE backend/probe and the 600-second safe-return timer remain enabled.
Test with `fastboot boot artifacts/munch-build/munch-iosroot-l11r-sde-autorefresh-r10.img`.
No device boot or flash was performed during this build.

### r11: SMMU fault-status decoding

The r10 handset log reports a qualified route and fails at `boot-translation`
with `hardware-fault`, before taking ownership of autorefresh or submitting any
frame. In that path the old code returned this result for any nonzero FSR.
That is incorrect: local `drivers/iommu/arm-smmu-regs.h` defines `FSR_FAULT`
as `0xc00001fe`, and `arm-smmu.c` tests only those bits. FSR bits 10:9 are
status-format metadata, not fault flags. The existing Adreno implementation
in this repository already uses the same distinction. The user's r10 dump
does not contain FSR itself, so the actual handset value is not yet known.

r11 uses the Linux fault mask in all three display paths: imported translation,
the per-frame domain guard, and loader-return completion. Values `0x200`,
`0x400` and `0x600` no longer fabricate a hardware fault. Translation,
permission, access-flag, external, address-size, TLB, unsupported-upstream,
stalled and multiple faults remain fatal. No FSR acknowledgement, RESUME,
context replacement, fault suppression or new hardware write is added.
Raw domain state, including FSR, is still guarded against unexpected change.

The repeating diagnostic now includes cached global/stream/context registers,
context physical address, TTBR/TCR/TCR2/MAIR, boot IOVA/PA, raw FSR versus real
fault bits and decoded format, FAR and both syndrome words. A checked page
walk names the exact rejected domain/descriptor condition and records its
IOVA, level, descriptor address/value, verified-page count and expected PA.
The permitted table-RAM bounds and permission checks are unchanged. Once the
route is qualified and a context snapshot exists, these SMMU rows replace
the repetitive CTL/LM/SSPP dump rather than scrolling the useful error away.
All repeating diagnostics read cached RAM, not live MMIO.

Tests compare the actual local Linux header across 4096 fault/format
combinations, cover every FSR bit and named walk rejection, and run 128 ARM64
scenarios. These include all four metadata encodings, imported/owned buffers,
real faults before initialization and during frames/return, untouched fault
registers, retained mappings, and complete boot-source/autorefresh restoration.
The original tests incorrectly used reserved FSR bit 0 to inject a fault;
those injections now use the real translation-fault bit 1. No hardware frame
completion is claimed until the handset test.

The full DEVELOPMENT build and independent boot-image verification pass.
Artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-smmu-r11.img`,
12,107,776 bytes, SHA256
`56aeecb6cc417a07bd18f25aee2899c37a49c29b80edc7736f340cef015881ff`.
Adjacent build/pack logs, `verify-sde-smmu-r11.py`/`.log` and `.img.sha256`
record exact payload and four staged display-source hashes. The r10 image,
live L11r DTB, FD/shim and boot arguments are preserved. Test reversibly with
`fastboot boot artifacts/munch-build/munch-iosroot-l11r-sde-smmu-r11.img`.
The image has not been booted or flashed by this build process.

### r12: inherited stage-1 context bypass

The user's r11 log identifies the actual startup rejection: `SCTLR=0xe0`
has `M=0`, `CBAR=0x1f000` selects stage-1 with stage-2 bypass, `S2CR=4`
routes to CB4, and the boot IOVA equals PA (`0x9c000000`). TTBR0/TCR/MAIR
are zero. FSR `0x400` contains no fault bits. Requiring active page tables
in this configuration was a backend error, not evidence of a broken SMMU.

Only `/home/danya2271/xiaomi_sm8250_kernel/` was used as the Linux reference:
`drivers/iommu/arm-smmu.c:arm_smmu_write_context_bank()` deliberately leaves
`SCTLR.M` clear for `DOMAIN_ATTR_S1_BYPASS`/`DOMAIN_ATTR_EARLY_MAP` in
stage-1; `arch/arm64/mm/dma-mapping.c:arm_iommu_get_dma_cookie()` selects
`arm64_swiotlb_dma_ops` for S1 bypass instead of an IOVA allocator. The port
imports that address-mode distinction, not the Linux device model or an
unconditional copy of its initialization writes.

The backend now validates the inherited stream/context and returns `PA=IOVA`
when stage-1 translation is off. The diagnostic check is `stage1-bypass`.
No table reader is required in that branch; stale/inactive table registers
are not interpreted as active descriptors. Non-AArch64, nested-stage,
unsupported SCTLR modes, ambiguous/shared streams and real faults still
reject. Every raw saved context register is guarded against later changes.
The display lifecycle remains deferred: only a composed back buffer can
trigger autorefresh drain, SSPP address/flush/START and real completion.
Boot restoration and fault pinning remain mandatory; no console fallback,
SMMU reconfiguration, fault acknowledgement or TE-copy experiment was added.

Offline coverage includes the previous 128 ARM64 runtime cases plus 39 new
bypass cases in `check-sde-bypass.inc`. They replay the observed CB4 context
and L11r geometry, four alternating completed frames, no-frame return, safe
boot-source/autorefresh restoration, all real fault bits, buffer overflow,
noncontiguity and overlap, missing/duplicate/shared streams, context changes
between acquire/commit and during return, completion/drain/restore timeouts.
The whole mock SMMU mapping is `mprotect(PROT_READ)` during backend execution:
any unexpected SMMU write aborts, and table-map counts must remain zero.
The boot pixel contents must remain byte-identical. This caught and fixed
clearing newly allocated memory before checking physical overlap.

`check-sde-smmu-linux.py` now executes the SCTLR setup block extracted directly
from the supplied local Linux writer for 256 stage/bypass/early-map/cache/fault
policy combinations, in addition to 4096 fault/format cases. GCC and Clang
ASan/UBSan core/Linux comparisons, queue/console tests, panel and export checks
are part of the offline validation. Transcript omissions in SMR, CB address,
layer-map and enable bits are not grounds for weakening raw-state checks;
the fixture preserves legal SID coverage and derives CB4 at `0x15084000`
from the reported IDR1.

The user declined further phone boots. Do not request another boot/photo or
claim that offline replay proves physical panel output. The backend remains
opt-in; device presentation is still unverified.

The DEVELOPMENT kernel builds successfully. Packaged artifact:
`artifacts/munch-build/munch-iosroot-l11r-sde-bypass-r12.img`,
12,107,776 bytes, SHA256
`bcd810f61577f2ac9f26da4bdd16dcb7d4a45025b119ba9e63bcb123c56c3b6f`.
Adjacent build/pack logs, `verify-sde-bypass-r12.py`/`.log` and `.img.sha256`
record Android header/boot-ID checks, exact kernel/helper/loader/DTB/FD/shim,
boot arguments and all four staged display-source hashes. The r11 image is
unchanged. No device boot, flash or installed-APFS write was performed.

### r13: reserve low-address buffers before the resident RAM disk

The r12 handset log verifies all 2532 boot pages with `stage1-bypass` and
matching PA, then fails at `back-buffer-mapping`. No swap was accepted and
no autorefresh/source change occurred. The old log groups allocation,
prepare and mapping failures; it does not identify which allocation check
returned. The local loader/XNU sources expose a concrete architectural
problem with that path: `topOfKernelData` follows the multi-gigabyte resident
APFS image, and `arm_vm_init.c` starts free VM pages after it. For the known
Munch RAM-root layout these pages are above 4 GiB. The same limitation was
already observed and fixed for SCM metadata (see `../GPU-BRINGUP.md`).
Requesting two approximately 10-MiB low-address allocations from that VM is
not a viable allocation strategy; the r12 mock unconditionally provided them.

For `--sde-backend`, `pack-iosroot.py` now reserves `0x1444000` bytes after
the kernel and existing SCM reservation, moves the small helper forward,
and publishes two u64 values (PA/size) in `/chosen/munch-sde-buffers`.
`munch-sde-memory.h` defines the shared geometry and bounds checks. Reservation
planning checks the claimed EFI window, 32-bit address limit and remaining
space before the resident root, including the helper. It does not borrow
Display Reserved RAM, relocate the large root, shrink XNU RAM or write MMIO.
Backend-off images have no new reservation or property.

The kernel accepts only the exact pool size/alignment in its static RAM
range, after the loaded kernel extent, disjoint from boot pixels, SCM and
both RAM disks. It verifies every 4-KiB physical offset before clearing any
pool bytes and uses the existing cached static mapping, not an I/O alias.
The two back buffers and, when required by a translated domain, private
page tables use this reservation without a VM allocation. In inherited
bypass the table workspace stays unused and SMMU remains read-only.
All storage remains reserved after failures and during unsafe loader return.
The legacy VM path now requests host physical contiguity and explicitly
disables mapper translation for both allocation and physical-segment lookup.

Offline checks now total 217 ARM64 runtime scenarios: the previous 167 plus
50 reserved-pool cases with the low-memory VM allocator forced to fail.
They cover repeated presentation/return, malformed/high/overlapping pools,
kernel/SCM/ramdisk/boot exclusion, broken static mappings, absent-property
allocation failure, faults/drift/timeouts, and owned-table publication/return
from the reserved workspace. Invalid reservations leave pool bytes unchanged.
`check-sde-memory.py` executes the actual generated loader allocation block
for all four GPU/SDE combinations and checks boundary failures under GCC/Clang
ASan/UBSan. Existing local Linux, queue/console, panel and export checks pass.

Artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-memory-r13.img`,
12,109,824 bytes, SHA256
`bbd93d71071b44c90b5ce1f31a14d1ab1fcdf56036783746c3235f4692a9beca`.
The DEVELOPMENT build and independent verifier pass, including Android boot
ID, exact payload parts, five staged display-source hashes and reservation
layout against the actual kernel/helper. At the previously observed example
window `0xf4000000`, the pool is `[0xf8308000, 0xf974c000)`, the helper ends
at `0xf9b4c000`, and the resident root starts at `0xfc000000`. These are modeled
layout results, not a new hardware memory-map capture. The r12 image and
FD/shim/live DT/boot arguments are preserved. No phone boot or flash was done
by the build process. The user subsequently confirmed that r13 reaches the UI
and tearing is gone, but reported roughly 10–20 FPS by impression and broken
screen-off. That is a device result, not a measured frame-rate benchmark.
Do not request another boot as the default next step.

## r14 frame-path overhead and panel blanking: 2026-10-29

This revision does not claim a completed native modeset or a measured handset
speedup. It preserves r13's working route, clocks, PHY, DSC/PPS, TE timing and
loader reservation. The default backend remains unchanged/off.

- Busy completion polling no longer rescans every SMMU stream and all
  CTL/LM/SSPP registers each millisecond. It checks power, context faults,
  underrun, ping-pong completion, DSI busy and CTL progress. A complete raw-state
  guard still runs before submission and before completion acknowledgement;
  drift at completion pins the pending buffer, never reports a successful swap.
  The shared SMMU mutex is released during the post-submit sleep, not held for
  an entire display transfer. First-frame autorefresh drain is unchanged.
- A validated full-screen bottom layer overwrites the back buffer directly,
  skipping a redundant 10,368,000-byte copy from the previous frame. Partial,
  invalid and missing bottom layers retain the original preservation path.
  GPU surface readiness, CPU scaling and cache cleaning are still required.
- `MUNCH SDE PERF r14` reports acquisition, CPU composition, commit, cache-clean
  and hardware-wait time plus guard/poll/copy counts over each five-second
  drawing window. These are driver measurements, not fabricated vsync events;
  clean/wait are subsets of commit time. Existing GPU-wait/FPS logs remain.
- `RequestPowerChange` no longer unconditionally returns `Unsupported` for SDE.
  Under the display lock it drains refresh, sends L11r DCS `0x28` (20 ms) or
  `0x29`, restores DSI command registers, then validates the unchanged route.
  The sender enables/restores the DMA-done mask, clears stale W1C completion,
  and rejects DSI errors/timeouts. No `0x53` frequency/control change or
  sleep-in/out is sent on this path. This is display blanking, **not** full
  rail/PHY suspend. Queued swaps canceled while blank return `Aborted`, release
  waiters and do not increment hardware frames or permanently fault the queue;
  new swaps return `NotReady`. Wake permits fresh hardware-completed frames.
- A failed panel transaction latches an error and keeps buffers pinned. While
  blanked, during a panel transaction, or after its failure, safe loader return
  is refused rather than starting DMA with a possibly absent TE source. After
  a successful unblank, the ordinary proven loader-return sequence is available.

Validation includes the original 217 ARM64/QEMU scenarios plus ten delayed-DMA/
power cases, including blank/resume with read-only bypass SMMU and the reserved
pool, clock loss, underrun, SMMU faults, route drift, timeout, pre-first-frame
blanking, and return refusal. A modeled 12-ms transfer uses thirteen light
polls but only five full guards for a steady-state frame, with no shared lock
held during those sleeps. This operation-count assertion is not a device FPS
measurement. Host tests execute the actual swap/power methods and W1C-aware
DCS sender, comparing FIFO writes with compiled functions from local Linux.

```sh
python3 devices/poco-f4/display/check-sde-runtime.py
python3 devices/poco-f4/display/check-sde-swap.py
python3 devices/poco-f4/display/check-sde-dsi.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel
```

### Native modesetting background (r14)

The local L11r 60-Hz profile has a 1.1-GHz DSI bit clock, 14,000-us MDP transfer
budget, 1080x2400 active area and DSC slices 540x8. Linux coordinates panel
commands with DSI host/PHY/PLL, DSC/PPS, SDE tear-check, clocks and bandwidth;
copying only porch values or DCS frequency selection is not that modeset.
`sde_encoder_phys_cmd.c` derives tear-check from the actual vsync-clock rate
and mode totals, not from an arbitrary `fb_hz` value. The live DT also differs
from the standalone panel source in PHY bytes, as recorded in
`l11r-observed.json`.

Linux itself has a continuous-splash path: `dsi_display_cont_splash_config()`
adopts host/panel state, and `dsi_display_enable()` skips ordinary panel enable
when `is_cont_splash_enabled` is set. Inheritance therefore does not by itself
prove a 10–20-FPS ceiling. A future XNU-owned mode must implement the complete
prepare/enable/disable/resume transaction and loader-state restoration, using
local `dsi_display.c`, `dsi_panel.c`, `dsi_ctrl_hw_cmn.c`, `dsi_phy_hw_v4_0.c`,
`sde_encoder_phys_cmd.c`, `sde_hw_intf.c` and Kona DISPCC/DSI PLL sources.
This revision deliberately does not represent partial register writes as a
native Android-equivalent modeset.

Built artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-pacing-r14.img`,
12,111,872 bytes, SHA256
`419a206e9a6256ee3ea882d1766963fa08c9db7b1204a3136096938d70f4583d`.
The DEVELOPMENT build, GCC/Clang sanitizer tests, all 227 ARM64 scenarios,
local Linux/panel/export checks and the independent payload verifier pass.
The exact FD/shim/live DT, boot arguments, loader reservation and r13 image
are preserved. No device test, flash or installed-APFS write was performed.

On October 28, 2026 the full DEVELOPMENT build completed successfully, including
IOKit/Libkern/Private symbol sets and both export checks. The Linux iig generator,
generic ARM64 private export selection and seekable check outputs were repaired;
see `../../../docs/xnu-linux-build.md`. The first packaged SDE image reached
the legacy GL-test triangle on the user's phone and then appeared stuck; no
runtime log was available. That direct test blit does not prove an SDE commit.
The verbose r2 image suppresses that blit and retains the boot console until
the first completed compositor frame. Later r13 presentation was user-confirmed;
the historical r2 result alone did not establish it.

Host checks (temporary binaries only):

```sh
python3 devices/poco-f4/display/check-sde.py
python3 devices/poco-f4/display/check-sde-runtime.py
python3 devices/poco-f4/display/check-sde-swap.py
python3 devices/poco-f4/display/check-sde-linux.py \
  --linux-tree /home/danya2271/xiaomi_sm8250_kernel
python3 devices/poco-f4/display/check-sde-refresh-linux.py \
  --linux-tree /home/danya2271/xiaomi_sm8250_kernel
python3 devices/poco-f4/display/check-sde-smmu-linux.py \
  --linux-tree /home/danya2271/xiaomi_sm8250_kernel
python3 devices/poco-f4/display/check-panel.py \
  --linux-tree /home/danya2271/xiaomi_sm8250_kernel
```

The first runs the actual backend under ASan/UBSan with mock MMIO/page tables.
It checks source/flush/start order, stale and unrelated IRQs, DSI busy, mapping
holes, aliasing, cache-clean failure, underruns, and buffer retention after
timeouts and isolated page-table construction/restoration. The runtime test
compiles the actual ARM64 adapter and snapshot reader with mock IOKit/MMIO and
runs 128 scenarios under `qemu-aarch64` (requires `aarch64-linux-gnu-g++`). It
checks imported and installed mappings, the observed DSI/mux/autorefresh tuple,
optional ECC/CRC bits, raw-state changes, repeated flips, refusal paths, timeout
pinning, loader restoration, qualified VIG/border presentation, new control
guards and rejection before takeover. r7 coverage includes both opaque blend
encodings, stages 4/6, CSC metadata, an opaque but unrelated blend0, unstaged
dim layers, full blend/CSC state changes and literal revision/CTL/format rejects.
The swap test compiles the actual queue/wait methods with
host locks under ASan/UBSan and checks compositor/notification wiring. Panel
tests also cover packaging gates. None of these emulate the physical display
or constitute hardware validation.

## L11r live-DT comparison, 2026-10-28

The connected phone's `/sys/firmware/fdt` was read over ADB without rebooting or
writing device state. `l11r-observed.json` is the sanitized comparison report;
it contains no serial number or complete boot command line. Reproduce with:

```sh
python3 devices/poco-f4/display/inspect-panel.py \
  --dtb /path/to/live.dtb \
  --linux-tree /home/danya2271/xiaomi_sm8250_kernel \
  --output /tmp/l11r-profile.json
```

Exit status 1 means the profiles differ, not permission to choose the Linux
values. The live selection is L11r, DSI0/PHY0, four lanes, external TE GPIO66.
Its 60/120/144-Hz timings, DSC geometry, full PPS, and on/off/timing-switch
command sequences match the local panel source. However:

- Live PHY timings for all three modes are
  `00 24 0a 0a 26 24 0a 0a 06 02 04 00 1e 1a`.
- The standalone local panel source supplies no PHY timings/topology for
  60/120 Hz, and supplies different 144-Hz PHY timings:
  `00 24 0a 0a 1a 19 09 0a 09 02 04 00 1e 0f`.
- Live topology is `<1 1 1>` for all three modes. The local standalone panel
  source supplies that topology only for 144 Hz. Board includes can supply
  additional overrides; this comparison does not claim to build every Linux
  board overlay.

The actual panel source is
`arch/arm64/boot/dts/vendor/qcom/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi`, outside
the sibling `display/` directory. The sim-panel and shared SW43404 defaults
are not authoritative for this phone. Do not program the PHY from those
defaults or from the differing standalone L11r value. The report still marks
the active **bootloader** mode and scanout takeover as unverified.

The full `MUNCH_STAGE=1 ./xnu-build.sh -j8 -k` build now succeeds, including
symbol-set generation and both export checks. The initial DriverKit/export
failures and Linux symbol-set output seek failure are fixed; see
`docs/xnu-linux-build.md` for the build changes and regression checks.
This is host/build validation only. The first packaged image's reported static
GL-test triangle is not proof of SDE presentation. No phone reboot or flash was
performed by the build scripts as part of this stage.

The framebuffer's `RequestPowerChange` blanks/unblanks the panel with DCS
display-off/display-on commands through the Kona DSI0 embedded FIFO. It checks
the existing display clocks and power domain before touching DSI registers and
waits for command completion. It deliberately does not send sleep-in/out or
enter ULPS: XNU does not yet own the DSI PHY, panel regulators, reset GPIO, or
their resume sequence. The path compiles but still needs handset validation.

Refresh-rate switching is not implemented. The panel DTS defines 60/120/144-Hz
timings and vendor command sequences, but Linux applies these inside a modeset
that also updates SDE timing, DSI host timing, PHY state, and clocks. XNU still
inherits the bootloader's 60-Hz scanout and advertises only 60 Hz; `fb_hz`
changes software pacing and notifications, not the panel's hardware mode.

## UMI command-mode scan adaptation: measurement first

The UMI `hk_display.c` command-mode path checks `te_initial == 2340`,
calibrates TE counter wraps, and checks the write-pointer counter before
copying. Munch's selected panel is 2400 lines tall. Its captured live DT
lists INTF offsets 0x6b000/0x6b800/0x6c000/0x6c800 at MDP base 0xae00000,
so the first DSI interface is 0xae6b800, as in UMI/Kona.

`munch-display-scan-r1.img` observes INTF1 TE configuration and three
200-ms windows of raw and write-counter ranges, wraps, busy/idle samples,
and the last observed busy phase. It does not boot iOS or modify DSI/DPU
registers. GCC display AHB at 0x10b00c and DISPCC AHB at 0xaf02080 may be
temporarily enabled, then restored on every ordinary return path. MDSS
power at 0xaf03000 must already be on; the probe does not power it up.
Clock/power guards precede INTF reads. This does not provide recovery
from a hardware bus hang or an unexpected reset.

The counter semantics and offsets were checked against Xiaomi's
[alioth-r-oss sde_hw_intf.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/techpack/display/msm/sde/sde_hw_intf.c),
[gcc-kona.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/drivers/clk/qcom/gcc-kona.c),
[dispcc-kona.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/drivers/clk/qcom/dispcc-kona.c),
and the GDSC offset against Linux v6.12
[dispcc-sm8250.c](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/dispcc-sm8250.c).
These references establish register layout, not a proven safe copy interval
on this handset. No UMI 2340-line calibration is assumed valid for munch.

`uefi/check-display-scan.py` executes the actual probe with mocked MMIO
and time. ASan/UBSan checks cover period measurement, power/clock guards,
clock timeout, restoration and an invalid timer. Hardware results are
required before enabling controller-based presentation in the iOS image.
