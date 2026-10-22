# iOS 16 port to POCO F4 (`munch`) - working notes

### HANDOFF 2026-10-23: KONA AFF1 SMP PATCH CORRECTED, CI QUEUED

- Observed: CI run `39841992478` reached the post-0088 source tree and rejected
  the old 0089 comment hunk. The following run `39843001146` was not a valid
  compiler result because it used an intermediate exception patch layout.
- Changed: commit `3b6cfe0` makes `LOAD_PMAP_CPU_DATA` use Kona MPIDR Aff1
  directly for MUNCH and keeps the `cluster_offsets` lookup only in the donor
  branch. It also guards all three Apple physical-cluster paths and uses the
  SM8250/Kona topology comment. The patch applies cleanly with ordinary
  `git apply` to the saved post-0088 CI build tree.
- Changed: commit `66f3336` adds validator checks for the direct Aff1 path and
  contextual patch shape. `python3 tools/validate-repo.py` passes all 89
  numbered patches and all script checks.
- CI: run `39848369447` targets commit `3b6cfe0`; its first attempt waited for a
  macOS runner for about 15 minutes and was cancelled without job steps or
  artifacts. It has been rerun and is currently queued again. This is runner
  capacity state, not a compiler error.
- Device safety: `fastboot devices` and `adb devices -l` are empty. No
  `fastboot boot`, flash, UFS, userdata, GPT, or partition operation occurred.
- Next: obtain a completed kernel artifact for `3b6cfe0`, verify MUNCH strings
  and the eight-CPU topology, build the fresh RAM-only payload with
  `/tmp/munch-8cpu-0089.adt`, then test only with temporary `fastboot boot`.
  Do not reuse the old A3 image.

### HANDOFF 2026-10-23: KONA MPIDR SMP FIX READY

- Observed: the 8-CPU panic came from the older boot image
  `/tmp/munch-ufs-userspace-a3-8cpu-boot.img`, built before the final MUNCH
  rebrand. Its kernel strings still identify the donor as `SWEET`; the panic
  occurs when that image parses Kona's `reg` values `0x0, 0x100, ... 0x700`
  with the donor cluster assumptions. This is an artifact diagnosis, not a
  device-side partition failure.
- Changed: new patch `0089-munch-kona-mpidr-topology.patch` makes the Munch
  exception path use MPIDR Aff1 directly as the logical CPU index, matching the
  live Kona FDT and PSCI targets. It also keeps Munch out of XNU's Apple
  physical-cluster offset branch even if an unrelated build macro leaks in.
- Changed: `tools/validate-repo.py` now checks all eight generated Munch MPIDRs
  and their E/P classes. Generated `/tmp/munch-8cpu-0089.adt` contains
  `0x0..0x300` E cores and `0x400..0x700` P cores, matching the captured FDT.
- Verified: repository validation, whitespace checks, 0089 dry-run/application
  against the post-0088 CI build tree, ADT generation, and local syntax-only
  checks for `machine_routines_asm.s` plus `machine_routines.c` all pass. The
  first 0089 draft had a malformed combined hunk; it was corrected and committed
  in `c0f6e15`. The Munch topology comment now also names SM8250/Kona rather
  than the inherited SM7150 donor.
- CI: run `39835665572` for `b628c8d` was cancelled after remaining queued;
  run `39840426951` for `2fcab0d` failed before compilation because a comment
  edit inside 0088 changed a context line. That context was restored; the
  SM8250/Kona comment now lives in 0089, and a fresh CI run is required.
- Device safety: no ADB/fastboot device is connected; no `fastboot boot`, flash,
  UFS, userdata, GPT, or partition operation occurred.
- Next: commit/push the corrected patch, wait for a real kernel artifact, build a fresh
  MUNCH-only 8-CPU RAM payload, and only then test with temporary
  `fastboot boot`. Do not reuse the old A3 image.

### HANDOFF 2026-10-22: KERNEL BUILD REACHED COMPILER; FIXED TABLE STRING

- Observed: CI run `39832968199` applied all 88 numbered project patches and
  continued past DXB's own optional patches. The first compiler error was in
  `iokit/KernelConfigTables.cpp:83`: the MunchBattery table's final C++ string
  literal lacked its closing quote. The build therefore exited 2; no usable
  kernel artifact was produced.
- Changed: patch 0088 now closes that table string with `"   },"`.
- Observed: repo validator and `git diff --check` pass. To test in the right
  context, the previous successful full replay was copied to a temporary tree,
  old 0088 was reverse-applied to recover post-0087 inputs, and the corrected
  0088 passed apply-check/application. `clang++ -fsyntax-only -Werror` on the
  resulting `KernelConfigTables.cpp` passes.
- Device safety: ADB device is disconnected. No fastboot boot, flash, UFS,
  userdata, GPT, or partition operation occurred.
- Next: commit/push this compile fix and inspect the next CI run for further
  compiler errors. Do not prepare a boot image until a kernel artifact builds.

### HANDOFF 2026-10-22: SCOPE CI FAIL-CLOSED TO OUR PATCH SERIES

- Observed: commit `41d4dbb` fixed the generated Xcode `TargetConditionals.h`
  rename issue. CI run `39832110083` then applied all local patches 0001–0088
  successfully, verified by the uploaded post-patch build tree, but stopped
  before compiling when the broad fail-closed guard rejected DXB's own
  `TrustCache.patch`; its headers already existed in XNU. This was a guard
  scope issue, not a MUNCH patch failure.
- Changed: the workflow guard now applies only to four-digit numbered project
  patches, so DXB's separate `TrustCache.patch` retains its existing skip
  behavior. `tools/validate-repo.py` checks that scope.
- Observed: the updated repo validator, YAML parse, injected Bash syntax check,
  and `git diff --check` pass. CI's uploaded source after 0088 has the MUNCH
  board config and both staged SDK headers moved to their MUNCH paths; their
  contents were preserved. No kernel was compiled in run `39832110083`.
- Device safety: ADB was no longer connected during a read-only topology query.
  No fastboot boot, flash, UFS, userdata, GPT, or partition operation occurred.
- Next: commit/push this guard-scope fix and rerun CI. Only prepare a RAM boot
  image after CI emits a real kernel artifact.

### HANDOFF 2026-10-22: CI EXPOSED GENERATED SDK HEADER RENAME BUG

- Observed: GitHub Actions run `39829505050` for commit `c66c8f8` correctly
  failed closed while applying patch 0088, before compiling. The staged
  `TargetConditionals.h` files in both libpthread and HFS are full Xcode SDK
  headers, while 0088 expected synthetic 14-line stubs and then tried to
  delete/recreate them. The failure is patch construction, not a compiler or
  device regression.
- Changed: 0088 now expresses both generated-header moves as content-preserving
  Git renames from the staged Sweet donor paths to the Munch paths. It no longer
  assumes SDK header contents or embeds a test-only stub.
- Observed: the modified patch passed `git apply --check` and application
  against the failed run's uploaded XNU build tree, including the real Xcode
  headers. Both resulting Munch headers have identical SHA-1
  `da1c6e37d0d23720db69954989ea1e3d80796d72`; the stale Sweet header paths are
  absent. `python3 tools/validate-repo.py` passes. CI did not produce a kernel;
  it uploaded only the diagnostic/build-tree artifacts.
- Device safety: phone remains in Android over ADB; no fastboot boot, flash,
  UFS, userdata, GPT, or partition operation was performed.
- Next: run the final diff checks, commit and push the corrected patch, then
  inspect the new CI result. Do not prepare or boot an image until a kernel is
  actually built and inspected.

### HANDOFF 2026-10-22: 0088 RECONCILED; CI MADE FAIL-CLOSED

- Observed: candidate 0088 was rebuilt from the clean post-0087 replay and
  verified with `git apply --check --whitespace=error`; candidate SHA-256 is
  `92b0cebb034a369d846e2662e0aef22d0ba40d6e32b4d0ad97ab5a68a48e8f44`.
- Observed: strict ordered replay of all 88 patches against XNU
  `xnu-8796.141.3` passed with the pinned dependency snapshots staged first.
  The staging used a fake SDK `TargetConditionals.h` copied byte-for-byte from
  the matching staged input. `--whitespace=error` flags pre-existing trailing
  whitespace in patch 0006, so the full replay used `--whitespace=nowarn`;
  patch applicability was checked before each apply. Staged files are generated
  inputs and are not in the repository.
- Observed: the candidate removes active SWEET source identifiers, renames the
  board/runtime symbols to MUNCH, enables `__ARM_AMP__`, removes `NO_ECORE`,
  sets `MAX_CPU_CLUSTERS=2`, and identifies Snapdragon 870/Kryo 585 with
  Cortex-A77/A55. This directly addresses the prior
  `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID` panic and the `SWEET EARLY`
  diagnostic label. A remaining ordinary comment contains “sweet-spot”; it is
  not a platform identifier.
- Changed: `.github/workflows/xnu.yml` now makes pinned DXB patch application
  fail closed for `MACHINE_CONFIG=MUNCH` instead of silently skipping a patch.
  `tools/validate-repo.py` validates this guard and the active MUNCH topology.
- Observed: `python3 tools/validate-repo.py`, workflow YAML parsing, workflow
  fetch-step `bash -n`, injected DXB `bash -n`, and `git diff --check` pass.
  Parser symbols exist in the replayed source, and a case-insensitive scan of
  active XNU source paths found no SWEET identifiers. The connected Android
  device is `munch` over ADB; no fastboot boot, flash, UFS, userdata, GPT, or
  partition operation was performed.
- Next: commit this patch reconciliation, trigger the macOS GitHub Actions
  build with `gh`, inspect the kernel artifact, then prepare a new temporary
  `fastboot boot` image with `cpus=8`. Do not flash or modify `userdata`.

### HANDOFF 2026-10-22: CURRENT PATCH EDITS NOT READY TO PUSH

- Observed: the current worktree has 86 modified files under `xnu/patches/` and
  an untracked `--help` file identified by `file` as an Excel BIFF 2 document.
  No patch files were staged, committed, or pushed during this check.
- Observed: `python3 tools/validate-repo.py` fails its MUNCH build-plumbing
  checks for patches 0019, 0020, 0083, and 0086. The edits contain duplicated
  conditions such as `MUNCH || MUNCH` and changed donor-side SWEET preimages.
  This is consistent with a global replacement having changed patch context,
  not just the MUNCH additions.
- Observed: `git apply --check` of both the worktree's 0088 and committed 0088
  fails against `/run/user/1000/xnu-munch-sequence`; that replay tree is
  modified, so this is not a clean-upstream replay result. Do not infer clean
  base applicability from it.
- Recommendation: do not commit/push the current patch set as-is. A failed
  validator is confirmed, and CI's existing guard only checks for the MUNCH
  config/header after the patch runner; it does not prevent individual patch
  failures from being skipped. Preserve the current edits, restore donor-side
  context selectively, then validate the full ordered patch series on a clean
  XNU 8796 tree and make patch application fail closed in CI.
- No device, UFS, userdata, partition, or GPT operation was performed.

### HANDOFF 2026-10-22: PATCH 0085 ORIGIN VERIFIED

- Observed: commit `aceb1e9` (`feat(munch): add Focaltech report parser`)
  adds `munch_touch.c` and `munch_touch.h` as new files; it is not a SWEET
  driver rename. The added CRC/frame-parser functions have no callers in the
  patch series, while patch 0060 contains the actual GENI/Focaltech touch
  bring-up driver.
- Observed: strict replay of committed patches 0001–0087 against XNU
  `xnu-8796.141.3` succeeds; patch 0088 fails `git apply --check`. The CI log
  for run `39780417091` records the same conflict: `bsd/dev/munch_touch.c`
  already exists, plus mismatches in `bsd/conf/files` and
  `bsd/kern/syscalls.master`. Patch 0085 registers the parser as
  `bsd/dev/munch_touch.c`; patch 0088 tries to rename the Sweet touch driver
  to that same path, so the patch must be reconciled rather than skipped.
- Root-cause inference from the applied XNU code: the failed 0088 leaves
  MUNCH at `MAX_CPU_CLUSTERS=1` with `NO_ECORE`, while topology parsing maps
  P-cluster CPUs to physical cluster ID 1 and asserts it is <= 0. This
  explains `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID`. The same skipped
  rebrand leaves `sweet_early` code active, explaining the `SWEET EARLY
  PANIC` label; it is not evidence that the device was identified as Sweet.
- No source patch, kernel build, or device operation was performed during
  this audit. Next: reconcile the parser/driver path and patch 0088, make CI
  fail on any skipped MUNCH patch, then build and test via temporary RAM boot
  only. Preserve Android, `userdata`, GPT, and existing backups.

### HANDOFF 2026-10-22: 8-CPU READ-ONLY RAM IMAGE READY

- GitHub Actions XNU run `39780417091` completed successfully for commit
  `152887d66ec74c3397e8d644d0c1765e44cf58c8`. Kernel artifact SHA-256:
  `92b48bfe0c1b16d12d548eb97379caafbba54690c1456037eae7207f8298cbf5`.
- The UFS ADT `/tmp/munch-8cpu-39780417091.adt` is 13,548 bytes, SHA-256
  `bc14a23ac7c9c6b6ebec918925a988983325b5cba67c92aa14648f655678e002`.
  Inspection confirms CPU0-3 are E (capacity 448) and CPU4-7 are P (three
  capacity 1023, prime capacity 1024); DRAM base/size are `0x100000000` /
  `0xfcc00000`.
- Built an XNU payload with
  `debug=0xc serial=3 cpus=8 keepsyms=1 rd=ufsroot0 iosrootrw=0
  unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1`.
  Payload SHA-256: `45137127596aa217cf68cf2a981c43498f13101c871a13c4e5ec5a78ec31d319`.
- Packed Android boot v2 image at
  `/home/danya2271/Загрузки/ios-munch-20H392/munch-8cpu-ro-20261022.img`,
  SHA-256 `17c0923be5a63397d26b7185681d87d70eacb9755562d7bfe7ca5c85b6578270`.
  Verified v2 header and byte-equal embedded shim and XNU payload. This is a
  temporary boot image, not a flashable partition image.
- `fastboot devices` and `adb devices -l` currently show no device. No boot,
  flash, partition, userdata, or GPT operation occurred. Next: reconnect the
  phone in fastboot, confirm product `munch`, then run only
  `fastboot boot /home/danya2271/Загрузки/ios-munch-20H392/munch-8cpu-ro-20261022.img`.
  Capture the CPU startup lines; expect XNU to report the requested/selected
  CPU count and per-core PSCI bring-up. `iosrootrw=0` keeps this test read-only.
- Existing user edits to patch names and the untracked `--help` file were left
  untouched.

### HANDOFF 2026-10-22: A77 CPU PROFILE CORRECTED

- Corrected active MUNCH CPU identity in patch 0088 and porting docs:
  Snapdragon 870 / SM8250-AC, Kryo 585, 4x Cortex-A55 + 4x Cortex-A77
  (3 big + 1 prime). XNU exposes the prime as P because this AMP scheduler
  supports only E/P classes. Kryo 470/A76 references remaining in the patch
  are the removed SWEET donor profile, not active MUNCH code.
- Patch 0088 applies cleanly with `git apply --whitespace=error --check` to a
  clean post-0087 XNU tree at `826ab5a3a9bd42258047d1486596ec6c41a539f4`.
  `python3 tools/validate-repo.py` passes: 88 contiguous patches, eight-CPU
  Munch ADT, Python and shell checks all pass.
- Audited the resulting XNU source after applying 0088: no board-specific
  SWEET identifiers or SWEET-named source files remain. The sole text match is
  the unrelated English phrase “sweet-spot” in `out_lz4.c`.
- Commit: `50d0e22d6ee7920d5dddde5ebd216d6dba3047bf`
  (`Correct Munch CPU topology to Cortex-A77`). It and the prior local docs
  commit `64bd075` remain unpushed. No CI run exists for this commit.
  Latest visible XNU Action run `39668666918` succeeded for unrelated commit
  `cc117ceffa3f50eea72cbaa76c424612faefb384`.
- No kernel build or device boot was performed; phone and storage were not
  touched. Remaining: push/dispatch CI for this commit, build/8-CPU RAM boot,
  then investigate touch and the ~4-pixel SpringBoard artifact.

### HANDOFF 2026-10-22: 8-CPU A3 RAM BOOT PREPARED

- Verified and unpacked the prior `xnu-kernel.zip` CI artifact; its
  `kernel.development.munch` SHA-256 is
  `10a5fa286959a72934a86f71417b6809b619ef56ecae4dbe489663e6a3c597f1`.
- Built A3 payload with the same UFS ADT and userspace boot arguments, changed
  `cpus=1` to `cpus=8`. Payload:
  `/tmp/munch-ufs-userspace-a3-8cpu-payload.bin`, SHA-256
  `60ea2b1e19b76454b295c2445d26b75bd6ce54664a7ff126411e6e74540679a6`.
- Built A3 Android boot v2 image using
  `/home/danya2271/xiaomi_sm8250_kernel/out/arch/arm64/boot/dts/vendor/qcom/kona-v2.1.dtb`;
  image `/tmp/munch-ufs-userspace-a3-8cpu-boot.img`, SHA-256
  `dbe50625f8fb740cc2fce4895115ebaee8de8915469fea9eb2a0866ff0e80978`.
  Unpacked the boot image and byte-verified its payload; cmdline is confirmed
  as `debug=0xc serial=3 cpus=8 keepsyms=1 rd=ufsroot0 iosrootrw=1
  unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1`.
- This uses the existing platform CPU code and the development kernel; not yet
  boot-tested. `hackios-xnu-kernel` is redwood-specific HackiOS infrastructure,
  not a direct Munch kernel replacement.
- User is separately preparing a 16-GiB debug root image with launchd console
  logging for `com.apple.bluetoothd`, `com.apple.fairplayd.H2`,
  `com.apple.SpringBoard`, `com.apple.backboardd`, `com.apple.mediaserverd`,
  `com.apple.medialibraryd` and `com.apple.assetsd`. A3 can first be RAM-booted
  against the current flashed root to validate 8-core bring-up.

### HANDOFF 2026-10-22: TRAMPOLINE FIX CONFIRMED; NEXT CONSOLE TRACE

- User flashed the trampoline-patched sparse candidate and booted the A2 RAM
  image. The previous dyld failures for `/usr/lib/libobjc-trampolines.dylib`
  no longer appear. Userspace now starts many more launchd jobs, including
  `assetsd`, `suggestd`, `bluetoothd`, `fairplayd.H2`, `medialibraryd`,
  `fileproviderd`, `nesessionmanager`, `symptomsd-diag`, `spindump`, `searchd`,
  `com.apple.datamigrator` and `OTATaskingAgent`.
- The supplied excerpt does not show `SpringBoard`; `bluetoothd` and
  `fairplayd.H2` appear repeatedly without visible crash reasons. This is
  progress, but not yet proof of a usable GUI.
- Next offline debug image: mount the verified raw candidate, add
  `/dev/console` stdout/stderr and `OS_ACTIVITY_DT_MODE=YES` /
  `CFLOG_FORCE_STDERR=1` to the visible restart loops plus SpringBoard and
  display/media jobs, rebuild sparse, then require explicit user confirmation
  before flashing `iosroot`. Do not touch `userdata`.

### HANDOFF 2026-10-22: TRAMPOLINE PATCH CANDIDATE READY

- User's root-shell patch completed successfully on the local raw keybag image.
  Added root-level copies from `Cryptexes/OS/usr/lib`:
  `libobjc-trampolines.dylib`, `libffi-trampolines.dylib`,
  `libstdc++.6.0.9.dylib`, `libstdc++.6.dylib` and `libstdc++.dylib`.
  The `App` cryptex did not provide a top-level `usr/lib`.
- Rebuilt Android sparse image after verifying/removing the previous sparse
  candidate. Final raw candidate:
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB-keybag.img`,
  SHA-256
  `eed0309520165015ce7a36bf0b0e79d4e74f0fce38741dfa54fc780ab5d692ee`.
  Final sparse candidate:
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB-keybag.simg`,
  SHA-256
  `b30e9988ec17f711a87cc7bbdf2ebf9b83d09ecc7b53b36282e883ed9b717cf3`.
- `fsck.hfsplus -f -y` reports `The volume iOSRoot appears to be OK`. The image
  has not yet been flashed or booted. The prior shell had a missing closing
  quote only in its final literal prompt; the executed logic and outputs were
  valid.
- Next: user explicitly confirms flashing the new sparse image to `iosroot`,
  then reboot to fastboot and run the existing A2 temporary RAM boot with
  `rd=ufsroot0 iosrootrw=1 unentitled_ios_sim_launch=1 ignition_level=1
  launchd_unsecure_cache=1`. Do not touch `userdata`.

### HANDOFF 2026-10-22: A1 REACHED LAUNCHD JOBS; FIX TWO FOLLOW-UPS

- Investigated `danya2271/hackios-xnu-userland` and its companion kernel
  repository. It is useful mainly as a reference for stock-iOS image assembly:
  it fetches iOS 16.5 from Apple, selects files, uses its own APFS writer, and
  keeps `/System/Cryptexes/{OS,App}` plus the shared cache. Our repository uses
  a direct 20H392 image instead and its `mkiosroot.sh` already applies the same
  cryptex/root linking logic. It is not a drop-in fix for MUNCH because its
  scripts, job lists, init and APFS format target HackiOS's loader/kernel and
  redwood (POCO X5 Pro), not our MUNCH XNU/UEFI flow.
- Confirmed the flashed candidate lacks root-level copies of the cryptex-only
  `libobjc-trampolines.dylib`, `libffi-trampolines.dylib` and `libstdc++` files;
  the userland log's dyld failure is consistent with this. The current builder
  contains a generic cryptex-link loop, but the exact reason this older image
  does not contain those files is not yet established. `/tmp` is a nearly full
  32-GiB tmpfs; possible pressure during the prior image build is only a
  hypothesis, not a verified cause.
- Patched `tools/mkiosroot.sh` to create its workspace in the current
  filesystem rather than tmpfs, avoiding pressure on `/tmp` during future
  rebuilds. Current root access is password-protected. The next test candidate
  can be made by copying the missing small libraries from the OS cryptex in the
  raw keybag image; use a 16-GiB-limited loop device because the HFSX volume is
  smaller than the raw 28-GiB partition image. Rebuild the Android sparse
  image after offline validation; do not flash without separate confirmation.

- User supplied output from two distinct boots. The first was this agent's
  `/tmp/munch-ufs-launch-a1-boot.img`: `launchd_cache_loader` aborted with
  `Unable to find AMFI`. That image accidentally omitted
  `launchd_unsecure_cache=1`, which this AMFI-less kernel needs. This result is
  a boot-argument mistake, not evidence of a new AMFI kernel regression.
- The user's separate `/tmp/ios-munch-ufs-userspace-20261022-a1/` boot got
  farther: launchd started jobs and SpringBoard, confirming the prior platform
  kill is passed. Multiple daemons then failed because dyld could not find
  `/usr/lib/libobjc-trampolines.dylib` in the cache. On the currently mounted
  HFS+ source image, this file exists only at
  `private/preboot/Cryptexes/OS/usr/lib/libobjc-trampolines.dylib`, not at the
  root path; `ignition_level=1` does not search the cryptex for that load.
- Prepared corrected temporary RAM boot A2 with the same kernel/ADT/DTB and
  arguments `rd=ufsroot0 iosrootrw=1 unentitled_ios_sim_launch=1
  ignition_level=1 launchd_unsecure_cache=1`. Payload SHA-256
  `df69feafaec0d33516d4c25567b4d9a4d4edf268e958776e3039c56774c470cb`; boot
  image `/tmp/munch-ufs-launch-a2-boot.img` SHA-256
  `d5a0c74c46206dd2764623eed4470079e64f1c3b1032b4e09cfc26af7c3ecc43`.
  Verified boot header v2. `fastboot devices` was empty at handoff, so A2 has
  not been dispatched.
- Next storage fix: on the host, add a hard link in the *local keybag raw
  image copy* from
  `private/preboot/Cryptexes/OS/usr/lib/libobjc-trampolines.dylib` to
  `usr/lib/libobjc-trampolines.dylib`; validate, rebuild full sparse image,
  check hash. Do not touch the phone until image inspection is complete; a
  separately explicit confirmation is needed before flashing this changed
  image. Never write `userdata`.

### HANDOFF 2026-10-22: RW BOOT REACHED OLD PLATFORM KILL; A1 RETRIED

- User console confirms the keybag image boots read-write: UFS enumerates
  `iosroot`, HFS journal initializes, `iOSRoot` mounts, launchd is selected.
  The screen shows the same known `unexpected SIGKILL of launchd (CS_KILLED)
  ... namespace 9 code 0xe` platform-policy failure seen before the keybag
  repair. UFS, HFS, keybag placement, and launchd loading are therefore no
  longer the immediate blockers.
- The prior `munch-ufs-userspace-a1` artifact was not retained. Rebuilt a
  temporary RAM payload with the same CI kernel and UFS ADT, using
  `rd=ufsroot0 iosrootrw=1 unentitled_ios_sim_launch=1 ignition_level=1`.
  Payload SHA-256
  `4f0b53eb432fd8c4eac5727454baa6217a8ec35f94797bdf8b073a3a34896e69`.
- Boot image `/tmp/munch-ufs-launch-a1-boot.img`, SHA-256
  `833db9945f6595c4992e4b83d2883802629dc96d92b837265dee7f6e3c59dd01`. After
  the user returned the phone to fastboot, preflight confirmed product
  `munch`, and `fastboot boot` completed successfully (`Sending OKAY
  0.365s`, `Booting OKAY 0.226s`).
- Await fresh console/photo or logs. No further device command until the user
  reports the result. This is a temporary RAM boot; it does not flash or erase
  anything.

### HANDOFF 2026-10-22: KEYBAG IMAGE FLASHED; RW BOOT DISPATCHED

- User explicitly confirmed flashing the verified keybag image. Fastboot
  preflight: serial `7c8c92ae`, product `munch`, partition
  `partition-size:iosroot = 0x6FFFFF000`, sparse SHA-256
  `2f115aa922259985f53bd43f0c376914887b69aed9d200eca4dfaee609a4b994`.
- Wrote only that sparse image to the dedicated `iosroot` partition. All
  16/16 chunks sent and wrote `OKAY`; `fastboot` reported `Finished. Total
  time: 265.972s`. No `userdata` operation or GPT change occurred.
- Built a temporary RAM boot payload with the same CI kernel and UFS ADT,
  changing only the boot arguments to `rd=ufsroot0 iosrootrw=1`. Payload:
  `/tmp/munch-ufs-keybag-payload.bin`, SHA-256
  `084cbf2c018f2b3ef36d6d2076c71426642704c00e01d874effdd853155b6c49`.
- Boot image: `/tmp/munch-ufs-keybag-boot.img`, SHA-256
  `acd01fea86f56a51d7332efa56e8c4f085242dfd9984b98b4c081f5157abaad9`.
  The first `fastboot boot` attempt failed with `Status read failed (No such
  device)` before completing; the device immediately reappeared in fastboot.
  A retry returned `Sending OKAY 0.369s`, `Booting OKAY 0.238s`.
- This is a temporary RAM boot with XNU writing only to the existing
  `iosroot`. Await the on-device console/photo or captured logs before any
  further device command. Keybag absence was the known blocker; the new image
  contains `/private/var/keybags/systembag.kb`.

### HANDOFF 2026-10-22: KEYBAG IMAGE BUILT; AWAIT FLASH CONFIRMATION

- User repaired the separate keybag raw copy with `fsck.hfsplus -f -y` on a
  loop device limited to the 16-GiB HFSX volume. The tool reported
  `repaired successfully`; a subsequent read-only `-f -n` still reports
  `Volume header needs minor repair` and exit 8, a known hfsprogs diagnostic
  for this journaled volume. The primary and alternate 512-byte volume headers
  are byte-identical after repair. The reported catalog folder-count repairs
  were applied successfully.
- Confirmed keybag content on the copy: root-owned directory mode 0700 and
  `systembag.kb` mode 0600 with expected `{KeyBagKeys: U1dFRVQ=}` plist.
- Built Android sparse image
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB-keybag.simg`
  (12,145,458,472 bytes). Its sparse header matches the previously installed
  image exactly: 4096-byte blocks, 7,340,031 blocks, 28,003 chunks. SHA-256:
  `2f115aa922259985f53bd43f0c376914887b69aed9d200eca4dfaee609a4b994`.
- Raw repaired copy
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB-keybag.img`
  SHA-256: `1669fb44f9f77def71ebb2fa88b335437256dc3fd313b62654cfbcec0785dfef`.
  The original raw and sparse images were preserved. No phone operation or
  `userdata` access occurred.
- Next: do not flash until the user explicitly confirms the destructive write
  to the dedicated `iosroot` partition. Before any flash, confirm the phone is
  in fastboot and re-check product `munch` and exact target partition size.
  Never write `userdata`.

### HANDOFF 2026-10-22: KEYBAG ADDED; COPY NEEDS HFS HEADER REPAIR

- User created the separate raw copy
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB-keybag.img`
  and added `/private/var/keybags/systembag.kb` successfully. The file is
  `root:root`, mode 0600; its parent directory is `root:root`, mode 0700, and
  its XML contents match the builder's expected `{KeyBagKeys: U1dFRVQ=}` plist.
- The source image and phone were not modified. The script stopped before
  creating the sparse result because `fsck.hfsplus -f -n` reported the known
  minor repair needed by this journaled HFSX image's header.
- The modified copy is now the only image that may be repaired. Next: attach
  it with a 16-GiB loop limit, run `fsck.hfsplus -f -y` on that loop, verify
  with `-n`, detach it, and build
  `iosroot-partition-28GiB-keybag.simg`. Do not flash until its checksum and
  contents are verified and the user separately confirms the write.

### HANDOFF 2026-10-22: READ-ONLY HOST MOUNT CONFIRMED; KEYBAG ABSENT

- User successfully mounted the raw image read-only after limiting the loop
  device to 16 GiB (`losetup --sizelimit 17179869184`). This confirms the
  28-GiB container tail caused the earlier whole-device mount failure.
- The mounted source has no `/private/var/keybags` directory and no
  `systembag.kb`. The user unmounted and detached the read-only loop device;
  no image writes or phone operations occurred.
- Next: create a separate sparse raw copy, attach it writable with the same
  16-GiB size limit, add the root-owned keybag plist, cleanly unmount, then
  convert the full 28-GiB raw copy to Android sparse format. Preserve the
  original raw and sparse images. Do not flash until the generated image is
  checked and separately confirmed.

### HANDOFF 2026-10-22 12:48 +05: HOST IMAGE MOUNT SIZE MISMATCH FOUND

- User attempted a read-only mount of the 28-GiB raw container as a whole. The
  mount failed; no image or device data was modified.
- Host inspection confirms the primary and alternate volume headers are HFSX
  (`HX`), allocation block size 4096, and 4,194,304 allocation blocks, so the
  HFSX volume is exactly 16 GiB. Its valid alternate header is at byte
  17,179,868,160 (16 GiB - 1024); the 12-GiB container tail starts at byte
  17,179,869,184 and is zero-filled.
- Linux hfsplus reads the alternate header from the end of the visible block
  device and rejects it if the signature differs from the primary header.
  Therefore mounting the whole 28-GiB loop device reads zeros instead of the
  valid header. This explains the mount failure; it does not indicate a missing
  keybag or by itself prove HFSX corruption.
- Next: retry read-only with a loop device limited to 17,179,869,184 bytes
  (`losetup --sizelimit 17179869184`), then inspect
  `/private/var/keybags/systembag.kb`. If absent, make a separate raw copy and
  repeat with a writable 16-GiB-limited loop. Do not run repair on the source;
  do not write to the phone or access `userdata`.

### HANDOFF 2026-10-22 12:42 +05: KEYBAG FILE CHECK NEEDED

- Latest user console logs show `keybagd` failing to open
  `/private/var/keybags/systembag.kb` (`No such file or directory`) and then
  reporting `Unable to load keybag with No Crypto: -7`. This is the current
  userspace blocker after the HFS+ root mounted and launchd started.
- The captured ADT at
  `/tmp/ios-munch-ufs-boot-39668666918/adt.dump` contains
  `/defaults/no-effaceable-storage = 1`. The `Could not get options entry`
  messages are separate; this setting is under `/defaults`, not `/options`.
- `tools/mkiosroot.sh` creates the expected root-owned plist
  (`KeyBagKeys` data `U1dFRVQ=`, directory mode 0700, file mode 0600), but the
  exact image installed on-device has not yet been inspected on the host, so
  absence from the image is not yet independently confirmed.
- Host image paths: raw
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB.img`
  (28 GiB logical, 12 GiB allocated) and installed sparse image
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB.simg`.
  `fsck.hfsplus` and `img2simg` are installed. There is 37 GiB free on the
  filesystem, enough for a sparse raw working copy and rebuilt sparse image.
- Next: user runs the host-side `sudo` commands from the handoff to inspect the
  original read-only, then, only if the file is absent, copy the raw image and
  add the plist to that copy. Build a new `.simg` from the copy and report its
  SHA-256. Do not write to the phone until the image is verified and the user
  separately confirms flashing. Do not access `userdata`.

### HANDOFF 2026-10-22 11:15 +05: UFS ROOT REACHED LAUNCHD; NEXT USERSPACE BOOT

- The earlier "UFS writes failing" conclusion was wrong: the latest read-write
  boot did write successfully (`SWEET UFS: WRITE lba 0x2c400 +1 == 512 bytes:
  OK`) and then reached Apple launchd.
- The `SIGKILL` (`namespace 9 code 0xe`) is the known OS X-config rejection of
  iOS-platform binaries first observed on 2026-10-15, not a storage failure.
- New diagnostic payload `munch-ufs-userspace-a1` adds
  `unentitled_ios_sim_launch=1`, retains `ignition_level=1`, and uses the
  normal release launchd cache. Payload SHA256
  `9021a45ae65bba772f27bb9dc5dbeb780e2cb00d55d3e1424c49a39336dca3b9`; boot
  image SHA256 `404c6381ae30b49ba11dca7d4afdd5be44e8a6ec502d15bff9568537dc95caac`.
- Continue with temporary `fastboot boot` only. Writes remain restricted to the
  dedicated `iosroot` partition.

### HANDOFF 2026-10-22 10:55 +05: HFS+ IOSROOT INSTALLED; RAM BOOT RETRIED

- After explicit user confirmation, verified from OrangeFox that
  `/dev/block/by-name/iosroot` resolves to `/dev/block/sda35` and is exactly
  `30064766976` bytes. No `userdata` path was used.
- Fastboot independently confirmed `iosroot` as a raw, non-slot partition with
  size `0x6fffff000` (28 GiB).
- Wrote only
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB.simg`
  to `iosroot` using `fastboot flash iosroot`. All `16/16` sparse chunks and
  writes returned `OKAY`; total write time was `266.758s`.
- The first immediate `fastboot boot` attempt lost USB during image transfer,
  before `Booting`; no conclusion about the installed root can be drawn from
  that failed transfer. After the user rebooted the phone into fastboot,
  `product: munch` returned normally and the same shim was dispatched again.
  The retry returned `Sending OKAY`, `Booting OKAY`, total `0.610s`.
- The retry is a RAM-only, read-only XNU boot (`iosrootrw=0`). Wait for the
  screen or user-provided console photo before issuing another device command.

### HANDOFF 2026-10-22: UFS ROOT PROBE REACHED HFS MOUNT

- The user's photo of the phone console confirms that XNU completed the UFS
  path: `SWEET UFS: "iosroot" on LUN 0: first block 0x34037fc, 28671 MiB,
  read-only`, followed by `BSD root: ufsroot0, major 1, minor 0`.
- The failure is after block-device registration, in HFS+ detection:
  `hfs_validateHFSPlusVolumeHeader: unknown Volume Signature : 1020`,
  `hfs_mountfs returned error=-22`, `cannot mount root, errno = 19`.
  The repeated mount attempts are therefore consistent with the phone's
  current `iosroot` contents still being F2FS, not with a missing UFS node or
  failed UFS enumeration.
- This is the first on-device confirmation that the Munch UEFI/XNU handoff,
  UFSHCI takeover, GPT lookup restricted to `iosroot`, and read-only root
  device registration all work together. No device write occurred.
- Next action is an explicit, separately confirmed installation of the
  prepared HFS+ image to the existing `iosroot` partition. Do not write it or
  enable `iosrootrw=1` until the user confirms that destructive step and the
  phone is back in a recovery/ADB environment.

### HANDOFF 2026-10-22 10:28 +05: READ-ONLY UFS PROBE DISPATCHED

- Confirmed the connected device in fastboot: serial `7c8c92ae`, product
  `munch`.
- Verified the temporary image SHA-256 before boot:
  `e5e429e672f7bd849855c005000ae2b33800f36c0ee77e922e8d06148322147c`.
- Ran only `fastboot boot` for
  `/home/danya2271/Загрузки/ios-munch-install-20H392/shim-boot-ufs-iosroot.img`.
  Fastboot reported `Sending 'boot.img' (17136 KB) OKAY [0.364s]`,
  `Booting OKAY [0.228s]`, total `0.601s`.
- No partition was flashed, erased, mounted, or otherwise written; `userdata`
  was not accessed. Do not issue another device command until the user reports
  the screen or provides the boot logs. The expected first result is UFS/GPT
  enumeration followed by an HFS+ mount failure because the phone's current
  `iosroot` contents are still F2FS.

### HANDOFF 2026-10-22: UFS ROOT CANDIDATE AND READ-ONLY BOOT IMAGE PREPARED

- Prepared the existing iOS 16.7.16 (`20H392`) HFS+ root as a local install
  candidate for the dedicated `iosroot` partition. Raw image:
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB.img`,
  exactly `30064766976` bytes (`28 GiB`), SHA-256
  `b24b363a5940dcdc5a96861989d35fa1e30f39c50863f67d8353807fd26fdeed`.
  Android sparse image:
  `/home/danya2271/Загрузки/ios-munch-20H392/iosroot-partition-28GiB.simg`,
  `12145450280` bytes, SHA-256
  `2c7e0b92ad88d171bbd319a056b5083610a9cb4b9c7ba92efcdc2394d37eea5d`.
- The raw container's first 16 GiB is byte-identical to the original,
  `fsck.hfsplus`-verified root; the remaining 12 GiB is verified zero padding.
  The HFS+ header still declares 16 GiB, so only 16 GiB is usable by the
  filesystem. The allocation bitmap and catalog were parsed offline: all
  642,638 catalog leaf records and the 184,121 file entries match the volume
  header. Do not claim a 28-GiB HFS+ filesystem yet.
- Added `mkadt.py --munch --ufs`, emitting `/munch-ufs` at the Munch UFSHCI
  address while keeping RAM-only Munch ADTs UFS-free. Updated the root builder
  default and documentation to use a dedicated 28-GiB `iosroot` image.
- Repaired `xnu/patches/0087-munch-ufs-iosroot-only.patch`. It now applies
  cleanly after patches 0001-0086: Munch resolves only `/munch-ufs` and GPT
  partition `iosroot`; writes require `iosrootrw=1`; SWEET-only input hooks are
  not started for Munch. `python3 tools/validate-repo.py` passes all 87 patches.
- CI run `39668666918` (workflow_dispatch, `DEVELOPMENT/MUNCH`, macOS 14) for
  commit `cc117ceffa3f50eea72cbaa76c424612faefb384` completed successfully on
  2026-10-21 20:27:56 UTC. Patch 0087 is shown applying in the run log. The
  `kernel.development.munch` artifact is 17,200,168 bytes, SHA-256
  `6b6d8f4706814cc2bc73dfebf70ef53a06b80b84c406b1e511282c7d481a6592`.
- Built a high-memory, UFS-enabled payload with `rd=ufsroot0 iosrootrw=0`;
  this is deliberately read-only and selects only the GPT partition named
  `iosroot`. Payload:
  `/home/danya2271/Загрузки/ios-munch-install-20H392/munch-ufs-payload.bin`,
  16,575,792 bytes, SHA-256
  `8be690a933dd2c8bb6cc70465716a20fe4d2f813db131e14f27d1343b4fc5101`.
- Prepared temporary shim boot image:
  `/home/danya2271/Загрузки/ios-munch-install-20H392/shim-boot-ufs-iosroot.img`,
  17,547,264 bytes, SHA-256
  `e5e429e672f7bd849855c005000ae2b33800f36c0ee77e922e8d06148322147c`.
  `unpack_bootimg` verified the embedded shim kernel, XNU payload ramdisk,
  and captured live Android FDT byte-for-byte. The shim's host FDT check accepts
  XNU memory `[0x100000000,0x1fcc00000)`.
- The assembled bundle is under
  `/home/danya2271/Загрузки/ios-munch-install-20H392/`; its `iosroot` sparse
  image is a symlink to the 12,145,450,280-byte prepared `.simg` to avoid a
  second 11.3-GiB copy. `SHA256SUMS` is included.
- The current phone partition is still F2FS; no image has been installed. No
  ADB/fastboot command, partition access, `userdata` access, flash, erase, or
  device write occurred. The next step after the user connects the phone is a
  temporary `fastboot boot` of the read-only probe only; it should confirm
  UFS/GPT `iosroot` reads, then HFS mount is expected to fail on the current
  F2FS contents. Installing HFS and enabling `iosrootrw=1` require separate
  explicit confirmation.

### HANDOFF 2026-10-22: TEMPORARY UFS-ENABLED UEFI BOOT

- `fastboot devices` detected one device, serial `7c8c92ae`.
- Ran only `fastboot boot
  /tmp/edk2-msm-munch-fork.m185A6/src/boot-munch.img`. Fastboot reported
  `Sending 'boot.img' (3840 KB) OKAY [0.084s]`, `Booting OKAY [0.231s]`,
  total `0.316s`. No `fastboot flash`, partition reads/writes, or `userdata`
  access occurred.
- Immediately after handoff, `fastboot devices` returned no device and
  `adb devices -l` listed none. This establishes ABL accepted the temporary
  boot image only; the UEFI screen and UFS enumeration are not yet confirmed.
- Wait for the user to report/photograph the current screen before issuing any
  further reboot or boot command. Next test, if UEFI is interactive: use only
  read-only enumeration (`map -r`); do not launch installers or write tools.

### HANDOFF 2026-10-22 00:12 +05: UFS PROFILE RESTORED; DEVICE NOT CONNECTED

- The EDK2 fork `/tmp/edk2-msm-munch-fork.m185A6/src` is on commit `85f4217`
  (`fix(munch): restore UFS storage profile`), pushed to
  `danya2271/edk2-msm-munch`. `configs/devices/munch.conf` now sets
  `MUNCH_RAM_ONLY=FALSE`; the built firmware contains `UFSDxe`, `ScsiBus`,
  `ScsiDisk` and `PartitionDxe`. Image:
  `/tmp/edk2-msm-munch-fork.m185A6/src/boot-munch.img`, SHA-256
  `e956a38d4bb2f63037b3d6d8a23c21a98b33cad6c375ab4fd836bc7310b34a45`.
- At this checkpoint both `adb devices -l` and `fastboot devices` are empty.
  No device command, partition access, `fastboot flash`, or `userdata` access
  was performed in this step.
- To connect for the next read-only UEFI test: boot Android and run
  `adb reboot bootloader`, or power off and hold Volume Down + Power until
  fastboot; then confirm `fastboot devices` prints the phone. Use only
  `fastboot boot ...` for the first UFS test.
- The XNU UFS patches are still donor SWEET code. Before a storage-backed iOS
  root test, first validate UEFI UFS enumeration and determine what controller
  state is handed to XNU on Munch. The donor driver takes over UFSHCI directly;
  its SWEET behavior is not evidence that this is safe after Munch UEFI.
  Do not port or enable its XNU path until that hardware handoff is verified.
  If porting proceeds, bind GPT lookup and every write to the dedicated
  `iosroot` partition; keep `userdata` out of all boot paths.

### HANDOFF 2026-10-21 23:43 +05: RUN 8 RAM-ONLY PID 1 TEST

- GitHub CLI is installed and authenticated as `danya2271` (keyring; `repo` and
  `workflow` scopes). Actions run `39651685900` (run 8), commit
  `1290e65424fec47fbcfa1026de6aa4a0aab2eb37`, completed successfully. Its
  `kernel.development.munch` artifact is 17,182,480 bytes, SHA-256
  `e0b5e3d90917afde1650900383a1808a61d9dd941ec31d28e9c863d0849063bc`.
- Built a fresh MUNCHXNU payload with the run 8 kernel and the 16,635-byte
  `munch-init.macho` extracted from the prior QEMU-tested payload (byte-identical
  to its saved source). The hardware ADT contains D22AP identity, the iPhone X
  `/product` properties, the Munch timer/GIC mapping, the candidate RAM window
  `[0x100000000,0x1fcc00000)`, and no UFS node. Payload: 16,591,008 bytes,
  SHA-256 `e58260a7ff56eb50e50ff6aef12227d3c66749e5f7cb5853c5133d03218d7e0f`.
- Rebuilt the temporary shim boot image with the captured live Android FDT
  (812,778 bytes, SHA-256
  `65f4987ebb592acf11867c22d92e7cd41a7b41ea1e10f96485863b38f7ac646d`).
  Image: `/tmp/ios-munch-init-hw-20261021/shim/out/shim-boot.img`, 17,563,648
  bytes, SHA-256
  `c68df12be03597874d19deb146d3d9415f79830a8b8b84cf791e7f482bac8937`.
  `unpack_bootimg` confirmed the shim kernel, payload ramdisk, and live DTB match
  their inputs byte-for-byte. Host FDT preflight accepts the proposed RAM
  window. `python3 tools/validate-repo.py` passes (86 patches, 16 Python tools,
  8 shell scripts); `git diff --check` passes.
- QEMU ran the rebuilt kernel/payload for 150 seconds: `no_panic_observed`,
  zero unknown glyphs, 75 decoded `MUNCH init: still alive` lines. This proves
  the minimal mockfs/PID 1 path in QEMU, not the full iOS userspace or hardware.
- Hardware test on 2026-10-21: read-only ADB checks confirmed Android build
  `V816.0.9.0.ULMMIXM` and KernelSU root (`uid=0`). `adb reboot bootloader`
  returned 0; `fastboot boot /tmp/ios-munch-init-hw-20261021/shim/out/shim-boot.img`
  returned 0 (send OKAY 0.373 s, boot OKAY 0.228 s, total 0.609 s). No `flash`
  command, partition access, or userdata access occurred. The user's photo
  `/tmp/codex-clipboard-Hq2XPC.png` confirms the on-device sequence reached
  `BSD root: md0, major 1, minor 0`, then `load_init_program: attempting to
  load /sbin/launchd`, `*** MUNCH userspace: init running as PID 1 ***`, and
  repeated `MUNCH init: still alive` heartbeats. This is a confirmed hardware
  boot through XNU, RAM mockfs root, and a live PID 1; it is not the iOS
  launchd/System volume or SpringBoard. `adb devices` and `fastboot devices`
  were both empty immediately after the fastboot handoff, before the photo.
  No further reboot has been sent.
- The 3-GiB HFS image at
  `/tmp/ios-munch-minroot-39597312881/minroot.img` remains invalid/truncated
  according to `fsck_hfs -fn`; do not use it. The current hardware attempt used
  only the RAM-resident mockfs PID 1 image.
- Next: rebuild a correctly sized, `fsck_hfs`-verified minimized iOS HFS root
  from the extracted 20H392 files; validate it in QEMU before attempting a
  temporary hardware boot. Keep the current PID 1 screen until the user is
  ready for another reboot. Do not flash or read/write `userdata`.

### HANDOFF 2026-10-21: DEVICE STORAGE CHANGED; PAUSE WRITES

- The user clarified that `/home/danya2271/Загрузки/recovery.img` is OrangeFox
  and was started with temporary `fastboot boot`, not flashed. ADB currently
  reports the POCO F4 (`munch`) in recovery. The live read-only check found
  `/data` mounted read-write as F2FS from `/dev/block/dm-5`.
- A read-only `sgdisk --print/--verify /dev/block/sda` check reports no GPT
  problems and 1 free 4-KiB sector. Partition 34 `userdata` is now sectors
  `2949120-54540282` (196.8 GiB); partition 35 `iosroot` is sectors
  `54540284-61880314` (28.0 GiB). The GPT was changed; do not describe it as
  untouched.
- In the previous session, `userdata` was formatted as F2FS with recovery's
  `make_f2fs`; its output reported discarding 201,527 MB. Treat the old
  filesystem and Android user data as destroyed. `iosroot` was also formatted
  as F2FS; it is not an iOS HFS+ root and no iOS root has booted on-device.
- The host directory
  `/home/danya2271/Загрузки/ios-munch-userdata-backup-20261021` contains six
  checksum-verified tar archives for selected shared-storage folders only:
  `DCIM`, `Pictures`, `Android`, `Movies`, `vkmusic`, and `Download`. They do
  not contain app-private data, Android app state, encryption keys, or a full
  userdata image. Copies and extracted staging trees were also observed under
  `/data/restore`; preserve them and the host archives. Do not claim Android
  data has been fully backed up or restored.
- Archive inspection on 2026-10-21 found 43 `.apk` entries in `Android.tar`
  and 37 `.apk` entries in `Download.tar` (some may be duplicates or merely
  downloaded installers). `Android.tar` also contains 16,778 entries below
  `Android/data`, 14 below `Android/obb`, and 19 below `Android/media`. These
  are shared-storage files and can help reinstall or recover media, but they
  are not the installed-app registry, `/data/app` package state, or private
  `/data/user/0` data. Installed apps, their private databases, credentials,
  and system settings were not backed up by this archive procedure.
- `sda.gpt.backup` in that host directory is zero bytes and invalid. The
  `sda.gpt.print*` files are text reports, not binary GPT restore backups.
- Do not format, repartition, restore, or otherwise write to the device now.
  Keep OrangeFox available and preserve `/data/restore`. The user asked to be
  notified before booting Android to check data and configure screen locking.
  Do not ask them to boot Android or set a lock yet; first establish a reviewed
  plan for the partial archives and current `userdata` state, then notify them
  before that checkpoint. Booting Android will not recover the old userdata.

### HANDOFF 2026-10-21: QEMU USERSPACE PASSED; RAM ROOT TOO LARGE

- `munch-init` QEMU test used the successful MUNCH kernel from Actions run
  `39597312881`, a temporary QEMU-compatible ADT copy (Munch identity retained,
  GIC addresses adjusted), and `rd=md0`. It ran for 150 seconds with
  `no_panic_observed`, zero unknown glyphs, and regular PID 1
  `MUNCH init: still alive` output. This validates kernel execution, RAM root
  selection, Mach-O userspace execution, syscalls, `/dev/console`, timers, and
  basic Munch ADT handoff. It does not prove hardware boot.
- Kernel artifact:
  `/tmp/ios-munch-artifacts-39597312881/kernel/kernel.development.munch`,
  17,182,480-byte ARM64 Mach-O, SHA-256
  `10a5fa286959a72934a86f71417b6809b619ef56ecae4dbe489663e6a3c597f1`.
- Local userspace test executable and payload:
  `/tmp/ios-munch-userspace-39597312881/munch-init.macho` and
  `/tmp/ios-munch-userspace-39597312881/munch-init.payload.bin`.
- Extracted official iPhone X iOS 16.7.16 build `20H392` to
  `/home/danya2271/Загрузки/ios-munch-20H392`. Verified OS, SystemOS, AppOS and
  DeviceTree CRCs; extracted `/home/danya2271/Загрузки/ios-munch-20H392/d22ap.adt`.
- Built a local 16-GiB HFS+ root at
  `/home/danya2271/Загрузки/ios-munch-20H392/ios16-root-20H392.img`, populated
  from the OS/SystemOS/AppOS DMGs, applied `tools/munch-root-tweaks.py`, and
  repaired it with `fsck.hfsplus -fy`. Its SHA-256 is
  `382834b6c90cecf51bf146f1947194313c52681e61f1c8d025a10eb2817ec06e`.
- Packing that root with `tools/mkpayload.py --ramdisk ... rd=md0` fails:
  `ValueError: RAM disk exceeds the ADT memory window (0xfcc00000 bytes)`.
  Current payload layout embeds the full image, so a full iOS root cannot fit.
  Next concrete step is building a minimized root containing only the files
  needed by launchd, testing it in QEMU first, and only later considering
  demand paging or UFS. Do not boot a full-root image on the phone.

### >>> HANDOFF 2026-10-21: FIRST MUNCH XNU FASTBOOT BOOT DISPATCHED <<<

- The user-provided `/home/danya2271/Загрузки/logs_107299279209.zip` is run
  `39594751289`, not the later successful run. It confirms patches 0083-0085
  applied and XNU reached the final link; that attempt failed on the two SWEET-
  only symbols `_sweet_crash_log` and `_sweet_sample_task`. Patch 0086 enables
  their shared implementations for MUNCH.
- GitHub API metadata confirms run `39597312881` (run 7), commit
  `a8fa0eefb3f99953a12f456010370f6fb2872b69`, completed successfully on
  2026-10-21 at 12:17:30 UTC. Its `xnu-kernel` artifact is ID `12345680427`,
  59,790,641 bytes, digest
  `sha256:6e296eda39716699404081a14a139b94a3f4eb847c682a995eb14348e346a844`;
  `xnu-buildtree` is ID `12345820245`, 23,181,143 bytes, digest
  `sha256:0cd3ef303ec382a7f6cb17174bfa0c578d53b86fe2fae2628a0e7a3c8f49b1ad`.
  CI success plus the full kernel artifact confirms XNU compiled and linked.
- The user supplied `/home/danya2271/Загрузки/xnu-kernel.zip` and
  `/home/danya2271/Загрузки/xnu-buildtree.zip`. Their SHA-256 digests match
  GitHub's artifact metadata. The kernel archive's internal checksum passes;
  `kernel.development.munch` is a 17,182,480-byte Mach-O 64-bit ARM64
  executable with SHA-256
  `10a5fa286959a72934a86f71417b6809b619ef56ecae4dbe489663e6a3c597f1`.
  Extracted artifacts are under `/tmp/ios-munch-artifacts-39597312881/`.
- Packed the kernel with `tools/mkpayload.py`, using a generated RAM-only
  Munch ADT with candidate DRAM `[0x100000000, 0x1fcc00000)`, no UFS node,
  no root disk, and `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress`.
  Payload SHA-256 is
  `a9c82935dd0fe5252b5fb2af7e46b099a98b0b7a33170778174923941674bbbf`.
  The memory candidate passes the shim's host FDT-range check against the
  saved Munch Linux FDT; this does not establish the runtime fastboot/UEFI map.
- QEMU with the original Munch GIC addresses predictably panicked because its
  `virt` GIC is at different addresses. Repacking only a temporary ADT copy
  with QEMU's GIC addresses kept the Munch identity and PPI; the 35-second run
  had no visible panic and logged CPU/GIC initialization, platform expert,
  AppleCLCD and HFS registration, then waited for an Apple_HFS root device.
  This is expected without a root disk and does not prove hardware boot.
- Built a separate test image under
  `/tmp/ios-munch-test-39597312881/`, leaving the repository's `shim/out`
  untouched. It is 17,543,168 bytes, Android boot header v2, SHA-256
  `75e5ab2308854e6c49d86061b0f21e0ba3ca18b36b1e96bcc5abce1b7655db21`.
  Unpacking verified the ramdisk equals the XNU payload and the embedded DTB
  equals the saved live FDT.
- Before boot, verified the external non-userdata backup read-only: all 116
  SHA-256 entries pass, manifest and partition names match exactly, and no
  `userdata` entry exists. This is the required backup preflight result, not
  permission to write any partition.
- Before boot, re-verified the external non-userdata backup: 116/116
  SHA-256 entries pass, manifest and partition names match exactly, and no
  `userdata` entry exists.
- The user approved testing. `adb reboot bootloader` moved the Munch device to
  fastboot; `fastboot boot
  /tmp/ios-munch-test-39597312881/shim/out/shim-boot.img` returned `Sending`
  OKAY (17,132 KB) and `Booting` OKAY. This confirms only ABL acceptance; the
  on-screen shim/XNU result is not yet observed. No more device commands have
  been issued; wait for the user to report or photograph the screen.
- No `fastboot flash`, partition operation, or `userdata` read/write occurred.
  The payload has no RAM root disk and its Apple Device Tree omits UFS. Keep all
  subsequent device tests temporary `fastboot boot` only.

### >>> HANDOFF 2026-10-21: FIXED MUNCH MACHINE-CONFIG PATCH APPLY <<<

- Inspected the user-provided Actions log archive
  `/home/danya2271/Загрузки/logs_107285093280.zip` for run `39589581233`.
  Xcode setup used Xcode 15.0.1 / macOS SDK 14.0 successfully. Build then
  stopped at `xnu/makedefs/MakeInc.kernel:22` with
  `Unsupported CURRENT_MACHINE_CONFIG MUNCH`.
- The cause was upstream of compilation: the build helper warned and skipped
  patch `0083-munch-machine-config.patch` because its `bsd/vm/vm_unix.c` hunk
  expected a SWEET-only conditional. Patch `0070-munch-pid-hibernate.patch`
  had already changed that conditional to SWEET-or-MUNCH, making the hunk a
  stale duplicate. Since `git apply` rejected patch 0083 atomically, the
  required MUNCH machine config was never registered; patch 0084 was then also
  skipped because its source context depended on 0083.
- Removed only the redundant `vm_unix.c` file diff from patch 0083. Replayed
  patches 0001-0085 in order on a clean Apple XNU `xnu-8796.141.3` checkout
  (commit `1b191cb58250d0705d8a51287127505aa4bc0789`) after staging the same
  pinned libpthread, BearSSL and HFS sources used by CI. All 85 patches now
  apply. The staging scripts' final SDK-header copy could not run on Linux
  because `xcrun` is absent; the vendor sources were staged before those steps,
  which was sufficient to verify patch applicability only.
- Added a workflow preflight after patch application that checks both the
  `MakeInc.def` MUNCH registration and `MUNCH.h`; this turns a future silent
  patch skip into a short, explicit failure before bootstrap/build time. The
  previous run's `xnu-kernel` artifact contains only KDK-library diagnostics;
  the collection log confirms there was no `kernel.*` output.
- Committed as `1b6359e` (`fix(ci): apply Munch machine config patch`) and pushed
  to `origin/main`. Repository validation and `actionlint` both passed before
  push.
- Actions run `39592383679` started at 2026-10-21 11:29:56 UTC and failed at
  11:30:25 UTC. Checkout, build-script fetch and Xcode preparation succeeded;
  `Build` exited 2. The unauthenticated API does not expose the run log
  (`403: Must have admin rights to Repository`), so the first build error is
  still unknown. The new `MUNCH` preflight result is also not visible without
  that log.
- Post-failure diagnostics had separate issues: packaging the build tree
  exited 1, and `xnu-kernel` artifact upload failed because the KDK scan
  created a literal `lib*.members.txt` when no archives matched. Added a
  regular-file guard to the KDK scan; verify it in the next run. The
  `xnu-buildtree` artifact uploaded, but its contents cannot be downloaded
  without GitHub authentication.
- Commit `e605723` added the KDK regular-file guard and was pushed. Run
  `39592897886` completed at 2026-10-21 11:34:21 UTC after 21 seconds with
  `Build` exit 2. The KDK diagnostic and both artifact-upload steps now pass;
  `xnu-kernel` is only 136 bytes, so it contains no kernel. Build-tree
  packaging still exited 1 and produced a 265-byte artifact. The run's check
  annotations expose only exit codes, not the first build error; it is not yet
  known whether the MUNCH preflight passed.
- Build-tree packaging listed generated paths unconditionally. They may not
  exist when the build exits early, so commit `d194655` changed it to add only
  existing paths. Run `39593432412` completed at 2026-10-21 11:39:21 UTC with
  `Build` exit 2 and build-tree packaging exit 1; both artifact uploads pass,
  but the artifacts are only 171 bytes (`xnu-buildtree`) and 136 bytes
  (`xnu-kernel`).
- The tar command still used relative `-C ..` for `sdk-kernel-platform`. If all
  optional `dxb` paths are absent, no earlier `-C dxb` changes tar's working
  directory, so `..` points outside the workspace. Switched both archive roots
  to absolute `$GITHUB_WORKSPACE` paths. This explanation is confirmed by the
  command's path semantics; a local smoke test passed with and without XNU
  paths.
- Commit `6aac5e6` was pushed. Run `39593841679` completed at 2026-10-21
  11:42:07 UTC: `Build` exited 2, while build-tree packaging, KDK inspection
  and both artifact uploads succeeded. Artifacts remain only 265 bytes
  (`xnu-buildtree`) and 136 bytes (`xnu-kernel`), so no kernel was produced.
  Their sizes suggest failure occurred before the XNU source/build tree was
  staged, but that is not confirmed without the log.
- The user provided the Build-step screenshot for run `39593841679`: Bash
  reports `build.sh: line 862: syntax error near unexpected token 'venv'`.
  Since Bash parses the whole script before executing it, no XNU checkout,
  patch application or MUNCH preflight ran; this also explains the tiny
  diagnostic artifacts. The previous multiline `sed` insertion produced an
  invalid script.
- Replaced the injection with an `awk` transformation and added `bash -n`
  immediately after rewriting the fetched build script. A local simulation
  of the generated insertion passes `bash -n`. Commit `8bf6718` was pushed.
- The user supplied `/home/danya2271/Загрузки/logs_107299279209.zip` for run
  `39594751289`. Patches `0083`, `0084` and `0085` applied; XNU compiled as
  `DEVELOPMENT_ARM64_MUNCH` and reached the final kernel link. The linker
  failed only on `_sweet_crash_log` (referenced by `proc_prepareexit`) and
  `_sweet_sample_task` (referenced by `sweet_sample_proc`). There was no
  kernel artifact; post-build diagnostic uploads succeeded.
- Root cause: patch `0083` enables the MUNCH caller in `bsd/kern/kern_exit.c`,
  and patch `0054` compiles `bsd/dev/sweet_sample.c`, but the shared
  implementations of both functions in `osfmk/kern/exception.c` remain inside
  `#if defined(ARM64_BOARD_CONFIG_SWEET)`. MUNCH therefore compiles both
  callers while excluding their definitions.
- Added `0086-munch-shared-exception-diagnostics.patch` to include MUNCH in
  that implementation guard, and added a repository-validator assertion for
  the guard. The patch applies to the replayed post-0085 XNU tree. Next: run
  repository checks, push, and verify that the next CI link resolves both
  symbols. No device or partition was accessed.

### >>> HANDOFF 2026-10-21: FIRST GITHUB ACTIONS MUNCH BUILD <<<

- The user updated the GitHub credential and pushed the branch. Remote `main`
  now points to `584ccfde4da3ed2f32c9661086cf7751657253e2`; local `main` is in
  sync with `origin/main`.
- GitHub Actions run `39589581233` (`xnu`, push event) completed with failure.
  Job `build` ran on the macOS runner: checkout, pinned build-script fetch, and
  Xcode/iig preparation all succeeded. The `Build` step ran from 2026-10-21
  11:07:34 to 11:10:46 UTC and failed with exit code 2. Post-failure buildtree
  and kernel-diagnostics artifact upload steps succeeded.
- The unauthenticated public API exposes step status and annotations but denies
  run-log download (`403: Must have admin rights to Repository`); no usable
  GitHub CLI credential is available in this shell. Therefore the exact make
  compiler/linker error is not yet observed. Annotation also warns that the
  Node 20 actions are forced onto Node 24; the checkout and artifact actions
  themselves completed successfully, so this is not yet linked to the build
  failure.
- Artifacts `xnu-buildtree` and `xnu-kernel` are present and not expired, but
  downloading them via the public API requires authentication. Next: inspect
  the failed `Build` step's logs in Actions (run `39589581233`) or provide an
  authorized read token/artifact; fix only the concrete first build error,
  rerun, then validate the kernel artifact offline. No device or partition
  was accessed.

### >>> HANDOFF 2026-10-21: GITHUB ACTIONS WORKFLOW RESTORED <<<

- Diagnosed the earlier GitHub error: it was a `git push` rejection because
  the stored Personal Access Token lacked permission to create/update workflow
  files, not a failing Actions build. Public GitHub API currently reports zero
  enabled workflows and zero workflow runs; `.github/workflows/xnu.yml` had
  been removed from the remote default branch.
- Restored `.github/workflows/xnu.yml` as an XNU build triggered by XNU patch
  or workflow pushes, with manual dispatch also available. The default machine
  config is `MUNCH`; `SWEET` remains selectable for reference. It fetches the
  pinned `darwin-xnu-build` revision, applies the checked-in XNU patch series,
  builds on macOS, and uploads kernel/build-tree diagnostics. Permissions are
  limited to `contents: read`.
- The runner is `macos-14` because the build needs Xcode 15 `iig` for the
  xnu-8796 IORPC ABI. GitHub currently schedules retirement of macOS 14 on
  November 2, 2026, so this runner choice is temporary and should be used for
  this test before that date.
- The workflow and this handoff are committed locally as `41a621a`; the branch
  is six commits ahead of `origin/main`. `git push origin main` was attempted
  and rejected with GitHub's exact message: `refusing to allow a Personal
  Access Token to create or update workflow '.github/workflows/xnu.yml'
  without 'workflow' scope`. The remote branch is unchanged, so no Actions run
  has started. Update the credential with classic PAT `workflow` scope (or use
  a fine-grained token with repository Workflows write permission), then push
  the branch to trigger the MUNCH build.
- No device or partition was accessed. No kernel or boot image is claimed as
  produced until the hosted job completes and its artifact passes local gates.

### >>> HANDOFF 2026-10-21: LINUX XNU BUILD RESULT <<<

- Answer: XNU C/C++ sources can be cross-compiled on this Linux host, but the
  current MUNCH kernel cannot be completed into a valid bootable image here.
  The upstream XNU README requires an SDKROOT (its examples use a macOS SDK);
  this host has LLVM/LLD but no `xcodebuild`, `xcrun`, Apple SDK/KDK, or Apple
  kernel support libraries.
- Re-audited the previous temporary Linux smoke build under
  `/tmp/ios-munch-local-build-20261021`. It contains 934 arm64 object files
  (~90.8 MB) and a 12,438,432-byte Mach-O link artifact with SHA-256
  `a5df6330d91d0d08e55e5c00466fad03754cbea25810c8ace191d298135e8750`.
  This was made from the temporary diagnostic staging tree, before patch
  `0085`; it is evidence of partial source compilation only, not a build of
  the latest complete patch series.
- Strict `ld64.lld` linking fails because `libcc_kext` and `libfirehose_kernel`
  are unavailable and Apple platform/IOKit symbols are unresolved. A prior
  relaxed link generated the Mach-O only by suppressing these errors;
  `llvm-nm -u` reports 3,089 undefined-symbol entries. Re-running
  `tools/mkpayload.py` rejects it with `kernel has binds; only rebases are
  supported`. Do not pass this artifact to the shim or `fastboot boot`.
- No source workaround, stub library, or linker relaxation was added to the
  repository: that would conceal unresolved runtime dependencies, not produce
  a usable kernel. Docker would not supply the missing Apple SDK/KDK, `iig`, or
  Apple support libraries. A valid MUNCH XNU artifact still needs an Apple
  build environment (or equivalent complete Apple build inputs).
- No ADB/fastboot or device command ran; no partition, `userdata`, `dtbo`, or
  persistent firmware was touched. All diagnostic artifacts remain under
  `/tmp`; no workflow or CI build was added. Next: obtain/prepare a genuine
  Apple XNU build environment, then build and validate the latest patch series
  before any RAM-only device test.

### >>> HANDOFF 2026-10-21: MUNCH FOCALTECH SPI PROTOCOL RE-AUDIT <<<

- Re-checked the exact local Munch sources under
  `/home/danya2271/xiaomi_sm8250_kernel/`. The earlier 71-byte transaction
  estimate is wrong for the active `focaltech_3658u` driver: normal report
  reads request 90 response bytes (`FTS_TOUCH_DATA_LEN +
  FTS_GESTURE_DATA_LEN - 1`), so the complete SPI transaction is 99 bytes:
  4-byte command header + 3 dummy bytes + 90 response bytes + 2 CRC bytes.
- The controller read is command `0x01`, read/CRC control `0xa0`, length
  `0x005a`; Linux accepts status when `(rx[3] & 0xa0) == 0`, then validates
  CRC-16/Kermit over the 90 response bytes with init `0xffff`, reflected
  polynomial `0x8408`, and little-endian CRC bytes. Touch data occupies the
  first 62 response bytes; 28 gesture bytes follow. The `0x01` marker in
  `FTS_TOUCH_DATA_LEN` is not part of the returned payload.
- Contact parsing in the Linux driver uses report byte 1 for point count,
  ten 6-byte slots starting at report byte 2, and packs 16-bit X/Y values
  across the coordinate and pressure nibbles. Linux scales values by `10/16`
  into the DTS extent `10800x24000`; the existing XNU HID service takes panel
  pixels (`1080x2400`), so a Munch parser must normalize to those pixels.
- Confirmed the existing `sweet_touch_init()` is reached only from
  `sweet_ufs_init()` in the SWEET UFS-root path. Munch RAM-root cannot depend
  on that path; a future Munch driver needs an independent init hook. The
  existing SWEET touch implementation targets a different SE/I²C controller
  and must not be reused for Munch's GENI SE4 SPI wiring.
- Munch DTS confirms SE4 at `0x00990000`, CS0, maximum 6 MHz, reset GPIO 33,
  interrupt GPIO 39, and L10C/L1C supplies. Linux's `setup_fifo_xfer()`
  explicitly selects DMA for transfers larger than computed FIFO capacity.
  Linux also has PIO IRQ handlers that refill TX and drain RX across FIFO
  windows; an XNU polling equivalent may avoid DMA, but that is only a design
  option until command setup, timing, and overflow handling are implemented
  and tested. Existing notes identify GCC SE4 source/branch registers, but do
  not establish the runtime controller, clock, pinmux, or rail state inherited
  by the RAM boot chain. Do not probe the controller or alter GPIO, clocks,
  rails, or reset until a separately gated Munch-only implementation is
  reviewed; no firmware commands or controller transfers were issued.
- Added `xnu/patches/0085-munch-focaltech-report-parser.patch`: it stages a
  pure frame/CRC/contact parser as BSD source but performs no MMIO, GPIO,
  clock, rail, reset, or SPI operation and is not wired to runtime init yet.
  The patch applies and reverse-checks on the local diagnostic post-0084 XNU
  staging tree. Clang syntax check passed with `-std=c11 -Wall -Wextra -Werror
  -Wconversion`. Temporary host vectors passed CRC (`123456789` -> `0x6f91`),
  contact ID/coordinates, touch-up, invalid length/status/CRC, and null input.
  The test harness stayed in `/tmp`; the repository has no established test
  suite to extend.
- No ADB or fastboot command was run, and no partition, `userdata`, `dtbo`, or
  persistent firmware was accessed. A strict Apple Clang/Xcode/KDK build is
  still unavailable on this Linux host (`xcodebuild` and `xcrun` are absent);
  Docker cannot supply that Apple toolchain by itself.
- Next: audit the GENI SPI initialization and clock/pinmux handoff, then
  implement an explicit-opt-in Munch SE4 transport with bounded polling PIO
  or DMA. Keep it independent of UFS, and keep phone testing blocked until
  strict XNU and valid RAM payload gates pass.

### >>> HANDOFF 2026-10-21: MUNCH SHARED IOKIT PATCH 0084 <<<

- Regenerated `xnu/patches/0084-munch-shared-iokit-services.patch` from the
  exact post-`0083` XNU sources; the former draft had stale hunks and matched
  the wrong `sweet,display` personality.
- `MUNCH` now compiles the shared AppleKeyStore, IOWatchdog, IOSurface and
  framebuffer implementations, publishes the two shared HID services, and
  matches the display node as `munch,display`. Donor MMIO/UFS/PMIC/touch/USB
  drivers remain SWEET-only and are not enabled by this patch.
- The patch applies cleanly to the diagnostic post-`0083` tree and reverse
  checks cleanly after application. `python3 tools/validate-repo.py` passes
  with 84 contiguous patches; `git diff --check` passes.
- Host syntax checks pass for `SweetKeyStore.cpp`, `SweetWatchdog.cpp`,
  `SweetSurface.cpp`, `SweetFramebuffer.cpp` and `KernelConfigTables.cpp`
  under the MUNCH flags. A standalone `serial_console.c` check is blocked by
  the existing incomplete Linux include staging (`cpumap_t`, `mpqueue_head_t`
  and related machine declarations), not by the new guard diff.
- The incremental XNU `make` target was also attempted in
  `/tmp/ios-munch-local-build-20261021/build-0084-iokit.log`; the Linux fake
  Clang profile's `-Weverything -Werror` turns Darwin/XNU header diagnostics
  into a large cascading failure. This does not invalidate the five relaxed
  syntax checks; Apple Clang/Xcode remains authoritative for the real build.
- No ADB, fastboot, device, `userdata`, `dtbo` or persistent firmware action
  occurred. Changes are committed as `dd89684`; the next gate is a strict
  Apple Clang/Xcode/KDK build before any RAM-only phone boot.

### >>> HANDOFF 2026-10-21: LOCAL XNU MACH-O LINK GATE <<<

- No ADB, fastboot, device command, partition access, `userdata` access,
  `dtbo` access, or permanent firmware change occurred in this milestone.
  All temporary build files remain under `/tmp/ios-munch-local-build-20261021`.
- The incremental Linux MUNCH smoke-build advanced past BSD, libkern, libsa,
  IOKit and symbolset generation after temporary-only DriverKit/iig header
  fallbacks and a `plistutil`-backed `plutil` wrapper. These fallbacks are
  diagnostic host scaffolding only and are not repository patches or boot
  inputs.
- Strict Darwin-style link with `ld64.lld` reached the real Apple SDK boundary:
  `/tmp/ios-munch-local-build-20261021/build-all-munch-28.log` reports missing
  `libcc_kext` and `libfirehose_kernel`, followed by unresolved platform and
  IOKit symbols such as `_IOMallocData_external` and `__ZN9IOService18getResourceServiceEv`.
- A separate relaxed link (`-undefined suppress`, no Apple support libraries)
  produced only an offline diagnostic artifact:
  `/tmp/ios-munch-local-build-20261021/build/obj/RELEASE_ARM64_MUNCH/kernel.release.munch.unstripped`
  is Mach-O 64-bit arm64, 12,438,432 bytes, SHA-256
  `a5df6330d91d0d08e55e5c00466fad03754cbea25810c8ace191d298135e8750`.
  `llvm-nm -u` confirms unresolved Apple platform/IOKit symbols. It is not a
  bootable XNU kernel and was not passed to the shim or sent to the phone.
- The relaxed `build_all` then stopped only at the secondary Darwin `libtool`
  archive command (`libtool -static -csD`), recorded in
  `/tmp/ios-munch-local-build-20261021/build-all-munch-30.log`. The kernel
  link itself completed, but the result remains invalid for boot.
- Offline payload packing was also rejected as intended:
  `python3 tools/mkpayload.py` stopped on
  `AssertionError: kernel has binds; only rebases are supported`. The relaxed
  Mach-O therefore cannot pass the repository's payload gate even before any
  shim or device test.
- Current repository tip remains `77e19c5` (`fix(munch): generate pid
  hibernate syscall args`), with the branch two commits ahead of
  `origin/main`. No repository source changes were needed for this smoke-build.
- Next required build gate is a real Apple Clang/Xcode/KDK environment (or
  macOS CI) providing `iig`, `libcc_kext`, `libfirehose_kernel`, Apple linker
  semantics and platform headers. Do not use the relaxed Mach-O for
  `fastboot boot`; keep phone testing RAM-only and only after a strict Apple
  kernel plus valid payload exists.

### >>> HANDOFF 2026-10-21: LINUX XNU SMOKE-BUILD / DOCKER DECISION <<<

- No device command, ADB, fastboot, partition access, `userdata` access, or
  permanent firmware change occurred in this milestone. All experiments remain
  in `/tmp/ios-munch-local-build-20261021`; repository changes are unchanged.
- A temporary Clang wrapper converts XNU's Darwin `-arch arm64` into
  `--target=arm64-apple-darwin`; a one-file test produced a real Mach-O 64-bit
  arm64 object. The full patched MUNCH tree then compiled deeply with temporary
  Linux-only fallbacks for Apple typed-allocator builtins and Firehose headers.
- The current smoke-build blockers are not Docker/environment blockers:
  DriverKit still requires Apple `iig`, and BSD compilation reaches a real
  XNU source/configuration error at `bsd/vm/vm_unix.c:1774` for an incomplete
  `struct pid_hibernate_args`. The temporary DriverKit exclusion was not copied
  into the repository.
- Docker would only package this same Linux approximation; it cannot provide
  Apple Clang's XNU builtins, `iig`, Xcode/KDK headers, or Apple's Mach-O/linker
  behavior. It is therefore useful later for reproducible helper scripts, but
  not a simpler route to a bootable kernel. The final Apple-toolchain build
  still belongs on macOS/Xcode or a macOS CI runner.
- Next practical step is to keep the Linux build as a diagnostic path, isolate
  the `pid_hibernate_args` dependency and remaining non-Apple blockers, then
  use a real Apple build for the first bootable XNU artifact. No XNU kernel or
  phone boot image was produced in this milestone.

### >>> HANDOFF 2026-10-21: MACH-O PAYLOAD PACKER GATE <<<

- `tools/mkpayload.py` incorrectly assumed `m.segs[0]` was `__TEXT`. A valid
  Mach-O from `tools/mkinit.py` starts with `__PAGEZERO`, so packing failed
  before this fix. The packer now locates `__TEXT` by name for both the
  segment-range check and chained-fixup image base.
- Offline gate passed: Clang assembled `tools/munch-init.S` for arm64,
  `tools/mkinit.py` produced `/tmp/munch-init.macho` (16,635 bytes), and
  `tools/mkpayload.py` produced `/tmp/munch-init-payload.bin` (28,128 bytes)
  from `/tmp/munch-munch.adt` (10,692 bytes, explicit candidate
  `[0x100000000,0x200000000)`). Payload SHA-256 is
  `4ee0765345b9d18be2e15c1dbb910d6f89580b746c0ec58d1aaec0c6819a1821`.
- A synthetic shim boot image was packed and unpacked successfully; the
  ramdisk matched byte-for-byte and the image was not sent to the phone.
  `munch-init` is a userspace test executable, not an XNU kernel, so it must
  never be used for `fastboot boot`. No device command or `userdata` access
  occurred in this gate.

### >>> HANDOFF 2026-10-21: RAM-ONLY SHIM LOCAL BUILD <<<

- Fixed `shim/build.sh`: it compiled `fdt_memory.c` but omitted
  `out/fdt_memory.o` from the linker command, causing an undefined
  `fdt_memory_range_usable` symbol. The relocation scan now covers that object
  too.
- Added `DTB_PATH` override while preserving the historical default
  `../dtb/boot_unpacked/dtb`; this lets local builds use a captured DTB in
  `/tmp` without putting a device dump in the repository.
- Successful local command:
  `cd shim && DTB_PATH=/tmp/munch-live.dtb ./build.sh`. The host FDT/console
  test passed. Outputs are ignored build artifacts:
  `shim/out/Image` (151,552 bytes, SHA-256
  `a5e82d5a0267a89445e55dc7cfe5b5147925a487af08b2503c770ee448bc7478`) and
  `shim/out/shim-boot.img` (974,848 bytes, SHA-256
  `203a9b9a0943c56e57084953af97b36a0eeae87e85502b267c4a6e479f6603b2`).
- The image contains the empty diagnostic cpio ramdisk, not an XNU payload;
  it was not sent to the phone. No device command or `userdata` access occurred.
- Source fix is committed locally as `9eb6f41`; the follow-up documentation
  syntax correction is `eb170b1`. Neither commit was pushed to `origin/main`.

### >>> HANDOFF 2026-10-21: UEFI IDENTITY BOOT + FRAMEBUFFER CHECK <<<

- The embedded UEFI DTB was corrected to match the live phone identity:
  `model = "Qualcomm Technologies, Inc. xiaomi munch"` and
  `qcom,board-id = <0x32 0x00>`. The change is pushed to the user fork as
  `a8b755c` (`fix(munch): match live device identity in embedded DTB`). The
  fork still has only the expected dirty `tools/Installer` submodule.
- Canonical rebuild from `a8b755c` completed locally with GCC 14 host tools:
  `/tmp/edk2-munch-a8b755c-build/boot-munch.img`, 1,536,000 bytes, SHA-256
  `6e0c8ade40a00cb5bf268169cd015b383b753494d1c8678f48fa8378dfeabece`.
  The generated `PcdFirmwareVersionString` is `a8b755c`. Embedded DTB SHA-256 is
  `66f1d179b219585dc4a2bfcf53a858cd8f141b4002d4dfb1599a1a8fdaa80981`.
- Two tests used only temporary `fastboot boot`; no `fastboot flash`, erase,
  partition read/write, `userdata` access, or `dtbo` modification occurred.
  The first boot returned fastboot in about 6 seconds and ADB in about 26
  seconds. The second run was started at `01:32:44 +05:00`; Android again
  returned. Host logs are `/tmp/munch-uefi-identity-boot-20261021-013117.log`
  and `/tmp/munch-uefi-identity-boot-20261021-013229-manual.log`.
- After both tests Android reported `ro.product.device=munch`,
  `androidboot.dtbo_idx=0`, and `androidboot.dtb_idx=0`; the phone is currently
  in Android on slot `_b`. `/sys/fs/pstore` is empty, so old UEFI framebuffer
  text cannot be recovered after reboot.
- The supplied frames visibly contain readable white early-UEFI text, but are
  too blurred for a reliable line-by-line transcription. This is not evidence
  of an XNU panic. The current framebuffer setup matches the live display
  reservation: base `0x9c000000`, `1080x2400`, 32 bpp, and reserved range
  `0x9c000000+0x2300000`. Do not change `dtbo` or framebuffer stride based on
  the camera artifact alone.
- Next gate: preserve the identity-fixed UEFI commit, add a short explicit
  memory-map marker if another RAM-only boot is needed, then validate the
  MUNCH RAM-root/XNU handoff. Do not start XNU or UFS until UEFI memory-map,
  framebuffer handoff, and a storage-free payload are independently verified.

### >>> HANDOFF 2026-10-21: LOCAL RAM-ONLY UEFI BUILD <<<

- The GitHub Actions XNU workflow was deleted locally from
  `.github/workflows/xnu.yml`; it is intentionally not pushed because the
  current GitHub token lacks the `workflow` scope. No device command,
  `fastboot`, partition read/write, or `userdata` access occurred.
- The Munch UEFI fork is `danya2271/edk2-msm-munch`, current pushed commit
  `3bb06c2` (`Fix RAM-only Munch UEFI build`). It adds the missing RAM-only FDF
  guard around `FvSimpleFileSystemDxe` and a valid `TRUE` DXE dependency for
  `MunchMemoryMapDxe`.
- Exact successful local command from that commit:
  `BUILD_CC=gcc-14 BUILD_CXX=g++-14
  EXTRA_OPTFLAGS=-Wno-error=unused-but-set-variable ./build.sh --device munch
  --release RELEASE --outputdir /tmp/edk2-munch-build`.
  Build target is `RELEASE`, EDK2 toolchain is `CLANG38`, and the build
  completed on 2026-10-21 in 38 seconds after dependencies were initialized.
- Artifacts are outside Git in `/tmp`: `boot-munch.img` is 1,536,000 bytes,
  SHA-256 `dcddcc2f3898257d8c50ceb8e12df0a9a29374c661f0a7d256b6fa3f718d7da5`;
  the raw `SM8250_UEFI.fd` is 7,340,032 bytes, SHA-256
  `a9ee2fddf8b5a7807a847f246e3e265842ec1da6e16296313699be901ce621bd`.
  The build embeds `FIRMWARE_VER=3bb06c2` and includes `MunchMemoryMapDxe`.
- The RAM-only build log contains no `UFSDxe`, FAT/GPT, USB mass-storage,
  Linux mass-storage, or Shell module. This is a build-time module boundary,
  not proof that unrelated firmware or a future payload cannot access storage.
- Observed build blockers and resolutions: GCC 16 failed on legacy BaseTools
  warnings and Pccts function-pointer rules, so host tools use installed
  GCC 14 plus a narrow warning override; `--skip-rootfs-gen` failed because
  `SimpleInitRootFS.inf` needs generated `build/rootfs.c`; the nested FreeType
  submodule was initialized at its pinned commit. These are host-build facts,
  not phone-runtime results.
- `xnu/` still contains the patched source series only. The removed workflow
  depended on macOS/Xcode/KDK/iig; `tools/xnu-cc.py` replays commands from a
  CI buildtree and is not a standalone Linux XNU builder. No XNU kernel was
  produced in this milestone. Next work remains local shim/XNU payload
  validation and only then a reviewed temporary `fastboot boot` test.

### >>> HANDOFF 2026-10-20: LIVE FDT + MUNCH UEFI INVENTORY <<<

- The phone remains in Android, connected over ADB as `munch`, root via `su`;
  no reboot, fastboot command, partition read, `userdata` access, or device
  write was performed in this milestone.
- A root read-only capture of `/sys/firmware/fdt` succeeded on the current
  boot. The FDT is v17 and 812,778 bytes; it reports memory tuples
  `<0x80000000 0x3bb00000>` and `<0xc0000000 0x1c0000000>`, with the second
  bank ending at `0x280000000`. The capture stays in `/tmp` and is not tracked.
- The public device-specific UEFI tree inspected is
  `0xSelenicDove/edk2-msm-munch`, at source commit `d4b3760`. Its bundled
  `munch.dtb` has the same two memory tuples and its Munch memory-map library
  exposes a conventional RAM descriptor `0xffc20000+0x1003e0000`, ending at
  `0x200000000`. The 4-8 GiB candidate `[0x100000000,0x200000000)` is
  therefore supported by both the live Android FDT and the published UEFI
  source map, but the UEFI runtime map is still not measured on this phone.
- Static inspection of the UEFI release `boot-munch.img` found Android boot
  header v1 and a 3,946,361-byte UEFI payload. It was downloaded to `/tmp`
  only and never booted. It is not yet integrated with the XNU shim.
- Added `tools/munch-fdt-pull.sh`: it accepts only a Munch ADB device, reads
  only `/sys/firmware/fdt` through `su`, validates the blob, and refuses
  repository paths. The validator now checks this helper's read-only boundary.
- Milestone committed as `bd57b48` (`Add safe live Munch FDT capture`).
  `python3 tools/validate-repo.py`, `git diff --check`, `bash -n`/`sh -n` for
  the helper, and the real helper capture all passed. Host FDT checks accepted
  `[0x100000000,0x200000000)` and rejected the framebuffer and an out-of-RAM
  range. No CI run was started; `gh` is still unavailable.
- Offline `tools/mkadt.py --munch --dram-base 0x100000000
  --dram-size 0x100000000` generated a 10,692-byte ADT in `/tmp` with no UFS
  node. It warned only that the optional Apple D22AP `/product` artifact is
  absent; no kernel, payload, boot image, or phone boot was produced.
- The user forked the UEFI tree as `danya2271/edk2-msm-munch`. I added and
  pushed commit `cb48979` (`Add RAM-only Munch UEFI profile`): the `munch`
  configuration now omits UFS, SCSI, GPT/partition, filesystem, USB
  mass-storage, Linux mass-storage and Shell components, while retaining the
  Munch DTB and platform firmware paths. This is a source change only; it has
  not been built or booted.
- The fork's existing Actions workflow is visible but has zero runs as of
  2026-10-21. The attempted workflow hardening commit was not pushed because
  GitHub rejected the token without the `workflow` OAuth scope. No installer,
  release, device command, UEFI boot, partition access, or `userdata` access
  occurred. Local `bash -n`, YAML parsing, and conditional storage scans pass;
  `actionlint` is unavailable and the EDK2 submodules were not initialized.
- Next: run the fork's build workflow from GitHub Actions (or provide a token
  with `workflow` scope), then inspect the UEFI artifact statically. Separately
  get the MUNCH XNU compile through CI and keep all phone tests on temporary
  `fastboot boot` with RAM-only payloads.

### >>> HANDOFF 2026-10-20: RAM-ONLY STORAGE ACCESS AUDIT <<<

- Disabled `tools/munch-log-pull.sh` as a fail-closed stub. `--help` explains
  that it never contacts ADB; all other invocations exit 1 without device
  commands. `tools/munch-log.py` is documented as an offline parser only.
- Audited the pre-XNU shim path: `shim/build.sh` compiles and links an explicit
  source list that excludes `shim/ufs.c`; neither `shim.c` nor `loader.c` calls
  `test_ufs()`. If the RAM payload is absent, the loader reports failure and
  the shim halts rather than falling back to UFS.
- The excluded legacy `shim/ufs.c` probe does program UFSHCI registers and
  submits SCSI reads for LBA 0/GPT, then reads filesystem signatures from
  `userdata`, `cust`, and `cache`. Source inspection found no SCSI WRITE
  command. Added a compile-time opt-in guard so accidental inclusion in a
  RAM-only build fails; the repository validator now checks this guard, source
  list exclusion, and absence of entrypoint calls. These are static checks,
  not runtime guarantees about unrelated firmware behavior.
- `python3 tools/validate-repo.py`, `git diff --check`, and local helper checks
  passed. The pull helper's help and disabled paths were exercised without
  ADB; no ADB/fastboot command, phone boot, or `userdata` access occurred.
- No `gh` binary is installed. A public GitHub Actions API check on 2026-10-20
  found zero workflow runs for this repository; no CI was started and nothing
  was pushed. Current local commit before this handoff: `bf10b91`.
- Next: get a MUNCH CI compile through the web UI or a host with authenticated
  `gh`, then continue runtime UEFI memory-map and RAM-root feasibility work.
  Any phone test remains temporary `fastboot boot` only and must not touch
  `userdata`; do not enable the legacy UFS probe.

### >>> HANDOFF 2026-10-20: MUNCH MACHINE CONFIG MILESTONE <<<

- Completed the isolated MUNCH build target in patch `0083`: plain `arm64`,
  `MUNCH.h`, `MASTER.arm64.munch`, `xiaomi,munch` platform matching,
  `qcom,kryo` boot-CPU matching, 16K pages, GICv3 and virtual timer INTID 27.
  MUNCH reuses only the RAM-root/crypto/trust-cache/early-debug plumbing needed
  for bring-up; inherited SWEET touch, PMIC/SPMI, UFS, USB, watchdog and
  battery paths remain excluded.
- Fixed the adjacent IOKit trace patch so the generated source has one valid
  `static` declaration, and extended dependency staging for MUNCH's built-in
  libpthread, BearSSL and HFS inputs. The workflow now accepts
  `machine_config=MUNCH` without changing the SWEET default.
- Clean validation on upstream `xnu-8796.141.3` commit
  `1b191cb58250d0705d8a51287127505aa4bc0789`: pinned dependency staging and
  all 83 patches applied in order; final `git diff --check` passed. Local
  `python3 tools/validate-repo.py` also passed: 83 patches, 16 Python tools and
  7 shell scripts.
- This milestone is source/patch-apply validation only. No macOS compile, GitHub
  Actions run, payload, ADB/fastboot command, phone boot, or `userdata` access
  occurred. `gh` is unavailable on this host, so the next build gate is a
  manual GitHub Actions run or an environment with `gh`.
- Do not implement Munch SE4 SPI touch, PMIC, UFS, SMP or phone boot yet. First
  confirm the MUNCH target on CI, then continue the separately gated runtime
  UEFI memory-map probe and RAM payload design.

### >>> HANDOFF 2026-10-20: MUNCH CPU MATCH / TOUCH AUDIT <<<

- Local Munch DTS review confirms the eight CPU nodes use
  `compatible = "qcom,kryo"` with MPIDRs `0x000` through `0x700`. Cores 0-3,
  4-6, and 7 are in frequency domains 0, 1, and 2, with capacities 448, 1023,
  and 1024. The generated Munch ADT already carries these eight nodes and the
  current XNU SMP patch accepts the `0x000..0x700` MPIDR range.
- Corrected an ADT timer binding gap found during the machine-config audit:
  Kona DTS declares the architected virtual timer as `GIC_PPI 11`, whose full
  GIC INTID is `16 + 11 = 27`; the Munch ADT generator had incorrectly reused
  SWEET's donor value `19`. `tools/mkadt.py` now emits Munch/QEMU `27` while
  preserving SWEET's `19`. This is an offline generator fix; no device boot
  was run.
- Found and fixed an actual binding gap: `SweetCPU` previously matched only
  `qcom,kryo470`, while the Munch ADT and live kernel DTS use `qcom,kryo`.
  New patch `0082-munch-kryo-cpu-match.patch` adds a second IOKit personality;
  the inherited SWEET match remains unchanged.
- Re-audited Munch touch facts from the local kernel: FT3658U is on QUPv3
  GENI SE4 at `0x990000`, CS0, up to 6 MHz, IRQ GPIO 39, reset GPIO 33. The
  Linux driver uses a 4-byte SPI header, 3 dummy bytes, three retries, and
  CRC-16/Kermit reads. Its automatic firmware-upgrade path writes controller
  nonvolatile firmware and is explicitly excluded from the XNU RAM bring-up;
  the first XNU touch implementation must be read-only probe/report handling.
- The prior `mkadt.py` / porting-notes milestone distinguishes explicit offline
  RAM candidates from the direct Android-ABL runtime gate. The shim's FDT
  preflight, not the unverified public UEFI source map, remains the required
  memory safety check for a future direct `fastboot boot` payload.
- Validation before patch `0083`: `python3 tools/validate-repo.py` passed with
  82 contiguous XNU patches; generated a 10,692-byte Munch ADT for offline
  inspection with
  `[0x100000000,0x200000000)` and confirmed all eight `qcom,kryo` nodes;
  `git diff --check` passed. This is source/offline validation only.
- Deeper source audit of `spi-geni-qcom.c` and `focaltech_3658u/focaltech_spi.c`
  corrected the proposed next step: the existing XNU touch transport is for
  SWEET SE7 I2C and must not be enabled on Munch. Linux report reads use one
  full-duplex transfer with `[0x01, 0xa0, length_hi, length_lo]`, three dummy
  bytes, report bytes, and a little-endian CRC-16/Kermit (init `0xffff`,
  reflected polynomial `0x8408`). Corrected length accounting: Linux's local
  `FTS_TOUCH_DATA_LEN` is 63 bytes including the `0x01` command marker; the
  controller's touch section is 62 bytes (two leading bytes plus ten 6-byte
  slots). Normal Linux reads request 90 response bytes (62 touch + 28 gesture),
  so the full SPI transaction is 99 bytes including header, three dummy bytes,
  and two CRC bytes. A shorter 62-byte read is not yet validated. Vendor report
  parsing scales 12-bit coordinates by `10/16` for Munch's `10800x24000`
  display extent. The boot-ID fallback, reset, mode/config and firmware-upgrade
  paths issue controller/GPIO writes; the firmware path can rewrite
  nonvolatile firmware. They are excluded from any XNU probe. Report protocol
  and the no-write boundary are documented in `docs/MUNCH_PORTING.md`.
- Read-only Android ADB snapshot on 2026-10-20: platform device `990000.spi`
  is bound to `spi_geni`, and SPI child `spi1.0` is bound to `fts_ts`.
  `/proc/interrupts` exposes SPI GENI lines, but the snapshot does not safely
  identify SE4's runtime FIFO/GSI selection; clock and pinctrl debugfs views
  are unavailable, and the sysfs scan exposed no useful named L10C/L1C rail
  state. The `devmem` utility exists, but it was not run and `/dev/mem` was not
  opened. This is Android-only evidence, not ABL handoff evidence. Queries read
  only metadata in `/sys`, `/proc`, and `/dev`; they did not read, mount, back
  up, or modify `userdata`. No SPI transfer or controller probe was issued.
- Critical unresolved dependency: the Linux SPI master chooses FIFO or GSI/DMA
  using `GENI_IF_FIFO_DISABLE`; otherwise it uses the FIFO path. In either case,
  GENI setup and transfers program controller registers; Linux also votes clocks
  and selects pinctrl states through resource frameworks and runtime PM. Static
  DTS/source and the Android sysfs snapshot cannot establish whether ABL leaves
  SE4 at `0x990000` usable, powered, clocked, SPI-configured, or available to
  XNU. Panel rail/reset state is also unknown after ABL. Do not implement or
  call an SE4 MMIO transport until runtime state and exclusive ownership are
  established. The new `MUNCH` machine configuration still must not enable the
  inherited SWEET touch/keys code; those paths remain donor-only and the SE4
  transport is not implemented.
- Machine-config inventory showed that MUNCH could not be enabled by selecting
  another workflow string alone. That gap is now addressed by patch `0083`:
  it adds `MUNCH.h`, `MASTER.arm64.munch`, the `MUNCH` compiler target, a
  `xiaomi,munch` platform personality and a `qcom,kryo` boot-CPU personality.
  Shared RAM-root, crypto, trust-cache and early-debug paths are enabled for
  MUNCH; donor I2C touch, PMIC/SPMI, UFS/logging, USB and watchdog/battery paths
  remain SWEET-only. The Munch CPU driver deliberately matches only CPU0 and
  reports one selected CPU; it does not start secondaries. Never alias MUNCH to
  `SWEET` or `ARM64_BOARD_CONFIG_SWEET`.
- The Kona DTS statically confirms eight `qcom,kryo` nodes, PSCI via SMC,
  GICv3, and vtimer PPI 11 / INTID 27; it does not confirm ABL's runtime EL,
  actual MPIDR handoff, GIC security/group state, or PSCI CPU_ON behavior. The
  DTS capacity groups are 4+3+1, so the inherited eight-core topology is not a
  validated final Munch SMP implementation. Details and guard inventory are in
  `docs/MUNCH_PORTING.md`.
- Munch machine-config milestone is source/patch-apply validation only. A clean
  xnu-8796.141.3 tree accepted all 83 patches after pinned dependency staging;
  no macOS compile, CI run, payload, device command, or `userdata` operation
  was performed. `gh` is not installed on this host. The final validation is
  recorded in the newer handoff above.
- Audit/documentation milestone is committed as `77352b3` (`docs: audit Munch
  XNU config boundary`). The isolated MUNCH machine config is now present, but
  the ABL EL/MPIDR/GIC/PSCI behavior and RAM-root/memory-map gates remain
  independently unresolved; do not start SMP or a device boot.
- Completed commits: `67ff9bf` touch safety audit, `489d55d` corrected report
  lengths, `f0dc497` audit handoff, and `8be542d` Munch timer INTID. Validation
  after the latest code change: `python3 tools/validate-repo.py` passed (82
  patches, 16 Python tools, 7 shell scripts); `git diff --check` passed.
  Generated a 10,692-byte explicit-candidate Munch ADT and parsed
  `virtual-timer-intid=27`; generator warned that the optional local D22AP
  product ADT is absent, so `/product` was omitted. No XNU CI, payload build,
  fastboot, phone boot, panel reset, UFS, or `userdata` operation occurred.
  Android ADB observations above were metadata-only and read-only.
- Next: run the MUNCH compile gate through GitHub Actions, then continue the
  runtime UEFI memory-map and RAM-root work without changing SWEET behavior or
  enabling board-specific drivers.

### >>> HANDOFF 2026-10-20: ABL-FDT MEMORY PREFLIGHT <<<

- Public UEFI release re-check: the latest available GitHub release remains
  `munch-uefi-d4b3760e83ed094230d2e34025cfdf1247d8e19f`, published August 20,
  2026. Its `boot-munch.img` is 3,950,592 bytes with SHA-256
  `a1d96e148becbcaec21d6e2f902ac0b9156c90719b6d94e28ed062406ec8e4aa`.
  The image is an Android boot header v1 containing an EDK2-MSM trampoline
  and gzip-compressed EDK2 payload. It was downloaded to `/tmp` for static
  inspection only; it was not sent to the phone.
- The released UEFI's source build includes `UFSDxe`, block/partition/FAT
  drivers, `VariableRuntimeDxe`, Shell and `EfiBootManagerConnectAll()`. Its
  platform boot manager explicitly connects every `BlockIo` handle and scans
  all filesystems for Windows/Linux boot files. Therefore this stock image is
  not an acceptable current test image under the user's rule that `userdata`
  must not be accessed; no UEFI boot was attempted.
- The backup directory's inventory is internally consistent: all 116 rows in
  its `partitions.tsv` have a matching manifest entry and `.img` file, with no
  `userdata` row/file; sizes match and every SHA-256 verifies. The two
  `ALIGN_TO_128K_*` entries are included. Hashing read file bytes, but no backup
  was mounted or semantically parsed. This is not an independent comparison
  against the phone's current GPT, so backup completeness is not certified.
- Added `shim/fdt_memory.c` and loader integration. Before copying XNU, the
  shim now requires the payload RAM window to be inside an ABL `/memory` node,
  outside the FDT reservation map and fixed/dynamic `/reserved-memory` ranges,
  and inside `linux,usable-memory-range` when supplied. This is a fail-closed
  Android-ABL safety check, not a UEFI runtime-map proof.
- Host tests on `research/munch-live.dtb`: `[0x100000000,0x200000000)` was
  accepted; `0x9c000000`, the low bank `[0x80000000,0x100000000)`, an address
  above `0x280000000`, and an overflowing range were rejected. ARM64 compile
  and link passed; all six C objects have zero `ABS64` relocations (`start.o`
  is intentionally excluded by `build.sh`). Repository validation and
  `git diff --check` passed. No ADB, fastboot, phone boot, or userdata access
  occurred.
- Completed change is committed as `ea46c23` (`Guard Munch XNU RAM range
  against ABL FDT`). Remaining blocker is unchanged:
  obtain a separately reviewed way to observe the actual runtime UEFI memory
  map, then create a Munch-specific ADT/profile. Do not use the accepted
  source-derived candidate to build or boot a payload yet.

### >>> HANDOFF 2026-10-20: RAM ROOT FEASIBILITY / PACKER GUARDS <<<

- User clarified that a later UFS boot may be useful, but the current device
  phase is RAM-only because there is no room to back up user data. `userdata`
  must not be read, backed up, mounted, or written. Do not flash; any future
  device test remains temporary `fastboot boot` only.
- Static XNU patch-path inspection: `sweet_ufs_rootdev()` is selected only for
  `rd=ufsroot0`; persistent log flushing starts only after UFS initialization
  finds a writable partition. A RAM boot with `rd=md0` and an ADT without UFS
  does not enter those paths. This is a source-code path analysis, not runtime
  proof that every unrelated kernel/userspace component is storage-free.
- Root feasibility blocker: `tools/mkiosroot.sh` defaults to a 16-GB raw HFS+
  image. `tools/mkpayload.py` embeds the entire RAM disk as an uncompressed
  resident segment, so this default image cannot fit the source-derived 4-GB
  candidate RAM window. The candidate window is not verified, and no iOS root
  image exists locally under `research/`; a smaller or demand-paged root design
  is unresolved.
- Updated `mkpayload.py` to reject oversized RAM disks from file size before
  reading their contents, and to reject empty RAM disks, conflicting root
  selectors, UFS-bearing ADTs, and overlong boot arguments. README and porting
  notes now distinguish the unvalidated Munch RAM design, historical SWEET
  UFS workflow, and possible future UFS work.
- Validation on 2026-10-20: `python3 tools/validate-repo.py` passed (81 XNU
  patches, 16 Python tools, 7 shell scripts); `git diff --check` passed.
  Synthetic packer tests accepted a valid RAM payload and rejected empty root,
  conflicting `rd=` selectors, a UFS ADT, overlong boot args, and an oversized
  sparse RAM disk before opening it. No XNU build, CI, ADB/fastboot command,
  phone boot, or handset storage access was performed.
- Milestone committed as `b11ba39` (`Constrain Munch bring-up to RAM-only
  path`). Existing `shim/out` artifacts and external non-userdata backups were
  preserved. Next: establish a realistic
  RAM-root footprint/design and verify the XNU RAM-disk path offline; only then
  resume Munch memory/loader integration. Do not use the unverified 4-8 GiB
  range or create a phone payload yet.
- Host-side preflight: no Munch UEFI image was found in the local home tree;
  `/usr/bin/fastboot` exists. The external backup directory lists 116 `.img`
  entries, including two `ALIGN_TO_128K_*` auxiliaries, and has no
  `userdata.img` filename. Image contents were not opened; do not treat backup
  completeness as re-verified for a device boot. Resolve its inventory and
  obtain the exact temporary UEFI image before considering a runtime-map probe.

### >>> HANDOFF 2026-10-20: MUNCH MEMORY / TOUCH PORT RESEARCH <<<

**Target identity:** `munch` is the POCO F4 / SM8250 target. `sweet` is the
Redmi Note 10 Pro / SM7150 donor platform. Existing `SWEET` XNU identifiers in
the patch series name the donor configuration; they are not Munch hardware
support.

- UEFI inventory via GitHub API found one device-specific project and two
  public image releases, both published 2026-08-20: `v1.1.0` (source tag
  `1b11fb1`, `boot-munch.img` 3,952,640 bytes) and later build
  `munch-uefi-d4b3760e83ed094230d2e34025cfdf1247d8e19f` (source `d4b3760`,
  image 3,950,592 bytes). Neither was tested on this phone. The newer source
  still bundles a compatibility DTB whose root identifies as `alioth`.
- Compared the later source's `PlatformMemoryMapLib.c` with live Linux FDT.
  EDK2 describes PSTORE `0xb0000000+0x400000`, display ramdump
  `0xb0400000+0x1000000`, and conventional RAM through
  `0xffc20000+0x1003e0000 = 0x200000000` (8 GiB address). Live Linux advertises
  `0x80000000+0x3bb00000` and `0xc0000000+0x1c0000000`, ending at
  `0x280000000` (10 GiB address). This is a 2 GiB address-span difference;
  it does not establish missing physical capacity or the map of a running UEFI.
- **Hypothesis only:** `[0x100000000,0x200000000)` is wholly inside the later
  EDK2 source's conventional RAM descriptor and avoids its listed low carveouts.
  This is a plausible XNU load region to verify against the handset's runtime
  UEFI memory map, not a boot-ready ADT or payload. Keep `mkadt.py` SWEET
  defaults unchanged and do not emit a Munch ADT from this guess.
- Kernel DTS `/home/danya2271/xiaomi_sm8250_kernel/arch/arm64/boot/dts/vendor/qcom/munch-sm8250.dtsi`
  describes `focaltech,fts` on QUPv3 SE4 SPI, CS0, max 6 MHz, IRQ GPIO 39,
  reset GPIO 33, 10 contacts, display coordinates `10800x24000`, supplies L10C
  and L1C. `munch_defconfig` enables `CONFIG_TOUCHSCREEN_FOCALTECH_3658U=y`;
  `/drivers/input/touchscreen/focaltech_3658u/` is an SPI driver matching this
  compatible, selects FT3658U, and embeds `fw_ft3658_k11.i`. Thus the exact
  model's transport/protocol and report parsing have a local reference. Existing
  donor XNU touch support is QUP I2C and cannot be reused as-is. XNU still needs
  a QUPv3 SE4 SPI master path, GPIO/regulator sequencing, and FT3658U report/event
  integration; whether controller firmware needs loading during this boot path
  remains to be checked.
- No files in the kernel repository were changed; its untracked
  `gcc-compiler/` remains untouched. No device writes, reboot, or fastboot.
- Validation: `python3 tools/validate-repo.py` passed (81 patches, 16 Python
  tools, 7 shell scripts); `git diff --check` and
  `tools/inspect-munch-dtb.py research/munch-live.dtb --expect-munch` passed.
  No XNU build/CI or phone boot was run; `gh` is not installed here.
- Research/docs milestone recorded in commit `b339c7e`
  (`docs: refine Munch memory and touch port research`).
- Next: obtain a read-only runtime UEFI memory map through a separately
  controlled UEFI boot; reconcile it with live FDT, then implement a Munch ADT
  and XNU loader profile. In parallel, map QUPv3 SE4 registers/protocol from
  the kernel sources before attempting a new SPI driver.

### >>> HANDOFF 2026-10-20 (HISTORICAL): LIVE MUNCH DEVICE EVIDENCE <<<

**Read-only Android inspection; no reboot, fastboot, or writes. Only partition links, sizes, and the four-byte boot-header version fields were read; no full partition was dumped.**

- ADB reports `ro.product.device=munch`, `ro.product.model=22021211RG`, active slot `_b`, and `MemTotal=7815544 kB`. `ro.build.version.release=16` / SDK `36`, while the fingerprint is `POCO/munch_global/munch:13/RKQ1.211001.001/V816.0.9.0.ULMMIXM:user/release-keys`; the Android-version fields disagree and need clarification before treating this as a firmware baseline.
- `/sys/firmware/fdt` is absent. Read-only `su -M` access to `/proc/device-tree` was reconstructed as FDT v17, 812030 bytes. Root identity is `Qualcomm Technologies, Inc. xiaomi munch`, compatibles `qcom,kona-mtp; qcom,kona; qcom,mtp`. SHA-256: `7ac4e2551f5d76d1f46d0ef6d53a3246bd5cd498417a466b23e1eb0cf985025c`. The local `research/munch-live.dtb` is ignored by Git and mode 0600; it contains a device serial in `chosen/bootargs`, so never publish or copy it into tracked files.
- The updated inspector passes with `--expect-munch`. The tree reports 8 CPUs (MPIDRs `0x0` through `0x700`, `enable-method=psci`, PSCI via SMC), GICv3 at `0x17a00000` / `0x17a60000`, UFS at `0x1d84000`, SDE display at `0xae00000`, Focaltech touch on SPI at `0x990000`, and RAM ranges `0x80000000+0x3bb00000` and `0xc0000000+0x1c0000000` (end `0x280000000`). This is a reconstructed live Linux tree, not the original DTB blob.
- Critical overlap: SWEET `tools/mkadt.py` uses `PHYS_BASE=0xb0000000`; the public Munch EDK2 map reserves PSTORE at `0xb0000000+0x400000` and display ramdump at `0xb0400000+0x1000000`. Do not build a Munch ADT with the SWEET base. Initial inspection noted that the EDK2 source map ends at `0x200000000` while live Linux RAM extends to `0x280000000`; the newer comparison and unvalidated 4-8 GiB candidate are recorded in the active handoff above.
- Public UEFI search found one device-specific project, `0xSelenicDove/edk2-msm-munch`; its latest release on 2026-10-20 check is `munch-uefi-d4b3760e83ed094230d2e34025cfdf1247d8e19f` (2026-08-20), alongside `v1.1.0`. Source commit `d4b3760` builds, but its bundled compatibility DTB identifies as `alioth`; build/release success is not a Munch phone boot. UEFI image is Android boot header v1; installed `boot_b` and `vendor_boot_b` are header v3. Do not use the installer or infer `fastboot boot` compatibility without a controlled, separately approved test.
- Read-only partition metadata: `boot_b` 201326592 bytes (192 MiB), `vendor_boot_b` 100663296 (96 MiB), `dtbo_b` 33554432 (32 MiB), `userdata` 241382174720 (~224.8 GiB). In addition to by-name links and sizes, only the four-byte header-version fields were read; no full partition was dumped. `userdata`, GPT, boot, recovery, modem, persist, and backups remain untouched.
- Milestone close: `python3 tools/validate-repo.py` passed (81 patches, 16 Python tools, 7 shell scripts); `git diff --check` and `--expect-munch` live-DTB inspection passed. Commit `ce20490` (`docs: record live Munch hardware evidence`) records the new inspector output and evidence. No kernel CI or phone boot was run; `gh` is not installed on this host. No Munch XNU profile/ADT exists yet, and the SWEET XNU RAM base is unsafe for this map.
- Next: establish the actual UEFI/DRAM usable map for this 8-GB device, reconcile the UEFI build's compatibility DTB and memory descriptors, then implement a separate Munch ADT/profile without changing SWEET defaults. Any boot test must use an exact temporary image and preserve Android; do not flash, repartition, or write `userdata` without separate explicit confirmation.

### >>> HANDOFF 2026-10-20 (HISTORICAL): DTB IDENTITY GUARD <<<

- The public EDK2 source has a Munch platform directory and `munch.dsc`, but its bundled `Platform/Xiaomi/sm8250/FdtBlob_compat/munch.dtb` is not a Munch-identifying tree: the examined blob reports model `Qualcomm Technologies, Inc. xiaomi alioth` and compatibles `qcom,kona-mtp; qcom,kona; qcom,mtp`. Do not take its board-level addresses as Munch facts.
- The UEFI DSC sets GOP values to framebuffer `0x9c000000`, 1080x2400. This is a build configuration, not evidence of a successful POCO F4 UEFI boot or proof that a compatibility DTB matches the phone.
- The initial release search found only `v1.1.0`; a later full API inventory on 2026-10-20 also found the same-day build `munch-uefi-d4b3760e83ed094230d2e34025cfdf1247d8e19f` (see active handoff). The `v1.1.0` installer is not a temporary-boot workflow: installer source patches the active `boot` slot and copies the original to `/sdcard/boot_backup<slot>.img`. Do not use it for initial bring-up.
- `tools/inspect-munch-dtb.py` now makes `--expect-munch` fail with exit 2 on root identity mismatch, classifies device nodes using node names/compatibles rather than arbitrary full-path substrings, and limits noisy output. Candidate test: normal inspection exited 0; `--expect-munch` exited 2 and printed the `alioth` identity. DTB/DTS remained in `/tmp`, outside Git.
- Added `docs/MUNCH_PORTING.md` with safe stock/live DTB collection and inspection steps, evidence requirements, and staged port/dual-boot boundaries. No XNU/ADT hardware constants were changed because the only public candidate is mismatched. No phone was connected, booted, flashed, or modified.
- Validation on 2026-10-20: `python3 tools/validate-repo.py` passed (81 patches, 16 Python tools, 7 shell scripts); `git diff --check` passed. Implementation commit: `f613b8e` (`docs: add Munch DTB verification guard`). No kernel CI run was started.
- Next concrete step: obtain the exact POCO F4 variant/firmware's stock or live DTB and read-only partition facts. Continue using temporary `fastboot boot` only after that evidence is reviewed; do not flash UEFI, change GPT, or write `userdata` without explicit confirmation.

### >>> HANDOFF 2026-10-20: MUNCH REBRAND <<<

- The repository is now named and branded for `munch` / POCO F4. This is a source-level rebrand and fork, not a completed SM8250 port.
- The XNU patch payloads and default CI build configuration remain `SWEET` to preserve the known-good Redmi Note 10 Pro baseline. `MUNCH` branding, renamed paths, and the MUNCHXNU payload tag do not make it an SM8250 build.
- Historical successful builds and phone boots in the notes below refer to the upstream `sweet` / Redmi Note 10 Pro / SM7150 device. Keep those observations attached to that hardware; do not present them as `munch` results.
- No device, partition, or UEFI was changed during the rebrand. No POCO F4 boot has been verified.
- Historical path references below are updated to renamed host tools where they refer to files in this repository; external artifact names remain unchanged.
- Rebrand validation on 2026-10-20: `python3 tools/validate-repo.py` passed (81 patches, 15 Python tools, 7 shell scripts); `git apply --stat` parsed all 81 XNU patches; `git diff --check` passed. The ARM64 shim cross-compiled and linked with zero absolute relocations, the host console smoke test passed, and the QEMU helper recognized both inherited SWEET and renamed MUNCH early-panic markers. No XNU kernel build or device boot was run; no hardware was attached or modified.
- Rebrand implementation commit: `555fd62` (`rebrand sweet fork as munch`). No CI run was started because this is a source rebrand, not an SM8250 port.
- Remaining blocker: XNU patches, machine config, staging scripts and device hardware identifiers are still SWEET/SM7150. MUNCH branding and payload format do not imply Poco F4 support. Porting and hardware validation remain unfinished.

### >>> UPDATE 2026-10-20: POCO F4 / UEFI feasibility research <<<

Read-only research; no POCO F4 was connected, booted, or modified.

**Observed:**
- `ios-sweet` still targets `sweet` / SM7150. Latest documented phone milestone is 2026-10-18: SpringBoard home screen, touch, hardware buttons and home/app-switcher gestures work. Eight-core support (6 A55 + 2 A76) is built; latest documented USB internet attempt is not working. README now reflects this baseline: eight cores are built, and the GUI supports home/app-switcher gestures.
- Public device-specific UEFI candidate found: `0xSelenicDove/edk2-msm-munch`, EDK2/SM8250, with release `v1.1.0` (2026-08-20). `munch.dsc` sets a GOP framebuffer at `0x9c000000`, 1080x2400. GitHub Actions run `32507483435` (2026-08-21, commit `a600ef86f32180d5c3b21ec54ed6bdf7f29cb78d`) completed successfully and produced build artifacts. This verifies CI build, not on-device boot.
- Its README claims broad Windows hardware support, but no independent hardware test evidence was found in the inspected public materials; treat those feature claims as unverified. Renegade/edk2-msm is a useful upstream base, not a separately confirmed ready-made `munch` image.
- The candidate installer patches the active Android `boot` slot and writes the result back; it backs up the original as `/sdcard/boot_backup<slot>.img`. Do not install it as part of initial iOS bring-up. Prefer a temporary boot image for first hardware tests, after device-specific validation.
- POCO F4 is `munch`, Snapdragon 870 / SM8250-AC, 4x A77 + 4x A55, Adreno 650, LPDDR5/UFS 3.1; panel is 1080x2400 AMOLED. It is ARM64-compatible in principle, but not a drop-in SM7150 board.

**Assessment (not yet hardware-validated):**
- Reuse the generic XNU 8796/iOS 16 userland and porting techniques; redo board-specific machine config, memory map/ADT, boot handoff, GIC/timer/PSCI, UFS, display and touch/power drivers for SM8250 and the exact `munch` variant. CPU rendering can plausibly reach the current GUI milestone without Adreno support; phone-class GPU, Wi-Fi, modem, audio and camera support are much larger separate projects.
- Dual boot is harder than getting the GUI to boot. Current `ios-sweet` uses the whole `userdata` GPT partition as its HFS+ root, which destroys Android user data. Preserving Android requires a separate iOS root partition or a new file-backed storage path, plus a boot menu/loader path that turns UEFI or the stock boot chain into XNU's `boot_args` + Apple Device Tree handoff. The existing UEFI does not itself solve either requirement.

**Unfinished / next concrete step:**
- No POCO F4 boot result exists. Before any test, identify the exact model/region and installed firmware, obtain its stock boot image and live DT, and inspect GPT/slot layout read-only. Then adapt the shim to the `munch` boot image and test only via `fastboot boot`; keep the Android partitions untouched. Do not flash UEFI, repartition, or write `userdata` without explicit confirmation.
- Repository state at research start: `11da58b` (`main`, clean). This documentation update is the only change; no code or device changes made.

## Agent handoff / active work (2026-10-15, latest)

### >>> UPDATE 2026-10-18 (read first, then the 10-17 handoff below) <<<

**STATE at end of 2026-10-18 (evening):**
- On the phone: userdata = **image v15** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v15.img/.simg`; = v14 + `/private/var/preferences/SystemConfiguration/preferences.plist` with one network service "USB Ethernet": Interface DeviceName ue0 Type Ethernet, IPv4 Manual 192.168.7.2/24 router 192.168.7.1, DNS 1.1.1.1 8.8.8.8, set "Automatic" as CurrentSet). Fallback image v14 (v12/v13 deleted).
- Kernels: **0078** = all 8 cores (`cpus=8`, cpu6/7 = A76, homogeneous SMP; mkadt now emits 8 CPU nodes; ADT `shim/out/sweet-0078hi.adt`); **0079** = display off/on via IOMFB selector 12 kern_RequestPowerChange(state) (fade over 6 vsyncs, then black; verified: power button gives "SWEET FB: display off/on"); **0080** = `SWEET NET: <proc> route add/delete/change ...` and `... add/delete address ...` trace (built, `shim/out/shim-boot-0080s.img`, NOT booted yet).
- **Internet over USB: not working yet.** Host side is ready (temporary iptables: FORWARD accept for 192.168.7.0/24 + MASQUERADE, comment "ios-sweet"; lost on host reboot). With v15 the phone sent no DNS and no outbound traffic (only mDNS/ARP) -> configd probably did not apply the service (or did, without default route/DNS). Next: boot 0080s on v15, watch `SWEET NET` lines (does configd add 192.168.7.2 / default route on ue0?). If configd ignores ue0 (not an IOKit IONetworkInterface; configd 1241.140.6 source in `research/configd-1241`, SCNetworkInterface createInterface needs IONetworkInterface -> controller (IOMACAddress, SelfNamed) -> bus), add IOKit stand-ins and/or rename the ifnet to en*.
- Live log listener (`nc -ukl 5140`) may still be running, writing `logs/live-0078s.txt`.

**LIVE LOG over USB (0077, boot 0077s): no more recovery round-trips.** Boot an image with `sweetusb=2`, run `tools/munch-usbnet.sh logs/live-NAME.txt` on the PC: it waits for the phone's CDC-ECM interface, sets 192.168.7.1/24 (sudo) and prints/saves the kernel console the phone streams every 100 ms as UDP to 192.168.7.1:5140 (starts with the last 64 KiB). `ping 192.168.7.2` checks the link. Current image: `shim/out/shim-boot-0077s.img` (kernel 0077, ADT sweet-0069hi, cpus=6, sweetfb_noconsole=1, sweetfb_hz=30, sweetusb=2). The recovery pull (`tools/munch-log-pull.sh`) is still the way after a panic/hang before USB comes up.

**🎉 USB networking works (boot 0076s, 18:24 local): `ping 192.168.7.2` from the PC answers in ~1 ms, 0 % loss (200-packet burst, 1500-byte frames, 512/1024-byte frames).** 0075: CDC-ECM function (bulk 0x81/0x01, interrupt 0x82 NETWORK_CONNECTION; host MAC 02:00:5e:77:ee:01) + kernel ethernet ifnet `ue0` (legacy output, header strip like if_fake) with 192.168.7.2/24 set from the kernel (proto_plumb + SIOCAIFADDR on an in-kernel socket). 0076: received frames into one cluster mbuf (mbuf_allocpacket gave a first mbuf of length 0 -> panic "Failed mbuf validity check: len -14"). Host side (Linux cdc_ether, interface `enp0s20f0u3u1`): `nmcli dev set IF managed no; sudo ip addr add 192.168.7.1/24 dev IF; sudo ip link set IF up`. Boot image `shim/out/shim-boot-0076s.img` (`sweetusb=2`). iOS itself does not see ue0 yet (configd needs an IONetworkInterface in IOKit).

**USB device mode works (boot 0074s, 17:45 local): the host enumerates the phone at high speed as `1209:0001 ios-sweet "Sweet iOS (USB bring-up)"`.** 0072 read-only probe: ABL leaves the USB30_PRIM GDSC and all 7 USB clocks on, DWC3 v3.20a (GSNPSID 0x5533320a) in device mode (GCTL 0x00112000), halted (DCTL RUN_STOP 0, DSTS 0x00d3de04), event buffer still at ABL's 0xfdfc8000, QUSB2 PHY alive. 0073 (`sweetusb=2`): DCTL soft reset, own event buffer (polled, interrupt masked), ep0 DEPSTARTCFG/SETEPCONFIG/SETTRANSFRESOURCE, setup TRB, RUN_STOP; control-transfer state machine as Linux ep0.c; descriptors: vendor class, no data endpoints. It ran but no event came: **0074** forces the VBUS session in the Qualcomm wrapper (QSCRATCH 0xa6f8800: HS_PHY_CTRL |= UTMI_OTG_VBUS_VALID(20)|SW_SESSVLD_SEL(28), SS_PHY_CTRL |= LANE0_PWR_PRESENT(24), as Linux dwc3_qcom_vbus_override_enable) -> connect + enumeration. Image `shim/out/shim-boot-0074s.img`. Black Phone app: main thread waits in -[VMVoicemailManager fetchAccounts] (sync XPC; voicemail needs the modem).

**Navigation works (boot 0071s on v14, log `logs/boot-0071s.txt`):** swipe up from the bottom edge returns to the home screen; app switcher works. Two fixes: **0070** pid_hibernate (syscall 435 was nosys on the macOS config -> SIGSYS killed SpringBoard in -[SBToAppsWorkspaceTransaction _didComplete] when an app left the foreground; the only CONFIG_EMBEDDED-only entry in syscalls.master); **0071** touches starting within 48 px of an edge carry kIOHIDDigitizerEventFromEdgeTip (UIKit `_UIEventHIDTouchEdgeTypeForHIDEvent` reads the digitizer EventMask 0xb0007 bits 0x800/0x1000/0x2000/0x40000). Diagnosed, not fixed: Settings (Preferences) SIGTRAP in PodcastsUI func_1cdc47c58: force-unwrap of `URL.volumeTotalCapacity` (nil on our HFS+ root). Phone/Photos/Messages show black (MobilePhone launches, no crash) - next: stack samples via boot-arg `sweetsample=MobilePhone,MobileSlideShow,MobileSMS,SpringBoard` (image `shim/out/shim-boot-0071s.img`).

**🎉🎉 MILESTONE 2026-10-18 ~15:30 local: iOS 16.7 SPRINGBOARD HOME SCREEN ON THE PHONE** (boot `shim/out/shim-boot-0069s.img` = kernel 0069 + ADT `shim/out/sweet-0069hi.adt`, image **v14**; log `logs/boot-0069s-v14-HOMESCREEN.txt`). Home screen with a few apps, Settings opens, Control Center / lock screen / notification center work, touch + buttons work, 6 A55 cores (`cpus=6`), `sweetfb_noconsole=1 sweetfb_hz=30`. Known issue: cannot leave Settings (home gesture / AssistiveTouch home do nothing) - to investigate.
What it took today (in order): 0058 HID services (Apple IOHIDFamily-1915 user client), 0059 iPhone X identity (hw.product etc.), 0060 SPMI keys + GENI touch, 0061 1125x2436 presented display, 0062 RPMh L11C touch power + IOPMPowerSource battery, 0063 GENI FIFO packing (Goodix GT9896), 0064 PRIV_WORK_INTERVAL grant (SpringBoard "Failed to create work interval"), 0065 SMP via PSCI (6 A55), 0066 crash map regions, 0067 console mute without cons_ops write, 0068 IOSurface lookup of an already-mapped surface returns base -1 (backboardd snapshot crash), 0069 UFS log flush 10 s. Image v13: CommCenter removed from launchd.plist, BKNoWatchdogs. **Image v14** = v13 + Setup skipped (`/var/mobile/Library/Preferences/com.apple.purplebuddy.plist` SetupDone/SetupFinishedAllSteps true, SetupVersion 11, daemonizeNeedsToRun false; purplebuddy.notbackedup mini-buddy flags false; managedconfiguration.notbackedup LockdownActivationIndicatesCloudConfigurationAvailable false; springboard MCForceShowCloudConfigurationUI false) + mobileactivationd data ark seeded (`/private/var/containers/Data/System/D90C94C4-F94E-4FCF-8A28-A93BE3846D8E/Library/internal/data_ark.plist`, -ActivationState Activated etc.) + MediaToolbox D22/D221/Default SubServerConfiguration StartRecorderServer=false (mediaserverd crash loop: portrait-filter prewarm called NULL GL). **Activation:** ADT /chosen certificate-production-status 0, effective-production-status-ap 0, certificate-security-mode 1, effective-security-mode-sep 1 -> mobileactivationd's native Hactivation branch (Codex `codex/activation/report.md`; alternative 4-byte daemon patch documented there). Fallback image v13; v12 deleted.

**🎉 TOUCH WORKS (boot 0063u on image v13, 2026-10-18 ~13:15 local):** tapping (i) on Setup's Hello opens the device-info sheet; swipe-up is recognised (spinner, then back to Hello - under investigation). Chain: RPMh L11C vote (0062, `bsd/dev/sweet_rpmh.c`) -> GT9896 @0x5d on GENI SE7 (0063 fixed FIFO packing: register address went out byte-swapped) -> sweet_hid_touch -> SweetHIDTouch -> backboardd. Battery icon OK (0062 SweetBattery IOPMPowerSource, static 100 %). **Image v13** (= v12 + CommCenter jobs removed from launchd.plist + `BKNoWatchdogs`=true and `FBLaunchWatchdogExceptions{com.apple.purplebuddy:100}` in /var/mobile/Library/Preferences/com.apple.springboard.plist): Setup no longer killed by the scene-create watchdog (it blocked in CoreTelephony getSubscriptionInfo -> CommCenter, Codex `codex/setup/report.md`; that report also lists how to skip Setup: SetupDone/SetupFinishedAllSteps/SetupVersion=11 in com.apple.purplebuddy.plist + activation check). v9 deleted, fallback = v12. Input latency ~1 min with 1 CPU -> Codex task 11 = SMP via PSCI (`codex/smp/`).

**INPUT WORKS (boot 0060u on a freshly flashed v12, log `logs/boot-0060u.txt`):** power, volume up, volume down reach iOS (PMIC -> sweet_keys.c -> SweetHIDButtons -> backboardd). Power locks/unlocks (screen "off" shows the kernel console because the panel never really turns off). **Device identity fixed: iPhone X** (home indicator, Face-ID-style Setup). Earlier test (0059u, `sweethidtest=15` fake Home presses) advanced Setup from Hello to the language list.
- **0058** HID: `iokit/Kernel/arm/hid/`: IOHIDEventServiceUserClient + IOHIDEventServiceQueue + IOHIDEvent compiled UNCHANGED from Apple IOHIDFamily-1915.140.3 (= exact version of the iOS 16.7 IOHIDLib, clone in `research/iohid-1915`) over our minimal IOHIDEventService (SweetHID.cpp: SweetHIDButtons consumer page 0x0C power 0x30 / vol 0xE9/0xEA / menu 0x40; SweetHIDTouch digitizer collections). C hooks `sweet_hid_key(key, down)` (0 power, 1 vol+, 2 vol-, 3 home) and `sweet_hid_touch(finger, down, x, y)`. HID logs go to IOLog. Boot-arg `sweethidtest=N` = fake Home press every N s.
- **0059** identity: hw.machine/hw.model/hw.target/hw.product from the ADT (macOS-config kernel used to answer "iPad8,6"/"Redmi Note 10 Pro"). mkadt: root compatible `D22AP|iPhone10,3|xiaomi,sweet|...`, model iPhone10,3, target-type D22, `/product` copied from the real D22AP device tree (`research/ipsw-x/d22ap.adt`, extracted from the IPSW, NOT in git), `/buttons home-button-type 2`. Codex report `codex/mg/report.md`: MG ProductType = sysctl **hw.product** (unrecognized -> iPod), HWModelStr = hw.target, HomeButtonType = `/buttons/home-button-type`; disk cache `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` (keyed by build + UUID only -> stale iPod answers survived; reflashed clean v12 to clear it).
- **0060** drivers by Codex (`codex/input/hw-report.md`): `bsd/dev/sweet_keys.c` (SPMI arbiter v5 observer reads, PON 0x810 bits KPDPWR/RESIN SID0, PM6150L GPIO2 SID4 0xc110; works), `bsd/dev/sweet_touch.c` (GENI SE7 I2C @0xa84000, starts 45 s after boot). Touch result: SE7 fw ok (proto 3) but **0x5d and 0x38 both time out -> touch IC unpowered** (L11C LDO needs an RPMh vote). Codex task 8 = RPMh client (`codex/rpmh/`), task 7 = battery (`codex/battery/`).
- **0061** (CI 39117078444): red status bar "rdar:45025538" = UIKit "No status bar visual provider for screen" (360x800 pt is no real iPhone). AppleCLCD now presents **1125x2436** (iPhone X) and shows the centered 1080x2400; touch mapped the same; ADT canvas 1125x2436. `sweetfb_native=1` reverts.
- Images: `shim/out/shim-boot-0060b.img` (0060 + /buttons ADT `sweet-0060hi.adt`). Next: 0061 image with ADT from current mkadt.

### >>> HANDOFF 2026-10-17 late (read this first) <<<

**STATE: iOS 16.7 Setup.app "Hello" screen is ON THE PHONE** (boot `shim/out/shim-boot-0057u.img` on image v12). Photo `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg`. SpringBoard + backboardd + Setup run; nothing crashes in the GUI path. Remaining crash/relaunch loops: installcoordinationd (should be fixed by 0057, unverified), mediaserverd (VirtualAudio removed in v12, unverified), mobileactivationd (~100/boot, not analysed), bluetoothd + fairplayd.H2 relaunch constantly (no hardware).

**What is on the phone now:** userdata = image **v12** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v12.img` + `.simg` + `.sha256`; v9 kept as fallback, v8/v10/v11 deleted or superseded). Kernel **0057** (`xnu/out-0057`, CI `39067532732`). ADT **`shim/out/sweet-0057hi.adt`** (QEMU twin `research/q0057-hi.adt`). Boot image **`shim/out/shim-boot-0057u.img`**. Boot args: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1`.

**Build a phone image** (kernel K, ADT A): `python3 tools/mkpayload.py $K $A shim/out/payload-X.bin --cmdline "$B" && (cd shim && ./build.sh ../shim/out/payload-X.bin && cp out/shim-boot.img out/shim-boot-X.img)` then check `python3 tools/unpack_bootv2.py shim/out/shim-boot-X.img shim/out/check-X && cmp shim/out/check-X/ramdisk shim/out/payload-X.bin`. QEMU smoke test: `python3 tools/mkpayload.py $K research/q0057-hi.adt $S/csq/payloadX.bin --ramdisk $S/csq/root-fork.img --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` + `PATH=~/swift/usr/bin:$PATH python3 tools/qemu-munch.py ... --secs 40 --kernel $K --fail-on-panic` (S = scratchpad; root-fork.img lives in the session scratchpad csq/ dir and may be gone in a new session).

**Kernel workflow:** edit `research/bt28/xnu/...` (keep the pre-edit copy in `research/preNNNN/`), syntax check `python3 tools/xnu-cc.py research/run28.log research/bt28 <file>`, make `xnu/patches/NNNN-*.patch` with `diff -u pre bt28 | sed` path rewrite, add new/changed files to `research/check-patch-order.py` list, run it (must say applied + MATCH), commit + push -> GitHub Actions `xnu` builds (~8 min) -> `gh run download <id> -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out-NNNN`. **Repo is PUBLIC since 2026-10-17** (private macOS runner minutes ran out: "recent account payments have failed"); public = free runners. Commit trailer: only `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>`.

**Phone workflow (user does buttons, Claude runs commands):**
- Boot test kernel: phone in fastboot -> `fastboot boot shim/out/shim-boot-X.img`. NEVER `fastboot getvar`.
- **Read logs WITHOUT filming:** after a test the user holds Power+VolDown -> fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox, temp) -> wait for `adb shell true` -> `tools/munch-log-pull.sh /run/media/dipa/android/ios-sweet-work/logs/<name>.txt` (reads the 4 MiB console ring the kernel keeps in the last 1025 blocks of userdata; patch 0047). Then `adb reboot bootloader` brings the phone back to fastboot without the user.
- **Flash a root image:** `fastboot flash -S 512M userdata <img>.simg` (7 min, 25 chunks). After a big flash the Xiaomi ABL fastboot hangs: next `fastboot boot` stalls at "Sending" -> the USER must re-enter fastboot by hand (Power+VolDown). Do not retry commands against the hung ABL. A flash right after `adb reboot bootloader` once failed on chunk 1 (USB protocol error): just retry. User must NOT unplug during flash (once interrupted at 25/25 -> reflash).
- **Edit the phone root without reflashing (same-length only):** find the file's raw offset in the local image (it is one extent; search the image for its first 4 KiB), compare with `adb exec-out dd if=/dev/block/sda34 ...`, then `adb push` changed 4 KiB blocks and `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back. Recovery kernel has no hfsplus.
- **Pull the whole phone root** (12 min): `adb exec-out "dd if=/dev/block/sda34 bs=1048576 count=16384" > img`; mount ro with udisksctl or `sudo mount -t hfsplus -o loop,ro`. To write into a pulled image: `sudo fsck.hfsplus -fy` first (volume is never cleanly unmounted -> Linux mounts ro even with -o force), then `sudo mount -t hfsplus -o loop,force`.
- Making images: copy latest img with `cp --sparse=always`, mount rw with force, change, `sync; umount`, `sha256sum`, `img2simg`. Keep only the latest image + one fallback on the SSD (/home is ~98% full; big work files live in `/run/media/dipa/android/ios-sweet-work/`, old kernels/images archived in `.../ios-sweet-work/archive/`).

**Diagnostics built into the kernel (all SWEET-only):** persistent console log (0047, boot-arg sweetlog=0 off; flush every ~1 s); `|name| text` copies of /dev/null writes for processes in `sweetnull` (default backboardd,SpringBoard; 0048); `IOK name:` IOKit lookup/open trace for `sweetiolog` (default backboardd,SpringBoard; 0049/0054, 400 lines max); `SWEET Sandbox: proc op N` (0050); `STR ...` heap text dump on SIGABRT/SIGTRAP for `sweetscan` (default backboardd,SpringBoard; 0051/0053) - this is how NSException/BSAssert reasons were found; `SMP name tN pc .. bt ..` user stack sampler for `sweetsample` (default SpringBoard,backboardd; first at 90 s then every 120 s, 40 samples, 16 frames; 0054/0055); `SWEET FB: N swaps` every 1000 swaps; `CRASH name ...` crash reports (0043/0044). Symbolicate cache addresses with `ipsw dyld a2s research/dsc/dyld_shared_cache_arm64 <addr - slide>`; main-binary addresses: addr - mh + 0x100000000.

**Patches added today (0046-0057, all CI-built):** 0046 IOSurface (IOCoreSurfaceRoot) + IOMobileFramebuffer (AppleCLCD on /arm-io/disp0) + arm-io nubs; 0047 persistent log; 0048 /dev/null tee; 0049 IOKit trace; 0050 permissive "Sandbox" MAC policy; 0051/0053 heap text on crash; 0052 RENAME_SWAP via exchangedata on HFS; 0054/0055 sampler, swap counter, AKS selectors 35 (extended device state = 17) and 101; 0056 panel copy from the vsync timer at 10 Hz (sweetfb_hz) instead of every swap; 0057 no config_ext_resolver (ACL group entries resolve without memberd). ADT (tools/mkadt.py): /arm-io/disp0, /chosen/display-scale 3, /chosen/unique-chip-id (synthetic 0x5357454554000001) + chip-id 0x8015 -> IOPlatformUUID A9683613-3704-53F2-95D0-98A8FB574E78. Image v9-v12: /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}; launchd.plist StandardOut/ErrorPath renamed to ...PatX (pointing jobs at /dev/console stops them launching: EACCES); v12: LastSystemVersion=20H392 in plain /var/mobile/Library/Preferences/com.apple.migration.plist (stops the 1-hour data migration that ran every boot), VirtualAudio.plugin moved to /Library/Audio/Plug-Ins/HAL.disabled/.

**Codex helper** (`codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=high -s workspace-write -C /run/media/dipa/android/ios-sweet-work/codex -o OUT "$(cat task.md)" < /dev/null`, run detached with setsid nohup; config already defaults to gpt-6.1-sol high). Reports in `/run/media/dipa/android/ios-sweet-work/codex/`: mediaserverd-report.md, installcoordinationd-report.md (-> 0057), ufs-performance.md (ranked UFS speed plan), **ufs/0058-munch-ufs-faster.patch** (READY, not yet applied: ufslogsecs default 10 s log flush batching, optional ufscache=1 directional cache ops, optional ufswait=1 spin-then-sleep; syntax-checked, host-stub verified; see ufs/README.md). Apply it as patch 0058 after review.

**Claude safety-classifier note:** several messages about QuartzCore renderer selection / writing an HFS reader tool were cut off by an automatic classifier (false positives, user agrees). Keep those topics short or let them be; they are not blockers now (rendering already works).

**NEXT STEPS (in order):**
1. **Input** (Setup stuck on Hello): touch + power + volume as IOHIDEventService/IOHIDDevice services backboardd matches (log shows backboardd matching IOHIDDevice with DeviceUsagePairs and IOService with HIDServiceSupport=true). Phone hardware: Goodix/Focaltech touch over I2C/SPI (check live DT `dtb/`), buttons via PMIC (pon) and GPIO keys. Start with a fake HID service (e.g. power button via a timer or boot-arg) to learn the ABI, then real drivers.
2. **Device identity**: MobileGestalt reports iPod + Home button. Add /product node / product-type (iPhone10,3, D22AP), device class iPhone, home-button-less (Face ID) properties in mkadt; find exact MG keys (MGIOMFBSupport.c, `IODeviceTree:/product` lookups seen in backboardd trace).
3. **Battery**: AppleSmartBattery/IOPMPowerSource style service (status bar shows empty red battery).
4. Apply Codex 0058 (UFS speed), consider cpus>1, check whether installcoordinationd/mediaserverd loops are gone, analyse mobileactivationd.
5. Hide kernel console over the GUI when not debugging: boot-arg sweetfb_noconsole=1 (but then panics are not on screen).

- **🎉 MILESTONE: iOS GUI ON THE PHONE - Setup.app "Hello" screen (2026-10-17 ~21:30 UTC, boot 0057u on image v12).** Photo: `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg` (animated "dobar d(an)" hello lettering, status bar "iPod" + battery icon, bottom "Pritisnite Home za otvaranje" = "Press Home to open" (locale hr), (i) button). Rendered by iOS 16.7 SpringBoard/Setup through CoreAnimation -> backboardd -> kernel IOSurface (IOCoreSurfaceRoot) -> kernel AppleCLCD swaps -> ABL panel memory. What made it: data migration finally skipped (v12: `LastSystemVersion` 20H392 in the plain `/var/mobile/Library/Preferences/com.apple.migration.plist`; ADT `/chosen/unique-chip-id` + `chip-id` for IOPlatformUUID), 0057 (no ext resolver -> ACL containers work), plus everything before (0046 display/IOSurface, 0050 Sandbox policy, display-scale 3, containers dirs, RENAME_SWAP, AKS 35/101, 10 Hz present).
  **Missing (next work, in rough order):**
  - **Input: no touch, no power button, no volume buttons** (no HID drivers; backboardd looks for IOHIDDevice / HIDServiceSupport services). Setup is stuck on "Hello".
  - **Device identity: it thinks it is an iPod with a Home button** (status bar "iPod", "Press Home to open"): MobileGestalt has no product/model identity (no /product node, product-type, etc.) - give it iPhone10,3 / home-button-less (Face ID class) identity.
  - **No battery detection** (no battery/charger/PMIC service; status bar battery shows empty/red).
  - Locale came up Croatian (hr) - harmless.
  - Performance: cpus=1, polled UFS (Codex UFS patch in progress: `codex/ufs/`), backboardd presents at 60 Hz.
- **README refreshed (2026-10-17):** read the full notes and updated the public overview through patch 0045, v7 hardware observations and prepared v8 diagnostics. Documents real Apple userspace, the missing display/IOSurface GUI work, limited security services, highmem, persistent userdata root, current tooling and build examples. Documentation only; no kernel build, tests, flashing or phone boot performed.

- **v11 still migrated (boot 0056u, 20:32 UTC, `logs/boot-0056u-v11.txt`).** On iOS kCFPreferencesCurrentHost lands in the plain plist: all DMEnvironment setters (setLastBuildVersionPref, setUserDataDispositionPref, ...) use CurrentHost and the phone's `/var/mobile/Library/Preferences/com.apple.migration.plist` holds DMUserDataDisposition; datamigrator's XPC service runs with `_RoleAccount: mobile`. **Image v12** = v11 + `LastSystemVersion` = 20H392 added to that plain `com.apple.migration.plist` (keeps DMUserDataDisposition, 501:501 0600) + `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` moved to `/Library/Audio/Plug-Ins/HAL.disabled/` (Codex: stops the mediaserverd SIGSEGV loop; untested). **installcoordinationd loop explained (Codex `codex/installcoordinationd-report.md`):** os_crash "Failed to create data directory" EACCES for `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library/InstallCoordination`: `Library`/`Library/Caches` in the container are ACL-only (uid/gid 0xfffffffe, mode 0000, ACE for group GUID ...012B = gid 299 systemusers); with `config_ext_resolver` the kernel asks an identity resolver (memberd/opendirectoryd) that iOS does not run -> EWOULDBLOCK -> ACE skipped -> EACCES. **Patch 0057**: drop config_ext_resolver from FILESYS_BASE (compatibility UID/GID<->GUID mapping is used instead), likely fixes other ACL-protected containers too. Codex task 3: UFS driver performance design (`codex/ufs-performance.md`).
- **mediaserverd SIGSEGV loop explained (Codex report `/run/media/dipa/android/ios-sweet-work/codex/mediaserverd-report.md`):** the fault (FAR 0x390) is in the HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio` (ApplyBeamFormerPreference <- VirtualAudio_PlugIn::SetPropertyData <- HALS_AHPPlugIn::ObjectSetPropertyData <- AudioObjectSetPropertyData <- MediaExperience vaemVADSerializationListener): the default virtual audio device ('vdef', VirtualAudioDevice_Default) does not exist (no audio hardware/driver, no /product/audio). Proposed workaround for GUI bring-up: move VirtualAudio.plugin out of /Library/Audio/Plug-Ins/HAL/ in the next root image (untested). v11 reflashed OK after an interrupted flash (USB unplugged at chunk 25/25). Codex task 2: installcoordinationd crash loop.
- **ROOT CAUSE of the endless migration: no IOPlatformUUID (2026-10-17).** datamigrator's `-[DMMigrator isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:]` gets lastBuildVersion from `-[DMEnvironment lastBuildVersionPref]` = `CFPreferencesCopyValue("LastSystemVersion", "com.apple.migration", kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)` -> a ByHost file. The phone had `/var/mobile/Library/Preferences/com.apple.migration.plist` (only DMUserDataDisposition) and NO `Preferences/ByHost` directory at all. On arm XNU publishes IOPlatformUUID (gethostuuid) only from `/chosen/unique-chip-id` (8 bytes) + `/chosen/chip-id` (4 bytes) (IOPlatformExpertDevice::generatePlatformUUID, called from IOPlatformExpert::start); the SWEET ADT had neither, so every CurrentHost preference write failed. Fix: mkadt adds `unique-chip-id` = 0x5357454554000001 (synthetic) and `chip-id` = 0x8015 -> UUID `A9683613-3704-53F2-95D0-98A8FB574E78` (same SHA1 namespace computation as the kernel). ADT `shim/out/sweet-0057hi.adt`, boot image `shim/out/shim-boot-0056u.img` (0056 kernel + 0057hi ADT, QEMU OK). **Image v11** = v10 + `Preferences/ByHost/com.apple.migration.<UUID upper and lower>.plist` {LastSystemVersion: 20H392} (501:501, dir 0755 files 0600). Flashing v11 2026-10-17 ~20:10 UTC. Codex (gpt-6.1-sol high) analysing the mediaserverd SIGSEGV loop in `/run/media/dipa/android/ios-sweet-work/codex/` (report `mediaserverd-report.md`).
- **v10 did NOT stop the migration (boot 0056n on v10, 19:18 UTC, `logs/boot-0056n-v10.txt`):** datamigrator + migrationpluginwrapper ran again, backboardd drew the progress UI (13000+ swaps in ~5 min). So `com.apple.DataMigration.plist`/LastSystemVersion is not the deciding input. What is known about the decision (datamigrator XPC service `DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator`, strings): `Migration sentinel present. Migration necessary.` (sentinel `/private/var/datamigrator/migrate`, created and removed by datamigrator itself; absent in v10), `System build version changed from %@ to %@. Migration necessary.` / `...unchanged... not necessary`, `Disposition dictionary with build version %@ is out of date relative to current build version %@`, `SyntheticDidUpgrade`, DMUserDataDispositionManager; previous build source not found yet (not in the binary's plain strings; likely in DataMigration.framework's DMUserDataDispositionManager / preferences domain `com.apple.migration`). backboardd calls `DMPerformMigrationIfNeeded` and holds SpringBoard in `checkInAndWaitForDataMigration` until it returns. NEXT: find where the previous build version / disposition dictionary is stored (disassemble DataMigration.framework DMUserDataDispositionManager + datamigrator's isMigrationNeeded path), write it into the image; or get datamigrator's os_log out (OS_ACTIVITY_DT_MODE needs a launchd env change).
- **Data migration never records completion -> skip it (image v10, 2026-10-17).** Boot 0056n (18:16 UTC, `logs/boot-0056n.txt`, 30 min): 10 Hz panel copies, backboardd still presents at ~60 Hz (107k swaps), SpringBoard still in `BKSSystemShellService _checkIn` at sample 14. Every migration run takes 1+ hour on cpus=1 and repeats each boot. DataMigration decides from `/var/mobile/Library/Preferences/com.apple.DataMigration.plist` key `LastSystemVersion` (vs ProductBuildVersion 20H392); the pulled phone root (21:01 local) had NO such file. **v10** = that pulled phone state + fsck.hfsplus -fy (volume was marked not cleanly unmounted -> Linux mounts it read-only even with -o force; fsck found only hard-link counts + minor volume header, repaired) + `com.apple.DataMigration.plist` {LastSystemVersion: 20H392} (501:501 0600). `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v10.img` (+ .sha256, .simg); v8 files deleted, v9 kept as fallback.
- **Repo made PUBLIC (2026-10-17, user request) so GitHub-hosted macOS runners are free:** the 0056 CI run was refused ("recent account payments have failed or your spending limit needs to be increased"). Checked before: no device dumps, serials, IMEI or Apple binaries in the history (.gitignore keeps backup/, dtb/, recovery/, research/ out); commit author emails are public now. 0056 rerun `39056660104` passed -> `xnu/out-0056`, QEMU OK, image `shim/out/shim-boot-0056n.img` booted 18:16 UTC.
- **Boot 0055n (17:16 UTC, ~65 min, `logs/boot-0055n.txt`): migration bar reached 99% in ~30 min (was ~60), then crawled.** Root cause of the slowness: backboardd presents the progress UI at ~58 Hz (**167,000 swaps in ~48 min**) and AppleCLCD copied the full 10 MB frame into the panel on every swap -> ~600 MB/s memcpy on the single CPU (cpus=1), starving everything (polled UFS I/O too). Sampler (16 frames): **SpringBoard waits in `-[BKSSystemShellService _checkIn]`** (synchronous BSXPC call to backboardd, from FBSystemShellInitialize <- SBSystemAppMain) - backboardd holds the system shell check-in until the data migration ends, so the migration is the gate to SpringBoard UI. **Patch 0056**: swap only records the latest swap record; the vsync timer copies it to the panel every 60/sweetfb_hz ticks (default 10 Hz); `SWEET FB: N swaps` every 1000.
- **Boot 0054n (16:53 UTC, ~12 min, `logs/boot-0054n.txt`, patch 0054 sampler):** the data migration runs AGAIN on every boot (it completed once in the 0053s run) - completion is not being recorded. backboardd keeps swapping the progress UI (3700+ swaps in ~10 min, `SWEET FB: N swaps`). Sample during migration: SpringBoard main thread blocked in a synchronous XPC call (`mach_msg2_trap <- dispatch_mach_send_and_wait_for_reply <- xpc_connection_send_message_with_reply_sync <- -[BSXPCServiceConnectionMessage _sendSynchronously:] <- BSXPCServiceConnectionProxy ...`). Unsupported AKS selectors seen: 101 `aks_assert_consume`, 93 `aks_get_internal_info`, 34 `_aks_stash_load` (datamigrator), **35 `aks_get_extended_device_state`** (SpringBoard, chronod, remindd, searchd, symptomsd...). **Patch 0055**: AKS 35 answered like 17 (same DER device state), 101 succeeds; sampler backtraces 16 frames, 40 samples (~80 min).
- **MILESTONE: FIRST iOS PIXELS ON THE PANEL (boot 0053s long run, 15:33 UTC, ~60 min; photos IMG_20261017_183029/183222/183419; log `/run/media/dipa/android/ios-sweet-work/logs/boot-0053s-long.txt`, 407 KB, 1428 flushes).** The iOS data-migration boot screen (Apple logo + progress bar) was shown and the bar advanced to full in ~60 minutes. It is drawn by **backboardd through our kernel display path**: `SWEET SURF: backboardd create id 2/3 1080x2400 fmt 0x42475241 (BGRA) bpe 4 bpr 4352 size 10452992 keys 11`, IOSurface sels 9, 27, 5, 34 and traps 2/3 (lock/unlock), IOMFB SwapBegin (4) / SwapEnd (5, 1132-byte struct) / GetLayerDefaultSurface (3) / 68 / 55, then `SWEET FB: swap N layer 0 surface 2|3 1080x2400 ... a 0,0 1080x2400 b 0,0 1080x2400` alternating (double buffering; only the first 16 swaps are logged). Triggered by `com.apple.datamigrator` (AKS sels 101/93/34 return unsupported) and `com.apple.migrationpluginwrapper`. Probably ProgressUI-style CoreGraphics drawing into IOSurfaces (no GPU used). After the migration: SpringBoard and backboardd stay up (exec once each, no crash), nothing new drawn; crash loops only for mediaserverd, installcoordinationd, mobileactivationd (~108 each in the hour); bluetoothd and fairplayd.H2 relaunch constantly (no hardware). Kernel console text overlays the image (sweetfb_noconsole=1 would hide it).
- **MILESTONE: SpringBoard and backboardd both stay up (boot 0053s on v9, 15:29 UTC, `logs/boot-0053s.txt`).** With /chosen/display-scale = 3: `exec SpringBoard` once, `exec backboardd` once, no CRASH for either; MobileGestaltHelper reads IOMFB size/dot pitch; backboardd opens AppleCLCD (sels 57,15,22,29,33,79,74 as before). No IOSurface creates and no swaps: backboardd keeps polling `IOProviderClass IOAcceleratorES` (-> 0 found) and AGXAccelerator/IOAcceleratorESLegacy - no GPU, so no CoreAnimation renderer yet. Log goes quiet after ~47 s (only 47 flushes): the system idles. NEXT WALL: CoreAnimation rendering without a GPU (CPU path) so the window server produces frames.
- **SpringBoard's invalid mode = scale 0 (boot 0053n, 15:19 UTC, `logs/boot-0053n.txt`, heap scan):** `FBSDisplayMode cannot be configured as requested : <FBSDisplayMode: ...; infxinf@0x (1080x2400/0) 60Hz sRGB SDR>` - pixel size and 60 Hz are fine, scale is 0. libMobileGestalt (MGIOMFBSupport.c, sub_19c560a54) reads `display-scale` (u32, default 0) and `display-rotation` from **IODeviceTree:/chosen**, pixel size via IOMobileFramebufferGetDisplaySize (MobileGestaltHelper sel 8/28 in the log). mkadt now adds `/chosen/display-scale` = 3 (1080x2400 @3x = 360x800 pt). ADT `shim/out/sweet-0054hi.adt` (QEMU `research/q0054-hi.adt`), image `shim/out/shim-boot-0053s.img` = 0053 kernel + 0054hi ADT. Possible catch: MobileGestalt may have cached scale 0 in `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/...`.
- **MILESTONE: backboardd survives and opens the display (boot 0052n on v9, 15:07 UTC, `logs/boot-0052n.txt`).** No `CRASH backboardd`. It walks `IODeviceTree:/arm-io` children, matches IOMobileFramebuffer, `open AppleCLCD type 0 -> 0x0`, calls IOMFB selectors 57, 15, 22, 29, 33, 79 (GetBlock, out 1580/40), 74 (SupportedFrameInfo), 12, 6, 27, 17, 84, 50; opens IOCoreSurfaceRoot (sel 13); looks for AGXAccelerator / IOAcceleratorES / IOAcceleratorESLegacy (none, repeatedly), `IODeviceTree:/product` (none, repeatedly), `/buttons`, aop-smart-cover, IOPMrootDomain. No swaps yet. **SpringBoard now gets past display discovery** and dies (sig 5) in `-[FBSDisplayMode _initWithWidth:height:preferredScale:scaleOverride:refreshRate:gamut:hdr:rotation:virtual:validityCheck:]+580` <- `_initWithCADisplayMode:scale:rotation:` <- `FBSDisplayConfiguration _initWithImmutableDisplay:` <- `FBSDisplaySource initWithDisplay:` <- `FBSDisplayMonitor` (slide 0x14504000): "FBSDisplayMode cannot be configured as requested". Checks: width/height >= 2, preferredScale > 0, scaleOverride >= 0, refreshRate non-zero, valid gamut/HDR/rotation. Suspects: refresh rate 0 (IOMFB timing blocks answered with zeros) and/or scale 0 (MobileGestalt main-screen-scale; no /product node in the ADT). **Patch 0053**: heap text scan also on SIGTRAP and for SpringBoard (default sweetscan list backboardd,SpringBoard) to read the exact mode description.
- **Image v9 (2026-10-17) = v8 + `/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}` (root:wheel 0755; on iOS these live on the data volume, absent from the rootfs) + launchd.plist with StandardOutPath/StandardErrorPath renamed to ...PatX (the in-place phone fix; daemons' stdout reaches the log via the 0048 /dev/null tee).** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v9.img` SHA256 aa9f9139...6e67, `.simg` 12,145,303,864 bytes; written through `sudo mount -t hfsplus -o loop,force`. **Patch 0052** (`xnu/patches/0052-munch-hfs-rename-swap.patch`, CI `39042761826` -> `xnu/out-0052`, QEMU OK, image `shim/out/shim-boot-0052n.img`): vn_rename falls back to VNOP_EXCHANGE when VNOP_RENAMEX(RENAME_SWAP) returns ENOTSUP for two regular files on one volume (HFS exchange swaps data and cnode descriptors = swap semantics). Flashing v9 started 2026-10-17 ~15:00 UTC.
- **backboardd EXCEPTION FOUND (boot 0051n, 14:41 UTC, `logs/boot-0051n.txt`, patch 0051 heap scan):** `*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[NSURL initFileURLWithPath:]: nil string parameter'`. First-throw stack (slide 0x337f0000): `+[NSURL fileURLWithPath:]` <- `-[BSSystemContainerForCurrentProcessPathProvider libraryPath]` <- `cachesPath` <- backboardd main+0x1d858 (StartWindowServer, shader cache path). BaseBoard: libraryPath = `BSSystemSharedResourcesDirectoryForIdentifier` = `BSSystemSharedDirectoryForIdentifier(id)` + "/Library"; that calls `container_system_path_for_identifier` (libsystem_containermanager -> `_container_legacy_lookup` -> containermanagerd XPC), which returned NULL (BaseBoard logs "Error getting shared system container ..."). With 0050 the sandbox denials are gone. containermanagerd.log.0 in the pulled (pre-0050) image: "Did not receive any persona attributes", "Expected to find at least one persona", notify_register_dispatch failed 9; `/private/var/containers` does NOT exist in the root image (on iOS it lives on the data volume). usermanagerd does create kernel personas 99/199/1000, but fails to save `/private/var/keybags/usersession.kb` and `persona.kb`: "Failed to atomically swap writing file: Operation not supported (45)" (HFS has no RENAME_SWAP). NEXT: create `/private/var/containers` (and check what containermanagerd needs for system group containers), fix or emulate RENAME_SWAP on HFS, then re-check backboardd.
- **Disk housekeeping (2026-10-17):** /home was at 4 GiB free. Moved (not deleted) `xnu/out-0010..0044` and old `shim/out/*.img`/`payload-*.bin` (everything before 0046) to `/run/media/dipa/android/ios-sweet-work/archive/{xnu,shim-out}` (6.3 GiB); /home now 8.7 GiB free. Large work files (phone-root.img 16 GiB, logs, tracev3 dumps) live in `/run/media/dipa/android/ios-sweet-work/`. **Patch 0051** (`xnu/patches/0051-munch-crash-heap-strings.patch`, CI `39041440935`): on SIGABRT of a process in boot-arg `sweetscan` (default backboardd) the crash log prints heap text runs as `STR ...` (to find the NSException reason). Boot 0051n 14:41 UTC.
- **Boot 0049n (14:11 UTC, `logs/boot-0049n.txt`, patch 0049 trace): backboardd's IOKit lookups before it dies** (each run): `IODeviceTree:/arm-io/pearl-sep` -> none, matching ApplePearlSEPDriver -> none, `IOService:/IOResources/AppleKeyStore` open OK, matching IOHIDDevice (DeviceUsagePairs) / `HIDServiceSupport` notifications, `usb-drd,t8027` notification; **never any IOMobileFramebuffer / display lookup** - the exception happens before display detection. launchd (tracev3) shows backboardd's mach lookups denied with `error = 159: Sandbox restriction`: com.apple.mobilegestalt.xpc, com.apple.iohideventsystem, com.apple.system.notification_center, com.apple.logd, containermanagerd, ACCHWComponentAuthService, oscard. libsystem_sandbox uses `__mac_syscall("Sandbox", op, arg)` (ops: 1 init, 2 check, 3 note, 4 container path, 5/6/7/8 extensions, 0x15 check_bulk, ...); with no policy named "Sandbox" every call gets ENOPOLICY. **Patch 0050** (`xnu/patches/0050-munch-sandbox-policy.patch`): built-in permissive MAC policy "Sandbox" in sweet_amfi.c (mpo_policy_syscall returns 0, container/extension ops ENOENT; logs first use of each op as `SWEET Sandbox: proc op N`).
- **Phone root pulled (16 GiB, 12 min, `adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` (loop-mounted ro at /run/media/dipa/iOSRoot).** No .ips crash reports. logd tracev3 (Persist, LZ4 `bv41` blocks, decompressed strings in `ios-sweet-work/tracev3/all.txt`): /private/var/db/uuidtext is EMPTY so only launchd's own messages are readable. Findings: v8 launch failure = `Service could not initialize: posix_spawn(/usr/libexec/backboardd), error 0xd - Permission denied` (xpcproxy exit 78) - /dev/console not openable for the mobile user; backboardd[98] spawned `com.apple.MTLCompilerService` before SIGABRT; **launchd denies many mach lookups with `error = 159: Sandbox restriction`** (mediaserverd -> com.apple.backboard.display.services, remoted, mobiletimerd -> sleepd, identityservicesd -> apsd) and **tccd exits OS_REASON_SANDBOX "profile not found"** - no Sandbox kext/profiles on SWEET; likely cause of many daemon crash loops (next wall after the display). CF's uncaught-exception text never reaches stderr for daemons (no CF handler; libc++abi prints only the type). **Patch 0049** (`xnu/patches/0049-munch-iokit-trace.patch`): `IOK name: ...` console lines for registry path lookups, child iterators, matching (with result count/class), matching notifications and IOServiceOpen of the processes in boot-arg `sweetiolog` (default backboardd), max 400 lines.
- **CONFIRMED: launchd StandardOut/ErrorPath=/dev/console stops backboardd from launching.** After the in-place rename (boot 0047n-b, log `logs/boot-0047n-b.txt`) backboardd launches again and dies as on v7: sig 6, `___pthread_kill <- abort <- demangling_terminate_handler <- _objc_terminate <- std::terminate <- _dispatch_client_callout <- _dispatch_once_callout <- main+0x9334` (window-server dispatch_once). It never opened AppleCLCD or IOCoreSurfaceRoot (no `SWEET FB/SURF: backboardd` lines) -> throws before/at display detection. **Patch 0048** (`xnu/patches/0048-munch-devnull-tee.patch`, CI `39037749668` -> `xnu/out-0048`, image `shim/out/shim-boot-0048n.img`): writes to /dev/null by processes named in boot-arg `sweetnull` (default `backboardd,SpringBoard`) are printed as `|name| text`. Boot 0048n (13:48 UTC, log `logs/boot-0048n.txt`): only `|backboardd| libc++abi: terminating due to uncaught exception of type NSException` (CF's reason line goes to os_log, not stderr, despite CFLOG_FORCE_STDERR/OS_ACTIVITY_DT_MODE). Next: pull the whole 16 GiB root from recovery (`adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` and read ReportCrash .ips files / logd tracev3 for the exception reason.
- **Persistent log WORKS (boot 0047n on v8, 13:31 UTC; pulled from OrangeFox: 70 KB, 111 flushes, `/run/media/dipa/android/ios-sweet-work/logs/boot-0047n.txt`).** In 3 min **backboardd was never exec'd** (v7 ran it), ~70 bare `exec xpcproxy`, SpringBoard loops on FBSDisplayMonitor (no display), mediaserverd sig 11 loop, locationd, lockdownd sig 5. Suspect the v8 plist (StandardOut/ErrorPath=/dev/console on backboardd). **In-place plist edit from recovery (no reflash):** launchd.plist is one extent at image/partition byte offset 0x1e025f000 (195 blocks; verified bytes identical on phone vs local v8 mount); renamed StandardOutPath->StandardOutPatX and StandardErrorPath->StandardErrorPatX (same length, both jobs) in blocks 68 and 105 via `adb push` + `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back VERIFIED. Phone now differs from local v8 image in those 2 blocks (files in `/run/media/dipa/android/ios-sweet-work/`). Test boot 0047n 13:38:42 UTC. Recovery kernel has no hfsplus (raw block edits only).
- **Patch 0047: persistent console log (CI `39036478663` -> `xnu/out-0047`, QEMU no panic; phone image `shim/out/shim-boot-0047n.img` = 0047 + sweet-0046hi.adt + 0045n cmdline).** `_cnputs` (all console output: kernel printf + daemons' /dev/console) also appends to a 4 MiB ring in sweet_ufs.c; after the UFS partition is ready and writable (ufsrw=1) a thread writes new ring blocks + header to the LAST 1025 blocks (4 KiB) of userdata every ~1 s and SYNCHRONIZE CACHE (prints `SWEET LOG: console log to the last 4100 KiB ...`). Read: phone to fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox) -> `tools/munch-log-pull.sh OUT.txt` (adb dd of that area + `tools/munch-log.py`). Not yet verified on the phone. Panics after the last flush are not in it (screen still shows them). 2x font attempt reverted: XNU's v_scale only scales the progress icon.
- **Boot 0046n on v8 (13:05 UTC, VID_20261017_150522_HSR_120):** phone prints `SWEET SURF: IOCoreSurfaceRoot registered` and `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1` (nubs + match work on hardware). Visible userspace: early boot tasks, launchd_cache_loader, `exec SpringBoard`; no `exec backboardd` / `SWEET FB: backboardd` line seen in the readable frames (video blurry). Frame 00:47: lower half of the screen shows OLDER console text (16:01:12) below newer lines -> unconfirmed hint that a swap copied an old snapshot of the panel (CA reads the default/displayed surface = panel memory). SpringBoard still dies in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` (sig 5, slide 0x30960000). **Shim: 2x console font** (`shim/loader.c` Video.v_depth scale field = 1) so phone videos are readable; image `shim/out/shim-boot-0046b.img` = 0046n + 2x font.
- **Patch 0046: built-in IOSurface + IOMobileFramebuffer (2026-10-17, offline, NOT phone-tested).** `xnu/patches/0046-munch-iosurface-iomfb.patch`: new `iokit/Kernel/arm/SweetSurface.{h,cpp}` (IOCoreSurfaceRoot + IOSurfaceRootUserClient: sel 13/0/4/34/35, traps 0-5, wired buffers mapped into the caller, per-client refs) and `SweetFramebuffer.cpp` (AppleCLCD : IOMobileFramebuffer on /arm-io/disp0: sizes, swap = CPU copy of enabled layers into the ABL panel memory, 60 Hz vsync via sendAsyncResult64WithOptions(CanDrop), panel memory published as the default IOSurface, kernel text console paused on the first swap only with `sweetfb_noconsole=1` (default keeps the console: pausing switches it to serial and would hide panics)); SweetPlatformExpert creates nubs for /arm-io children (not gic) attached to the platform expert (IOPlatformDevice::compareName casts its provider to IOPlatformExpert, so nubs under the arm-io nub never matched - first CI build showed that in QEMU). mkadt: /arm-io/disp0 {device_type display-subsystem, compatible sweet,display, canvas-width 1080, canvas-height 2400}. CI `39021572169` -> `xnu/out-0046`. **QEMU** (`research/q0046-hi.adt`, RAM disk init, msgbuf): `SWEET SURF: IOCoreSurfaceRoot registered`, `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1`, init alive, no panic. Phone image **`shim/out/shim-boot-0046n.img`** (SHA256 9c4d2fe9..., CI `39024108588` with the review fixes: swap bounds check, zeroed 64-bit async ref, port release, 1.5 GiB wired budget, ref leak/race fixes; QEMU re-checked) = 0046 kernel + `shim/out/sweet-0046hi.adt` + the 0045n cmdline; boot it on image v8 (backboardd/SpringBoard log to console) and film. Expected new console lines: `SWEET SURF: <proc> create id ...`, `SWEET FB: backboardd sel N ...`, `SWEET FB: swap ...`. Open question for the next wall: QuartzCore's IOMFB server builds its renderer from Metal first (`CAMetalContextCreate`), GLES only when enabled; how the CPU renderer gets selected on a device without a GPU is not yet established.
- **IOSurface values ABI (offline RE, `/run/media/dipa/android/ios-sweet-work/iosurface-values.md`) - TODO in SweetSurface (0046 only accepts and drops values, so IOSurfaceCopyValue always returns NULL):** sel 9 SetValue in = {u32 id, u32 pad, IOCFSerialize([value, key]) or ([dict] = merge)}, sel 0xb RemoveValue in = {u32 id, u32 pad, key\0} (empty key = remove all); both out 4 bytes = new generation, client accepts only (gen+2)|1. Kernel after each change: gen = (gen+2)|1 (odd, never 0) -> state+0x20, serialized size (>=1) -> state+0x28. sel 0xa in {u32 id, u32 0, u8 0}, out {u32 ?, u32 gen, serialized non-bulk dict}; too small -> 0xe00002db + needed size (client retries x16). sel 0x1b in 0x88: bulk struct 0x70 @0, u64 valid size @0x70, u64 changed mask @0x78, u32 id @0x80 -> copy masked fields into state+0x30 (bit0 32B @0, bit1 16B @0x20, bit2 8B @0x30, bits3-10 1B @0x38..0x3f, bits11-14 24B @0x40/4B @0x58/8B @0x5c/1B @0x64).
- **IOMFB display matching confirmed (offline RE, `/run/media/dipa/android/ios-sweet-work/iomfb-match.md`):** the framework never matches on "AppleCLCD"; any registered IOMobileFramebuffer subclass works. `iomfb_match_callback` reads only `dfr`/`AppleTV`/`external` (CFBoolean, service itself, no parents); internal main display = all absent/false (type 0, name "primary"). Display count = /arm-io children whose device_type starts with "display-subsystem" (count 0 -> CreateDisplayList returns NULL; more nodes than services -> 30 s wait). IOMobileFramebufferOpen fails only on IOServiceOpen failure; canvas-width/height read as 4-byte CFData from the IOService-plane parent (optional). CopyProperty = IORegistryEntrySearchCFProperty on the service only. Patch 0046 (disp0 node, `external`=false, AppleCLCD : IOMobileFramebuffer) fits all of this.
- **IOMobileFramebuffer swap + notification ABI (2026-10-17, offline).** Swap record = fb+0x18, **0x46c bytes**, zeroed by SwapBegin and sent whole by `kern_SwapEnd` = IOConnectCallStructMethod(sel 5, rec, 0x46c, no output). **SwapBegin** = sel 4, no input, 1 scalar out = swap id (stored at rec+0x50). `kern_SwapSetLayer(fb, layer<4, IOSurfaceRef, CGRect a, CGRect b, flags)`: rec+0x54+4*layer = **IOSurface id**; rec+0x104 bit layer = layer enabled (rec+0x108 bit cleared); rect a (int x,y,w,h, scaled) at rec+0x64+16*layer, rect b at rec+0xc4+16*layer; rec+0x114+4*layer = transform from flags&0xf; flag bits 5/31/6/7/8/9 set bytes rec+0x2ca/0x2cc/0x2e3/0x2e5/0x2e6/0x467+layer. A NULL surface only sets the enable bit with zero id. SwapWait = sel 6 scalars [swap id, timeout, 0].
  - **Notifications**: `IOMobileFramebufferScheduleWithDispatchQueue`/`GetRunLoopSource` create an IONotificationPort and `IOConnectSetNotificationPort(conn, type, port, 0)` (kernel `registerNotificationPort`). `kern_EnableNotifications(fb, type, cb, ctx)` = **sel 72 scalars [NotifyCallbackFuncs[type], fb, type, 0]** -> the kernel keeps an async reference {port, func, refcon=fb} per type and fires it with `sendAsyncResult64`. Types: 0 HotPlug, 1 Power, 3 FrameInfo (args[0] = mask of 31-bit keys, args[1] u32, then values), 4 CRC, **5 Vsync**: 6 args, each a 32-bit half in a u64 slot: (ts lo, ts hi), (t2 lo, hi) passed through the time-ingest helper, (t3 lo, hi); the framework then calls the client's vsync callback(fb, ts, t2', t3, ctx). Vsync callbacks are dropped while the notification port has queued messages.

- **IOSurface.framework user-client ABI reversed (2026-10-17, offline, step 2 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOSurface` (selector scan script: scratchpad `selscan.py`).
  - Connect (`_iosConnectInitalize`, pthread_once): `IOServiceGetMatchingService(IOServiceNameMatching("IOCoreSurfaceRoot"))`, retried with sleep(1) up to 241 times (a missing service stalls every client ~4 min, no crash); `IOServiceOpen(type 0)`; then **sel 13** (no inputs, struct out 0x18 bytes = `_ioSurfaceLimits`) and requires u16 at out+0x16 == 1 (ABI version); out+0 = offset/plane-base alignment mask, out+4 = bytes-per-row alignment mask (used as (x+m)&~m), +0..+0x14 also copied into IOSurface remote-method init, otherwise logs and closes the connection.
  - **Create = sel 0**: scalar in [parent id or 0], struct in = `IOCFSerialize(props, kIOCFSerializeToBinary)` (OSUnserializeBinary format; keys IOSurfaceWidth/Height/BytesPerRow/BytesPerElement/PixelFormat/AllocSize/PlaneInfo/CacheMode/IsGlobal/...), struct out **0xa68 bytes** ("lock result"). **Lookup = sel 4**: scalar in [id], same struct out. LookupFromMachPort = sel 0x22 (scalar in [port name]); CreateMachPort = sel 0x23 (scalar in [id, options], scalar out [port name]).
  - Lock result (copied to client+0x68, size 0x68 + planes*0x80, min 0xe8): 0x00 u64 base address **mapped in the caller's task**; 0x08 u64 task VA of a per-surface shared state page (+0 u64 registry id, +8/+0xa u16 bound accel ids, +0xc u32 seed, +0x14 u8 tiled, +0x20 u32 value generation, +0x24 YCbCr matrix, +0x28 u32 non-bulk values size, +0x2d u8 displayable, +0x2e u8 sysmem-only, +0x30 0x70 bytes bulk attachments); 0x10 u64 task VA of timestamp page (4 u64 indexed timestamps, +0x20 detach code); 0x18 u32 surface id; 0x1c u32 alloc size; 0x20 width; 0x24 height; 0x28 bytes per row; 0x2c buffer offset; 0x30 pixel format; 0x34 u32 (dcache flush length); 0x38 plane count; 0x40 u16 bytes/element; 0x42 u8 element width; 0x43 u8 element height; 0x44 cache mode; 0x4c u8 subsampling; 0x4d u8 prohibit-use count; 0x50 parent id; 0x54 slice count; 0x58 u64 trace id; 0x60 u64 protection options; planes[n] at 0x68+n*0x80 (+0 width, +4 height, +8 bytes/row, +0xc offset, +0x14 size, +0x18 u16 bpe, +0x1a/+0x1b u8 element w/h, +0x1c u8 component count, +0x1d address format, +0x54 u8 compression type (non-zero -> no CPU base address)).
  - **Traps** (IOConnectTrapN -> `IOUserClient::getTargetAndTrapForIndex`): 0 IncrementUseCount(id, category), 1 DecrementUseCount(id, category), 2 **Lock**(id, options, &seed_out), 3 **Unlock**(id, options, &seed_out), 4 Release(id), 5 Release after a failed create/lookup setup (id).
  - Other selectors: 5 SetYCbCrMatrix, 6 WrapClientImage, 7 WrapClientMemory, 9 SetValue (serialized), 0xa UpdateNonBulkValueCache (struct in 9), 0xb RemoveValue, 0xc BindAccel, 0x11 SetSurfaceNotify (async), 0x12 RemoveSurfaceNotify, 0x13 Log, 0x14 SetPurgeable, 0x15 SetOwnership, 0x16 SetTiled, 0x18 SetTimestamp, 0x19 GetTileFormat, 0x1a CopyDataValueBytes, 0x1b SetBulkAttachments (struct 0x88), 0x1e/0x2e-0x31 (via outlined helpers), 0x1f SetCompressedTileDataRegionMemoryUsed, 0x20 GetGraphicsCommPageAddress, 0x21 SetIndexedTimestamp, 0x24-0x26/0x2d IOSurfaceAccelerator, 0x27-0x29 shared events, 0x2a SignalEvent, 0x2b Set/InitDetachModeCode, 0x2c SetOwnershipIdentity, 0x32-0x34 data properties.
  - => SWEET kernel service: class/name **IOCoreSurfaceRoot** + IOSurfaceRootUserClient; minimum = sel 13, 0, 4, 9/0xa/0xb (values), traps 0-5, 0x22/0x23 later for cross-process surfaces (CA sends surfaces to backboardd by mach port). Memory: IOBufferMemoryDescriptor per surface, mapped into each task with createMappingInTask; ids from a global table.

- **Image v8: daemon logs on the console.** `tools/patch-launchd-plist.py IN OUT LABEL...` rewrites launchd.plist (binary, plistlib): renames every `_PanicOnCrash` to `_PanicOnCrasX` and gives the listed jobs `StandardOutPath`/`StandardErrorPath` `/dev/console` (devfs console is 0622, writable by mobile) plus `OS_ACTIVITY_DT_MODE=YES` (os_log mirrored to stderr) and `CFLOG_FORCE_STDERR=1`. v8 = v7 + that plist for com.apple.backboardd and com.apple.SpringBoard, written through the mount (796310 bytes, root:80 0644), SHA256 in `ios16-root-20H392-v8.img.sha256`, `.simg` ready; boot with `shim/out/shim-boot-0045n.img`. Goal: see backboardd's uncaught exception text and the window server's display probing. ODDITY (unresolved): the v5 raw-image rename at offset 0x1e025f000 is present in the v5-v7 image bytes, but the plist read through a Linux mount of v7 still had the original `_PanicOnCrash` keys; filefrag shows the file as one extent. Never patch file contents through raw offsets again; write through the mount.
- **IOMobileFramebuffer.framework reversed (step 1 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOMobileFramebuffer`.
  - Display discovery (`IOMobileFramebufferCreateDisplayList` -> `getDisplayListNumber`): iterates children of `IODeviceTree:/arm-io`, counts nodes whose `device_type` is "display-subsystem" or "ext-display-subsystem" (or `compatible` "paravirtualizedgraphics,gpu"); then `iomfb_populate_display_infos` adds an IOServiceAddMatchingNotification(IOServiceFirstMatch, **IOServiceMatching("IOMobileFramebuffer")**) and runs the run loop up to 30 s until that many services matched. `iomfb_match_callback` reads service properties `dfr`, `AppleTV`, `external` (CFBoolean) and names displays (external ones "%s-%d").
  - `IOMobileFramebufferOpen(service, task, type, &fb)`: `__ioMobileFramebufferAlloc` (CFPreferences com.apple.iokit.IOMobileGraphicsFamily: canvas_width/height, override_display_width/height, override_panel_width/height, benchmark, enable_ktrace), `IOServiceOpen(service, task, type)` (GetMainDisplay passes type 0), `getEDTProperty` reads `canvas-width`/`canvas-height` from the service's IODeviceTree parent, then fills a table of `kern_*` function pointers.
  - User client selectors (`kern_*` -> IOConnectCall*): 3 _kern_GetLayerDefaultSurface[ScalarMethod_stub]; 4 _kern_SwapBegin[ScalarMethod_stub]; 5 _kern_SwapEnd[StructMethod_stub]; 6 _kern_SwapWaitWithTimeout[ScalarMethod_stub] _kern_SwapWait[ScalarMethod_stub]; 7 _kern_GetID[ScalarMethod_stub]; 8 _kern_GetDisplaySize[ScalarMethod_stub]; 10 _kern_SetTVOutMode[ScalarMethod_stub]; 11 _kern_SetWSSInfo[ScalarMethod_stub]; 12 _kern_RequestPowerChange[ScalarMethod_stub]; 13 _kern_EnableStatistics[ScalarMethod_stub]; 14 _kern_SetFlags[ScalarMethod_stub]; 15 _kern_SetDebugFlags[ScalarMethod_stub]; 16 _kern_SetTVOutSignalType[ScalarMethod_stub]; 17 _kern_SetGammaTable[StructMethod_stub]; 18 _IOMobileFramebufferIsMainDisplay[ScalarMethod_stub]; 19 _kern_SetWhiteOnBlackMode[ScalarMethod_stub]; 20 _kern_SwapSignal[ScalarMethod_stub]; 22 _kern_SetDisplayDevice[ScalarMethod_stub]; 23 _kern_SetDigitalOutMode[ScalarMethod_stub]; 24 _kern_GetHDCPDownstreamState[StructMethod_stub]; 25 _kern_GetDigitalOutState[ScalarMethod_stub]; 26 _kern_printDisplayRegs[ScalarMethod_stub]; 27 _kern_GetGammaTable[StructMethod_stub]; 28 _kern_GetDotPitch[ScalarMethod_stub]; 29 _kern_GetDisplayArea[StructMethod_stub]; 30 _kern_EnableDisableDithering[ScalarMethod_stub]; 31 _kern_SetUnderrunColor[ScalarMethod_stub]; 33 _kern_EnableDisableVideoPowerSavings[ScalarMethod_stub]; 34 _kern_SetVideoDACGain[ScalarMethod_stub]; 35 _kern_SetLine21Data[ScalarMethod_stub]; 41 _IOMobileFramebufferEnableMirroring[ScalarMethod_stub]; 42 _IOMobileFrameBufferGetMirroringCapability[StructMethod_stub]; 43 _IOMobileFramebufferSetRenderingAngle[StructMethod_stub]; 44 _IOMobileFramebufferSetMirrorContentRegion[StructMethod_stub]; 47 _kern_HDCPSendRequest[ScalarMethod_stub]; 48 _kern_HDCPGetReply[ScalarMethod_stub]; 49 _kern_SurfaceIsReplaceable[ScalarMethod_stub]; 50 _kern_SetBrightnessCorrection[ScalarMethod_stub]; 51 _kern_SetColorRemapMode[ScalarMethod_stub]; 52 _kern_SwapCancel[ScalarMethod_stub]; 53 _IOMobileFramebufferSetCanvasSize[ScalarMethod_stub]; 54 _IOMobileFramebufferGetMatrix[Method_stub]; 55 _IOMobileFramebufferSetMatrix[Method_stub]; 56 _IOMobileFramebufferKernelTests[StructMethod_stub]; 57 _kern_GetColorRemapMode[ScalarMethod_stub]; 66 _kern_SPLCSetBrightness[ScalarMethod_stub]; 67 _kern_SPLCGetBrightness[ScalarMethod_stub]; 68 _kern_SetParameter[Method_stub]; 69 _kern_SwapSetColorMatrix[StructMethod_stub]; 72 _kern_DisableNotifications[ScalarMethod_stub] _kern_EnableNotifications[ScalarMethod_stub]; 73 _kern_ChangeFrameInfo[ScalarMethod_stub]; 74 _kern_SupportedFrameInfo[Method_stub]; 75 _kern_FactoryPortal[StructMethod_stub]; 77 _kern_GetCurrentAbsoluteTime[ScalarMethod_stub]; 78 _kern_SetBlock[Method_stub]; 79 _kern_GetBlock[Method_stub]; 80 _kern_GetLinkQuality[ScalarMethod_stub]; 81 _kern_SwapCancelAll[ScalarMethod_stub]; 82 _kern_GetProtectionOptions[ScalarMethod_stub]; 83 _kern_CopyLayerDisplayedSurface[ScalarMethod_stub]; 84 _kern_SetContrast[StructMethod_stub]; 85 _kern_GetBufBlock[Method_stub]; 86 _IOMFBGainEncoderFinishEncoding[Method_stub]; UNKNOWN {}
  - => Kernel needs: an `/arm-io` child node with device_type "display-subsystem" (+ canvas-width/height) and an IOService of class **IOMobileFramebuffer** (subclass name match) attached under it, opened with type 0, implementing at least GetDisplaySize(8), IsMainDisplay(18), GetID(7), GetLayerDefaultSurface(3), SwapBegin(4)/SwapEnd(5 struct)/SwapWait(6), Enable/DisableNotifications(72), frame info(73/74), CopyLayerDisplayedSurface(83), plus IOSurface for the swapped buffers. NEXT: CoreAnimation's AppleDisplay/IOMFBDisplay usage of these (constructor, first swap, vsync), then IOSurface.framework's IOSurfaceRoot selectors.
- **GUI PLAN (decided 2026-10-16): CPU rendering + kernel IOMobileFramebuffer.** User: CPU rendering is fine for now (goal = prove the GUI runs), may later research an Apple-style GPU driver for the Adreno 618 -> keep display output and renderer separable. QuartzCore findings (symbols from the cache's .symbols subcache: `ipsw dyld symaddr research/dsc/dyld_shared_cache_arm64 --image QuartzCore`; strings: `ipsw dyld macho ... QuartzCore --strings`):
  - Renderers: `sw_new_context` (CA::OGL::SWContext, full CPU renderer incl. blur, images, IOSurface-backed surfaces, make_buffer_current(WindowServer::Surface)), `gles_new_context`, `metal_new_context`, `new_null_context`. Env knobs include CA_ENABLE_OGL, CA_DISPLAY_TYPE, CA_FORCE_LOCAL_SERVER, CA_FORCE_TIMER, CA_CANVAS_WIDTH/HEIGHT, CA_MAIN_DISPLAY_SCALE/ORIENTATION, CA_DISABLE_RENDER, CA_ALWAYS_RENDER (selection logic not traced yet).
  - `-[CAWindowServer _detectDisplays]` (0x1884e3150) iterates static tables `open_funcs`/`count_funcs` = AppleCLCDDisplay, AppleRGBOutDisplay, AppleWirelessDisplay only; each `count()` = `display_count_of_type(IOMFBDisplayType)` over **`IOMobileFramebufferCreateDisplayList()`**, `open(i)` = `create_display_of_type` -> `CA::WindowServer::AppleDisplay(CFString name, ..., IOMFBDisplayType)` (an IOMFBDisplay driven by IOMFBServer: vsync_callback, frame_info_callback, swaps). `CA::WindowServer::VirtualDisplay` exists but is never auto-detected (explicit API only). => the main display MUST come from an IOMobileFramebuffer kernel service.
  - NEXT: reverse IOMobileFramebuffer.framework: IOMobileFramebufferCreateDisplayList / IOMobileFramebufferOpen (service matching, display type property), the user-client selectors used by IOMFBServer (display size/modes, swap begin/set-layer/end with IOSurface ids, vsync notifications), then the IOSurface kernel side (IOSurfaceRoot user client: create/lookup/lock, memory mapping) that both IOMFB swaps and CA surfaces need. Kernel: SWEET IOMobileFramebuffer service that copies the swapped IOSurface to the 1080x2400 framebuffer at 0x9c000000.
- **logd fixed; backboardd = DISPLAY WALL (boot 0045n on v7, 21:08:57 UTC, frames VID_20261016_230850 00:55-01:28, slide 0x4c3ec000, `research/crash-0045n-v7.txt`).** No `CRASH logd` any more; new daemons run (runningboardd, WirelessRadioManagerd, networkserviceproxy, dasd, cloudpaird, magicswitchd, atc, askpermissiond, ManagedSettingsAgent, containermanagerd uses AKS 0/17). New: locationd sig 11 at CFRunLoopAddSource+132 (NULL source) from its main+0x2af138 in a dispatch_once. **backboardd** sig 6: abort <- std::terminate <- _objc_terminate <- dispatch_once <- main+0x9334 = the "start %{public}@..." wrapper (0x100009264) for **"window server"** (block 0x10000a770 -> dispatch_once body 0x10001d7f0 = StartWindowServer): MTLSetShaderCachePath, `[CAWindowServer serverWithOptions:@{kCAWindowServerDisableUpdatesOnMainDisplay: @YES}]`, `setRendererFlags:`, **`mainDisplay`** -> `displayId` -> `[CADisplay displayWithDisplayId:]`, setWindowServerDisplay:, bounds, pointScale, currentMode, preferredScale, nativeOrientation, touchScales... With no display, mainDisplay is nil and it throws. (HID system and GS start fine before it.) SpringBoard dies on the same missing display (FBSDisplayMonitor). QuartzCore display backends (from the cache's .symbols subcache, `ipsw dyld str`): `CA::WindowServer::AppleCLCDDisplay` (IOMobileFramebuffer hardware) and **`CA::WindowServer::VirtualDisplay`** (open(), new_server, clone_update). `CA_DISPLAY_TYPE` and `CA_FORCE_LOCAL_SERVER` strings exist in QuartzCore but have no direct xref (likely read via a table of CA_* env names). NEXT: find how CA::WindowServer::Server enumerates displays (IOMobileFramebuffer services vs VirtualDisplay / CA_DISPLAY_TYPE) and choose: (a) kernel IOMobileFramebuffer service + IOSurface, or (b) a VirtualDisplay rendered by CA's CPU path (CA::OGL::SW) and copied to the framebuffer.
- **watchdogd + CommCenter + identityservicesd fixed (boot 0045n on v6, 20:41:30 UTC, frames VID_20261016_224133 00:39-01:13, slide 0x20fec000).** `SWEET WDT: watchdogd sel 0 in 0/0 out 1/0 -> ok`, no watchdogd/CommCenter/identityservicesd crashes (CommCenter/identityservicesd now call AKS 0/17/7). usermanagerd boot task runs (UMD_INIT creates primary session uid 501 "mobile", personas 99 System and 199 Default registered with the kernel); `Failed to atomically swap writing file /private/var//keybags/usersession.kb: Operation not supported (45)` (HFS lacks the atomic swap rename; non-fatal so far). Remaining: SpringBoard (FBSDisplayMonitor, no display), backboardd (uncaught exception), mediaserverd, lockdownd (crash inside obfuscated code, main+0x4c1b4?), logd. **logd**: main+0x18160 = `BUG IN LIBTRACE: Failed to migrate directory` from 0x10001801c(dir 0): 0x100016538 requires the dir to exist with uid/gid/mode from the table at 0x10002df14 (0: /private/var/db/diagnostics 272:272 0751, 1: /private/var/db/uuidtext 272:272 0755, 2: /private/var/db/diagnostics/timesync 272:272 0755) plus a `.migrated` file, else XPC "operation 6" to a helper and crash. **Image v7** = v6 + those dirs/markers (also in mkiosroot.sh), SHA256 in `ios16-root-20H392-v7.img.sha256`, `.simg` ready. Boot with `shim/out/shim-boot-0045n.img`.
- **Exit-reason text works (boot 0044n, frames VID_20261016_221726 00:32-00:53, slide 0x22d5c000).** CommCenter/identityservicesd: `couldn't dlopen libobjc-trampolines.dylib: dlopen(/usr/lib/libobjc-trampolines.dylib, 0x0106): tried: '/usr/lib/libobjc-trampolines.dylib' (no such file, not in dyld cache)`: the file only exists in the OS cryptex and dyld does not search cryptexes at ignition_level=1. 19 cryptex files have no root counterpart (OS: DriverKit dyld cache + .symbols, VisualTestKit, libffi-trampolines, libobjc-trampolines, libstdc++ 6.0.9 + 2 symlinks; App: SafariSupport.bundle, AuthenticationServicesAgent, com.apple.Safari.History, passwordbreachd, safarifetcherd, webbookmarksd, webinspectord, webpushd) -> hard-linked (symlinks copied: Linux hfsplus cannot hard-link symlinks); `tools/mkiosroot.sh` does it generically. watchdogd still crashed: the `-wdt_no_pmu` / `/arm-io/wdt no-pmu` check (0x10000a01c) only runs when os_variant_has_internal_diagnostics; otherwise 0x100009b18 looks up IOServiceMatching("IOService") + IOPropertyMatch {IOPMUPrimary: 1} (NSConstantDictionary) and crashes if absent, then IORegistryEntrySetCFProperty(boot stage 0xff) (failure only logged). Patch **0045**: IOWatchdog sets IOPMUPrimary = 1 and accepts setProperties; CI `38961814194` -> `xnu/out-0045`. **Image v6** = v5 + the 19 cryptex links, SHA256 7c610c6a...297a, `ios16-root-20H392-v6.simg` (full flash needed). Boot image `shim/out/shim-boot-0045n.img` (0045 + sweet-0044hi.adt). Still open: SpringBoard (no display), backboardd (uncaught exception, reason none), mediaserverd (Fig NULL dict), logd (main+0x18160), lockdownd (main+0x4c1b4).
- **System survives daemon crashes (boot 0043p, video frames VID_20261016_215006 00:25/00:42/00:54).** With the v5 plist patch no panic: launchd keeps launching (runningboardd, powerd, tccd, healthd, peopled, cryptexd, routined, aksd, mediaremoted, familynotificationd, nanoprefsyncd, navd, cloudpaird, passd, amsaccountsd, accessoryupdaterd, wcd ...). Shared cache slide that boot 0x406b4000 (`research/crash-0043p.txt`). watchdogd still crashed at main+0xb82c from the PMU step: `-wdt_no_pmu` is only honored when os_variant_has_internal_diagnostics; customer builds read `no-pmu` from IODeviceTree:/arm-io/wdt (IORegistryEntrySearchCFProperty) -> mkadt adds /arm-io/wdt {no-pmu}. **cryptexd** sig 6: abort_with_payload <- os_crash_fmt <- `__darwin_arm_secure_boot_chip.cold.7` ("panic: failed to get crypto algorithm") <- `_img4_chip_select_personalized_ap`: reads /chosen use-ddi-secure-boot, allow-ecid-mismatch, crypto-hash-method ("sha1" | "sha2-384"), /chosen/asmb lp-smb0/1 -> mkadt adds crypto-hash-method "sha2-384". **identityservicesd** sig 6 = same `_objc_fatal` in imp_implementationWithBlock as CommCenter (block trampolines; message needed). lockdownd sig 5 at main+0x4c1b4. Patch **0044**: crash report adds `CRASH name reason ns N code X: text` (exit reason description = abort_with_payload text). CI `38959096811` -> `xnu/out-0044`, QEMU OK. Image `shim/out/shim-boot-0044n.img` = 0044 + `shim/out/sweet-0044hi.adt` (wdt no-pmu, crypto-hash-method) + cmdline (wdt=-1, no -wdt_no_pmu).
- **First symbolicated daemon crashes (boot 0043u on journaled v4, 19:38:53 UTC; video frame chat image 36; transcribed in `research/crash-0043u.txt`; shared cache slide 0x40d60000 for every process).** (Gemini's video transcription invented values, use frames only.) `tools/munch-crash.py research/dsc/dyld_shared_cache_arm64 x research/crash-0043u.txt`:
  - **watchdogd** pc main+0xb82c (`_os_crash` trap helper) from 0x100009bb0 = set PMU boot stage via IORegistry (no PMU) -> fix: boot-args `wdt=-1 -wdt_no_pmu` (Apple's own test mode writes "%s wdt=-1 -wdt_no_pmu"; watchdogd logs "skipping setting PMU boot stage because -wdt_no_pmu found in boot-args or no-pmu found in EDT"). Kernel effect of wdt=-1: sched hygiene debug off, zone_exhausted_timeout -1 (harmless).
  - **SpringBoard** sig 5 in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` <- `+[FBDisplayManager sharedInstance]` <- `FBSystemShellInitialize` <- `SBSystemAppMain`: no main display. GUI WALL: display service (IOMobileFramebuffer / CoreAnimation display).
  - **backboardd** sig 6: abort <- std::terminate <- _objc_terminate (uncaught exception; message not shown).
  - **CommCenter** sig 6: `_objc_fatal` in `imp_implementationWithBlock` (from `+[_PFRoutines initialize]`, CoreData): objc block trampolines cannot be allocated -> likely kernel gap (trampoline page remap); will hit many processes.
  - **mediaserverd** sig 11: CFDictionaryGetValue(NULL) in FigNotificationListenerStartNotifications (routing session manager).
  - **logd** sig 5 at its main+0x18160 (bt main+0x9dfc); lockdownd sig 5 (not yet symbolicated).
  - Image v5 = v4 + launchd.plist (`/System/Library/xpc/launchd.plist`, contiguous at image offset 0x1e025f000, 1456980 bytes) with the 7 `_PanicOnCrash` keys renamed `_PanicOnCrasX` in place (raw image patch, no metadata change) -> `diff-v4-v5.simg` = 7 blocks; v5 SHA256 78f96974...185a. Boot image `shim/out/shim-boot-0043p.img` = 0043 + `wdt=-1 -wdt_no_pmu`.
- **0043 console crash reports (QEMU verified); image `shim/out/shim-boot-0043u.img`; NEXT: flash v4 + boot 0043u.** Boot 0042u (video photo IMG_20261016_211646): short exec lines work; watchdogd crashes with NO `SWEET WDT:` line (fails before its first IOWatchdog call: lookup or IOServiceOpen, or elsewhere); lockdownd, SpringBoard, mediaserverd, CommCenter, identityservicesd, backboardd, watchdogd all crash repeatedly ("Too many corpses being created" = corpse limit, no crash reports). AKS callers: lockdownd (0, 7), imagent/sleepd (0, 17, 7), backboardd (0, 17), UserEventAgent (0, 7, 102, 103 -> unsupported). Patch **0043**: `sweet_crash_log` (osfmk/kern/exception.c, called from proc_prepareexit for crash exits on the current proc) prints `CRASH name[pid] sig S code (exc<<32|code) sub X slide SR mh MH`, `CRASH name pc lr sp fp far`, `CRASH name bt` (10 fp frames); AKS sel 103 notify_state_is_set -> 0. CI `38954520419` -> `xnu/out-0043`. QEMU test: init forks, child `brk #1`: `CRASH launchd[2] sig 5 code 0x600000001 sub 0x1000004f0 slide 0x0`, pc correct, parent keeps running (pid 1 crashes panic before proc_prepareexit, so test with a child). `tools/munch-crash.py CACHE ROOT [report]` symbolicates typed-in CRASH lines (`ipsw dyld a2s` with addr - slide; main binary offsets = addr - mh + 0x100000000).
- **v3 reflashed 18:50-18:55 UTC (`research/flash-userdata-v3b.log`, 24/24 OKAY); 0042u booted 19:01:17 UTC on the clean disk (`research/boot0042u-2.log`), SLOW MO VID pending (watchdogd failure reason).** ABL hangs on the first `fastboot boot` after every big flash; user re-enters fastboot. User gave standing approval to reflash/boot without asking (memory standing-flash-approval). Commits now end with `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>` (user request; never a Claude trailer).
  - watchdogd flow: connect_to_kext (0x100005408): IOServiceGetMatchingService("IOWatchdog") fails -> "failed to discover" + _os_crash; IOServiceOpen(type 1) fails -> "IOServiceOpen failed with error : 0x%x" + _os_crash. Then sel 0 (out 1 scalar): 0 -> "userspace monitoring disabled", 0x100009bb0, sleep 2, exit(0); nonzero -> "userspace monitoring enabled", normal operation.
  - **Image v4 (JOURNALED):** `tools/mkiosroot.sh` now does `mkfs.hfsplus -J`, mounts with `-o loop,force`, adds the /System/Library/Caches/com.apple.dyld hard links (44) and /private/var/keybags/systembag.kb (0700/0600 root). Built from the IPSW dmgs (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047) -> `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v4.img` SHA256 fba4aeae...ec0f9 + `.simg`. Header: attr 0x80002100 (unmounted, journaled), journal info block 129, journal 8 MiB at 532480 flagged need-init. Linux `fsck.hfsplus -n` reports "Volume header needs minor repair" on any fresh journaled volume even after -y (quirk). QEMU: 64 MB journaled test root (`rd=md0 rootrw=1`, 0042 kernel): `hfs: Initializing the journal (joffset 0x3000 sz 0x80000)` -> mounted -> init runs, no panic. NEXT: flash v4 (full), boot 0042u.
- **SPRINGBOARD STARTS (boot 0041u 18:39:53 UTC, photo IMG_20261016_204142).** backboardd, bluetoothd, fairplayd.H2, SpringBoard (pid 111, crashed: "Too many corpses"), xpcproxy run; `SWEET AKS: backboardd sel 0 -> 0`, `sel 17 in 1/0 out 0/4096 -> 0`. Panic: watchdogd[113] exited, namespace 2 subcode 5 (SIGTRAP = its _os_crash): watchdogd crashes on purpose when IOServiceGetMatchingService("IOWatchdog") fails ("connect_to_kext() unexpectedly failed") or IOServiceOpen(type 1) fails ("IOServiceOpen failed with error"); IOWatchdog DOES register (QEMU msgbuf: `SWEET WDT: IOWatchdog registered`), cause not on screen yet -> need video. Note `wdt_test=no_user_checkin` makes watchdogd log "detected testing boot-args to not run, exiting" (path not reached here?). **HFS CORRUPTION:** `hfs_swap_BTNode: offset #0 invalid (0xBC1D) (blockSize 0x2000 numRecords 81)`, `node=19677 fileID=4` (catalog), `hfs_mark_inconsistent` repeated. Likely cause: non-journaled HFS + unclean panics while writing (0038e wrote a 2.7 GB core then panicked; every boot since ends in panic). Plan: reflash v3 (clean), longer term HFS journaling. Patch **0042**: one short `exec NAME` console line instead of the 3-line "Allowing spawn of iOS binary" notice; CI `38949451276` -> `xnu/out-0042`; image `shim/out/shim-boot-0042u.img` (same cmdline as 0041u).
- **LAUNCHD DAEMONS RUN (boot 0039u, 18:29:13 UTC, photo IMG_20261016_203052).** With `launchd_unsecure_cache=1` all boot tasks pass and launchd starts LaunchDaemons (pids > 100: identityservicesd 114 and lockdownd 106 crash, "Too many corpses being created"). Fatal: `watchdogd: ERROR: failed to discover watchdog KEXT service` -> exit (namespace 2 = signal, subcode 5) -> `_PanicOnCrash` panic. watchdogd: IOServiceMatching("IOWatchdog"), IOServiceOpen type 1, IOConnectCallScalarMethod sel 0/1 (check in)/3/4 (disable/enable userspace monitoring)/5, IOConnectCallStructMethod sel 2 (1 KiB in) and 9 (small out); VM path ("detected virtual machine environment and no watchdog KEXT found, exiting...") only when it detects a VM. Console also flooded with `hfs: set hardlink bit on vol=iOSRoot cnid=321318` (Linux-made hard links lack kHFSHasLinkChainBit; repaired only in memory on each lookup). Patches **0040** (`iokit/Kernel/arm/SweetWatchdog.cpp`: IOWatchdog + IOWatchdogUserClient, all selectors succeed with zeroed outputs, each logged once as `SWEET WDT: ...`) and **0041** (sweet_hfs hfs_catalog.c: the three link-repair printfs limited to 8 per boot); CI `38948245372` PASSED -> `xnu/out-0041` (QEMU regression OK). Image `shim/out/shim-boot-0041u.img` (0041 + sweet-0038hi.adt + ufsrw rootrw + launchd_unsecure_cache=1) booted 18:39:53 UTC (`research/boot0041u.log`), photo pending.
- **v3 FLASHED + keybag passes; next wall AMFI (launchd_cache_loader).** User said "flash, nothing to lose": `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg` 17:51:52-17:57:01 UTC, 24/24 chunks OKAY (`research/flash-userdata-v3.log`). userdata = v3 (SHA256 fa84274c...22df) until the next rw boot. ABL fastboot then hung ("Sending" forever, `reboot bootloader` timed out) until the user re-entered fastboot by hand (common Xiaomi ABL crash). Boot 0038e 18:06:15 UTC (`research/boot0038e-3.log`), photos IMG_20261016_200812/200838: keybag OK, then boot tasks tzinit (`Bad zoneinfo link`, resets to system partition, OK), finish-restore, finish-demo-restore, sysstatuscheck, prng_seedctl (`failed to load kernel prng seed`, `failed to load virtual random (-147)`, AllowCrash), auearlyboot, then **launchd_cache_loader: `abort: Unable to find AMFI`** -> 2.7 GB core written to /cores in 91 s -> launchd panic `launchd_cache_loader[22] exited -- exit reason namespace 7 subcode 0x1`. launchd_cache_loader: SecStaticCodeCheckValidity of /System/Library/xpc/launchd.plist with detached .sig, then IOServiceMatching("AppleMobileFileIntegrity") + IOServiceOpen type 0 + IOConnectCallMethod sel 6 (1 scalar = 1, struct in = cdhash) = trust the cdhash, then sends the cache to launchd. Boot-arg `launchd_unsecure_cache=1` skips signature + AMFI ("Using unsecure cache"); the value is parsed with strtoul base 10 (helper 0x100002da8), so a path = 0 = off: boot 0039w (18:17:13 UTC, `=/System/Library/xpc/launchd.plist`, photo IMG_20261016_202008) still aborted `Unable to find AMFI` (core dump now skipped: `Failed to generate core file ... error 14`, so 0039 works). Image `shim/out/shim-boot-0039u.img` uses `launchd_unsecure_cache=1`. Patch **0039**: do_coredump = 0 on SWEET (kern.coredump sysctl still works); CI `38945267341` -> `xnu/out-0039` (_do_coredump now in __common). Image `shim/out/shim-boot-0039w.img` = 0039 + sweet-0038hi.adt + ufsrw rootrw + `launchd_unsecure_cache=/System/Library/xpc/launchd.plist`. TODO later: real AppleMobileFileIntegrity IOService (sel 6 trust cdhash etc.).
- **Image v3 ready, awaiting user "flash".** Boot 0038w 17:38:10 UTC, photo (chat image 30): `SWEET AKS: keybagd sel 0 -> 0x0`, `sel 14 out 1/0 -> 0x0` (x2), `MKB_INIT: mount is with NO CPROTECT ... does not support Data Protection`, `System Keybag loaded`; then keybagd still opens /private/var/keybags/systembag.kb (0x100012d3c, for BackupKeyBagKeys) -> ENOENT, `no-effaceable-storage` not set -> `Can't load the keybag. tears in rain... Time to die` -> `FATAL KEYBAG ERROR: kb_load` -> reboot -> Halt/Restart Timed Out panic. Fix: ADT `/defaults/no-effaceable-storage` (mkadt.py; `shim/out/sweet-0038hi.adt`, `research/q0038-hi.adt`, QEMU OK) + file `/private/var/keybags/systembag.kb` (0600 root, dir 0700; XML plist {KeyBagKeys: data}). **v3 image** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v3.img` = v2 + that file, fsck OK, SHA256 fa84274c...22df; `ios16-root-20H392-v3.simg` (img2simg: zero blocks become FILL chunks, so a full flash overwrites every block the rw boots changed). Flash: `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg`, then boot `shim/out/shim-boot-0038e.img` (0038 + sweet-0038hi.adt + ufsrw=1 rootrw=1).
- **0038 AppleKeyStore (CI 38938744130 PASSED -> `xnu/out-0038`; QEMU msgbuf: `SWEET AKS: AppleKeyStore registered (no passcode, unlocked)`; phone image `shim/out/shim-boot-0038w.img` booted 17:38:10 UTC, `research/boot0038w.log`, photo pending).** QEMU full kernel log trick (fb.txt is only the last screen): `llvm-nm KERNEL | grep _msgbufp` -> `--hmp "x /1gx ADDR"` gives struct msgbuf address (static, 0xfffffe0007b56678 on 0038) -> `--hmp "x /3gx STRUCT"` gives bufc (3rd quad) -> `--hmp "memsave BUFC 0x20000 \"FILE\""` (quote the file name), `tr -d "\0" < FILE`. Same inputs give the same addresses. Boot 0037w 17:08:35 UTC, photo IMG_20261016_191052: bindfs fixed mount; mount-phase-1/2 pass; `cc_fips_test` boot task: all corecrypto FIPS POST tests PASSED in userspace; then keybag boot task `keybagd --init`: `MKB_INIT: FATAL KEYBAG ERROR: failed to load system bag`, `Could not get options entry from the device tree` (x2, IODeviceTree:/options = NVRAM, non-fatal), `REBOOTING INTO RECOVERY MODE` -> reboot -> panic `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` (no restart path on SWEET).
  - keybagd (static libaks): load_devicebag (0x100012c30) calls aks_get_system (sel 14, 0-1 scalar in, 1 out) and loads /private/var/keybags/systembag.kb only if the result is 0xe00002f0 (NotFound); any other error -> fatal; SUCCESS -> skip the file (userspace-reboot path). systembag.kb is a plist {KeyBagKeys}; decrypted with effaceable storage unless IODeviceTree:/defaults has `no-effaceable-storage`. Image has no /private/var/keybags. has_data_protection: statfs of /private/var; no cprotect on our HFS -> device-bag volume mapping skipped.
  - libaks connection: IORegistryEntryFromPath / IOServiceMatching("AppleKeyStore"), IOServiceOpen type 0, then selector 0 (no args) must succeed.
  - **AppleKeyStore selector map (iOS 16.7 AppleKeyStore.framework, via `ipsw dyld disass`):** 0 open, 2 create_bag, 3 save_bag, 4 unload_bag, 5 set_system_with_passcode, 6 load_bag, 7 get_lock_state (1 in, 1 out; bit 2 = unlocked since boot), 9 unlock_device, 10 wrap_key, 11 unwrap_key, 12 unlock_bag/test_stashed_kek, 13 lock_bag, 14 get_system, 15 change_secret_se, 16 internal_state, 17 get_device_state (1 in, struct out <=4K: DER SET of SEQUENCE{UTF8 key, INTEGER}: ss state, sls lock_state (MKBGetDeviceLockState returns it), sb backoff, sbo, sfa failed attempts, sfao, sgs generation, sas assertions, sgpe grace, srcd countdown, sr reserved), 18 recover_with_escrow_bag, 19 obliterate_class_d, 20 drain_backup_keys, 21/22 set/clear_backup_bag, 23 get_bag_uuid, 24 rewrap_key_for_backup, 26/27 assert_hold/drop, 30/31 set/get_configuration, 33-37 stash, 38 auth_token_create, 40 generation, 41 fdr_hmac_data, 42 verify_password, 43 _aks_operation, 44 remote_session, 49/50 signing keys, 51/53/54 stash enable/persist/escrow, 55 unload_session_bags, 57/58 remote peer, 59-62 smartcard, 63 migrate_s_key, 65 set_keybag_for_volume_with_cookie_persona, 66/67 sync bags, 68 copy_volume_cookie_persona, 69-73 se_*, 74-79/84/85/99 fv_* (FileVault/VEK), 80/81 absinthe, 82 make_public_backup_bag, 83 drop_auxiliary_auth, 86 kext get/set options, 87 invalidate_bag, 88-91/94/95/106/107 se_*, 92 lower_iteration_count, 93 get_internal_info, 96-98/117 keybag_persona create/list/delete, 100/101 assert promote/consume, 102/103 notify_state set/is_set, 104 fairplay, 105 change_secret_epilogue, 108 delete_xart_leak, 110-113 backup volume, 114 sealed_hashes_set, 115 prewarm_sps, 118-128 AKSIdentity*/AKSGetLastUser.
  - Patch **0038**: `iokit/Kernel/arm/SweetKeyStore.cpp` (class AppleKeyStore on IOResources, IOUserClientClass AppleKeyStoreUserClient; personality in KernelConfigTables.cpp): 0/4/5/9/12/13/26/27/55/102 success, 14 -> handle 0, 7 -> 0x4, 17 -> DER state (ss 4, sls 0), others kIOReturnUnsupported; logs `SWEET AKS: <proc> sel N arg0 in a/b out c/d -> ret` for every call.
  - Tools: shared cache copied to `research/dsc/` (git-ignored; ipsw needs a writable dir for its .a2s symbol cache); `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image AppleKeyStore|MobileKeyBag`; scratchpad `annot2.py BIN DIS FIXUPS` annotates llvm-objdump output with strings and import names (`ipsw macho info --fixups`).
- **0037 bindfs (phone boot 0037w pending photo).** Boot 0036mw 16:57:47 UTC, photo IMG_20261016_190102: WRITABLE ROOT WORKS: `/dev/ufsroot0 on / (hfs, local, noatime)` after mount_hfs update (GetMasterBlock EBUSY harmless), coredump of mount written to /cores (rw). Then `mount: failed to mount /private/preboot/5CBE3601.../usr/standalone/firmware -> ...: Operation not supported by device (19)` -> exit 74 -> launchd panic `mount[2] exited`. Cause: the macOS-derived config had nullfs but no bindfs (ENODEV = unknown fs type). Caller passes env 0 -> bind not required, ENOENT (missing source dir) tolerated. Patch **0037** adds `bindfs` to FILESYS_BASE in MASTER.arm64.sweet; CI `38936550483` -> `xnu/out-0037` (bindfs_vfsops present; QEMU probe regression OK). Image `shim/out/shim-boot-0037w.img` (0037 + sweet-0036hi.adt + ufsrw=1 rootrw=1) booted 17:08:35 UTC (`research/boot0037w.log`). NOTE: userdata has now been modified by rw boots; it no longer equals the v2 image, so sparsediff with v2 as OLD is not safe any more (full reflash or a new baseline needed).
- **0036 phone: launchd boot tasks run; mount -P 1 needs /chosen/boot-manifest-hash.** Boot 0036hr 16:15:32 UTC, photo IMG_20261016_185236: platform check passes; fsck boot task runs (`fsck: not booting main or diagnostic OS. Skipping fsck on OS container`); mount-phase-1: `failed to get filesystem info` x2 (no IODeviceTree:/filesystems/fstab, non-fatal), `mount: data volume missing, but not required in env: 0`, mount_hfs update of /: `GetMasterBlock: Error 16 opening /dev/ufsroot0` (EBUSY, mounted block dev, non-fatal) then mount() EROFS (read-only boot) -> `mount: / failed with 1`; then `failed to get boot-manifest-hash` -> errx exit 66 -> launchd panic `mount[3] exited`. /sbin/mount: fn 0x100005f90 reads IODeviceTree:/chosen boot-manifest-hash (hex), bindfs-mounts /private/preboot/<HEX>/usr/standalone/firmware (ENOENT tolerated unless required). `tools/mkadt.py` now adds `boot-manifest-hash` = SHA-384("sweet boot manifest") (5CBE3601...5205). ADTs `shim/out/sweet-0036hi.adt`, `research/q0036-hi.adt` (QEMU probe OK, csops 0x26000001). Image `shim/out/shim-boot-0036mw.img` (0036 + new ADT + ufsrw=1 rootrw=1), booted 16:57:47 UTC (`research/boot0036mw.log`): FIRST WRITABLE BOOT of userdata (restore source: v2 image on SSD).
- **0036 CS_SIGNED (phone boot 0036hr pending photo).** Phone boot 0035hr 2026-10-16 15:57:02 UTC (`research/boot0035hr.log`, photo IMG_20261016_180249): highmem works on hardware; shared cache maps (trust-cache warnings only), Apple launchd runs (`Darwin Bootstrapper Version 7.0.0`, `entering ondemand mode`), starts boot task fsck, then userspace panic `boot task is not a platform binary (fsck, /sbin/fsck)`. launchd+0x3fea0: `csops(pid, CS_OPS_STATUS)` must have bit 0x04000000. Cause: nothing set CS_SIGNED (AMFI does it in vnode_check_signature), so csproc_get_blob() returns NULL for every process and csops never reports CS_PLATFORM_BINARY (also breaks per-process cdhash/entitlement queries). QEMU probe init (`csops` status printer, scratchpad csq/) on 0035: `0x02000001`; patch **0036** (`*cs_flags |= CS_SIGNED` in the SWEET AMFI hook), CI `38929635791` -> `xnu/out-0036`: `0x26000001`. Images `shim/out/shim-boot-0036hr.img` / `-0036hw.img` (same cmdlines as 0035). 0036hr booted 16:15:32 UTC (`research/boot0036hr.log`). Next launchd boot tasks (embedded __config plist): fsck -qL and mount -P 1/2 (RequireSuccess); no /etc/fstab on the image.
- **0035 ENTITLEMENTS + PLATFORM BINARIES (QEMU verified).** `xnu/patches/0035-munch-amfi-entitlements-platform-binaries.patch`: new `iokit/Kernel/arm/SweetEntitlements.cpp` (OSObject subclass: OSDictionary from OSUnserializeXML of the signature's XML entitlements + original XML for CS_OPS_ENTITLEMENTS_BLOB); `bsd/kern/sweet_amfi.c` answers queryEntitlementBoolean/String(+WithProc, string also matches array members), copyEntitlementAsOSObject(+WithProc, retained), asdict (retained), get_xml (kalloc_data copy); built-in MAC policy "SWEET AMFI" (registered from bsd_init before vfs/exec) with mpo_vnode_check_signature: every signed binary on the MNT_ROOTFS mount gets CS_PLATFORM_BINARY (stand-in for AMFI's static trust cache: iOS system binaries are ad-hoc, cd_flags 0x2, platform byte 0), and attaches entitlements ONLY for the writable copy (cs_blob != csb_ro_addr; revalidation passes the RO blob). LESSON: first version attached in adjustContextWithoutMonitor -> QEMU `unable to adjust OSEntitlements context without monitor: 53` because ubc_cs_convert_to_multilevel_hash (4K-page signatures on 16K kernel) sets csb_entitlements_blob = NULL; ALL iOS 16 binaries (launchd, backboardd, SpringBoard) use 4K code pages, so this would have broken launchd on the phone. Boot-arg `sweet_amfi_debug=1` logs `SWEET AMFI: <ident>: N entitlements (test: ...)`. `tools/mkinit.py OBJ OUT [ENTS.plist]` adds an XML entitlements blob (special slot -5; default output byte-identical). QEMU `research/q0035-ent` (RAM disk `research/root-hfs-ent.img`, init with 3 test entitlements, highmem): `org.sweet.init: 3 entitlements (test: bool 1 string 1 array 1)` then init PID 1. MAC policies loaded: 0034 = 1, 0035 = 2 (QMP `mac_policy_list`). CI `38909807230` -> `xnu/out-0035`. Phone images: `shim/out/shim-boot-0035hr.img` (highmem, ro), `-0035hw.img` (highmem, +ufsrw=1 rootrw=1), `-0035r.img` (low mem fallback).
- **PHONE TEST ORDER (updated):** 0035hr -> if instant reset, 0035r -> then 0035hw.

- **HIGHMEM: XNU in the upper DDR bank, ~3.95 GiB (was 1.2 GiB).** Live FDT memory = <0x80000000 2G> + <0x1_00000000 0xfcc00000>; all fixed reserved-memory carve-outs are in the low bank (highest: ramdump_fb 0xaf000000, rmtfs ~0xfde01000), dynamic pools have alloc-ranges <= 0xffffffff, UFS node has no `iommus` (Linux DMAs anywhere). `tools/mkadt.py --highmem` sets /chosen dram-base 0x100000000 dram-size 0xfcc00000; `tools/mkpayload.py` now takes phys_base/mem_size from the ADT (default payloads byte-identical, verified); `tools/qemu-munch.py` sizes -m from the payload (8G for highmem). Patch **0034** (`0034-munch-ufs-dma-above-4g.patch`): UFS refuses DMA memory above 4G without UFSHCI CAP.64AS (bit 24), logs `DMA at 0x...`. CI `38905889883` success -> `xnu/out-0034`. **QEMU verified** (`research/q0034-hi`, ADT `research/q0034-hi.adt` --qemu --highmem, HFS RAM disk): phys_base 0x100000000, init PID 1 heartbeat, no panic; `max_mem` = `mem_size` = 0xfcc00000 read via QMP (`research/q0034-hi-mem`). Phone images: `shim/out/shim-boot-0034hr.img` (read-only) / `-0034hw.img` (+ufsrw=1 rootrw=1), ADT `shim/out/sweet-0034hi.adt` (asmb SIP + sweet-ufs + highmem). PHONE RISK: unknown runtime TZ/XPU protection in the upper bank would show as an instant reset; fall back to 0033r/w. Watchdog ruled out for the overnight reset: msm-watchdog bark is 20 s (qcom,bark-time 0x4e20) yet panic screens stay up for many minutes.

- **Offline research 2026-10-16 (user away, phone unavailable). Images rebuilt** `shim/out/shim-boot-0033r.img` / `-0033w.img` now also carry `wdt_test=no_user_checkin`. NEXT ON PHONE: boot 0033r (expect past in6_nigroup), then 0033w (first writes: rw root fixes HFS link-chain bits itself and gives /var for launchd sockets /var/run/syslog, /var/run/lockdown.sock).
- **launchd.plist** (`/System/Library/xpc/launchd.plist`, compiled; 532 daemons, 66 RunAtLoad/KeepAlive, list in scratchpad `early-daemons.txt`). `_PanicOnCrash` daemons: containermanagerd/usermanagerd (InternalOnly), lockdownd/remoted/trustd/remotepairingdeviced (AutomationOnly), **watchdogd unconditional** (KeepAlive SuccessfulExit=false). watchdogd needs IOKit `IOWatchdog` kext service; if absent it exits only when `kern.hv_vmm_present` (ADT `/defaults/vmm-present`, hvg_sysctl.c) or with boot-arg `wdt_test=no_user_checkin` ("detected testing boot-args to not run, exiting"; reads kern.bootargs). Using the boot-arg (vmm-present would change many daemons; keep as an option).
- **GUI path research (QuartzCore in the shared cache, `ipsw dyld str CACHE STR`, ~10 s/search):** CA::WindowServer has IOMFBServer/IOMFBDisplay (Apple DCP/IOMobileFramebuffer), AppleCLCDDisplay, AppleRGBOutDisplay, **VirtualServer/VirtualDisplay** and **MemorySurface**, EAGLServer. Renderers: CA::OGL::MetalContext, GLContext and a **CPU software rasterizer CA::OGL::SW** (samplers/blend modes; "image is too large for software renderer"). Env knobs: `CA_ENABLE_OGL`, `CA_DISPLAY_TYPE`, `CA_FORCE_LOCAL_SERVER` (249 CA_* vars). Hypothesis (unverified): backboardd's render server could run a VirtualServer/MemorySurface with the SW renderer and we copy the surface to the ABL framebuffer -> needs disassembly of CA::WindowServer server creation. Expected other GUI walls: IOSurface (closed-source kext; needs a reimplementation), IOMobileFramebuffer user client, IOHID touch events, AppleKeyStore/keybag (SEP) for lock state, MobileGestalt answers (AppleCLCD etc.).

- **0032r RESULT (boot 05:07:50 UTC, `research/boot0032r.log`, photo IMG_20261016_071200): SHARED CACHE MAPPED, LAUNCHD RUNS.** With SIP relaxed (ADT asmb) the `not in trust cache` lines are only warnings for every subcache (.31 ... .42.dyldlinkedit); no SIP rejection; libSystem loads; launchd configures networking (`skip attaching fsw to lo0 using legacy TX model`) -> kernel data abort far 0x28 in `SHA1Init` (symbolized as sha1_init) from `in6_nigroup` (IPv6 on lo0): g_crypto_funcs NULL. Kernel addresses are unslid: `~/swift/usr/bin/llvm-symbolizer --obj=KERNEL ADDR` works directly.
- **0032** BearSSL SHA-1 (`0032-munch-bearssl-sha1.patch`), **0033** BearSSL SHA-384/512 (`0033-munch-bearssl-sha384-sha512.patch`, BearSSL ctx stored in SHA512_CTX, static-asserted to fit; ubc_subr.c uses SHA-384 for code directories). `0020-stage-bearssl.sh` now also stages hash/sha1.c, hash/sha2big.c, codec/{dec,enc}{32,64}be.c. CI `38854909053` success (0032-only run `38854802073` superseded). Kernel `xnu/out-0033` (SHA1Init -> _br_sha1_init verified by disassembly; nm symbols have a leading underscore). Images (ADT sweet-0032): `shim/out/shim-boot-0033r.img` (read-only), `shim-boot-0033w.img` (+ufsrw=1 rootrw=1). Still NULL g_crypto_funcs users: des, chacha20poly1305, rsa, aesxts, rand, flow_divert.
- `research/check-patch-order.py` now skips patches touching CI-staged sweet_hfs sources (0031) instead of stopping.

- **0031r RESULT (boot 05:03:52 UTC 2026-10-16, `research/boot0031r.log`, photo IMG_20261016_070508): SHARED CACHE FOUND.** dyld now opens `dyld_shared_cache_arm64` (v2 image hard links), kernel refuses to map it: `shared_region: ... map(...'dyld_shared_cache_arm64'): not in trust cache` -> `vnode is not SIP-protected` (vm_unix.c: macOS arm64 path needs trust cache, else root-owned + SF_RESTRICTED unless CSR_ALLOW_UNRESTRICTED_FS) -> `dyld cache '(null)' not loaded: syscall to map cache into shared region failed` -> libSystem not loaded -> coredump to /cores fails error 30 (EROFS) -> panic initproc failed -> "MACH Reboot" (spins; screen kept). Lots of `hfs: set hardlink bit on vol=iOSRoot cnid=... linkid=...` spam (Linux-made hard links lack the link-chain bit; read-only mount cannot persist the fix). FIX: ADT `/chosen/asmb` with `lp-sip0`=0x6f (u64; incl. CSR_ALLOW_UNRESTRICTED_FS) and `lp-sip1`=1 (unauthenticated root), `tools/mkadt.py` -> `shim/out/sweet-0032.adt`. Images with kernel 0031: `shim/out/shim-boot-0032r.img` (read-only) and `shim-boot-0032w.img` (+ufsrw=1 rootrw=1).

- **2026-10-16 morning:** userdata now = **v2** image (`diff-v1-v2.simg` flashed 21:06:50 UTC 2026-10-15, OKAY, `research/flash-userdata-diff-v2.log`); future diffs use `ios16-root-20H392-v2.img` as OLD. Booted `shim-boot-0029k.img` (ignition_level=1) right after (`research/boot0029k-v2.log`); **no photo** (user asleep). Next morning the phone was in PixelOS recovery = some hardware reset (ABL then booted Android, which cannot use userdata). OBSERVATION: our kernel cannot reset the SoC itself (`PE_halt_restart` is never set; reboot/halt print "MACH Reboot"/"CPU halted" and spin), so the reset came from outside XNU: Qualcomm watchdog or a TZ/XPU violation. Unknown which; re-boot 0029k and watch/video until the reset. Do NOT tap factory reset/format data in recovery (would wipe the iOS image).
- **0030** (`xnu/patches/0030-munch-ufs-writes.patch`, CI `38852655542`): UFS WRITE(10) + read-modify-write for partial 4 KiB blocks + SYNCHRONIZE CACHE (DKIOCSYNCHRONIZE), only with boot-arg `ufsrw=1`; `ufs_write_blocks` re-checks partition bounds before every write (only place data goes out).
- **0031** (`xnu/patches/0031-munch-read-write-root.patch`, CI `38852837176`): boot-arg `rootrw=1` clears MNT_RDONLY before the root mount (no data volume on sweet: /private/var is on the root; image has no /etc/fstab); Apple HFS accepts a dirty non-journaled ROOT read-write on SWEET (logs "mounting dirty non-journaled root read-write"). Later: journaled HFS for crash safety. Kernel `xnu/out-0031` (CI `38852837176` success, 5m31s; 0030 run cancelled by the newer push). Boot images (phone ADT `sweet-0028.adt`, ramdisk byte-matched): `shim/out/shim-boot-0031r.img` = 0029k args (read-only, ignition_level=1) and `shim/out/shim-boot-0031w.img` = same + `ufsrw=1 rootrw=1`. PLAN: boot 0031r first and film until the reset (diagnose overnight reset), then 0031w (first ever writes to userdata; v2 image on the SSD is the restore source). `research/check-patch-order.py` cannot check 0031 (staged sweet_hfs files are not in its index); CI stage scripts run first, so CI is the check.

- **ignition_level=1 result** (boot 21:01:39 UTC, `research/boot0029k.log`, photo IMG_20261015_230308): preboot/cryptex stages skipped (need >= 2/3), `dylib_cache: finding shared cache on system volume: ignition level = 1, search root fd = 4, subpath = System/Library/Caches/com.apple.dyld/` -> not found -> `ignition boot failed: 19` -> no shared cache -> libSystem not loaded. FIX IN PROGRESS: image v2 = copy of the flashed image + `/System/Library/Caches/com.apple.dyld/` with HARD LINKS to the 44 cryptex cache files (no extra space; avoids symlink-following doubts), then `tools/sparsediff.py ios16-root-20H392.img ios16-root-20H392-v2.img diff.simg` and flash only the diff. Boot with `ignition_level=1` (shim-boot-0029k.img).
- **ignition_level=0 result** (boot 20:58:07 UTC, `research/boot0029j.log`, photo IMG_20261015_230037): all stages skipped as "ignition level insufficient" (hello/preboot/cryptex1 sniff >= 2, select os cryptex path/graft >= 3, **dyld_cache >= 1**), `ignition boot failed: 89`, "ignition partially disabled", then same `Library not loaded: /usr/lib/libSystem.B.dylib (no such file, no dyld cache)`. => dyld gets the shared cache directory from ignition's dyld_cache stage. Next: `ignition_level=1` (`shim/out/shim-boot-0029k.img`) to skip preboot/cryptex stages but run dyld_cache; its log "finding shared cache on %s ... subpath = %s" will show where it searches. If it searches rootfs /System/Library/Caches/com.apple.dyld, add a symlink there (image rebuild; consider a diff-only sparse flash tool to avoid rewriting 12 GB).
- **APPLE dyld/launchd RUNNING (libignition) with `unentitled_ios_sim_launch=1`** (boot 20:53:24 UTC, `research/boot0029i.log`, photo IMG_20261015_225617): kernel prints "Allowing spawn of iOS binary ... boot-arg set to true"; `shared_region: ... check_np ... vm_shared_region_start_address() failed` (no region yet, normal); dyld's built-in **libignition** (Darwin Ignition Sequence 1.0.0, libignition-51~1974; prints rd=ufsroot0, hardware target sweet, ignition level 0x5, platform 0x2, boot spec local, 7 stages): `hello from launchd.1` -> `preboot: mounting preboot` -> `failed to lookup preboot: 25` (ENOTTY: looks for a separate Preboot volume by role) -> ignition fails -> dyld `Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dyld cache)` -> launchd exit -> panic initproc failed. dyld boot-args found via strings: `ignition_level`, `ignition_force_dylib_root` (forced rootfs/livefs/cryptex fallback), `ignition_halt_after`, `ignition_live_app_graft`; dyld cache search paths include /System/Cryptexes/OS, /System/Volumes/Preboot/Cryptexes/OS/, /private/preboot/Cryptexes/OS/ (present in our image). Next try `ignition_level=0` (`shim/out/shim-boot-0029j.img`).
- **APPLE launchd EXECUTED FROM UFS (2026-10-15).** Flashed iOS image: `img2simg` -> `ios16-root-20H392.simg` (12.1 GB) then `fastboot -S 512M flash userdata ...simg` (24 chunks, 307 s, `research/flash-userdata-ios16.log`; a raw-image flash was killed by earlyoom at 2.6 GB RSS - always flash the sparse image). ABL fastboot hung on getvar after the flash; power+vol-down fixed it. Boot 0029u 20:49:04 UTC (`research/boot0029u-ios16.log`), photo IMG_20261015_225134: `hfs: mounted iOSRoot` -> load_init_program /sbin/launchd -> panic `unexpected SIGKILL of launchd (CS_KILLED) with reason -- namespace 9 code 0xe` = OS_REASON_EXEC / EXEC_EXIT_REASON_WRONG_PLATFORM: our kernel is XNU_TARGET_OS_OSX, iOS-platform binaries are "alien" and refused unless spawn attr platform set or DEVELOPMENT TUNABLE `unentitled_ios_sim_launch`. Next try: same kernel, cmdline + `unentitled_ios_sim_launch=1` (`shim/out/shim-boot-0029i.img`). LONGER TERM: macOS-config kernel marks iOS maps alien (vm_map_mark_alien) and differs from iOS in many policies; an iPhoneOS-config kernel build may eventually be needed.
- **iOS 16 ROOT IMAGE BUILT:** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392.img` (16 GiB case-sensitive HFS+, label iOSRoot, 12 GB used, fsck OK after `fsck.hfsplus -y` rewrote 32 non-ASCII localized WorkflowKit Shortcut names Linux decomposes differently). Built by `tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]` (needs sudo; apfs-fuse as root; OS dmg is UDIF -> link must end .dmg). Contents: iPhone X 20H392 OS root + SystemOS cryptex at /private/preboot/Cryptexes/OS (so /System/Cryptexes/OS -> cache, 44 files, dyld_v1 arm64) + AppOS at .../App. /sbin/launchd and /usr/lib/dyld root:wheel 0755. NEXT: (confirm) `fastboot flash userdata ios16-root-20H392.img` (auto-sparse), boot 0029u (rd=ufsroot0) -> Apple launchd attempt.
- **MILESTONE ON HARDWARE: BOOT FROM INTERNAL FLASH.** 0029 CI `38797924912` PASSED, `xnu/out-0029` SHA256 `a68bc847...2a37`. Image `shim/out/shim-boot-0029u.img` (payload-0029u = 0029 kernel + `shim/out/sweet-0028.adt` + `rd=ufsroot0`, NO RAM disk). Boots 20:01:34 and 20:12:37 UTC (`research/boot0029u*.log`), photo IMG_20261015_221403: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `SWEET: root volume is not authenticated (error 25); continuing unsealed` -> `load_init_program ... /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> heartbeat (ran for minutes unattended). userdata currently holds the 8 MB `research/root-hfs.img`. NEXT: build a multi-GB HFS+ image with the iOS 16 (iPhone X 20H392) userland (OS rootfs + SystemOS cryptex contents incl. dyld shared cache at /System/Library/Caches/com.apple.dyld; preserve modes/owners/symlinks; decompress LZBITMAP via apfs-fuse), flash it to userdata (sparse, needs user OK), then try Apple's dyld/launchd. Driver is read-only (root mounts RDONLY), fine for first launchd attempts.
- **userdata FLASHED with the 8 MB HFS test volume** (user said "flash it", 2026-10-15 19:53:17 UTC, `research/flash-userdata-hfs-test.log`: `fastboot flash userdata research/root-hfs.img` OKAY; pre-checks: one sweet device, partition-size:userdata 0x1AC4BFB000, partition-type f2fs, image fsck OK, SHA256 3626ea69...). Android userdata is now destroyed (expected/approved). Boot 0028u at 19:53:20 (`research/boot0028u-hfs.log`), photo IMG_20261015_215541: `BSD root: ufsroot0` -> **`hfs: mounted SweetRoot on device b(1, 0)` from internal UFS** -> panic `rootvp not authenticated after mounting @bsd_init.c:931` (arm64 macOS-config requires FSIOC_KERNEL_ROOTAUTH sealed root for non-ramdisk roots). Patch **0029** (`dd1c284`): SWEET logs `SWEET: root volume is not authenticated (error N); continuing unsealed` instead of panicking (Apple configs keep the panic; pivot_root still requires auth). CI **`38797924912`** running. Next: download out-0029, phone image with sweet-0028.adt + rd=ufsroot0 (no RAM disk), boot -> expect init from UFS.
- **UFS ROOT DRIVER VERIFIED ON HARDWARE (read-only).** 0028 CI `38796139570` PASSED, `xnu/out-0028` SHA256 `8b981640...9da6`; QEMU HFS RAM-disk regression `research/q0028-hfs` still runs init. Phone boot 2026-10-15 19:50:24 UTC (`research/boot0028u.log`, image `shim/out/shim-boot-0028u.img`, phone ADT `shim/out/sweet-0028.adt` with /sweet-ufs, NO RAM disk, `rd=ufsroot0`), photos IMG_20261015_215151/215220: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : f188` (Android-encrypted data, as expected) -> `cannot mount root, errno = 19`, kernel retries mountroot (no panic). Nothing written to flash. NEXT (destructive, needs user confirmation right before): `fastboot flash userdata research/root-hfs.img` (8 MB HFS with test init) then boot rd=ufsroot0 -> expect init from internal flash.
- **PATCH 0028 (`33aceac`): XNU read-only UFS root device**, CI **`38796139570`** running (next artifacts xnu/out-0028, research/bt29, run29.log). `bsd/dev/sweet_ufs.c`: reuses ABL's UFSHCI state (like the shim probe), takes over UTRL at a kmem_alloc_contig region (DMA layout: UTRL 0x0, UCD 0x1000, 1 MiB bounce 0x4000), verifies UTRLBA readback before any doorbell, IE=0 polled, one SCSI cmd at a time on LUN0 (TUR x3, READ(10) via bounce, PRDT 256 KiB chunks), flush_dcache64 by PA around each command (host not dma-coherent per DT), finds GPT partition by name (`ufsroot=` boot-arg, default `userdata`), exposes `/dev/ufsroot0`/`rufsroot0` (512-byte logical sectors, physical 4 KiB, DKIOC* ioctls), refuses writes (EROFS). IOKitBSDInit: `rd=ufsroot0` -> sweet_ufs_rootdev() (lazy init) else panic. ADT: `tools/mkadt.py` adds `/sweet-ufs` reg <0x1d84000 0x3000> for the PHONE only (not --qemu). Local compile OK, imports resolve. Sandbox research/bt28 (pre0028 originals). FIRST PHONE TEST PLAN (non-destructive): boot 0028 with phone ADT + `rd=ufsroot0` and NO RAM disk: expect `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB` + `BSD root: ufsroot0`, then root mount failure (userdata is still Android-encrypted) -> panic. That validates the driver before flashing an HFS image into userdata (needs explicit user confirmation at that time).
- **SUPERSEDES the 64 GiB iosroot GPT plan: user said "u can delete android" (2026-10-15).** Use the existing `userdata` partition (LUN0 sda34, 4K LBA 0x2B8C00, 0x1AC4B MiB ~107 GiB) directly as the iOS root: NO GPT changes. Keep bootloaders, recovery (OrangeFox), modem/persist/fsg/modemst untouched. Write the HFS image later with `fastboot flash userdata` (sparse, auto-split under the 768 MiB limit). Android will no longer boot normally (expected; reinstallable). Order: XNU UFS driver first, read-only against the current GPT; flash only once it can locate userdata. Nothing erased yet.
- **DECIDED (user, 2026-10-15): 64 GiB `iosroot` on LUN0 (sda), taken from userdata (wipes Android user data; Android/super kept).** Planned GPT (4K LBAs): userdata 0x2B8C00..0xD7D6FF (~43 GiB, keep type/unique GUIDs and attributes); iosroot 0xD7D700..0x1D7D6FF (0x1000000 blocks = 64 GiB, 1 MiB aligned); backup GPT area unchanged. Procedure: user backs up phone data -> boot OrangeFox recovery (adb) -> back up GPT (sgdisk --backup + raw dd of LBA 0-5 and last 6 blocks) to research/ -> sgdisk edit -> Format Data -> verify Android boots to setup. NOT EXECUTED YET.
- **INTERNAL UFS WORKS FROM THE SHIM (read-only), user authorized internal-UFS approach incl. flashing (2026-10-15, "i allow everything").** Shim probe `shim/ufs.c` (commit `12ed3ed` + filter): ufshc@1d84000 is UFSHCI 2.1 (ver 0x210, cap 0x1587031F, 32 slots), ABL leaves HCE=1, HCS=0x10F (device present, lists ready, UIC ready); probe takes over UTRL at its own buffer (verifies UTRLBA readback before any doorbell), TEST UNIT READY/READ CAPACITY/READ(10) all succeed. LUN0 = 0x1D7D800 x 4 KiB (~126.6 GB), valid GPT (64 entries at LBA 2). Photos IMG_20261015_212916/213325/213429. Big LUN0 partitions (start in 4K LBAs / MiB): super 0xCA00/0x2200 (8.5 GiB), logdump 0x22CA00/64, minidump 0x230A00/128, rawdump 0x238A00/128, recovery 0x240C00/128, cache 0x248C00/384 ext4, exaid 0x260C00/384, cust 0x278C00/1024 ext4, **userdata 0x2B8C00/0x1AC4B MiB (~107 GiB), fs unreadable = encrypted (metadata encryption)**. cache..cust contiguous = only 1.75 GiB (too small for iOS ~10+ GiB). Plan needing user consent: shrink userdata in GPT (wipes Android data) and add an `iosroot` partition at the end of LUN0. LUNs 1-5 not listed yet (sdb..sdf backed up in backup/lun).
- **fastboot limit measured (2026-10-15, read-only getvar):** `max-download-size: 805306368` (768 MiB); `max-fetch-size` unsupported; `is-userspace: no` (ABL fastboot). A `fastboot boot` image (shim+kernel+RAM disk) must be < 768 MiB, so the 2.5 GiB iOS shared cache cannot ride in the boot RAM disk even if XNU gets all RAM. Real userland needs storage access (UFS driver, or USB host) or a much smaller cache. Unlocking full RAM is still needed later (GUI).
- **iOS 16 USERLAND SOURCE (Claude, 2026-10-15).** Chose **iPhone X (iPhone10,3/10,6, D22/D221AP)** iOS **16.7.16 (20H392)** over iPhone 8: same A11 (no PAC -> plain arm64, runs on Kryo 470/ARMv8.2), last iOS for A11; X userland is home-button-less and tall-screen (1125x2436 vs sweet 1080x2400), better fit for later GUI. Note the X has its OWN SystemOS cryptex (different digest from iPhone 8's). IPSW URL in `research/ipsw-x/URL.txt` (+ BuildManifest). Images on the user's external SSD (NOT in git, do not format that disk: it holds the user's photos): `/run/media/dipa/android/ios-sweet-ipsw/iPhone10,3_16.7.16_20H392/` — `087-86593-047.dmg` = Cryptex1,SystemOS (2,743,074,816 B), `087-86698-044.dmg` = OS rootfs (4,730,814,044 B), both zip CRC-32 verified, plus Firmware/*.root_hash/.trustcache/.mtree. AppOS cryptex is 087-86715-047.dmg (10 MB, same as iPhone 8).
  - Images are raw APFS; files use LZBITMAP (decmpfs type 14). 7z cannot decode. Tools: `ipsw` 3.1.738 (AUR ipsw-bin, user-approved) needs apfs-fuse on Linux; **apfs-fuse** built in user space (system cmake broken: cmake 4.4.4 wants libjsoncpp.so.27, jsoncpp 1.9.6 installed = partial upgrade, NOT fixed) with pip cmake + `-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"`, installed to `~/.local/bin/apfs-fuse` (+apfs-dump-quick). Mount via a comma-free symlink (apfs-fuse splits options on ',' in `iPhone10,3`; exFAT has no symlinks -> put link elsewhere): `apfs-fuse LINK/087-86698-044.dmg MNT` -> `MNT/root/...`; unmount `fusermount3 -u MNT`.
  - Verified from headers: `dyld_shared_cache_arm64` magic `dyld_v1   arm64`; `/sbin/launchd`, `/usr/lib/dyld` = cputype 0x100000c subtype 0 (plain arm64). libSystem exists only inside the cache.
  - **BLOCKER for real userland via RAM disk:** shared cache = 3.10 GiB in 44 files (~2.5 GiB without the 592 MB `.symbols`), but the kernel manages only 1.20 GiB (0xb0000000..0xfd000000). Options: (A) give XNU the phone's full ~6 GB (banks 0x80000000+2G and 0x100000000+0xfcc00000, carve-out holes need pmap support) AND check fastboot max-download-size for a multi-GB boot image; (B) storage driver (Qualcomm UFS) to read a root from flash (no flashing allowed: would need a file/partition strategy); (C) run against individual dylibs extracted from the cache (uncertain).
- **MILESTONE ON HARDWARE: HFS+ ROOT FILESYSTEM -> PID 1.** Boot 2026-10-15 18:22:12 UTC (`research/boot0027b.log`, image `shim/out/shim-boot-0027b.img`), photo `IMG_20261015_202334.jpg`: `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive`, no panic. Kernel 0027 (built-in Apple OSS HFS + BearSSL MD5), init with SP alignment fix. NEXT: real iOS 16 userland on the HFS image (dyld, libSystem/dyld shared cache, launchd). Known gaps: entitlements (none granted), SHA-1/SHA-384/512 providers, no storage driver (RAM disk only), root mounted read-only.
- **PHONE HFS ROOT (0027) + SIGBUS ROOT-CAUSED.** Phone boot 2026-10-15 17:49:06 UTC (`research/boot0027.log`, photo `IMG_20261015_200031.jpg`): `hfs: mounted SweetRoot on device b(1, 0)` (HFS works on hardware), init printed its first line, then `pid 1 exited -- exit reason namespace 2 subcode 0xa` = SIGNAL/SIGBUS -> `initproc failed to start` panic. Diagnostic boot 18:08:13 UTC (`research/boot0027d.log`, image `shim/out/shim-boot-0027d.img`, diag init `research/diag-init` from scratch diag-init.S with [a][b][c] markers, `cs_debug=1`; photo `IMG_20261015_201547.jpg`): output `PID [a][b]` then SIGBUS -> fault at first `strb w21,[sp]`; no code-signing messages. Cause: kernel enters a static binary with sp at argc, not necessarily 16-aligned (depends on arg/apple string lengths, differs mockfs vs HFS); EL0 SP alignment check -> EXC_ARM_SP_ALIGN -> SIGBUS (sleh handle_sp_align). QEMU does not enforce SP alignment. NOT a kernel bug: real binaries start in dyld which aligns. Fix `1a8fc15`: sweet-init aligns sp at entry. Rebuilt `research/root-hfs.img`, QEMU `research/q0027-hfs2` OK. Phone image `shim/out/shim-boot-0027b.img` SHA256 `098ba46c...c5c` (payload-0027b, ramdisk match check-0027b) ready, awaiting FASTBOOT. Lesson: Assembly `;` is a comment in Apple arm64 asm (use .macro, not ;-joined #define).
- **HFS ROOT WORKS (QEMU).** 0027 CI `38779441895` PASSED, `xnu/out-0027` SHA256 `a8643886...5f6a`. QEMU `research/q0027-hfs` (RAM disk = `research/root-hfs.img`, rd=md0): `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` + heartbeat, no panic. Phone image `shim/out/shim-boot-0027.img` (payload-0027 = 0027 kernel + sweet-0021.adt + root-hfs.img + rd=md0, ramdisk byte-match check-0027) ready; awaiting FASTBOOT for phone test.
- **HFS ROOT WORK (Claude), patches 0026-0027.**
  - 0026 (`f0e990c` + link fix `64c96f4`): Apple OSS **hfs-627.100.6** (commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03`, paired with xnu-8796.141.3 in macos-135 release.json) staged by `xnu/patches/0026-stage-hfs.sh` into `bsd/sweet_hfs` (core + hfs_encodings; AppleKeyStore include/key prototypes guarded by CONFIG_PROTECT; SDK TargetConditionals copied). New config option `sweet_hfs` (MASTER + MASTER.arm64.sweet); 40 `optional sweet_hfs` files; Makefile.template foreach adds `_CFLAGS_ADD = -include sweet_hfs_compat.h -I...` (MUST come after kernel -D flags; compat undefs BSD/XNU_KERNEL_PRIVATE and the CONFIG_* HFS tests so kext-config.h decides: macOS profile, no content protection). `sweet_hfs_iokit.c` replaces hfs_iokit.cpp/hfs_encodings_kext.cpp: `sweet_hfs_register()` called in bsd_init after vfsinit (encodings init, vfs_fsadd VFS_TBLCANMOUNTROOT, zones, sysctls); is_ejectable 0, iterate_media none, serial KERN_NOT_SUPPORTED, OSKextGetCurrentLoadTag -> kOSKextInvalidLoadTag (renamed via compat macro). hfs_encodings' own `hfs_allocated` renamed per object (separate kexts on Apple; first CI `38777325964` failed on that duplicate at link). mockfs now validates Mach-O magic (its TODO) so non-Mach-O roots fall through to HFS (new fs entries come AFTER mockfs in vfsconf). CI `38778133720` PASSED, `xnu/out-0026` SHA256 `0b49170c...701a`, prints `SWEET HFS: hfs-627.100.6 registered`.
  - HFS+ image: `research/root-hfs.img` (8 MB, mkfs.hfsplus from AUR hfsprogs installed with user approval; loop-mounted with sudo; /sbin/launchd = research/sweet-init, /dev; fsck.hfsplus OK). QEMU `research/q0026-hfs`: `BSD root: md0` then data abort in MD5Init from hfs_getvoluuid (mount computes volume UUID).
  - 0027 (`14bf2b7`): 0020 stager also copies BearSSL hash/md5.c + codec/dec32le.c/enc32le.c; `corecrypto_md5.c` SWEET branch converts MD5_CTX <-> br_md5_context around br_md5_* calls; wrappers sweet_md5.c/sweet_dec32le.c/sweet_enc32le.c. Host test 40/40 match md5sum. CI **`38779441895`** running.
  - Local tooling: `tools/xnu-cc.py` has `XNU_CC_POST` (flags after logged ones). Sandbox `research/bt28/xnu` = 0025 CI tree + 0026/0027 edits; originals `research/pre0026`, `research/pre0027`. HFS local objects `research/hfs-objs`. Checker entries updated to bt28.
  - SHA-1 / SHA-384/512 still have no provider.
- **MILESTONE ON HARDWARE: USERSPACE PID 1 RUNS ON THE PHONE.** Boot 2026-10-15 17:16:00 UTC (`research/boot0025.log`: send OKAY0.410s, boot OKAY0.135s), image `shim/out/shim-boot-0025.img` SHA256 `9dbfa0b1...89db0`. Photo `IMG_20261015_191733.jpg`: `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive` (5 s heartbeat), no panic. Kernel 0025 + mockfs RAM disk + built-in no-entitlements AMFI. No flashing performed.
- **NEXT (toward real iOS userland):** real root filesystem with real binaries. Options: (a) build Apple OSS `hfs` (macOS 13.5 tag `hfs-627.100.6`) into SWEET like libpthread, then an HFS RAM disk image; (b) iOS 16 dyld + libSystem/dyld shared cache from the IPSW (needs APFS or repacking onto HFS). Expected hurdles: entitlements (sweet_amfi grants none), SHA-1/MD5/SHA-384/512 providers (BearSSL), missing kexts/daemons. Current init is a static test program, not launchd.
- **MILESTONE: USERSPACE RUNS (QEMU).** 0025 CI `38774788048` PASSED, `xnu/out-0025` SHA256 `5496069b...4877`. QEMU `research/q0025-rd` (150 s, no panic): `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> `SWEET init: still alive` every 5 s. Exec, ad-hoc SHA-256 code signature, syscalls, devfs /dev/console, select timeouts all work. Phone image prepared: `shim/out/shim-boot-0025.img` 16,515,072 bytes SHA256 `9dbfa0b15140c0bd733a16c4f727f6a7df44a8741f98d8e266d5f2fa4f689db0` (payload-0025 = 0025 kernel + sweet-0021.adt + research/sweet-init + rd=md0; ramdisk byte-match check-0025). Waiting for user to put phone in FASTBOOT.
- **0024 CI `38773277811` PASSED**, `xnu/out-0024` SHA256 `a0ba964b...4348`. QEMU `research/q0024-rd` and **PHONE (boot 2026-10-15 17:05:55 UTC, `research/boot0024.log`, image `shim/out/shim-boot-0024.img` SHA256 `cf3dac86...6964`, 16,510,976 bytes, payload `shim/out/payload-0024.bin` = 0024 kernel + `shim/out/sweet-0021.adt` + RAM disk `research/sweet-init` + rd=md0; ramdisk byte-match `check-0024`)** agree: `Added memory device md0`, `BSD root: md0`, `load_init_program: attempting to load /sbin/launchd`, then data abort in accelerate_entitlement_queries (PC 0x...77d2458, FAR 0x1a0) = NULL AMFI interface. Photo `IMG_20261015_190715.jpg`. RAM disk root is physically verified.
- **0025 (`2518c79`)**: new `bsd/kern/sweet_amfi.c` registers a built-in AMFI interface at EARLY_BOOT/MIDDLE: no entitlements (adjustContextWithoutMonitor succeeds without creating a context, queries KERN_DENIED, copies NULL/KERN_NOT_FOUND, get_xml/transmuted false); WithMonitor KERN_NOT_SUPPORTED; OSEntitlements_query and query_context_to_object panic explicitly (CoreEntitlements closed; no kernel caller); TrustCache version 0 (0015 keys on trust_cache_rt==NULL, img4 absent -> still unavailable); CoreEntitlements table zero (PMAP_CS only). Compiles locally; checker updated (bt27 entries). CI **`38774788048`** running. Next artifacts `xnu/out-0025`, `research/bt29`, `research/run29.log`. Real iOS binaries will need real entitlements later (no OSS CoreEntitlements exists).
- **0021-0024 RAM-disk-root iteration (Claude), QEMU only so far, no new phone boot since 0020.** Each kernel tested with `tools/mkpayload.py KERNEL research/q0021.adt OUT --ramdisk research/sweet-init --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` then `tools/qemu-munch.py OUT DIR --secs 150 --kernel KERNEL --fail-on-panic` (PATH needs ~/swift/usr/bin for llvm-nm). Fresh ADTs: `research/q0021.adt` (QEMU), `shim/out/sweet-0021.adt` (phone).
  - 0021 CI `38769532000` PASSED, `xnu/out-0021` SHA256 `2b7500a1...962e`, `research/bt25`/`run25.log`. QEMU `research/q0021-rd`: md0 root, then assert `iomediaClass` in IOService::publishHiddenMedia.
  - 0022 (`20ece9c`, publishHiddenMedia without IOMedia class) CI `38770666158` PASSED, `xnu/out-0022` SHA256 `a4d0e787...30c5`, `research/bt26`/`run26.log`. QEMU `research/q0022-rd`: data abort in SHA256_Init (NULL g_crypto_funcs) from vm_kernel_addrhash_internal.
  - 0023 (`c8089d1`, libkern SHA256_Init/Update/Final use built-in ccsha256_ltc_di + ccdigest_init/update on SWEET; also serves code-signing SHA-256; SHA-1/MD5/SHA-384/512 still have NO provider - BearSSL from 0020 could supply them later) CI `38771876423` PASSED, `xnu/out-0023` SHA256 `d763fed7...1003`, `research/bt27`/`run27.log`. QEMU `research/q0023-rd`: **mockfs mounted md0, `load_init_program: attempting to load /sbin/launchd`**, then panic `mockfs_fsnode_vnode failed to create fictitious pages ... rvalue = 5` (mockfs.c:351).
  - 0024 (`3293c80`): mockfs used memdev mi_base (4K page number of a VIRTUAL kernel address for IOKit RAMDisk, truncated to 32 bits) << PAGE_SHIFT as a physical address. Now zero-copy pager mapping only when mi_phys (with 4K units); virtual md uses pagein/blockmap/strategy -> memdev bcopy. memdev's own mdPhys path assumes 4K ppnums (unsafe on 16K arm64), so do NOT switch the RAMDisk to physical. CI **`38773277811`** running. Next artifacts `xnu/out-0024`, `research/bt28`, `research/run28.log`.
  - Sandboxes: bt25/bt26/bt27 contain the next patch's edited files; pristine copies in `research/pre0022`..`pre0024`.
- **PATCH 0021 (Claude): RAM disk root via mockfs -> first userspace program.** Commits `9c37771` (initial; CI `38768500280` FAILED: mockfs_fsnode.c kfree_type(mockfs_fsnode_t) typed-alloc static assert; local xnu-cc stubs `__builtin_xnu_types_compatible` so it can't catch this) and `193772f` (fix), CI **`38769532000`** running: https://github.com/dipa-studio/ios-sweet/actions/runs/38769532000. Next artifacts: `xnu/out-0021`, `research/bt25`, `research/run25.log`.
  - Kernel: `config/MASTER.arm64.sweet` FILESYS_BASE += `mockfs` (XNU's built-in FS: root device is itself a Mach-O, served as /sbin/launchd, plus /dev for devfs). Upstream mockfs bit-rot fixed: kalloc_type/kfree_type used the POINTER typedef mockfs_fsnode_t (nodes were 8 bytes) -> `struct mockfs_fsnode`. `IOService::setRootMedia` asserted IOBlockStorageDriver class exists (IOStorageFamily absent): SWEET-only, a missing class means root=NULL (same as IOSetImageBoot); Apple path keeps assert. All 21 patches apply (`research/check-patch-order.py`); 5 TUs compile locally with -DMOCKFS (`research/mockfs-objs`), imports resolve vs 0020 kernel. Sandbox `research/bt24/xnu` now holds 0021 versions of those 3 files; pristine 0020 copies in `research/pre0021`.
  - Tools: `tools/mkpayload.py --ramdisk IMAGE` appends IMAGE as an extra payload segment right after the kernel, moves kernel_end past it (shim unchanged: ADT+boot_args follow it, image stays below topOfKernelData), and adds ADT `/chosen/memory-map` `RAMDisk`=<phys,size>. Boot with `rd=md0`. Without --ramdisk output is byte-identical to existing 0020 QEMU and phone payloads (verified). `tools/munch-init.S` + `tools/mkinit.py`: PIC static init (open /dev/console, print PID, heartbeat every 5 s via select, never exits) wrapped into a static arm64 Mach-O (__PAGEZERO, __TEXT@0x100000000, LC_UNIXTHREAD, ad-hoc SHA-256 CodeDirectory v0x20400, 4K pages). Build: `~/swift/usr/bin/clang -target arm64-apple-ios16.0 -c tools/munch-init.S -o X.o && tools/mkinit.py X.o research/sweet-init` (ld64.lld cannot link static iOS). Signature not verified by a real codesign; the kernel will tell.
  - QEMU with 0020 kernel + RAM disk (`research/q0020-rd.bin`, rd=md0, 150 s `research/q0020-rd150`): `Added memory device md0`, **`BSD root: md0, major 1, minor 0`** (plumbing works), then the setRootMedia assert (now fixed in 0021). NOTE: QEMU needs >60 s to reach IOFindBSDRoot (60 s run showed nothing after stf0; likely RTC/inittodr wait), so use --secs 150.
  - NEXT: download 0021, QEMU 150 s with ramdisk; expect mockfs mount then init exec -> `*** SWEET userspace: init running as PID 1 ***`. Then phone boot with same payload (phone ADT shim/out/sweet-0020.adt pattern + --ramdisk + rd=md0).
- **0020 PHONE RESULT (user transcription of screen, 2026-10-15): KERNEL INIT COMPLETE, WAITING FOR ROOT.** After Skywalk self-tests PASSED, kminit, ifnet_attach lo0/gif0/stf0, screen prints `BSD`, then `Waiting on <dict><key>IOProviderClass</key><string>IOMedia</string><key>Content</key><string>Apple_HFS</string></dict>` and `Failed to write boot-picker-bringup-reason to NVRAM.` No panic. This is IOFindBSDRoot waiting for a root device (none exists); NVRAM warning is expected (no NVRAM driver). QEMU `cpu_idle` snapshot is presumably the same wait (not GDB-confirmed; milestone GDB run was cancelled by user since phone answered it). Draft `research/check-milestones20.gdb` (hbreaks inittodr/vfs_mountroot/IOFindBSDRoot/bsd_utaskbootstrap) left uncommitted/ignored.
- **NEXT MILESTONE: root filesystem -> launchd.** Plan: (1) RAM disk: shim places an image in RAM, ADT /chosen/memory-map `RAMDisk` entry, boot-arg `rd=md0` (XNU md driver). (2) HFS is a separate kext on Apple; build Apple OSS `hfs` (tag paired with macOS 13.5 in distribution-macOS macos-135 release.json) into SWEET like libpthread 0019. APFS is closed. (3) Minimal HFS image with iOS 16 launchd (later: dyld/shared cache; AMFI/code-signing absent will be a later hurdle). User's end goal is full iOS GUI.
- **0020 (AES) VERIFIED + PHONE BOOTED (Claude took over after Codex usage limit).** Actions `38765422261` (`ab9e23f`) PASSED; kernel `xnu/out-0020/kernel.development.sweet` checksum OK; buildtree `research/bt24`, log `research/run24.log`. Actual-kernel AES checks PASS (`research/check-aes20.gdb` / `.log`, 12 extra CBC cases match OpenSSL). 45s QEMU `research/q0020`: no panic, zero unknown glyphs, past old TCP/AES panic -> Skywalk self-tests PASSED -> bsd_autoconf kminit -> ifnet_attach lo0/gif0/stf0; final snapshot PC in `cpu_idle` (idle or waiting; NOT yet diagnosed, could be root-device wait). Image `shim/out/shim-boot-0020.img` 16,461,824 bytes SHA256 `775200cb02d95fc750981516b79d91dcdc96184bbe67a38f9d57c59b4a391828`, ramdisk byte-matches `payload-0020.bin` (`check-0020`). **Phone temporarily booted 2026-10-15 16:10:00 UTC**: one FASTBOOT product sweet, send OKAY0.407s, boot OKAY0.134s, total0.549s, exit0 (`research/boot0020.log`). Awaiting user photo; do not reboot the phone before it. NEXT: read photo; diagnose what QEMU waits on after stf0 (GDB backtrace of threads / longer run). Codex's STOP request below is complete.
- User requests STOP after completing the current0020 AES iteration (Actions/QEMU/phone), then leave a precise committed handoff for another AI. User says FASTBOOT ready again after0019 photo. Do not start a new blocker fix after this iteration.
- Goal: iOS16-generation XNU on Redmi Note10Pro sweet/SM7150. Always commit, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups. No userland/root filesystem boot yet.
- **Patch0020 update:** initial `e71098d` / Actions `38765012466` superseded by scalar-only compile flags. Added -mgeneral-regs-only to all six provider/BearSSL TUs and corecrypto_aes.c; object disassembly now confirms no SIMD. Updated implementation `ab9e23f`, Actions `38765422261` queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38765422261. Do not launch another run. portable BearSSL AES CBC and legacy one-block ECB wired directly into SWEET corecrypto_aes.c. Genuine bitslice implementation, no CPU AES/SIMD requirement, heap or fake global crypto table. Source fetched by `0020-stage-bearssl.sh` from official BearSSL git at immutable `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`; MIT license retained, downloaded source stays out of git. Kernel limits include adapter only. All128/192/256-bit keys, CBC encrypt/decrypt, copied contexts, in-place/out-of-place; zero block count and size overflow handled, invalid keys clear context. Seven TUs compile to arm64 objects (`research/aes-objs`), imports resolve, all20 patches apply and sources match (`research/patch0020-check.log`). Sandbox `research/bt23/xnu`, originals `research/pre0020`. NEXT: Actions then actual-kernel NIST/random CBC/ECB checks and >=45s QEMU before another phone boot. Next artifacts out-0020 / bt24 / run24.log. 0020 updated CI pending. Local sources/checks match the scalar-only patch.
- **Latest built kernel0019**, implementation `256d3f0`, patches0001-0019. Actions `38763072907` PASSED (4m5s): https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23` (no nested .git). Seven pthread source/build/header files and staged adaptations byte-match local compile inputs.
- **0019 hardware photo received** `IMG_20261015_175306.jpg`: pthread registration visible, passes mbinit and DLIL checksum/scheduling, same AES fault as QEMU: PC`0xfffffe00079174e0` (`aes_encrypt_key128`), LR`0xfffffe000767c3a4` (`tcp_init`), FAR0x78, x8=0. Disassembly proves NULL g_crypto_funcs->ccaes_cbc_encrypt. Photo processed; no more0019 observation pending. General crypto table remains absent, other hash/mode operations may still fault after0020. DO NOT bypass TCP or install fake crypto. XNU already contains SHA256/HMAC implementations; AES source there is a dispatch wrapper. Full future provider interface libkern/libkern/crypto/register_crypto.h. Apple/corecrypto default README describes evaluation-only licensing, so no external corecrypto was fetched or staged; BearSSL selected for this portable AES integration.
- **0019 pthread FIXED in QEMU and observed beyond its init on phone**: 45s QEMU `research/q0019`, zero unknown glyphs, then AES panic. Actual-kernel API checks PASS `research/check-pthread19.gdb` / `.log`, guest `research/q0019-api`: function/callback table identity, actual provider pthread_init returns, locks/global hash/threadcall/typed zones initialized, thread_selfid via syscall shim + buffer bounds, process hash init/delete via shims restores NULL. Monitor quit prints expected remote-disconnect after PASS. All fixture guests stopped.
- **0019 provider**: genuine Apple libpthread-514.100.2 commit `a6c70ca0e544347a0377babf748f831577ec24f9`, exact paired release with xnu8796.141.3 in distribution-macOS/macos-135/release.json (`research/macos-135-release.json`). 0019 shell stage runs before patches (workflow copies *.sh); three SWEET wrapper TUs use kext BSD header view, vm_fault_external, renamed provider-side pthread_kern/current_uthread; register real function table at EARLY_BOOT SECOND, real pthread_init later. Stager applies bounded address casts, zero user addresses, proper panic format. SWEET opaque ksyn storage enlarged48->56 for trailing kwe_uth, checked with provider static assertion. Local pre-CI TargetConditionals fixture was syntax-only; artifact bt23 carries actual SDK header and CI is authoritative.
- **Phone0019 temporarily booted** 2026-10-15 15:51:36 UTC: exactly one FASTBOOT/product sweet verified; send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). Image `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`. Phone ADT/payload `shim/out/sweet-0019.adt` / `payload-0019.bin` timer19, unpacked ramdisk byte-match (`check-0019`). EntryPA0xb146b488, kernelendPA0xb2000000, boot_argsPA0xb2004000/top0xb2008000. Last user FASTBOOT+USB reply preceded this boot; do not assume phone is still in FASTBOOT.
- **CPU0018 physically verified** by `IMG_20261015_173951 (2).jpg`: boot CPU0 registered, real SGI1 verified, initialization complete -> BSD/MAC/mbinit -> old pthread-table panic. SweetCPU uses legacy IOCPU, ml_processor_register/processor_start/initCPU, real SGI1 clears SIGPdisabled before max CPUs completion; no forced flags/skipped waits. CPU APIs and two-CPU rejection passed (`research/check-cpu18.gdb`, `q0018-api`, `q0018-smp`). Single core; SMP/suspend unsupported. CURRENT SWEET has NO USE_APPLEARMSMP.
- QEMU next run: generate fresh `tools/mkadt.py research/q0020.adt --qemu` (timer27), mkpayload with `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`, then `tools/qemu-munch.py PAYLOAD OUT --secs 45 --kernel KERNEL --fail-on-panic`. 12s can miss later panics because IORTC waits30s. Runner snapshot only: panic exit2, blank/incomplete exit3; absence of panic does not mean complete boot. Qualcomm XPU/ABL/panel and phone SMC PSCI require hardware checks.
- Local checker `tools/xnu-cc.py research/run23.log research/bt23 FILTER --per-file`, XNU_CC_OBJDIR for objects. For new BearSSL TUs add XNU_CC_EXTRA with -I bt23/xnu/libkern/crypto/sweet_bearssl/{inc,src}. Patch checker `research/check-patch-order.py` uses temporary upstream index; source fixtures/artifacts ignored under research. GDB mask registers to unsigned64, use function-entry manual-call return traps, discard manipulated guests. No QEMU guest intentionally left running.

## Session log and earlier handoffs (superseded by the active summary above)

### Prior 0019 transition notes (superseded)

- Goal: iOS 16 generation XNU on sweet. Always commit completed work, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups.
- **0019 build PASSED** Actions `38763072907` (4m5s), implementation `256d3f0`. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23`; all seven patched files and staged provider adaptations match local compile inputs. Prefer bt23 as the next baseline.
- **0019 pthread FIXED in QEMU**: 45s run `research/q0019` registers provider, passes mbinit, DLIL SUM16 checks and DLIL thread scheduling. Actual-kernel GDB checks `research/check-pthread19.gdb` / `.log` PASS: function/callback table identity, real _pthread_init returns, locks/global hash/cleanup threadcall/typed zones non-NULL, thread_selfid via actual syscall shim with output bounds, process hash init/delete through actual shims restores NULL. Guest stopped after tests; no fixture left running. GDB monitor quit prints expected remote disconnect after PASS.
- **Current next blocker:** QEMU0019 panic data abort FAR0x78 at `aes_encrypt_key128` PC`0xfffffe00079174e0`, called by `tcp_init` LR`0xfffffe000767c3a4`. Disassembly confirms `g_crypto_funcs == NULL`, loading ccaes_cbc_encrypt at+0x78. General crypto registration is still absent; DO NOT bypass TCP or supply fake crypto. Next integrate a genuine compatible crypto implementation/provider (AES first needed here, other digest/mode APIs also used). XNU contains SHA256/HMAC but no AES implementation; its corecrypto_aes.c is a dispatch wrapper. Full crypto table is in libkern/libkern/crypto/register_crypto.h.
- **0019 phone image prepared and verified**, user reports FASTBOOT+USB again; exactly one device/product sweet verified. `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`; unpacked ramdisk matches phone payload (`shim/out/check-0019`, `payload-0019.bin`, `sweet-0019.adt`, timer19). EntryPA0xb146b488; kernelendPA0xb2000000; boot_argsPA0xb2004000/top0xb2008000. Usual debug cmdline cpus=1/-noprogress. **Temporarily booted 2026-10-15 15:51:36 UTC**: send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). User asked for photo after45s. Await the photo; DO NOT reboot again before observation. Bootloader acceptance only; no0019 physical progress observed yet.
- **Hardware photo confirms 0018 progress** (`IMG_20261015_173951 (2).jpg`): real phone prints `SWEET CPU: boot CPU 0 registered, SGI1 verified, initialization complete`, enters BSD/MAC, reaches `mbinit: done`, then the same missing-pthread-function-table panic as QEMU at pthread_shims.c:318. CPU completion/interrupt delivery is now physically observed. The photo was received; no further phone boot performed yet.
- **Patch0019 committed/pushed `256d3f0`; Actions `38763072907` queued/running:** https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Do not launch a duplicate. genuine Apple OSS libpthread-514.100.2 provider (immutable commit `a6c70ca0e544347a0377babf748f831577ec24f9`), paired with xnu-8796.141.3 by Apple's `distribution-macOS/macos-135/release.json` (`research/macos-135-release.json`). `0019-stage-libpthread.sh` fetches/stages it before patches; workflow now copies shell scripts too. SWEET-only wrappers compile all three real kernel sources, rename provider-side symbols, use kext header view and vm_fault_external, register the real function table at EARLY_BOOT SECOND. Existing pthread_init then runs the actual provider initialization. No pthread wait/syscall is bypassed. Source staging includes bounded explicit address casts/zero user addresses and panic-format fixes. SWEET kernel opaque ksyn storage enlarged from48 to56 bytes to contain this provider's trailing kwe_uth, with a provider size assertion. Local compile of three provider TUs plus pthread_shims/kern_fork passed; imports resolve and provider exports do not collide with existing kernel symbols. All19 patches apply and changed sources match (`research/patch0019-check.log`). Source sandbox `research/bt22/xnu`, originals `research/pre0019`, objects `research/pthread-objs`. Local TargetConditionals fixture only has target macros for syntax; CI stages the real macOS SDK header and is authoritative. Next Actions artifacts: out-0019 / bt23 / run23.log; run QEMU >=45 seconds. 0019 CI is pending; no 0019 QEMU run yet.
- **Latest kernel implementation `e7d9550`, patches0001-0018; CI `38760004398` PASSED (3m56s)**: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. No build is pending. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; checksum verified. Source/log `research/bt22` / `research/run22.log` (baseline for patch0019; no nested .git). New driver/config sources match CI exactly.
- **CPU wait FIXED in QEMU**: SweetCPU uses legacy IOCPU, validates single boot CPU, ml_processor_register -> processor_start -> initCPU real SGI1 -> scheduler/IOKit completion -> ml_set_max_cpus. It does NOT force a flag or skip a wait. CPU APIs passed: Running CPU0, processor_array[0] association, SGI signalling enabled, name, unsupported secondary-start error (`research/check-cpu18.gdb` / `.log`, `research/q0018-api`). Single-core only; SMP and suspend unsupported. Two-CPU topology fails explicitly (`research/q0018-smp`).
- **Next blocker reproduced in 45s QEMU** (`research/q0018-long`, zero unknown glyphs): BSD initialization reaches MAC framework, network mcache and `mbinit: done`, then **`pthread kernel extension not loaded (function table is NULL). @pthread_shims.c:318`**. Do not bypass pthread_init: find/integrate the matching open-source libpthread kernel implementation and register its real function table. A 12s run `research/q0018` looked panic-free before the 30s IORTC wait elapsed; use >=45s or GDB checkpoints for the next iteration. No userland/root filesystem boot yet.
- **Phone 0018 TEMPORARILY BOOTED** 2026-10-15 **15:31:15 UTC**: exactly one FASTBOOT device verified product sweet; send OKAY0.400s, boot OKAY0.134s, total0.539s, exit0. Local log `research/boot0018.log`. This confirms bootloader acceptance only. User was asked for a photo; await it, DO NOT reboot the phone again before their observation. No flashing performed.
- Current hardware image `shim/out/shim-boot-0018.img`, **16,437,248 bytes**, SHA256 **`b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`**. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin` (timer19), unpacked ramdisk exactly matches payload (`shim/out/check-0018`). EntryPA0xb146b488; kernelendPA0xb1ffc000; boot_argsPA0xb2000000/top0xb2004000. Command line `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`.
- QEMU payload/ADT `research/q0018.bin` / `research/q0018.adt` (timer27). Run `python3 tools/qemu-munch.py research/q0018.bin research/NEW-OUT --secs 45 --kernel xnu/out-0018/kernel.development.sweet --fail-on-panic`; fresh ADT seed for a new payload. Output fb.png/fb.txt/regs.txt/qemu.log/result.json. Visible panic exit2; blank/incomplete decode exit3; no_panic_observed is snapshot-only, not successful boot. -noprogress prevents the graphical indicator corrupting console text. Qualcomm XPU/bootloader/panel and SMC PSCI still need physical validation.
- Patches0014-0017 fixed PRNG registration/direct SHA256, explicit unavailable trust-cache service with rejected loads/no matches, built-in SweetPlatformExpert, and timer PPI from ADT (phone19/default19, QEMU27). All their checks/artifacts are preserved in the history below. CURRENT SWEET has NO USE_APPLEARMSMP; do not assume gCPUIC/gAIC/gPMGR AppleARMSMP globals exist.
- Local single-file checker: tools/xnu-cc.py research/run22.log research/bt22 FILTER --per-file, output via XNU_CC_OBJDIR. Patch checker `research/check-patch-order.py` applies all18 via temporary index, matches sandbox sources. Sandbox for0018 `research/bt21/xnu`, originals `research/pre0018`, objects `research/cpu-objs`. First compiler issues were missing machine/atomic.h and nonexistent getNameSymbol; fixed and both TUs compiled successfully before CI. Fixtures/artifacts under research are local-only/ignored.
- GDB: mask int(register)&((1<<64)-1) for unsigned kernel pointers; use function-entry manual-call return traps, discard manipulated guests. No QEMU fixture is intentionally left running.

### Earlier active handoff snapshot (superseded)


- **0018 milestone:** Actions `38760004398` PASSED (3m56s), checksums passed, new CPU/build files byte-match CI tree. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; log/tree `research/run22.log` / `research/bt22`. QEMU `research/q0018` 12s: boot CPU registered, SGI1 verified, CPU initialization complete, **BSD initialization entered** (copyright, MAC framework, buffer headers), no panic/zero unknown glyphs. Direct CPU API checks PASS (`research/check-cpu18.gdb`, `.log`, `research/q0018-api`): CPU0 Running, processor matches processor_array[0], real SGI enabled, CPU name correct, secondary-start returns KERN_NOT_SUPPORTED. Two-CPU ADT correctly panics before registration (`research/q0018-smp`). Longer 45s run `research/q0018-long` still running; next BSD wait/fault not yet identified.
- **Prepared 0018 phone image:** `shim/out/shim-boot-0018.img`, 16,437,248 bytes, SHA256 `b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin`, usual debug args with cpus=1 and -noprogress. Unpacked ramdisk byte-compared (`shim/out/check-0018`). Entry PA remains 0xb146b488; kernel end PA 0xb1ffc000, boot_args 0xb2000000/top0xb2004000. User reports FASTBOOT+USB ready; read-only check found exactly one product sweet. NEXT: temporary fastboot boot of this verified image, record send/boot results, then await hardware photo without rebooting again.
- Patch 0018 committed/pushed **`e7d9550`**; Actions **`38760004398`** queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. Do not launch a duplicate run.
- Active continuation: patch **0018** implemented in `iokit/Kernel/arm/SweetCPU.cpp` + arm64 source list + exact qcom,kryo470 personality. Keeps legacy IOCPU, accepts ONLY the single boot CPU, fills ml_processor_info from parsed topology with CPU object as cpu_id, calls ml_processor_register and processor_start. initCPU sends SGI1 via ICC_SGI1R_EL1 and waits (1-second deadline) for real cpu_signal_handler to clear SIGPdisabled. Only after successful startup calls ml_cpu_init_completed/cpusRunning and publishes completion/max CPUs. Secondary startup/suspend are explicitly unsupported. Local C++ compile passed for both TUs (`research/cpu-objs`), all imports resolve, object contains dsb ishst / ICC_SGI1R / isb; all18 patches apply in order and match sources. Sandbox `research/bt21/xnu`, originals `research/pre0018`; next CI artifacts should use `xnu/out-0018`, `research/bt22`, `research/run22.log`. User has the phone available; first run QEMU, then prepare/temporarily boot useful hardware progress.
- Goal: iOS 16 generation XNU on Redmi Note 10 Pro / sweet. User wants persistent progress, commits, kernel builds via `gh` Actions, and this handoff kept current across agent switches. Temporary `fastboot boot` only; preserve Android and backups.
- Latest kernel implementation: **`aa3f991`**, patches **0001-0017**. All four new Actions builds PASSED: 0014 `38748543058` (4m0s), 0015 `38749549779` (4m3s), 0016 `38750733996` (4m3s), 0017 **`38751507437` (4m7s)**. Latest: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. No build is pending.
- **Current blocker: CPU initialization has no SWEET CPU driver.** QEMU registers SweetPlatformExpert, then kernel_bootstrap_thread waits in **kperf_init_early -> ml_wait_max_cpus**. Confirmed via actual-kernel GDB call stack (`research/locate-cpu-wait2.log` / `.symbols`) and normal-run `max_cpus_initialized == 2` (MAX_CPUS_WAIT). Do not force the flag or remove the wait. Next implement real CPU registration and completion using XNU's CPU/processor interfaces. CURRENT SWEET uses legacy IOCPU (`USE_APPLEARMSMP` is absent); enabling AppleARMSMP is a separate architectural change requiring GIC/PMGR services. No userland/root filesystem boot yet.
- Fixed this session: PC=0 unregistered PRNG callback (0014, built-in HMAC-SHA256 provider + direct entropy/write_random hashing); absent Image4/AMFI trust-cache service (0015, unavailable runtime, loads rejected and queries no-match); missing platform driver (0016, SweetPlatformExpert exact personality); incorrect QEMU timer IRQ (0017, validated ADT PPI property, phone default19/QEMU27).
- **Latest kernel** `xnu/out-0017/kernel.development.sweet`, SHA256 `055a80190ab998b4df9203f1102072dcc78faad1779a537a514e522b682b582f`. Buildtree/log **`research/bt21` / `research/run21.log`**; all nine changed C/C++/header/config files byte-match local checked sources. Checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`. Preferred baseline for patch 0018: `research/bt21/xnu` (no nested .git).
- **QEMU verified**: `research/q0017-noprogress` ran **30 seconds**, no visible panic, **zero unknown glyphs**, IRQ exceptions returned normally (4 IRQs, tickless idle). It still waits for CPU completion, so this is NOT a completed boot. Earlier 15s run `research/q0017` captured 122 IRQs, timer value27, GICR_ISENABLER0 `0x08000002`, and CPU WAIT; graphical progress overlay corrupted 13 glyphs. Add **`-noprogress`** to keep the console readable. New runner classifies incomplete/blank decoding explicitly (exit3 with --fail-on-panic), visible panic exits2. Snapshot-only; use regs/GDB to diagnose stalls.
- Current QEMU payload/ADT: **`research/q0017-noprogress.bin` / `research/q0017.adt`**. Run: `python3 tools/qemu-munch.py research/q0017-noprogress.bin research/NEW-OUT --secs 30 --kernel xnu/out-0017/kernel.development.sweet --fail-on-panic`. Generate fresh seed with mkadt for a new payload. QEMU virt models generic ARM execution, not Qualcomm XPU/bootloader/panel or the phone's SMC PSCI conduit.
- **Prepared phone image**: **`shim/out/shim-boot-0017.img`**, 16,416,768 bytes, SHA256 `51ff7fe30b4a6668c3452ffe1da922c5ac30cb8de60625c2a652587ecbd0ec92`. Phone ADT/payload `shim/out/sweet-0017.adt` / `shim/out/payload-0017.bin` (timer19). Unpacked ramdisk exactly matches payload (`shim/out/check-0017`). Command line **`debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`**. NOT sent to phone this session: known CPU-driver wait should be addressed in QEMU first. Last actual phone boot remains 0013; user's latest photo `IMG_20261015_160033.jpg` confirmed the old PRNG PC=0 panic.
- Direct QEMU/GDB API checks: PRNG 600-byte chunking, buffer bounds, changing output, zero-length reads/writes, user reseed, contended refresh, generator1 init/output PASS again on latest0017 (`research/check-prng17.gdb` / `.log`, guest `research/q0017-api`). Trust-cache unavailable-service/argument/output cases PASS on0015 (`research/check-trustcache.gdb` / `.log`). Platform registration + target/product normal/truncated/zero/null/bounds PASS on0016 (`research/check-platform.gdb`, `research/check-platform3.log`). Different platform ID correctly retains IOPanicPlatform fallback (`research/q0016-negative`). Timer PPI32 and size2 correctly panic (`research/q0017-badppi`, `research/q0017-badsize`); omitted property selects19 (`research/q0017-default`). These are API/bring-up checks, not crypto certification or SMP scheduling validation.
- Local checker **`research/check-patch-order.py`** verifies all17 patches and changed source equality using a temporary upstream git index. Fixtures/logs/artifacts under research are local-only and ignored. For GDB unsigned kernel pointers, mask `int(register_value) & ((1<<64)-1)`; x30/x29 may be rendered signed. Stop at a function entry for manual-call return traps, and discard each manipulated guest after API tests. All fixture QEMU guests have been stopped.


- User wants persistent progress toward iOS 16, commits for completed changes, builds through `gh` / GitHub Actions.
- Keep this file current at every milestone so another agent can resume immediately, including when usage limits interrupt a session.
- Patch 0017 committed/pushed `aa3f991`, Actions `38751507437` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. Next artifacts: xnu/out-0017, research/bt21, research/run21.log. Do not duplicate.
- Patch 0016 kernel downloaded/checksum verified: SHA256 `d06336c54baa06e0df6561a8bd7df98a9b9f7502c9381c62d1a44c045cd1ce46`, `xnu/out-0016/kernel.development.sweet`; buildtree/log `research/bt20` / `research/run20.log`. QEMU `research/q0016` (10s) prints platform expert registered, zero unknown glyphs, no panic observed BUT stalls (timer mapping still wrong until 0017). Direct actual-kernel GDB tests passed for registered instance and target/product names, including maxLength=1/0 and NULL, sentinels intact (`research/check-platform.gdb`, `research/check-platform3.log`, `research/q0016-api3`). First two fixture attempts used premature/wrong return breakpoints; discarded, corrected fixture uses start-return observation then a function-entry return trap. No phone boot yet.
- Patch 0016 CI `38750733996` PASSED (4m3s); artifacts not yet downloaded/booted. Patch 0017 prepared to fix QEMU timer fidelity BEFORE interpreting future no-panic/stall runs: actual `qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=research/qemu-virt.dtb ...` timer property has virtual PPI 11 => INTID 27, while phone uses INTID 19. Kernel formerly hardcoded 19. New optional /arm-io/gic virtual-timer-intid property selects the PPI (validate exact u32 and 16..31); missing property retains phone default 19. One RO-late global drives GIC enable and sleh_irq dispatch. tools/mkadt.py emits phone19/QEMU27. Both C TUs compile (`research/timer-objs`), ADT serialize/parse checks confirm values/addresses, all 17 patches apply and match. Sandbox `research/bt19/xnu`, originals `research/pre0017`; initial local sleh compile failed due read-only exported header not updated, fixed export and BOTH recompiled successfully. CI exports normally.
- Architecture correction for next agent: CURRENT SWEET board_config does NOT define USE_APPLEARMSMP; it compiles the legacy IOCPU path, not cpu_boot_thread/IOPMGR AppleARMSMP. Existing roadmap mentions AppleARMSMP as a future approach. Do not look for gCPUIC/gAIC/gPMGR globals in the current kernel: they are absent. Determine the actual CPU-init wait after platform registration before choosing the next driver architecture.
- Patch 0016 committed/pushed as `0dcdf5c`; Actions `38750733996` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38750733996. Next artifacts: `xnu/out-0016`, `research/bt20`, `research/run20.log`.
- Patch 0016 implemented: new `iokit/Kernel/arm/SweetPlatformExpert.cpp`, arm64 source list, built-in personality in KernelConfigTables.cpp guarded by ARM64_BOARD_CONFIG_SWEET. Matches only xiaomi,sweet at score 10000, keeps IOPanicPlatform fallback for other hardware. Uses IODTPlatformExpert base initialization/power domain and createNubs CPU logical-ID annotation; overrides top-level publishing because base requires nonexistent Apple NVRAM /options. Publishes real CPU/top-level nodes, provides bounded target/product names, and registers the platform service. GIC/PMGR and arm-io child-bus drivers remain separate work. Both C++ TUs compile (`research/platform-objs`), all imports resolve against 0015/own objects, all 16 patches apply and sources match. Sandbox `research/bt19/xnu`; originals `research/pre0016`. NEXT: full Actions/QEMU; inspect CPU boot waits after the platform registers, do not equate absence of panic with successful boot.
- Patch 0015 CI `38749549779` PASSED (4m3s), checksum verified. Kernel `xnu/out-0015/kernel.development.sweet`, SHA256 `ca1b424a596c7058550ad1659080a333e7643270f019656401d80bf2b876645f`; buildtree/log `research/bt19` / `research/run19.log`. QEMU `research/q0015` (10s, zero unknown glyphs) passes trust-cache handling and machine lockdown, then panics `Unable to find driver for this platform: "xiaomi,sweet" @IOPlatformExpert.cpp:2319` (IOPanicPlatform fallback). Next concrete work: add a built-in SWEET IODTPlatformExpert personality/class; keep the fallback for mismatched platforms. No phone boot yet.
- Direct trust-cache API checks PASSED in the actual 0015 kernel under QEMU/GDB (`research/check-trustcache.gdb`, `research/check-trustcache.log`, `research/q0015-api`): runtime pointers NULL, hash/UUID queries return NOT_FOUND, typed/untyped/legacy loads return NOT_SUPPORTED, capabilities outputs zero and bounds preserved, invalid-argument cases still reject. Second PRNG check also verifies contended refresh returns immediately (`research/check-prng2.log`, `research/q0014-api2`).
- Patch 0015 committed/pushed `8f851d4`, Actions run `38749549779` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779. Do not duplicate.
- Direct PRNG API checks passed in the REAL 0014 kernel under QEMU/GDB, stopped before trust-cache init (`research/check-prng.gdb`, log `research/check-prng.log`, guest `research/q0014-api`). Invoked read_random for 600 bytes (chunking and buffer sentinel intact), repeated output differed, zero-length read/write succeeded, write_random(64-byte input) succeeded, secondary generator 1 initialized and generated distinct output. QEMU log has no exceptions during these checks; guest was discarded after the tests. This tests API behavior, not a cryptographic certification or SMP scheduling.
- Patch 0015 implemented in `bsd/kern/kern_trustcache.c`: SWEET with missing Image4/AMFI marks both runtime pointers NULL and logs service unavailability. Existing static loader handles NULL. Public load APIs return KERN_NOT_SUPPORTED; hash/UUID queries return KERN_NOT_FOUND; capabilities outputs are zero with KERN_NOT_SUPPORTED. Invalid-input checks remain, and Apple paths/available-provider initialization retain existing behavior. No trust is granted. Local arm64 compile passed (`research/trustcache-objs`), all 15 patches apply and sources match. Sandbox `research/bt18/xnu`, baseline `research/pre0015`. Next: Actions/QEMU and direct unavailable-service API checks using GDB.
- Patch 0014 CI `38748543058` PASSED (4m0s), artifact checksum passed. Kernel `xnu/out-0014/kernel.development.sweet`, SHA256 `741de20c851755d333759d7e668346f3edb4f325efac4cc7c142d80d1f5ff75c`; buildtree/log `research/bt18` / `research/run18.log`. QEMU run `research/q0014` (10s, zero unknown glyphs) prints `SWEET PRNG: HMAC-SHA256 registered`, passes commpage initialization, loads vnguard policy, THEN panics `image4 interface not available @kern_trustcache.c:253`. PC=0 fault FIXED IN QEMU. No new phone boot performed.
- Next work: SWEET currently has neither AppleImage4 nor AMFI. `trust_cache_runtime_init` unconditionally requires both; unlike BCM2837, SWEET enters this startup path. Investigate an explicitly unavailable trust-cache runtime with rejected loads/no query matches while providers are absent; preserve all Apple-platform checks. Merely skipping the startup call would leave later AMFI/IMG4 dereferences unsafe. Existing load_static_trust_cache already handles trust_cache_rt == NULL.
- QEMU runner now supports `--kernel MATCHING_KERNEL --fail-on-panic`: runtime font extraction from the payload produces exact `fb.txt`, plus `result.json`; a visible panic returns 2. Baseline 0013 re-run `research/q0013-text` detects the same NULL callback panic with zero unknown glyphs. Synthetic glyph roundtrip (including literal `?`) and invalid-VA rejection passed. Screen rows remain circular, not a full log; `no_panic_observed` does not prove successful boot. README has the invocation.
- Patch 0014 committed/pushed as `be61f99`; Actions run `38748543058` is running: https://github.com/dipa-studio/ios-sweet/actions/runs/38748543058. Do not start a duplicate build.
- 2026-10-15 active: hardware photo `IMG_20261015_160033.jpg` confirms the same PC=0 instruction-fetch abort as QEMU (LR `0xfffffe0007443fe4` = read_random). Independently reproduced with kernel 0013 in `research/q0013` (8-second run); prior allocator fix works. Latest tooling commit reviewed: `8d66d2d`.
- Patch 0014 implemented: registers a built-in HMAC-SHA256 DRBG provider at EARLY_BOOT/FIRST, before commpage creation. A shared mutex serializes output/reseed across CPUs (correctness first; no per-CPU scaling yet), initgen validates CPU identities, refresh uses try-lock and the existing entropy filter/health tests. SWEET entropy accumulation and write_random now use the built-in SHA256 directly; no incomplete g_crypto_funcs table is registered. The existing bootloader-seed requirement remains enforced.
- Local validation: both PRNG translation units compile to arm64 objects in `research/prng-objs`; no SHA256 wrapper imports remain. All 14 patches apply in CI order and resulting PRNG sources match local inputs. Sandbox `research/bt17/xnu`, originals `research/pre0014`, checker `research/check-patch-order.py`. NEXT: Actions kernel build, download to `xnu/out-0014`, run the QEMU payload before requesting another phone boot. Baseline QEMU ADT/payload are `research/q0013.adt` / `research/q0013.bin`.
- 2026-10-15 08:15 QEMU EMULATION WORKS (`tools/qemu-munch.py`, see "QEMU emulation" below). Patch 0013 VERIFIED in QEMU: boot passes kmem_crypto_init and ulock_initialize. NEXT BLOCKER (reproduced in QEMU, expected on hardware too): `pmap_create_commpages -> random() -> RandomULong -> read_random -> prng_funcs.refresh == NULL` (instruction fetch abort at pc 0, lr read_random+0x34). Nobody calls `register_and_init_prng` (closed corecrypto kext does it on Apple). Fix = patch 0014: SWEET-registered cckprng_funcs provider (osfmk/prng/prng_random.c); note random_cpu_init also panics while !prng_ready. The 07:26 hardware boot of 0013 will most likely show this same panic.
- Previous hardware boot: patch 0013 (`a3a46e2`) CI `38686345689` PASSED (3m44s), image `shim/out/shim-boot-0013.img` was temporarily booted on 2026-10-15 at 05:26 UTC: send OKAY (0.407s), boot OKAY (0.134s), total 0.546s. Superseded by the hardware/QEMU PC=0 observation and patch 0014 work above.
- Latest kernel implementation commit: `8f851d4`. Latest successful kernel CI: `38749549779` (patch 0015), https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779.
- CURRENT image: `shim/out/shim-boot-0013.img`, 16,412,672 bytes, SHA256 `110d1c46aee4b60253db24b6535e9781c4cf0fd25ca2a70e9016cf81378cea25`. Kernel `xnu/out-0013/kernel.development.sweet`, SHA256 `f0b1d375995316c550ce7014ff234d8271e9b9ac2857795ceff41b76ba2cd740`, plain arm64. CI log confirms 0013 applied; checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`.
- Current log/buildtree: `research/run17.log`, `research/bt17` (extracted, no nested .git; preferred baseline for patch 0014). Payload/ADT: `shim/out/payload-0013.bin`, `shim/out/sweet-0013.adt` (includes /defaults). Unpacked ramdisk byte-compared with payload in `shim/out/check-0013`.
- Patch-0012 original image: `shim/out/shim-boot-0012.img`, 16,412,672 bytes, SHA256 `cf4b8ec4a65a51888d71c17d09cd0831d6b761b90e2531fdb8e575c32f4fd24a`. Kernel `xnu/out-0012/kernel.development.sweet`, SHA256 `69b78f49f6459cc63a0af44e7cf73c1779ef72130ef72b12ae86c88183ffc742`; checksums passed and sweet_early_relocate symbol present. Entry PA remains `0xb146b488`, kernel end PA `0xb1ff4000`.
- Patch-0012 log/tree: `research/run16.log`, `research/bt16` (extracted, no nested .git; preferred baseline for next patch). CI log confirms 0012 applied. Payload `shim/out/payload-0012.bin`, ADT `shim/out/sweet-0012.adt`; packed ramdisk byte-compared with payload in `shim/out/check-0012`.
- Active payload command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.
- Latest observed photo: `/home/dipa/Downloads/IMG_20261015_071346.jpg` (local only). Stages 9 (kernel framebuffer VA), 10 (TTBR0 off/TLB flush), 11 (physmap table pointers) and 12 (calling pmap_bootstrap) all visible. Persistent diagnostics are VERIFIED ON HARDWARE.
- pmap.c:1987 is the assert after SecureDTLookupEntry(NULL, "/defaults", &entry). The pmap-max-asids property itself is optional; an empty /defaults uses XNU's MAX_ASIDS. Do not bypass the assertion or guess an ASID limit.
- Previous payload-only fix image: `shim/out/shim-boot-0012-defaults.img`, SHA256 `3ff61a268853c6ea7cbf284833f1346a1d791980ca366b6e1a215a86e040512b` (16,412,672 bytes). Same kernel/CI as 0012; ADT 2516 bytes (`shim/out/sweet-0012-defaults.adt`), root /defaults present with no ASID override. Payload `shim/out/payload-0012-defaults.bin`; unpacked ramdisk exactly matches payload (`shim/out/check-0012-defaults`). This boot reached the allocator crypto NULL-pointer fault below.
- Latest hardware photo: `/home/dipa/Downloads/IMG_20261015_071608.jpg` (local only). Normal serial framebuffer console works. VM bootstrap/zone initialization, logging, scheduler, IPC, KDP and ulock_initialize print before the kmem_crypto_init data abort. gPanicBase warnings are expected because PLATFORM_PANIC_LOG_DISABLED is configured, and are not the faulting call.
- Faulting instruction: `ldr x0, [x8, #0x150]` in crypto_random_kmem_ctx_size; x8 = NULL g_crypto_funcs. Source: libkern/crypto/corecrypto_rand.c and osfmk/vm/vm_kern.c:2448. rand.h explicitly permits NON-cryptographic kmem RNGs, but requires no locking in generation and context size <=256. Do not substitute read_random here: the global PRNG provider is also unregistered and kmem generation must remain lock-free. early_random may seed contexts during single-CPU bootstrap; it uses the built-in HMAC-SHA256 DRBG with the host-generated ADT seed.
- Patch 0013 keeps the replacement entirely private to vm_kern.c: SplitMix64 state is 8 bytes per CPU/sizeclass, initialized using early_random. Uniform selection uses rejection sampling to avoid modulo bias. Other platforms retain the original corecrypto calls; SWEET's generic crypto APIs/registration remain untouched. Reference: https://prng.di.unimi.it/splitmix64.c (Sebastiano Vigna, public domain).
- Validation: vm_kern.c compiles to arm64 (`research/kmem-rng-objs`, using `research/run16.log` + `research/bt16`). Undefined symbols include early_random and NO crypto_random_* references. All 13 patches apply in CI order and match the checked source. Original saved as `research/vm_kern.pre0013.c`.
- RNG host checks: extracted actual new helper in `research/kmem-rng-check.c`; 16,384 outputs match the author's downloaded reference (`research/splitmix64-reference.c`), 147,456 bounded outputs satisfy their limits (including 1 and UINT64_MAX), and a forced rejection advances correctly. Full kernel crypto services and the registered kernel PRNG provider remain future work; this patch only supplies the allocator RNG contract.
- Patch-0011 artifacts: kernel `xnu/out-0011/kernel.development.sweet`, log `research/run15.log`, extracted buildtree `research/bt15` (no nested .git). CI log confirms patch 0011 applied; artifact checksums passed. Kernel is plain arm64, entry VA `0xfffffe000746b488` -> PA `0xb146b488`, kernel end PA still `0xb1ff4000`.
- Patch-0011 image: 16,412,672 bytes, SHA256 `29ad5cbfc752473de4af2c471a24a8b34f92da4bff5afec65c614b90cd3ad53a`. Kernel SHA256 `8ac3a956894b09f7bd0d890de8c5de38c65513287feb1e1bcd12db8ca772ee5c`. Payload/ADT in `shim/out/payload-0011.bin`, `shim/out/sweet-0011.adt`. Header unpacked/checked in `shim/out/check-0011`.
- First patch-0011 attempt timed out waiting for FASTBOOT; superseded by the successful send/boot recorded above. Do not repeat the boot before the user reports the current screen.
- Patch-0011 photo: `/home/dipa/Downloads/IMG_20261015_070218.jpg` (local only). Visible stages 1-8 and old red shim text; entry PA B146B488, boot args PA B1FF8000 / top B1FFC000. No evidence yet locating execution after stage 8.
- Patch-0012 sandbox: `research/bt15/xnu` (NO nested .git). Proposed permanent debug VA: VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE, in the reserved gap below gVirtBase and above low globals; map the same PA framebuffer as Normal-NC in the final TTBR1 before arm_vm_physmap_init changes phystokv(). Keep the early panic sink until normal kprintf startup.
- Patch 0012 validation: all 4 changed C translation units compiled to arm64 objects in `research/post-ttbr0-objs`; internal header export added to osfmk/arm64/Makefile and recreated locally in EXPORT_HDRS for pe_serial.c. All 12 patches apply in CI order and match all 6 edited files. Baselines are saved under `research/pre0012/`.
- Actual mapping-helper host model (`research/early-map-check.c`, barrier stubbed only for host compilation) confirms: VA `0xfffffe0002000000`, L1 index 2016, L2 index 1; descriptor `0x006000009c000605` = framebuffer PA, Normal-NC, outer-shareable, EL1 RW, PXN+UXN. All other L2 entries preserved. The gap is excluded by ARM_LARGE_MEMORY's pmap_virtual_region(0).
- Next boot command line MUST add `-enable_kprintf_spam`: DEVELOPMENT disables kprintf_output by default even when DB_KPRT is set (`pexpert/arm/pe_kprintf.c`). Planned command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.

### Patch 0012 screen legend (after original stages 1-8)
- 9: kernel framebuffer VA live; 10: TTBR0 off / TLB flushed; 11: physmap pointers rebound.
- 12: calling pmap_bootstrap; 13: pmap_bootstrap returned; 14: early I/O page tables ready; 15: arm_vm_init returned.
- 16: calling serial_init; 17: fbcon config read / mapping; 18: fbcon mapping returned; 19: serial_init returned; 20: kprintf startup complete.
- Normal serial initialization clears the screen, so earlier stages may disappear before 19. If serial exists, early console disables after 20; otherwise early panic output stays enabled.
- Local source sandbox: `research/bt13/xnu` (git baseline `623b060`, patches 0001-0008 already in baseline; uncommitted export edits correspond to patch 0009). Preserve those edits when producing patch 0010.
- Previous compile log/tree: `research/run13.log`, `research/bt13`; previous kernel: `xnu/out/kernel.development.sweet`. New successful log/tree: `research/run14.log`, `research/bt14`; new kernel: `xnu/out-0010/kernel.development.sweet`.
- Current device access: patch-0012 image was temporarily started from FASTBOOT; no adb connection is expected. Never flash partitions for bring-up; use `fastboot boot` with the shim image.
- Patch `0010` lives in `xnu/patches/0010-munch-early-framebuffer-debug.patch`. Its exact compiled sources are under `research/bt13/xnu`; generated objects are in `research/early-objs`.
- Local checks passed: all 5 changed/new translation units compiled to arm64 Mach-O objects; new console only imports `_doprnt` and `iso_font` (both exist in the previous kernel). All 10 patches applied to a temporary upstream git index in CI order, and all 7 resulting changed source files matched the local compile inputs.
- Reproduce changed-file compilation: `XNU_CC_OBJDIR="$PWD/research/early-objs" python3 tools/xnu-cc.py research/run13.log research/bt13 arm_init.c arm_vm_init.c kern/debug.c arm64/start.s osfmk/arm64/sweet_early.c --per-file` (create the output directory first). The helper now accepts explicit new source paths absent from the old artifact's Makefiles and rejects empty selections.
- Device command result: `adb reboot bootloader`, then `timeout 30s fastboot boot shim/out/shim-boot-0010.img`: send OKAY (16028 KB), boot OKAY, total 0.541s. This confirms bootloader acceptance only, not XNU progress.
- The successful boot above was on 2026-10-14. On 2026-10-15, `timeout 45s fastboot boot ...` timed out waiting for a device; no image was sent. Boot image SHA256 rechecked and unchanged. No new build is needed to repeat the trial.
- Earlier retry: `timeout 90s fastboot boot shim/out/shim-boot-0010.img` timed out (exit 124). This is superseded by the successful 2026-10-15 repeat boot recorded above; do not repeat it until the user reports the screen.
- User observation confirms MMU-on C entry, platform expert, CPU topology/bootstrap, startup bootstrap and cpu_init all complete. Failure is within arm_vm_physmap_init (non-XNU_MONITOR branch), before stage 8 / TTBR0 teardown. Old red shim text remains behind the early console; this is expected because only written rows are cleared.
- New edit/compile sandbox is `research/bt14/xnu` (extracted artifact, NO nested .git; `git diff` there would act on the parent project). Use a saved baseline file / generated unified diff, or explicitly initialize a local baseline before editing. `research/bt13/xnu` does have its own baseline git repo.
- Exact user-reported panic: `arm_vm_init.c:1606 Assertion failed: (temp_ptov_table[ptov_index - 1].va + temp_ptov_table[ptov_index - 1].len) <= physmap_end`.
- Root cause: MacOSX build sets `ARM_LARGE_MEMORY=1`; `XNU_MONITOR` is off. Block alignment in arm_vm_physmap_slide can add gaps, but the non-PPL large-memory physmap_end omitted the padding already present in the small-memory branch. Patch 0011 reserves PTOV_TABLE_SIZE*ARM_TT_TWIG_SIZE (8*32MB) in both the aperture bound and its L1 allocation; monitor/PPL builds retain zero padding. The assertion is preserved.
- Validation: `arm_vm_init.c` compiled to an arm64 object using `research/run14.log` + `research/bt14` (object in `research/physmap-objs`). All 11 patches apply in CI order and produce exactly that source. Host harness `research/physmap-check.c` extracts the real XNU slide function with page-table-write stubs: all 65,536 page-aligned slides within 1GB checked against the current kernel memory layout; old bound fails 65,504 cases, padded bound fails zero; worst overrun 0x1ffc000 bytes. Baseline source saved as `research/arm_vm_init.pre0011.c`.
- CI: https://github.com/dipa-studio/ios-sweet/actions/runs/38638702083 (success; artifacts and full log downloaded).
- New image: `shim/out/shim-boot-0010.img` (16,412,672 bytes, SHA256 `ed2c270e0081bad5ff517be415f9bfe8805463aea7866732203fa97ac737e64e`). Previous image preserved as `shim/out/shim-boot-pre0010.img`.
- New kernel SHA256: `3ee05270cc291c49589a101678f904b1b69cb2f2575e05398bce8a4e052f409a`. Artifact checksums passed; arm64 (not arm64e), early-console symbols present. Payload entry VA `0xfffffe000746a488` -> PA `0xb146a488`; kernel end PA `0xb1ff4000`. Command line remains `debug=0xc serial=3 cpus=1 keepsyms=1`.

### Patch 0010 screen legend
- Bottom-left white 64x32 tile: assembly entered `start_first_cpu`, before page tables/MMU.
- Text `SWEET EARLY 1`: entered `arm_init` with MMU on; 2: platform expert done; 3: CPU topology done; 4: CPU bootstrap done; 5: startup bootstrap done; 6: CPU init done; 7: entering `arm_vm_init`; 8: leaving bootstrap TTBR0.
- `SWEET EARLY PANIC:` prints a copy of panic arguments before normal panic handling needs per-CPU/thread state.
- In patches 0010/0011 the console disabled before TTBR0 invalidation; patch 0012 replaces that with a persistent kernel VA alias and diagnostics through normal serial/kprintf startup (see its legend above).
- Screen contents require a physical observation from the user: adb cannot capture a framebuffer after Android has been replaced by a bare-metal kernel.

## QEMU emulation (2026-10-15)
- `tools/mkadt.py OUT.adt --qemu` = same ADT with QEMU virt GICD 0x08000000 / GICR 0x080a0000 / CNTFRQ 62.5 MHz.
- `tools/qemu-munch.py PAYLOAD OUTDIR [--secs N] [--hmp "x /8gx ADDR"] [--gdb]`: host-side copy of shim/loader.c
  (segments + ADT + boot_args at their PAs, 6-instruction stub sets x0 and jumps), `-M virt,gic-version=3 -cpu cortex-a76`
  (same ID_AA64MMFR0 as Kryo 470), 4G RAM so 0x9c000000 fb and 0xb0000000 kernel are plain RAM.
  Outputs `fb.png` (the screen), `regs.txt`, `qemu.log` (exceptions with ESR/FAR/ELR). `--gdb` halts with a stub on :1234
  (system `gdb` + `set architecture aarch64`, `hbreak *VA` works with the MMU on).
- Not emulated: Qualcomm XPUs, ABL, real panel, PSCI conduit (QEMU virt uses hvc). Hardware still decides those.
- Example: `python3 tools/mkadt.py /tmp/q.adt --qemu && python3 tools/mkpayload.py xnu/out-0013/kernel.development.sweet /tmp/q.adt /tmp/pq.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam' && python3 tools/qemu-munch.py /tmp/pq.bin /tmp/q`

## Layout
- `backup/`  partition dumps + full sdb..sdf LUNs, `SHA256SUMS` (all verified on-device == local)
- `dtb/`     live FDT (from OrangeFox), PixelOS boot.img dtb, dtbo, dts versions
- `shim/`    bare-metal boot shim (`./build.sh` -> `out/shim-boot.img`)
- `research/` mainline sm7150 dts, XNU pe_fiq.c etc, PocketDarwin patches
- `tools/unpack_bootv2.py`

## Device access
- No root in PixelOS 17 (user build, adb root disabled). Root = `fastboot boot recovery/recovery.img` (OrangeFox R12.0_2, temp boot, not flashed).
- `fastboot boot` works for stock boot.img, OrangeFox, and our shim.
- Shim resets itself via PSCI SYSTEM_RESET; IMEM restart_reason 0x77665500 is seen
  (bootreason=bootloader) but ABL still boots Android, so it does not stop in fastboot.

## Hardware facts (live DT + shim measurements)
- Framebuffer (ABL cont splash): 0x9c000000, 1080x2400, stride 4320, a8r8g8b8, panel is DSC *video* mode (no kick needed).
- RAM: 0x80000000+2G, 0x100000000+0xfcc00000. Live reserved-memory = PixelOS base dtb + sweet dtbo fixups.
- GICv3: GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000, 8 PEs). GICD_TYPER 0x007B041D: SecurityExtn=1 (DS=0).
  ABL leaves GICD_CTLR = 0x10 (ARE_NS on, groups off). Writing 0x13 reads back 0x12: EnableGrp1A (bit1) is the one that matters.
- GIC Group 1 VERIFIED (shim 2c): vtimer INTID 19 = 30 IRQs/300ms, ptimer INTID 18 = 30, SGI 1 to self = 10/10.
  ICC_SRE=1 already, ICC_CTLR 0x8440, priority/enable writes stick. GICR_IGROUPR0 and GICD_IGROUPRn read 0 from NS
  even though Group 1 delivery works (RAZ in NS view) - do not use them as a health check.
  INTID 28 shows pending constantly (unknown PPI 12).
- Timer 19.2 MHz. Qualcomm PPI numbering: secure-phys/phys/virt/hyp = PPI 1/2/3/0 -> vtimer INTID 19, ptimer 18 (NOT 27/30).
- PSCI 1.1 via SMC, SMCCC 1.1. CPU_ON ok; SYSTEM_SUSPEND and SYSTEM_RESET2 unsupported.
- SMP: CPU_ON works for all 7 secondaries (MPIDR 0x81000n00).
- Entry EL1, MMU off, x0 = DTB (magic ok). ID_AA64MMFR0 = ...101122: 16K granule YES, 4K YES.
- No serial console: debug UART SE is in SPI mode on sweet. Screen is the only debug channel.

## XNU implications
- XNU GICv3 path (pexpert/arm/pe_fiq.c, VMAPPLE) puts the timer (PPI 27) in Group 0 = FIQ. With SecurityExtn=1,
  Group 0 is secure -> impossible from NS EL1. Need: Group 1 IRQ, ICC_IGRPEN1, INTID 19, and the timer handled in the IRQ path.
- XNU VMAPPLE PSCI uses `hvc`; sweet needs `smc`.
- PocketDarwin NonAppleGenericCpu.patch only swaps -mcpu=apple-h7 for -march=armv8-a.

## Milestone 3 (2026-10-14): 16K MMU verified
- ABL loads the Image at 0x80080000 (base 0x80000000 + 0x80000) regardless of text_offset=0.
- 16K granule, T0SZ=28 (L2 start, 32MB blocks), MAIR 0x44ff00, TCR 0x1409CB51C, PARange=2 (40-bit PA).
- Only own image pages WB (Qualcomm XPUs: never map carve-outs cacheable); fb Normal-NC; rest Device.
- AT S1E1R checks all as expected; unmapped -> translation fault L2.
- Fill fb: 110 ms (MMU off) -> 3.5 ms (NC). Memory loop 70 ms -> 0.7 ms. vtimer + SGI IRQs still fine with MMU on.

## XNU loader design notes (xnu-8796)
- Entry: `_start` -> `start_first_cpu`, x0 = boot_args (PA), MMU off, EL1. boot_args: Revision/Version 2,
  virtBase/physBase/memSize/topOfKernelData, Boot_Video (fb 0x9c000000, rowBytes 4320, 1080x2400, depth 32),
  deviceTreeP/Length (ADT), CommandLine[608], bootFlags, memSizeActual.
- start.s maps [kernel mach header .. physBase+memSize) with cacheable BLOCKS. On Qualcomm every XPU carve-out
  must therefore lie below the kernel: use physBase = 0xb0000000 (above ramdump_fb 0xaf000000+16M,
  cdsp_sec, qseecom), end before rmtfs 0xfde01000 -> memSize ~0x4d000000 for bring-up.
- Base ADT: iPhone 8 d20ap DeviceTree from iOS 16.7.16 IPSW (tools/remotezip.py + tools/adt.py; LZFSE payload).
  Template values are empty: the loader must fill chosen/*, memory-map, cpu*/reg etc. like iBoot.
- XNU build: GitHub Actions (dipa-studio/ios-sweet, xnu.yml) via blacktop/darwin-xnu-build, MACOS_VERSION=13.5.
  New-clang fixes in xnu/patches; Xcode 16 iig is incompatible (IORPC.kernelContent) -> macos-14 runner, oldest Xcode.

## Kernel architecture plan (SWEET machine config)
- First stock build (VMAPPLE) is arm64e (PAC). Kryo 470 = ARMv8.2, no PAC -> SWEET must build plain arm64
  (HAS_APPLE_PAC is only defined for __arm64e__, so arm64 drops PAC automatically).
- Template: VMAPPLE (16K pages, GICv3 code in pe_fiq.c) minus APPLEVIRTUALPLATFORM hypercalls/paravirt PAC,
  plus BCM2837 (generic ARM: no APPLE_ARM64_ARCH_FAMILY Apple sysregs). Do not link the KDK lib<cfg>.os.*.a.
- Timer: XNU expects it on FIQ (sleh_fiq -> rtclock_intr). SWEET: GIC Group 1, so sleh_irq must ack ICC_IAR1,
  route INTID 19 to the same timer path, SGIs to cpu_signal_handler, EOI via ICC_EOIR1.
- SMP/interrupts: open-source AppleARMSMP (USE_APPLEARMSMP) needs an IOInterruptController (AIC role) and an
  IOPMGR (CPU power -> PSCI CPU_ON via SMC). Plan: built-in IOKit classes (SweetGIC, SweetPMGR, platform
  expert) compiled into the kernel and matched via iokit/KernelConfigTables.cpp -> no closed kexts needed.
- Debug: xnu patch 0003 sweet-fbcon = kprintf drawn on the framebuffer (ADT node "sweet-fbcon").
- ADT /arm-io ranges must not map to parent 0 (pe_arm_get_soc_base_phys()==0 disables serial): use parent 0x1000000.

## Status 2026-10-14 late: first XNU boot attempt
- SWEET kernel builds (CI run 38635343732, patches 0001-0009). Verified offline: arm64, no PAC, no >v8.2 code,
  70300 classic relocs all in range (slide 0 = no loader work), entry = __start.
- Boot: shim loader worked (initrd 0x846F0000-0x855FE3E0, BOOTARG 0xB1FF8000, JUMP 0xB146A488) but screen stayed
  red -> xnu died/hung before serial_init (fbcon). kprintf is a no-op before STARTUP_SUB_KPRINTF.
- Historical next-step plan (patch 0010, now implemented and built; see active handoff above): early fb debug channel
  1. start.s start_first_cpu (after mov x20,x0 / mov x21,#0): PA marker band (MMU off, direct store to 0x9c000000).
  2. start.s after both create_bootstrap_mapping calls: add ONE block entry to the existing V=P L2 table
     (x25 + 2*PGBYTES; do NOT call create_bootstrap_mapping again: fb and RAM share L1 entry 0, it would replace
     the L2 table). Entry = TYPE_BLOCK|VALID|SH(OUTER)|ATTRINDX(CACHE_ATTRINDX_WRITECOMB=1)|AF|PNX|NX|0x9c000000,
     index = (va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT.
  3. new osfmk/arm64/sweet_early.c (+files.arm64 entry): sweet_early_mark(n) colored bands at screen bottom and
     sweet_early_putc using iso_font via VA=PA while the bootstrap TTBR0 is live; disable before
     arm_vm_init's set_mmu_ttb(invalid_ttep) (arm_vm_init.c:2148).
  4. markers in arm_init after PE_init_platform, ml_parse_cpu_topology, cpu_bootstrap, kernel_startup_bootstrap,
     cpu_init, before arm_vm_init.
  5. panic hook at top of panic_trap_to_debugger: va_copy + _doprnt(fmt, &copy, sweet_early_putc, 16).
  Verify with tools/xnu-cc.py (compile+symbols) and patch-order simulation before the single CI run.

## Handoff 2026-10-20: RAM-only Munch boundary
- User confirmed `userdata` must never be read, backed up, mounted, or written; all device tests are to run from RAM via temporary `fastboot boot` only.
- Offline worktree changes prepared for that boundary: RAM-only boot guidance in README and Munch port notes; shim no longer falls back to its UFS probe and the build excludes `ufs.c`; `mkadt.py` has an explicit Munch profile requiring supplied DRAM bounds and omitting UFS; RAM-disk payload construction requires `rd=md0` and rejects ADTs exposing UFS.
- Munch ADT generation is scaffolding only, not boot-ready: the running UEFI memory map is unverified and XNU still uses SWEET-only platform matching/drivers. No claim of Munch XNU boot support.
- Existing `tools/mkpayload.py` memory-window checks predate the RAM-only edits and were preserved.
- After the user asked to stop, no device command was run. `userdata` was not accessed. No cleanup, reset, commit, CI run, or device test was performed; preserve all tracked and untracked artifacts and the non-userdata backups.
- At this checkpoint the work was paused; it resumed on 2026-10-20 and continued
  in the active handoff at the top of this file.
