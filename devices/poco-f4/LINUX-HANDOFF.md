# Linux handoff: POCO F4 (munch)

## Device state

- iOS 16.5 / 20F66 reaches SpringBoard with Mesa/freedreno and native FT3658U input.
- The latest preferred UI image is `munch-ios165-gpu-quiet-r1.img`. It requests
  the reviewed 587-MHz GPU selection, retains the original framebuffer path,
  sleeps 4 ms between touch reads, disables the 600-second loader return,
  and suppresses XNULOADER/XNU screen logs. The user reports slightly better touch.
- TE-staging r1 felt slower and did not resolve tearing. Do not make it the default.
- GPIO66 TE was measured at approximately 60.68 Hz. Physical 120-Hz switching
  has not been implemented. Eight CPUs are requested; do not infer the online
  count without runtime evidence.
- The pending test is `munch-display-scan-r1.img`: a standalone UEFI probe,
  not an iOS image. Obtain its TE configuration and three Window summaries
  before adapting UMI's command-mode write-pointer synchronization.
- The installed iOS root is on the dedicated 16-GiB `iosroot` partition. The
  current small RAM-boot images do not require reinstalling that root.

## Preserve files outside Git

The source repository is `hakios/` beneath the workspace. Git does not include
OS images, captured phone data, firmware, generated SDK/toolchain dependencies,
prepared XNU trees or build outputs. Preserve the workspace before removing WSL:

- `artifacts/`, especially `munch-build/` and the device information captures;
- `references/`, including Mu-Silicium and the captured/downloaded hardware sources;
- `kernel-umi/` and `userland-umi/` reference checkouts;
- ignored `hakios/userland/`, `hakios/prebuilt/`, `hakios/xnu-deps/`,
  and `hakios/xnu-src/` inputs/outputs needed to reproduce this local setup.

Preserve the small JSON manifests along with their image/kernel/helper files.
Existing local manifests contain absolute paths. After moving the workspace,
update paths in the iOS/native GPU/graphics configuration files and recompute
parent manifest hashes when a referenced manifest changes. Do not bypass the
packer's hash checks. Keeping the same Linux workspace path simplifies migration.
Do not commit the iOS images or phone captures to a public repository.

Temporary compiler installations under `/tmp/munch-host-tools`,
`/tmp/munch-theos` and `/tmp/munch-pack-tools` are disposable and may already
be missing. `artifacts/munch-build/gpu-build-env.sh` references the earlier
local temporary setup; it is not a portable Linux installation script.
Install/recreate the host compiler, cctools and iOS SDK before using it.
The latest probe packer ran with clang/lld 18; the native kernel was built
with clang 21 and the prepared local dependencies.

## Source/build entry points

- `gpu/stage-kernel.py TREE --touch-poll-ms 4 --report REPORT.json` stages
  the A650 adaptation and native touch, restoring the baseline framebuffer.
  `--te-staging` explicitly opts into the unsuccessful presentation experiment.
- Native kernel build: source a valid toolchain environment, then run
  `bash xnu-build.sh -j8 BUILD_DSYM=0 build`. This target passed; the full
  optional `all` packaging target was not established as passing.
- `gpu/prepare-gl-helper.py` prepares/verifies the Mesa helper.
- `loader/pack-checkpoint.py --help` packs the native iOS profile using
  checkpoint 58, `--cpus 8 --touch --no-early-bootargs`, pinned iOS config
  and native GPU config. The quiet profile sets `loader_quiet=true` and
  `verbose_console=false`.
- `touch/verify-image.py IMAGE --fd FD --dtb LIVE_DTB` independently verifies
  the native iOS image and its pinned inputs.
- `uefi/pack-diagnostic.py --display-scan-probe --fd FD --dtb LIVE_DTB
  --output IMAGE` builds the pending controller observation probe.

See `GPU-BRINGUP.md`, `display/README.md`, `loader/README.md` and
`uefi/TEST-RESULTS.md` for the detailed investigation. The GPU adaptation
uses UMI's A650 work; the controller-based UMI display path has not yet been
integrated into the preferred iOS image. The latest probe only observes it.

## Checks

Host checks use Python, a C compiler and ASan/UBSan:
`gpu/check-scm.py`, `touch/check-xnu-events.py`,
`loader/check-bottom-console.py`, `uefi/check-panel-te.py`, and
`uefi/check-display-scan.py`. Compile/run `display/check-te-frame.c` similarly.
`uefi/check-bootshim.py` additionally requires Unicorn, clang, llvm-readelf
and aarch64-linux-gnu-objcopy. Passing host tests does not establish a
hardware-safe presentation interval; phone results remain required.
