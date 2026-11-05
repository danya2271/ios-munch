# Munch SDE Porting Handoff

## r47 regression rollback and queue wakeups: 2026-11-01

- Reject r46: the handset reports `frames=17 fps=3 compose=31655` microseconds,
  `full_compose=17`, `completion=17016`, `queued=0`, and no active completion
  IRQ. The new serial scaler regressed CPU composition from earlier 3–4 ms
  samples to about 31.7 ms. Mathematical pixel equivalence did not establish
  hardware performance; do not reuse that scaler.
- Full-frame copying now accepts only equal source/destination dimensions.
  All scaling returns to the existing parallel row-worker implementation.
  Direct framebuffer behavior, logical display geometry and panel timing stay
  unchanged. `MUNCH SDE PERF r22` identifies this revision.
- The presenter used to check its queue only once before waiting until PP_DONE.
  Enqueue now signals the same event as completion, and the idle presenter may
  yield its completion wait when a swap arrives. Submission/error-drain waits
  still require actual hardware completion. The sequence is sampled before
  polling and queue checks so registration races do not lose a wakeup. Previous
  `queued=0` telemetry alone did not establish that the producer submitted no
  frames during transfer.
- Queued surfaces get a nonblocking GPU-ready check. A late GPU drains the
  older transfer before blocking for rendering, so a GPU stall cannot prevent
  the older PP_DONE from being serviced until after its completion deadline.
- Partial composition preserves the latest pending frame, not the older front
  buffer. It only reads the pending DMA source and composes into another buffer.
  Portable tests check alternating updates retain unchanged pixels and never
  write the pending source; asynchronous swap tests exercise queue arrival
  during an IRQ/status wait and before wait registration.
- The r47 candidate omits per-frame GL/touch tracing to avoid adding diagnostic
  overhead. Keep default GL fences, eight CPUs and periodic SDE performance
  summaries. The wallpaper issue remains unconfirmed, and no 60/120-FPS result
  is claimed before a new handset measurement. Never flash or probe the phone
  automatically.
- Built candidate:
  `artifacts/munch-build/munch-iosroot-l11r-sde-native-r47-queue-wakeup.img`,
  SHA-256 `66d938c67f9c860e934d49bebc78d8e164eb211b4ca5c503b0c1dd1a0c1a71d0`.
  Source XNU build, ASan/UBSan queue/core tests, full ARM64 runtime suite,
  reservation checks and independent boot-payload verification pass. The
  source snapshot, kernel, build/test logs and manifest are retained beside it.

## Full-frame compositor fast path: 2026-11-01

Historical r45/r46 experiment, rejected by the device result recorded above.

- The user reports direct framebuffer at roughly 40–50 FPS and much worse
  pacing with SDE; wallpapers have also been absent for several builds.
- Native SDE now has a full-screen CPU composition path for a full IOSurface
  into the full logical display. It maps rows and columns with the compositor's
  nearest-neighbour rule, uses a 5:3 unrolled reducer for 1800-to-1080 panels,
  and falls back to the existing compositor for partial layers and diagnostics.
- `MUNCH SDE PERF r21` reports `full_compose=N` so a device run confirms whether
  the new path is hit. Host swap tests compare it against the generic scaler,
  including padded strides and arbitrary ratios; native runtime, mode and memory
  tests pass.
- Candidate: `artifacts/munch-build/munch-iosroot-l11r-sde-native-r46-fast-compose.img`.
  It retains `cpus=8`, USB/GPU diagnostics and `wifi_qca_status=1`; system/helper
  APFS hashes match r44. The handset has not tested it. This only reduces CPU
  composition work; the vendor command-mode transfer budget is about 14 ms, and
  no SDE bandwidth/QoS vote has been added because the remaining Kona paths have
  not been qualified in XNU.
- The wallpaper issue is not confirmed fixed. r42's `glsync=0` test was rejected;
  r46 leaves the default GL fence enabled. Compare the initial swap `set` /
  `cleared` masks, layer surface IDs, and `full_compose` count if the wallpaper
  remains absent. The current compositor still does not consume `layers cleared`
  as an independent operation.

# SwapWait retirement and CA pacing: 2026-11-01

- The user reports persistent low FPS and stutter, with recent samples showing
  roughly 15–24 FPS and about 15–18 ms of hardware completion per swap. The
  inherited panel mode remains approximately 60 Hz; this change does not claim
  120-Hz operation.
- The native presenter previously waited for PP_DONE before beginning CPU
  composition of the next swap, serializing a roughly 3–4 ms compose stage with
  the DSI transfer. It now takes one queued swap while the prior transfer is
  active and composes into the alternate reserved buffer. It waits for the
  previous PP_DONE before programming the next source address and submitting.
- Native `SwapWait` returns after the frame has been copied into a driver-owned
  back buffer, allowing CoreAnimation to submit the next swap. IOSurface refs
  and hardware completion state remain held until PP_DONE; a transfer failure
  stops future presentation rather than rewriting the already-returned result.
- Buffer acquisition excludes the pending DMA source. With an in-flight transfer
  it uses the completed front buffer as the compose target; the pending source is
  never modified. Partial updates avoid a redundant self-copy when that target
  is already the previous front buffer. Error completion remains FIFO and keeps
  uncertain DMA buffers pinned.
- Device testing of r38 still reports `overlap=0` and 7 FPS, with input lag.
  The early SwapWait change alone therefore did not establish producer overlap
  or improve pacing. r20 adds `retired=N` (SwapWait released) and `queued=N`
  (a queued swap was found during transfer), alongside `overlap=N`.
- The r39 diagnostic image adds only `ca_vbl=1`, routing CoreAnimation through
  the driver's sampled hardware VSync notifications. It does not enable
  `glwait=1`, which would add a GPU completion stall. Device result is pending.
- After the user reports r39 stalls on the Apple logo, r40 drops `ca_vbl=1`.
  It also transfers graphics-console ownership at successful native submit/
  retire, before PP_DONE, so console logging cannot write into a back buffer
  while SDE DMA is reading it. This is host-tested; it is not yet a confirmed
  cause of r39's boot stall or r38's low FPS.
- The user's r40 report shows `retired=6`, `queued=0` and `overlap=0`, so the producer
  is not reaching the native swap queue during the hardware transfer. The r41
  helper disables per-call GL error polling in `eagl.c`; `glerrors=1` restores
  it for diagnostics. This targets WindowServer stalls that can also make touch
  appear stuck. The kernel/display path is unchanged from r40.
- r42 adds `glsync=0` as a separate A/B test. `my_glFlush()` still calls
  `glFlush()` before `frame_end`; only the extra zero-timeout fence used to
  force Mesa batch submission is removed.
- r42 telemetry rejects that test: HakiosGPU fell to 9--17 submissions per
  interval, so the Mesa fence is required to flush batches. r43 restores the
  default `glsync=1` and enables `glframes=1 glslow=20000 touch_lat=1` for a
  short producer-side trace.
- Swap queue, buffer ownership, native runtime, memory, DSI, refresh, mode and
  local Linux-reference checks pass. `MUNCH_STAGE=1 ./xnu-build.sh -j8 -k`
  succeeds. Candidate image:
  `artifacts/munch-build/munch-iosroot-l11r-sde-native-r43-glframes-60hz.img`
  (SHA-256 `89d7a04e6d58b1f890c051ebd8aa82a36308de0c7483b3eff0cd92ef04c4c1ae`).
  It uses `cpus=8` and has not been device-tested; no flash or APFS write was
  performed.

## r34 refresh recovery and panel wake candidate: 2026-10-31

- The user reports that r33 displays Apple about 10 seconds after the CPU-7
  firmware message, then turns the display off after another 20–25 seconds
  without reaching the desktop. USB logs are unavailable; do not keep polling
  ADB, change the CPU count, or call this an established root cause.
- Safe-native refresh restoration now distinguishes a missing autorefresh
  completion from a failed submitted frame. Only a timeout with a healthy
  guard, the expected source and an idle route can restore the inherited
  controls and release refresh ownership after verified readback. This avoids
  permanently poisoning swaps for a completed frame; actual faults and pending
  DMA remain pinned. Full takeover and the strict restore API are unchanged.
- The inherited panel wake path was missing Sleep Out despite sending Sleep In
  on power-off. It now sends `0x11`, waits 120 ms, restores `0x53=0x20`, then
  sends `0x29`, stopping on any command failure. No reset, PLL, PHY or clock
  takeover is added. This is a wake fix, not the full vendor panel-init sequence.
- Runtime tests exercise two successive recovered frames and panel wake,
  along with no-recovery on pending-DMA timeout. Portable tests inject guard
  faults and failed readback; swap tests check wake order and command failures.
  Device boot, sustained display operation and performance still require a
  handset test. Build r34 separately and retain r33 and older recovery images.

## Native L11r safe handoff: 2026-10-31

- `--sde-backend --sde-native` now keeps the bootloader's active panel mode and
  takes ownership only of guarded SDE buffer commits. It drains and restores
  inherited autorefresh around each commit, so the first native frame cannot
  blank the panel by entering an unverified sleep/PLL/reset sequence.
- Native VSync samples the inherited INTF1 hardware frame counter directly.
  The safe path does not map or write PHY/PLL/clock registers and does not send
  panel initialization or brightness commands.
- The old full takeover remains behind `munch_sde_takeover=1` for diagnostics.
  It is not the default because XNU still lacks the complete Linux panel rail,
  reset GPIO and DSI power sequencing required to make that takeover reversible.

## Historical full native L11r mode, r15: 2026-10-29

- The user tested r14: severe stuttering and occasional approximately half-second
  freezes remain. Do not describe r14 as a verified performance fix. The user
  explicitly requests ownership of the display mode, not continued boot timing
  inheritance. The GPU has not been established as the cause.
- `--sde-backend --sde-native` selects the new fixed 1080x2400@60 mode. This is
  separate from the inherited r13/r14 path; old images must remain available.
  `munch-sde-mode.h` implements a deferred transaction, first entered after a
  composed back buffer, its mappings, and autorefresh drain are verified.
- The transaction sends panel off/sleep, stops link clocks, configures DPHY4.1
  and the 7nm-v4.1 PLL for 1.1 GHz, programs DISPCC byte/pixel/escape/vsync
  clocks, DSI command streams, DSC0/PPS, VIG0/LM0/CTL0 and tear-check. The vsync
  source is explicitly XO/19.2 MHz; the Linux line count is 89, from
  `19200000 / (3592 * 60)`, rather than inherited `0x885`.
- Register programs are reproduced by executing functions extracted from the
  user's **local** Linux tree. The generated DSC PPS is checked byte-for-byte
  against the L11r panel command. PHY timing uses the previously captured live
  DT bytes, not the differing standalone DTS bytes. Packaging rejects a changed
  live 60-Hz profile. No network source is used.
- The 129-byte panel PPS uses memory-backed DSI DMA in the verified unused tail
  of the composed back buffer, never the 64-byte command FIFO. This is the same
  unsecure SMMU address space selected by local `dsi_host_alloc_cmd_tx_buffer()`.
  No new SMMU context, SID, or forced bypass is installed.
- Native VSync is sampled from the INTF1 tear-check read-pointer frame counter
  (`INTF_TEAR_INT_COUNT[31:16]`) even when there is no swap. r14 only delivered
  VSync after a completed swap, coupling the next producer frame to completion
  of the previous one. A 1-ms timer now samples a hardware event; it does not
  manufacture VSync or SwapWait completion. The inherited mode retains its old
  notification behavior.
- Backlight stays zero during panel initialization, then becomes DCS value 512 only
  after the first hardware-completed frame. The normal blank/unblank API remains
  DCS 0x28/0x29, not full suspend. The one-time sleep/PLL/panel sequence can blank
  the display while that first frame is prepared for scanout.
- Native takeover is deliberately not reversible to ABL. The image disables the
  600-second automatic loader-return timer; `loader_return()` also refuses a
  native request **before** stopping other CPUs. Failed DMA/clock/PLL/readback
  operations stop presentation and retain potentially referenced memory. No
  success, source switch, or buffer reuse is synthesized on a failure.
- This is a fixed active-display modeset, **not** a complete DRM/Android stack.
  It still needs powered firmware resources and a qualified single inherited
  route to make the takeover safe. Panel rails/reset GPIO, system bandwidth
  votes, full suspend/resume, hardware-interrupt delivery and adaptive refresh
  are not ported here. The CPU compositor remains; this is not native DRM plane
  composition. VSync polling can coalesce events during a long busy
  section. Handset bring-up and FPS with r15 are not established by host tests.
- `check-sde-mode.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel` covers
  successful programming, full PPS DMA, hardware-only idle VSync, readback drift,
  every write failure boundary, and rejection of busy/invalid resources. Keep
  running the existing runtime, swap, memory, DSI and Linux-reference checks.

## Current device result and r14: 2026-10-29

- The user confirms r13 boots into the UI and tearing is gone. They report
  roughly 10–20 FPS by impression and non-working display-off. Preserve this
  working scanout path; neither GPU slowness nor wrong panel timing has been
  established as the remaining bottleneck.
- r14 removes full-route/SMMU rescans from busy polls, releasing the shared
  SMMU lock while waiting for hardware. Complete validation still occurs at
  submission and completion; faults/drift/timeouts never recycle DMA memory.
  Full-screen bottom layers skip the old-frame copy; partial updates preserve
  pixels. Five-second `MUNCH SDE PERF r14` separates driver stages and counts.
- SDE display blank/unblank now uses guarded DCS 0x28/0x29 under the display
  lock, with proper fresh DMA completion and register restoration. Queued
  cancellation is not a sticky rendering fault or a fake frame. This is not
  full panel sleep/PHY shutdown. Loader return is refused while blanked or
  after an uncertain panel command; successful wake restores eligibility.
- Native modesetting is still incomplete. The user wants Android-like mode
  ownership, but do not call isolated porch/TE/DCS writes a complete modeset.
  Local Linux also supports continuous-splash inheritance. See README's r14
  section for exact local source responsibilities and the existing live-DT
  PHY mismatch. Do not modify working timing/PLL/DSC merely to hide frame-path
  overhead, and do not assume a measured device speedup from host tests.
- Ten new ARM64 scenarios supplement the 217 existing ones. Tests cover
  delayed completion, light-poll operation counts, faults, pre-frame and
  running blank/resume, immutable bypass SMMU and reserved buffers. New
  `check-sde-dsi.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel` compares
  the actual DCS sender against local Linux FIFO functions with W1C MMIO.
- No device reboot/flash or installed-APFS write is performed by this work.
  Do not request another boot/photo as the default next step.
- DEVELOPMENT build, 227 ARM64 scenarios, GCC/Clang sanitizer checks, local
  Linux comparisons, panel/export checks and independent image verification
  pass. Image: `artifacts/munch-build/munch-iosroot-l11r-sde-pacing-r14.img`,
  12,111,872 bytes, SHA256
  `419a206e9a6256ee3ea882d1766963fa08c9db7b1204a3136096938d70f4583d`.
  Adjacent build/pack/verification logs and checksum are retained. The r13
  image is unchanged. Subsequent user testing reports continued severe lag with
  r14; there is no subsequent blanking result.

## Port progress: 2026-10-28

- `munch-sde.h` and `munch-sde-xnu.h` now implement an opt-in fixed-mode
  presentation path, connected to `MiniIOMobileFramebuffer`: pinned back
  buffers, verified imported/owned SMMU mappings, cache clean, SSPP source
  switch, CTL commit and bounded hardware completion. The compositor no longer
  draws into ABL scanout when this path is requested. Queue completion,
  `SwapWait` and notifications use real completion, not timer success.
- Enable with `loader/pack-iosroot.py --sde-backend`; default remains off.
  Packaging checks the selected L11r live DT and kernel presenter/quiesce
  signatures. Runtime qualification is still mandatory. Unsupported autorefresh
  configurations, active VIG processing, inaccessible firmware tables and unsupported SMMU modes reject
  startup; no live-scanout fallback, forced context replacement or PHY writes.
- Runtime imports identity mappings if fully verified, otherwise installs
  private L2/L3 tables in an empty root slot only. Timeout/fault retains all
  potentially DMA-visible memory. Loader return restores the boot source and
  removes owned mappings after completion; unsafe handoff is refused.
- Portable ASan/UBSan, actual ARM64 runtime with mocked IOKit/MMIO under QEMU,
  actual swap-method tests with host locks, and L11r/packer checks are available
  in `display/check-sde*.py` and `check-panel.py`. See `display/README.md` for
  prerequisites, invariants and activation. These host tests alone do not
  prove presentation; the later r13 user report does confirm visible output.
- The user confirmed L11r. A fresh live DT was captured from the connected
  Android phone over read-only ADB on October 28, 2026. The sanitized
  `l11r-observed.json` confirms DSI0/PHY0, GPIO66 TE, 1080x2400, DSC 540x8,
  and the local source's timing/command/PPS values. It also records differing
  PHY timings and missing standalone-source topology for some modes.
- The correct local L11r file is
  `arch/arm64/boot/dts/vendor/qcom/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi`, not
  the shared `display/dsi-panel-sw43404-amoled-dsc-wqhd-cmd.dtsi`. Inspecting
  only the shared display directory misses this panel source.
- The user's r3 log supplies a partial **ABL/XNU** register snapshot (see
  below), but not the complete CTL/SSPP route or SMMU context. Android's live
  DT and DRM mode list cannot establish those missing runtime values.
- The full DEVELOPMENT build now passes, including symbol sets and both export
  checks. Linux iig export generation, generic ARM64 private ABI selection and
  the `/dev/null` seek failure were repaired without disabling validation.
  `tools/check-xnu-exports.py` checks reproducibility and negative export cases.
  The first packaged image was tried by the user: it displayed the legacy
  GL-test triangle and then appeared stuck, with no runtime log available.
  This proves neither SDE initialization nor a completed SDE commit.
- The verbose r2 image uses `--verbose --sde-backend --sde-probe`. Legacy GPU
  test blits are rejected in requested SDE mode, and the boot console is hidden
  only after a completed compositor frame. Named initialization/acquisition/
  commit errors aid on-screen diagnosis. No SpringBoard-ready IPC is inferred
  from this display handoff, and unsafe scanout rollback remains prohibited.
- The user's r2 log shows active WindowServer with repeated `SwapEnd` errors
  transcribed as `0xe00002d0`, but no initialization reason. That code is
  `kIOReturnNotAligned`, not the backend's initialization-rejection code
  `0xe00002d8`. The r3 diagnostic kernel repeats the saved initialization
  phase/result and actual kernel `SwapEnd` statistics every five seconds until
  healthy presentation. No alignment workaround or weakened qualification was
  applied without evidence; modem/Bluetooth service crashes remain separate.
- The r3 log identifies `init=unsupported phase=inherited-route`, zero
  accepted/completed swaps, and `rev=60000000 mux=0 tear=1 auto=00000001
  dsi=1f5`. The route check incorrectly compared the entire `DSI_CTRL` with
  `0x5`, omitting active data/clock lanes. Linux `dsi_ctrl_hw_cmn_host_setup()`
  in `techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c` confirms bits 4–7/8 for these
  lanes and optional receive ECC/CRC bits 20/24. The r4 check requires the
  four-lane command configuration (`0x1f5`) and allows only those optional
  checks. Missing lanes, video mode and other unknown bits remain rejected.
  The full raw baseline/guard is unchanged; no DSI/PHY programming is added.
  Linux `sde_hw_intf_get_autorefresh_config()` confirms that `auto=1` does not
  enable autorefresh (bit 31). Core and ARM64 runtime fixtures now use the
  observed values instead of the old unrealistic `dsi=5`, with regressions
  for optional checks, invalid links and post-init state changes.
- The r4 fix passes core GCC/Clang ASan/UBSan checks, 16 ARM64 runtime
  scenarios, swap/console checks, 15 panel tests and 14 export tests. The
  full DEVELOPMENT build and packing completed. The new artifact is
  `artifacts/munch-build/munch-iosroot-l11r-sde-dsi-r4.img` (12,099,584 bytes),
  SHA256 `c5db5f4a6c52a1cd078892036148876ec9491b72072aa8ee25b9248f99c67e34`.
  The adjacent manifest/checksum and `verify-sde-dsi-r4.py`/`.log` record
  independent Android header, boot ID, rebuilt shim, FD, live DTB, payload,
  kernel marker and stage-hash checks. It preserves the r3 boot arguments,
  including verbose logging, SDE probe/backend and the 600-second return
  timer. No device boot/flash or installed APFS modification was performed.

- The user's r4 log now reaches `init=ambiguous-or-unsupported-route` at
  `inherited-route`, still with zero accepted/completed swaps. Link values
  remain `rev=60000000 mux=0 tear=1 auto=1 dsi=1f5`. This confirms the previous
  DSI fix advances qualification, but does not identify the rejected CTL or
  layer condition. No CTL/layer/SSPP register values accompany that log.
- The r5 diagnostic change preserves route acceptance and all write guards.
  `munch_sde_find_route_checked()` is the same route checker with an optional
  diagnostic result: exact `check` and CTL/LM/pipe indices. The original API
  wraps it without diagnostics. Periodic status now repeats `MUNCH SDE ROUTE`,
  all six CTL register rows, candidate CTL/LM maps and configured SSPP rows
  from the immutable saved snapshot. Reporting does not read/write MMIO,
  retry initialization, select a guessed route or enable border/blend modes.
  Core tests cover each named guard, unchanged snapshots/output routes on
  rejection and zero MMIO writes; ARM64 and swap tests cover failed route
  initialization, exact raw log values and the five-second reporting limit.
- The r5 DEVELOPMENT build, core GCC/Clang ASan/UBSan checks, 21 ARM64
  scenarios, swap/console diagnostics, 15 panel tests and 14 export tests pass.
  Packaged artifact: `artifacts/munch-build/munch-iosroot-l11r-sde-route-r5.img`
  (12,101,632 bytes), SHA256
  `278787fb9728a399086dfab8b98bcdf274918c0e8a17329be492a2737e4075ad`.
  `verify-sde-route-r5.py`/`.log` independently check the header, boot ID,
  rebuilt shim, FD, live DTB, payload, exact source-built kernel, r5 route
  markers and stage hashes. Boot arguments match r4. This remains a
  diagnostics-only change: the handset route is still unknown and unverified;
  no device boot/flash or installed APFS modification was performed.

- The user's r5 photograph now identifies `check=border-stage ctl=0 lm=0
  pipe=-1` and an active full-size VIG0 source. The check rejects the border
  bit before decoding the pipe, so `pipe=-1` is not an absent-pipe finding.
  Do not treat every blurred hexadecimal digit as a verified register value.
- The r6 port supports one base-stage source with inherited border color and
  one first-blend-stage source over border with verified constant opacity
  (`blend_op=0x100`, packed alpha `0x00ff0000`). It also enables VIG0-3 only
  after verifying QSEED/CSC bypass and zero pre-downscale/decimation. Higher
  stages, multiple sources, unknown controls, nonopaque/dim blending and
  active scaling remain rejected. Kona offsets and semantics come from
  `kona-sde.dtsi`, `sde_hw_ctl.c`, `sde_hw_lm.c`, `sde_hw_util.c` and
  `sde_hw_sspp.c` in the supplied Linux tree. No routing/blending/scaler/CSC
  registers are written: the existing qualified source/flush/start sequence
  is reused for VIG. The snapshot reader and immutable-state guard now include
  those controls, and the periodic log repeats them. This resolves an
  unsupported route class, not evidence of physical presentation or success
  through the remaining SMMU/idle checks.
- The r6 DEVELOPMENT build and packaging pass. Core GCC/Clang ASan/UBSan,
  35 ARM64 runtime scenarios, swap/console logging, 15 panel tests and 14
  export tests pass. Coverage includes VIG/border flips and loader restoration,
  rejection of active scaling/CSC/resampling/nonopaque blending, raw-control
  changes, timeout pinning and unsafe-return refusal. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-vig-r6.img` (12,101,632 bytes),
  SHA256 `4c50263f143887082291d2e8d649069411484115284c1d91d7d12a0baa28686e`.
  `verify-sde-vig-r6.py`/`.log` verify the Android header/ID, rebuilt shim,
  reviewed FD, live L11r DTB, exact kernel/helper payload, r6/control-check
  markers and stage hashes. Boot arguments match r5. No device boot/flash or
  installed APFS modification was performed; physical presentation is untested.

## r7 qualification follow-up: 2026-10-29

- The latest transcription cannot literally come from the verified r6 path:
  `rev=0` fails `revision` and suppresses r6's saved block; `blend-setup` is not
  an r6 check name. `CTL_TOP=21f00` and `format=23ff7` also reject before blend
  checks. Preserve these guards and request an original photograph rather than
  treating every transcribed digit as authoritative.
- Verified Linux semantics allow a second opaque blend form: op `0x400`
  (background inverse foreground-constant), with packed alpha `0x00ff0000`.
  The original `0x100` form remains supported. CSC10 bit 0 enables conversion;
  bit 1 is source-format metadata, so disabled `csc=2` is accepted without
  writes. Unknown/enable bits and raw-state mutations still reject.
- One directly encoded source at logical stage 1–6 now uses its own blend
  pair. `map=01000005` selects stage 4, LM+`0x68`/`0x6c`; blend0 alone cannot
  qualify it. All eleven Kona pairs are saved/guarded, and enabled dim layers
  in any slot reject. Extra sources and extended-stage encodings still reject.
  Snapshot validity is tracked independently of revision, and r7 logs every
  blend pair with its offset, without MMIO rereads or duplicate mixer dumps.
- Core GCC/Clang ASan/UBSan, 50 ARM64 runtime scenarios, swap/console tests,
  15 panel tests and 14 export tests pass. The literal transcribed tuple is
  a negative regression, not a new accepted route. No DSI/PHY, routing, blend,
  scaler or CSC writes, early takeover, active-buffer fallback or TE experiment
  are added. Successful physical presentation remains unverified.
- Full DEVELOPMENT build and independent image verification pass. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-blend-r7.img` (12,103,680 bytes),
  SHA256 `d268814f69d04c6feb0b8b0e88f94ca62f16b594d3834f17b1ccb5f41d8e77e1`.
  Adjacent `verify-sde-blend-r7.py`/`.log`, build/pack logs and checksum record
  Android header/ID, exact rebuilt shim, unchanged FD/live DTB, kernel/helper
  hashes, r7 markers, source-stage hashes and unchanged r6 boot arguments.
  r6 is preserved. No device boot/flash or installed APFS changes were made.

## r8 layout diagnostics: 2026-10-29

- The user explicitly requires the local Linux tree
  `/home/danya2271/xiaomi_sm8250_kernel`; do not consult substitute GitHub
  branches. The local SSPP format/rectangle and CTL/LM implementation were
  checked during this follow-up. No hardware code from a different tree was
  incorporated.
- The new transcript still cannot reproduce its reported gate: `map=01800005`
  rejects at `multiple-pipes` (VIG0 plus DMA1); using `01000005` with all other
  listed fields unchanged passes the route check. Keep the literal and the
  one-field variant as tests, not as proof of handset register values.
- r8 preserves every r7 layout acceptance condition but replaces `pipe-layout`
  with fifteen named conditions. Saved-state reporting now gives the failed
  condition mask, first condition, raw observed value, comparator and expected
  value(s) directly below the route line. No retries, new MMIO writes, changes
  to scanout ownership or SMMU bypasses are introduced. This is diagnostics,
  not a completed port of the Linux DRM/display stack.
- GCC/Clang ASan/UBSan core tests compare the replacement against the original
  predicate using field mutations and stride bounds. All 65 ARM64 scenarios
  and actual swap/console tests pass, including each layout failure without
  allocations/route takeover, precise log values and rate limiting.
- Full DEVELOPMENT build, 15 local-tree panel tests, 14 export tests and
  independent payload verification pass. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-layout-r8.img` (12,103,680 bytes),
  SHA256 `0e9f7d5bc37e4c7f025407e6417a38dae40c6918b9ff70116ab0fba847ecb4ed`.
  Adjacent build/pack logs, `verify-sde-layout-r8.py`/`.log` and checksum record
  exact source-stage/kernel/helper/DTB/FD/shim identities and r8 diagnostics.
  Boot arguments are unchanged from r7, which is preserved. No device boot,
  flash or installed APFS modification was performed.

## r9 pixel-extension qualification: 2026-10-29

- The user's r8 log names `pipe-opmode`, but `got=0` cannot fail the previous
  equality-to-zero predicate; `map=01800005` also still implies multiple pipes.
  Do not assert that the raw PE flag was observed or that the transcription is
  an exact capture. The following extension is independently backed by the
  supplied local Linux implementation.
- Local `sde_hw_sspp.c` defines PE_OVERRIDE as bit 31 and sets it unconditionally
  during format setup. r9 supports this previously rejected mode, provided all
  C0/C1C2/C3 LR/TB fetch/repeat fields are zero and each required-size register
  matches the source rectangle. Local `sde_plane.c` same-size RGB QSEED3 defaults
  and `sde_hw_sspp_setup_pe_config()` establish these values. Other opmode bits,
  scaling, resampling and nonidentity PE remain rejected.
- Snapshot/guard coverage adds nine PE registers for all eight SSPPs. Mode zero
  keeps its old acceptance but still guards later PE-register changes. No new
  hardware writes, early scanout takeover or forced SMMU/loader rollback are
  added. Status r9 prints decoded `pe-override`, forbidden bits and PE groups.
- New `check-sde-linux.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel`
  compiles the actual local Linux PE writer into a mock-register test and
  verifies its output against the XNU checker. The local writer mirrors LR into
  C3 TB; the test records that behavior without modifying the Linux source.
  Accepted identity PE has both values zero, so it is unaffected by that quirk.
- Core GCC/Clang tests cover 448 pipe/stage/control variants plus all PE-register
  bit mutations and forbidden opmode bits. All 87 ARM64 scenarios and swap/log
  tests pass, including PE-mode frames, mapping ownership, raw-state guards,
  timeout pinning and restoration. Handset frame completion is not yet proven.
- Full DEVELOPMENT build and independent image verification pass. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-pe-r9.img` (12,103,680 bytes),
  SHA256 `850283560fce647283982c85258a45a6b48699bdd3a579569c28faf0aed118c0`.
  Adjacent `verify-sde-pe-r9.py`/`.log`, build/pack logs and `.img.sha256`
  record boot-header/payload integrity and exact source-stage/kernel/helper/
  loader/DTB/FD/shim identities. The r8 image is preserved. Boot arguments
  retain the verbose console, SDE backend/probe, eight CPUs, GPU/DVFS/touch
  and 600-second safe-return timer. No flashing or handset test was performed.

## r10 autorefresh handoff: 2026-10-29

- The r9 log progresses to `check=autorefresh`; PE groups qualify. This gate
  tests bit 31, so the transcribed `auto=1` remains inconsistent. Decimal
  `AUTO: enabled/frames` diagnostics remove that ambiguity; do not rewrite
  the transcript as a verified register dump.
- Allow enabled frame-count-one autorefresh without touching hardware during
  initialization. Before the first composed frame, follow local Linux
  `sde_encoder_phys_cmd_prepare_commit()`: disconnect external TE (VSYNC bit
  20), clear autorefresh enable (bit 31), drain write-pointer/DSI/CTL activity,
  require debug-bus idle twice 25 ms apart, then restore exact TE state.
  `sde_hw_intf.c` and `sde_hw_top.c` provide the accessors; local `kona-sde.dtsi`
  places the MDP debug block at +0x1000. DSI0/INTF1 uses test selector 0x405.
  Debug selectors are restored, and the source address is not changed until
  this succeeds. The core independently refuses START while autorefresh is on.
- Do not port the Linux recovery loop that changes tear write count/start
  position: a failed drain/status/readback check stops the frame, pins memory
  and refuses loader handoff. Console ownership stays unchanged, but an
  already-disabled autorefresh can freeze its physical picture on this path.
  No TE-copy experiment, early takeover, forced SMMU replacement or unsafe
  fallback is reintroduced.
- Successful handoff uses ordinary CTL START for subsequent frames. Return
  completes the boot-source commit, restores original autorefresh and waits
  for a fresh hardware completion before releasing mappings. Only busy waits
  are used during return after CPU shutdown; pause delays run in thread context.
- `check-sde-refresh-linux.py --linux-tree /home/danya2271/xiaomi_sm8250_kernel`
  compiles actual local Linux accessors and checks offsets, selector, control
  decoding and prepare-commit ordering. Portable GCC/Clang fault-injection,
  all 105 ARM64 scenarios, swap diagnostics, panel and export checks pass.
  New RAM-only `AUTO` / `AUTO STATE` rows report decoded state and pause phase.
- Full DEVELOPMENT build and independent image verification pass. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-autorefresh-r10.img`
  (12,105,728 bytes), SHA256
  `8835cf0318f0dbbc4d8c7566a92ab3397642d10a0cf8fdfc00f4e438db0d78fe`.
  Adjacent verifier/build/pack logs and checksum record the exact payload and
  four staged display-source hashes. r9, FD/shim, live L11r DTB and boot options
  are preserved. No handset test, flashing or installed-APFS write was done.

## r11 SMMU status fix: 2026-10-29

- The r10 device reaches `ROUTE: check=ok`, stage 4, then rejects
  `boot-translation` as `hardware-fault`; no refresh controls or source have
  been changed. The exact return came from `if (domain->fsr)`, not from a PTE
  failure. The transcript omits raw FSR, so do not claim its actual value.
- Local `arm-smmu-regs.h` defines `FSR_FAULT=0xc00001fe` and local
  `arm-smmu.c` masks FSR before declaring a fault. SDE now does this in the
  translator, domain guard and loader-return poll. Syndrome-format bits 10:9
  (`0x200/0x400/0x600`) no longer cause false hardware faults; every real fault
  bit is still fatal. The repository's Adreno path already used this mask.
  No clear-FSR, RESUME, context replacement or other new hardware write exists.
- Add a cached complete SMMU diagnostic: globals, selected stream, actual CB
  address, context registers, raw/masked FSR, decimal format, FAR/syndromes,
  boot PA/IOVA and exact checked-walk condition/level/PTE/expected PA. Table
  read refusals distinguish reserved-RAM bounds, boot-pixel overlap and map
  failure. A qualified route switches the recurring dump to SMMU rows instead
  of repeating the already-qualified CTL/LM state. Other checks are unchanged.
- Host GCC/Clang tests cover every FSR bit and named walk failure. A test
  against the actual local Linux header covers 4096 fault/format combinations
  and checks context-register offsets. All 128 ARM64 scenarios pass, including
  real faults at startup/frame/return, metadata-only statuses, imported/owned
  mappings, safe restoration and no fault acknowledgement. Earlier tests used
  reserved bit 0 for fault injection; corrected to translation-fault bit 1.
- Full DEVELOPMENT build and independent image verification pass. Artifact:
  `artifacts/munch-build/munch-iosroot-l11r-sde-smmu-r11.img` (12,107,776 bytes),
  SHA256 `56aeecb6cc417a07bd18f25aee2899c37a49c29b80edc7736f340cef015881ff`.
  Adjacent verifier/build/pack logs and checksum record the exact payload and
  four display-source identities. r10, FD/shim, live DTB and boot options are
  preserved. No flashing, installed-APFS write or handset test was performed.

## r12 inherited context bypass: 2026-10-29

- The r11 handset log has `SCTLR=0xe0` (`M=0`), `CBAR=0x1f000`,
  `CBA2R=1`, S2CR translation into CB4, zero TTBR0/TCR/MAIR, and matching
  boot IOVA/PA `0x9c000000`. FSR `0x400` has no faults. The backend's
  unconditional active-table requirement caused `translation-disabled`.
- Local `arm_smmu_write_context_bank()` leaves M clear for stage-1
  `DOMAIN_ATTR_S1_BYPASS`/`EARLY_MAP`; local `arm_iommu_get_dma_cookie()`
  uses non-IOMMU DMA operations for S1 bypass. Implemented this distinct
  inherited context mode without enabling translation, changing streams,
  clearing faults, reading PTEs, allocating/publishing tables or issuing TLBI.
  All SID/context/stage/fault/raw-state qualifications remain in force.
  S2CR stream bypass and nested-stage translation remain unsupported.
- The two replacement buffers are physically contiguous, pinned and below
  4 GiB, with `IOVA=PA`. Physical alias checks now precede memory clearing.
  Bypass cannot fall into the owned-table installation branch even when
  inactive TCR/TTBR retain values from an earlier configuration.
  First-frame deferred refresh drain, hardware completion, console handoff,
  boot-source restoration and fault pinning are unchanged.
- Offline runtime: 128 translated-mode regressions plus 39 bypass scenarios
  with the observed CB4 context/geometry, repeated frame completion and safe
  restoration, boot-pixel immutability, buffer/stream/fault/timeouts and raw
  state changes before commit or during return. The mock SMMU is read-only
  at the page-protection level throughout production-code execution;
  table mapping/allocation/publication counts remain zero in bypass.
  IDR1 `0x6000004a` places CB4 at `0x15084000`; the transcript's shorter CB
  address and incomplete SMR/layer-map/enable fields are not used to loosen
  validation. Actual local Linux SCTLR setup runs for 256 combinations;
  fault decoding still covers 4096 combinations, with GCC/Clang sanitizers.
- Status marker is `MUNCH SDE STATUS r12`; successful boot address checking
  reports `WALK: check=stage1-bypass` (no PTE read, level sentinel 4).
- Full DEVELOPMENT build, queue/console, panel and export checks pass. The
  independently verified image is
  `artifacts/munch-build/munch-iosroot-l11r-sde-bypass-r12.img`
  (12,107,776 bytes), SHA256
  `bcd810f61577f2ac9f26da4bdd16dcb7d4a45025b119ba9e63bcb123c56c3b6f`.
  Adjacent verifier/build/pack logs and checksum record exact payload and
  four display-source identities. r11, FD/shim, live DTB and boot arguments
  are preserved. No phone or installed APFS was written.
- The user explicitly declined further boots. Continue offline; do not ask
  for another boot/photo, flash, reboot or touch installed APFS. A host/QEMU
  pass is not evidence of panel output. Keep the backend opt-in and do not
  claim SpringBoard or reduced tearing is proven on the phone.

## r13 loader-owned low DMA memory: 2026-10-29

- r12 now verifies all 2532 boot pages in stage-1 bypass but fails at
  `back-buffer-mapping`, with no accepted/completed frames. Inspecting the
  local loader and `arm_vm_init.c` reveals the same low-VM-memory problem
  previously documented for SCM: free pages start beyond the resident APFS
  image, above 4 GiB in the known Munch window. The old mock always supplied
  low contiguous buffers; it did not reproduce this layout constraint.
- `pack-iosroot.py --sde-backend` reserves `0x1444000` bytes after kernel/SCM,
  before helper/resident root, and publishes `/chosen/munch-sde-buffers` as
  two u64 values. Shared `munch-sde-memory.h` validates geometry and window/
  low-address/root/helper bounds. Source staging now protects and records a
  fifth display header. No fixed physical address, SMMU initialization,
  Display Reserved RAM borrowing or root relocation was introduced.
- Runtime imports the static region only after exact-size/alignment/bounds,
  loaded-kernel/boot/SCM/ramdisk exclusion and per-page `kvtophys` checks.
  It uses two 10-MiB buffer slots and an optional 272-KiB table workspace;
  neither needs a free low VM page. Bypass leaves that table area untouched
  and performs no SMMU write. Reserved memory is never returned to VM.
  Absent-property fallback explicitly requests host physical contiguity and
  `kIOMemoryMapperNone`; a malformed property rejects without fallback.
- `MUNCH SDE STATUS r13` adds cached `MEMORY` and `BACK1/BACK2` rows describing
  the pool, exact import/allocation phase and resulting physical/IOVA ranges.
  It retains the deferred first-frame ownership and fault pinning rules.
- 217 ARM64/QEMU scenarios pass (167 regressions plus 50 with the low VM
  allocator forced to fail), including bypass/translated reserved storage,
  unsafe-return pinning and malformed reservations that must not change
  pool bytes. GCC/Clang sanitized tests execute the generated loader's layout
  with/without GPU/SDE, plus existing core/local-Linux/queue/panel/export checks.
- DEVELOPMENT kernel and independently verified image:
  `artifacts/munch-build/munch-iosroot-l11r-sde-memory-r13.img`
  (12,109,824 bytes), SHA256
  `bbd93d71071b44c90b5ce1f31a14d1ab1fcdf56036783746c3235f4692a9beca`.
  Adjacent build/pack/verifier logs and checksum preserve the payload and five
  staged display-source identities. r12, FD/shim/live DT and arguments are
  unchanged. At modeled window `0xf4000000`, pool `0xf8308000..0xf974c000`
  and helper end `0xf9b4c000` fit before root `0xfc000000`; this is not a new
  handset capture. No flash, reboot or installed-APFS write was performed.
  The user subsequently confirmed UI output without tearing, but low perceived
  FPS and broken screen-off. Do not infer measured FPS or request another boot
  by default.

The remaining sections describe the original baseline and required milestones;
the progress notes above supersede their earlier device-connection status.

## Goal and current symptoms

Port a minimal Qualcomm Kona SDE display backend to XNU for POCO F4 (Munch).
The goal is smooth, low-latency presentation without tearing, followed later
by panel power and refresh-mode control. The user reports that Adreno/Mesa now
works, but display presentation still has severe lag, tearing, and noticeable
delay after touch. The touch polling path was previously investigated; do not
assume it is the cause of the remaining presentation latency.

The strongest code-level suspect is still the display presentation path, but
it has not been proven to be the only cause. XNU inherits ABL's active
framebuffer and `MiniIOMobileFramebuffer.cpp` composites/scales IOSurface
layers with CPU code directly into that live scanout buffer. Mesa rendering an
IOSurface does not make this final composition/page presentation hardware
accelerated. The current path has no hardware page flip or SDE commit.

## Workspace state and preservation

- iOS repository: `/home/danya2271/ios-munch`, HEAD `5f489c7021e8c54c7a8e3a45f280165c6f8ae37b`.
- Linux source: `/home/danya2271/xiaomi_sm8250_kernel/`, HEAD `31aa532db070d8353b361770139789cb9702db0e` (`stable-bpf`).
- Both repositories have local state. The iOS repository has many modified, deleted, and untracked files, including display, loader, GPU, Wi-Fi, and power changes. Do not reset, clean, stage, or commit them without explicit instruction.
- The Linux worktree has an untracked `gcc-compiler/` directory. Preserve it.
- The last user-reported device result is that GPU acceleration works while display lag/tearing/input-to-display delay remain. No fresh device connection or test result was obtained while preparing this handoff.
- Previous build notes report that an earlier framebuffer object compiled, but a later full XNU build failed at `check_all_exports` with an `IOService` export mismatch. The current dirty tree has changed since then; re-run the build before relying on that result.

## Existing XNU work

- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` is the current framebuffer provider and CPU compositor. It has an optional, read-only SDE/SMMU snapshot enabled with `munch_sde_probe=1`. The probe logs current SSPP source state, CTL routes, INTF1 state, and the apps-SMMU stream/context state. It is one-shot at framebuffer startup, not a continuous logger.
- `xnu/files/iokit/Kernel/munch-kona-display.h` contains Munch/Kona MMIO offsets, SSPP/CTL layout, SMMU identifiers, and panel timing placeholders.
- `devices/poco-f4/display/README.md` records the prior command-mode scan measurements: about 60.68 Hz, a 16.48 ms frame, approximately 12 ms active write phase, and 4.5 ms idle gap. Those measurements do not define a safe framebuffer-copy window.
- The shadow-buffer/TE experiment was removed after device testing made pacing and tearing worse. Do not restore the TE staging patch, fixed-delay copy, or assumed UMI TE window.
- An earlier CTL kickoff experiment was removed: it could restart the existing source without safely switching to another mapped buffer.
- DCS display on/off support exists but is incomplete and not handset-validated. It does not own the DSI PHY, panel rails, reset GPIO, or full resume sequence. Do not build modesetting around that partial path.
- `fb_hz` only changes software pacing/notifications. It does not change the physical panel refresh rate.

## Critical hardware facts and caveats

- Kona SDE MDP base is `0x0ae00000`; the Munch DTS window is `0x84208` bytes. INTF1 is at offset `0x6b800` (`0x0ae6b800`).
- The Kona SDE DTS describes 6 CTLs, 6 mixers, 8 SSPPs (VIG0-3 and DMA0-3), 4 INTFs, ping-pong blocks, DSC, VBIF, and command-mode support. Use the Kona catalog and actual runtime routing to choose resources; do not infer the active CTL/pipe from block numbering alone.
- SDE unsecure traffic uses apps-SMMU SID `0x820`, mask `0x402`, with the DTS IOVA pool starting at `0x00020000` and size `0xfffe0000`. Secure SID is `0x821`, mask `0x400`. These are configuration values, not proof that an XNU buffer is mapped or accessible to SDE.
- The apps SMMU is at `0x15000000`. Linux DTS marks it `qcom,skip-init`; boot firmware may leave the active translation context and stream mapping configured. Read-only probe values must be captured before deciding whether XNU can reuse or must replace that context.
- Do not write an SDE source address or take ownership of active CTL/INTF until both the live CTL/SSPP route and the matching SMMU translation are understood. Keep ABL's scanout untouched until a valid replacement buffer is mapped and ready.
- There is a panel-description mismatch to resolve first. Linux `arch/arm64/boot/dts/vendor/qcom/munch-sm8250.dtsi` is attribution-oriented and selects `dsi_sim_vid`; shared `display/kona-sde-display.dtsi` defaults to `dsi_sw43404_amoled_cmd` (1440x2880 WQHD). Neither is automatically the panel profile currently used by the phone. Existing Munch notes/captured live DT identify `qcom,mdss_dsi_l11r_38_08_0a_dsc_cmd` at 1080x2400. Use the phone's live DT/vendor panel data as authority and verify active DSI controller/link count, DSC parameters, command sequences, porch values, clocks, and mode list before implementing panel modes.
- The XNU header's 60/120/144 Hz table is not validated as a complete modeset description. Start with the verified bootloader mode only; do not enable 120/144 Hz based on that table.

## Linux source map

All paths below are relative to `/home/danya2271/xiaomi_sm8250_kernel/`.

- Kona block layout/capabilities: `arch/arm64/boot/dts/vendor/qcom/display/kona-sde.dtsi`, `techpack/display/msm/sde/sde_hw_catalog.c`, and `techpack/display/msm/sde/sde_hw_catalog.h`.
- SDE orchestration/resource assignment: `techpack/display/msm/sde/sde_kms.c`, `sde_rm.c`, `sde_plane.c`, and `sde_crtc.c`.
- Minimum scanout blocks: `techpack/display/msm/sde/sde_hw_sspp.c`, `sde_hw_lm.c`, `sde_hw_ctl.c`, `sde_hw_intf.c`, and `sde_hw_pingpong.c`.
- Command-mode synchronization and commit: `techpack/display/msm/sde/sde_encoder_phys_cmd.c`, `sde_encoder.c`, `sde_core_irq.c`, `sde_irq.c`, and `sde_hw_interrupts.c`.
- Bandwidth, QoS, and power: `techpack/display/msm/sde/sde_vbif.c`, `sde_hw_vbif.c`, `sde_core_perf.c`, `sde_power_handle.c`, plus Kona GCC/DISPCC clock sources under `drivers/clk/qcom/`.
- SMMU behavior and mapping: `techpack/display/msm/msm_smmu.c`, `arch/arm64/boot/dts/vendor/qcom/msm-arm-smmu-kona.dtsi`, and the `smmu_sde_unsec` node in `display/kona-sde.dtsi`.
- DSI host, PHY, panel power, and commands: `techpack/display/msm/dsi/dsi_display.c`, `dsi_ctrl.c`, `dsi_ctrl_hw_2_2.c`, `dsi_phy.c`, `dsi_phy_hw_v4_0.c`, `dsi_panel.c`, `dsi_pwr.c`, and `dsi_clk_manager.c`.
- Shared Kona panel examples: `arch/arm64/boot/dts/vendor/qcom/display/kona-sde-display.dtsi` and `dsi-panel-sw43404-amoled-dsc-wqhd-cmd.dtsi`. Treat these as reference implementations, not Munch's verified panel configuration.

Do not port Linux DRM/KMS, its device model, or all of techpack wholesale into
XNU. Port the smallest hardware sequence and state machine needed for one Munch
panel and one verified scanout mode, using XNU's MMIO, interrupt, memory, and
power APIs.

## Suggested implementation order

1. Establish a baseline. Keep a known-good image and record the current kernel,
   boot arguments, panel mode, perceived lag/tearing, and input-to-visible
   latency. Do not mix display experiments with touch polling or GPU DVFS edits.
2. Capture one `munch_sde_probe=1` snapshot on the phone using
   `devices/poco-f4/loader/pack-iosroot.py --sde-probe`. Save the complete,
   finite startup output. Decode which SSPP, mixer, CTL, and INTF1 are active;
   identify the real framebuffer address/format/stride and matching SMMU SMR,
   S2CR, context bank, TTBR, and faults. The probe is read-only; do not add
   register writes to it.
3. Resolve live panel configuration against the phone's live DT/vendor files.
   Identify the exact panel, DSI controller(s), DSC geometry, current timings,
   TE source, and bootloader-owned clock/power state. Do not reuse UMI's
   2340-line assumptions for Munch's 2400-line panel.
4. Design explicit memory ownership. Allocate or import at least two
   page-aligned scanout buffers, map them in the correct SDE IOVA domain,
   maintain cache coherency, and preserve ABL scanout until the new mapping is
   proven. Confirm SMMU faults and teardown/lifetime behavior before display
   programming.
5. Implement one fixed-mode SDE presentation path: one verified linear format,
   stride and dimensions; one validated SSPP-to-mixer-to-CTL-to-INTF1 route;
   CTL flush/commit; and a back-buffer switch only after command-mode transfer
   completion. Do not start by supporting arbitrary IOSurface formats,
   scaling, rotation, HDR, or multiple planes.
6. Add hardware-driven command-mode pacing. Port only the necessary TE/tear
   check, ping-pong/MDP completion, DSI completion, IRQ acknowledgement, and
   timeout/recovery sequence. Connect completion to XNU fences/vsync so
   `SwapWait` and frame queues track actual presentation rather than a timer.
   Keep the queue shallow to avoid input-to-display latency.
7. Add required VBIF/QoS/bandwidth and clock votes based on the Kona Linux
   sequence. Measure missed deadlines and underruns. Keep the panel in its
   existing verified refresh mode until 60-Hz presentation is stable.
8. Only then add blank/unblank, suspend/resume, and dynamic refresh switching.
   A mode change must coordinate panel DCS, DSI host, PHY/PLL, SDE timing,
   clocks, TE, and rollback on failure; changing `fb_hz` is not a modeset.

## Build and device workflow

The source-built image path documented in `devices/poco-f4/loader/README.md`
is:

```sh
MUNCH_STAGE=1 ./xnu-build.sh -j8 -k
python3 devices/poco-f4/loader/pack-iosroot.py \
  --fd /path/to/SILICIUM_UEFI.fd \
  --dtb /path/to/live.dtb \
  --kernel xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  --ios-config /path/to/ios-config.json \
  --output /tmp/munch-iosroot.img
fastboot boot /tmp/munch-iosroot.img
```

For the one-shot read-only SDE snapshot, add `--sde-probe` to the packer.
Use `fastboot boot` for tests; do not flash a partition for this work. Keep
normal boot logging quiet and capture only the bounded probe output. A
successful host build or register snapshot does not prove a safe SDE commit;
each write-path milestone needs a device test and a known-good rollback image.

## Acceptance criteria

- Baseline and test images are identifiable and reversible; no unrelated local
  modifications are lost.
- The active panel, mode, DSI route, SDE route, SMMU mapping, buffer format,
  and stride are backed by live-device evidence.
- No writes target ABL's active scanout buffer during composition.
- Every submitted frame either completes on hardware or returns a bounded
  error; no unbounded waits, logging loops, or silent fake-vsync success.
- Repeated UI animation and touch interaction show materially lower visible
  latency and tearing without new blanking, SMMU faults, or underruns.
- Power and refresh switching remain disabled until separately validated.
