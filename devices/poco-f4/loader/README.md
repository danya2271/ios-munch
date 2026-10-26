# Fixed XNU entry-checkpoint probe

XNU has reached early BSD initialization, and the static hello RAM-root program
has produced user-space output on the phone, followed by its intentional panic
and return. The original checkpoint-10 failure was bypassed by skipping its
premature PE bootcpu query. Storage FD revision 3 has read 64 KiB from iosroot.
The first iOS 16.5 APFS/userland boot package is prepared but not device-tested.
The per-stage notes below retain the history and limits of each diagnostic.

The source overlay is against upstream `xnuloader/loader.c` at `2404c65`, SHA256
`7104a1403bb0007a8fc7a7cf4dfb327bc7136a09ba9479f17e5eca449499557b`.
The prebuilt kernel hash is pinned in `pack-checkpoint.py`. The loader overlay
inherits the upstream loader's MIT license; the XNU kernel remains APSL-2.0.
Open diagnostics use a placeholder identity. iOS mode 58 uses an untracked local
identity generated from the downloaded IPSW device tree.

## Behavior

- Require the verified F4 GOP base, dimensions, pixel format, and stride.
- Use an explicit munch platform; never fall back to QEMU ramfb registers.
- Use the loader's EFI-memory-map-based window allocation.
- Permit only explicitly listed diagnostic stages and fixed command lines.
  Mode 57 supplies a pinned hello RAM root; mode 58 supplies the locally built
  APFS helper and verified system image. USB command mode stays disabled.
- Publish only the current CPU in the generated device tree. The original probe
  inherited upstream's behavior of listing every available CPU when any `cpus=`
  argument was present; this did not start those CPUs at checkpoint 1.
- Keep logs in RAM and on screen; remove the raw partition logger.
- Leave Redwood watchdog SMC and charger paths disabled by platform identity.
- Pause for 90 seconds after the result, then attempt a reset.

Checkpoint 1 is immediately after masking interrupts and saving the boot-args
pointer in XNU's `start_first_cpu`, before the later platform initialization.
Success means a valid kernel handoff and return path, not a working XNU boot.
The kernel's eight-second session argument is a later-stage mechanism and cannot
recover a hang before that mechanism is initialized.

Checkpoint 2 additionally installs XNU's low exception vector and writes
`OSLAR_EL1` to unlock core debugging, then returns before the later memory setup.
This tests whether that short additional path executes under the phone's
hypervisor; it still does not establish a full kernel boot.

Checkpoint 3 is after the initial translation-table preparation and its `dsb ish`
barrier, before `common_start`. The later MMU-enable sequence is beyond this
checkpoint. It checks the intervening early memory setup without progressing
into platform drivers or userspace.

Checkpoint 4 is in `common_start`, immediately before the write of
`SCTLR_EL1_DEFAULT` that enables caches and the MMU. It tests the intervening
system-register setup; reaching it does not prove MMU enable or translated execution.

Checkpoint 5 follows the write of `SCTLR_EL1_DEFAULT` and its instruction barrier.
It tests the first execution after enabling the MMU/caches, then disables the
MMU through the checkpoint return path and resumes UEFI. Success would not
establish later virtual-address execution or full platform initialization.

Checkpoint 10 is early in `arm_init`, after copying the boot arguments and before
`cpu_data_init` and `PE_init_platform`. It is the next instrumented checkpoint
after 5 in the normal boot path. It tests the assembly-to-C transition and the
C checkpoint-return implementation. A failure may occur in that transition or
the return mechanism; it does not by itself identify which one failed.

## Build

With clang, lld-link, patch, dtc/fdtget, and aarch64-linux-gnu-objcopy available:

```sh
python3 devices/poco-f4/loader/pack-checkpoint.py \
    --fd /path/to/revision-2/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd \
    --dtb /path/to/captured/live.dtb \
    --checkpoint 10 \
    --output /path/to/munch-xnu-checkpoint10.img
```

The script checks the loader/kernel baseline hashes and F4 FD manifest, applies
the source overlay in a temporary directory, creates a placeholder identity,
compiles the loader, and packages a fixed XNUP container. There is no command-line
option to enable later checkpoints or device drivers. `--checkpoint` accepts
only 1 through 5 and 10, defaulting to 1.

Test using `fastboot boot`, then photograph the result. Expected success text:
`STOPPED AT CHECKPOINT N`, matching the selected test number.
These images do not start the USB probe or a resident
fastboot server; inspect its on-screen result rather than issuing USB commands.

## Prepared artifact, 2026-10-26

`munch-xnu-checkpoint1.img`: 7884800 bytes, SHA256
`497d16d4f704aefdd1200cd4b583e227952c069862d23a73db50a08b4fb33eb7`.
The loader compiled and linked successfully. Re-read the Android image and
verified the exact FD/live DTB, pinned Mach-O hash, unique `XNULDRST` return
slot, ARM64 EFI loader, and the complete fixed XNUP entry set. No root ramdisk,
USB mode, or additional boot arguments are present.

The user supplied a photograph confirming `STOPPED AT CHECKPOINT 1` with a
return log length of zero, expected before the kernel logger initializes.
The loader claimed `[0xF4000000, 0x200000000)` with EFI success and entered the
kernel on the same CPU. The photograph showed eight advertised CPU MPIDRs,
which exposed the inherited enumeration behavior described above. It does not
demonstrate eight-core XNU execution. The return path and pause message appeared;
reset timing was not separately reported.

Prepared checkpoint 2: `munch-xnu-checkpoint2.img`, 7884800 bytes, SHA256
`fdc285e5876fb37856f22e79f0391bac8c5b518d8861e776a092b92b98b34ab1`.
The revised loader compiled with warnings treated as errors (unused inherited
helpers excluded). Verified the fixed checkpoint entry, command line, kernel
hash, unchanged FD/DTB, and image checksum.

The user supplied a photograph confirming `STOPPED AT CHECKPOINT 2` and a return
log length of zero. The loader's claim succeeded and the kernel returned to the
90-second pause message. This confirms the low-vector/OSLAR path on this phone;
it does not establish later MMU or platform initialization. The revised loader
publishes one CPU; the previous eight-MPIDR enumeration line is absent.

Prepared checkpoint 3: `munch-xnu-checkpoint3.img`, 7884800 bytes, SHA256
`5c7ac6b15b10a1b1707e860e1e6327fe3631dd5eb4c82bf446c6d9ba1e4fa394`.
Loader compilation passed with warnings treated as errors. Verified the selected
checkpoint, fixed arguments, complete payload entry set, unchanged FD/DTB/kernel,
and image hash. The user subsequently reported `STOPPED AT CHECKPOINT 3` in text.
This text report confirms the early
table-preparation path and return to UEFI, before MMU enable.

Prepared checkpoint 4: `munch-xnu-checkpoint4.img`, 7884800 bytes, SHA256
`614432e0dbf88738a46c078d15167164a2bde8b1d6aa29559b25a365d9dd5a0f`.
The loader build passed. Verified the selected checkpoint, fixed single-CPU
arguments, exact payload entry set, unchanged FD/DTB/kernel, and image hash.
The user reports checkpoint 4 passed. This confirms the path through
`common_start` to the pre-MMU checkpoint; no screenshot accompanies this report.

Prepared checkpoint 5: `munch-xnu-checkpoint5.img`, 7884800 bytes, SHA256
`8b72e4a4818a7396e99dddd5c77360685ed46b6b4204a9a04499e5400418ed6a`.
The loader build passed. Verified checkpoint 5, fixed single-CPU arguments,
exact payload entry set, unchanged FD/DTB/kernel, and image hash. The user reports
checkpoint 5 passed. This confirms initial execution after MMU/cache enable and
return to UEFI. No photograph accompanies this successful text report.

Prepared checkpoint 10: `munch-xnu-checkpoint10.img`, 7884800 bytes, SHA256
`c12e42e7a0cda2b1ffcda0f78572cd5ffc135f1aec61fbf1a030d7e3e1d8123a`.
The loader build passed. Verified checkpoint 10, fixed single-CPU arguments,
exact payload entry set, unchanged FD/DTB/kernel, and image hash. Device result
is pending.

## Intermediate checkpoint 6

Checkpoint 6 is a diagnostic-only binary overlay on the SHA-pinned prebuilt.
It redirects the boot CPU handoff at VA `0xfffffff0074454a8` into verified unused
entry padding at `0xfffffff0074424ac`. The stub reuses the existing assembly
return sequence, with its ADRP relocated and reported checkpoint changed to 6.
It stops after CPACR_EL1, TPIDR_EL1 and MIDR_EL1 access, before entering arm_init.
The original kernel file is never modified. The packer checks all replaced bytes
and records both baseline and payload kernel hashes. This is a temporary probe,
not a fix for the checkpoint-10 hang. A hang before returning to UEFI bypasses the
90-second reboot delay; use the physical power button to recover.

## Intermediate checkpoint 7

Checkpoint 6 returned successfully on the phone. Checkpoint 7 redirects the first
instruction of arm_init (VA `0xfffffff00743952c`), before its stack prologue, into
a guarded stub in the same unused entry padding. The stub uses the bootstrap
x22/x23 VA/PA bases to branch to its identity-mapped physical alias before
executing the MMU-off assembly return sequence. This isolates the virtual
handoff from C stack accesses, stack-canary loads, helper calls and boot-argument
copying. It does not prove any of those later operations work.

Host validation checked the image header, payload entries, FD and live DTB
identity, image/kernel hashes, and disassembly of the hook and relocated stub.
Checkpoint 7 returned successfully on the phone.

## Intermediate checkpoint 8

Checkpoint 8 redirects the instruction at VA `0xfffffff007439568`, immediately
before the call to arm_set_kernel_tbi. Reaching it executes the arm_init stack
prologue, register saves, stack-canary load/store and local-variable stores.
The x22/x23 bootstrap VA/PA bases have not yet been overwritten, so the same
physical-alias return stub used by checkpoint 7 applies. The original BL encoding
is checked before patching. It does not execute arm_set_kernel_tbi or copy boot
arguments. Image structure, unchanged FD/DTB, payload hashes and hook/stub
disassembly passed host checks. Hardware testing is pending.

## Intermediate checkpoint 9

Checkpoint 8 returned successfully on the phone. Checkpoint 9 redirects the BL
at VA `0xfffffff0074396ec`, immediately before loader_checkpoint(10), after the
boot-argument copy and BootArgs assignment. arm_set_kernel_tbi is a bare RET in
this pinned kernel. The stub recovers the original bootstrap x22/x23 from the
arm_init save slots [sp+0x90] and [sp+0x88], then uses the verified physical-alias
assembly return. It bypasses the C checkpoint/loader_jump helpers, which allows
separating their behavior from the preceding initialization. The expected BL,
zero padding, original kernel hash and assembly return instructions are guarded.
Image contents, FD/live DTB identity, hashes and hook/stub disassembly passed
host validation. Hardware testing is pending.

## Checkpoint 10 revision 2: early PE query probe

Checkpoint 9 returned successfully on the phone, isolating the original hang to
the subsequent C checkpoint/return path. loader_jump calls PE_parse_boot_argn
for bootcpu before arm_init reaches PE_init_platform. PE_boot_args reads the
Platform Expert boot-argument pointer, rather than the BootArgs global just set
by arm_init. This is a suspected early-initialization dependency, not yet a
hardware-confirmed root cause.

`--checkpoint 10 --no-early-bootargs` replaces only the guarded BL at VA
`0xfffffff007456160` with `mov w0, #0`, treating the bootcpu query as absent.
This is appropriate only for this fixed current-core, single-CPU diagnostic;
it is not a general fix for the upstream alternate-core return mode. The rest
of the C return, log flush and translation-table switch remains in place.
The SHA-pinned original kernel stays untouched. Host checks verified exactly
four changed kernel bytes, disassembly, boot/payload structure, unchanged FD and
live DTB, and manifest hashes. Revision 2 awaits hardware testing.

## Checkpoint 11

Checkpoint 10 revision 2 returned successfully on the phone. This supports the
early PE bootcpu query as the cause of the original checkpoint-10 failure in this
configuration. Checkpoint 11 retains the identical four-byte diagnostic kernel
overlay and stops after cpu_data_init and PE_init_platform(FALSE, args), before
configure_timer_apple_regs. It does not initialize XNU storage or boot iOS.
The query bypass remains limited to the fixed current-core diagnostic.
Host checks verified the boot image, payload checkpoint, unchanged FD/live DTB,
manifest hashes and kernel identity with the successful 10-r2 image. Hardware
result pending.

## Checkpoint 31

Checkpoint 11 returned successfully on the phone. configure_timer_apple_regs is
a bare RET in the pinned prebuilt, so checkpoint 30 was skipped as a separate
hardware test. Checkpoint 31 runs wfe_timeout_configure and stops before
wfe_timeout_init. The inspected function parses a PE boot argument, computes a
WFE timeout field from timebase information, and stores the result; it does not
issue the later WFE timeout register setup. The payload kernel is identical to
the successful 11 and 10-r2 images, including the fixed current-core bootcpu-query
bypass. Boot image/payload checks, unchanged FD/live DTB, hashes and kernel
identity passed host validation. Hardware result pending.

## Checkpoint 32

Checkpoint 31 returned successfully on the phone. Checkpoint 32 stops after
wfe_timeout_init and before configure_misc_apple_boot_args. Disassembly shows
that this stage validates the computed timeout field and reads/modifies
CNTKCTL_EL1; unlike the earlier timer helper, it is not a no-op. No kernel
instructions beyond the previously tested bootcpu-query bypass are patched.
Host image/payload checks, unchanged FD/live DTB, hashes and kernel identity
with successful checkpoint 31 passed. Hardware result pending.

## Checkpoint 34

Checkpoint 32 returned successfully on the phone. Both configure_misc_apple
helpers are bare RETs in this prebuilt, so checkpoint 33 is not separately tested.
Checkpoint 34 additionally executes the early SecureDT lookup and optional
platform-stall property handling, followed by platform_stall_panic_or_spin.
It stops before page-size bookkeeping and CPU-topology parsing. The kernel is
identical to the successful checkpoint-32 payload, retaining the diagnostic
bootcpu-query bypass. Host checks verified payload structure, checkpoint, hashes,
unchanged FD/live DTB and kernel identity. Hardware result pending.

## Checkpoint 35

Checkpoint 34 returned successfully on the phone. Checkpoint 35 stops after the
early page-size bookkeeping and before ml_parse_cpu_topology. The inspected
instructions choose the page-shift field from the boot-argument memory size
and update bookkeeping globals. This does not establish that later VM setup
supports the selected page size. No additional kernel bytes are changed; the
payload retains the successful checkpoint-34 kernel and diagnostic return query
bypass. Boot/payload structure, hashes, unchanged FD/live DTB and kernel identity
passed host validation. Hardware result pending.

## Checkpoint 12

Checkpoint 35 returned successfully on the phone. Checkpoint 12 is next in
execution order, after ml_parse_cpu_topology and before obtaining the boot CPU
number. The loader continues to publish only the current CPU with cpus=1;
this probe does not start secondary cores. The payload retains the same kernel
as successful checkpoint 35, including the diagnostic early bootcpu-query bypass.
Boot/payload structure, hashes, unchanged FD/live DTB and kernel identity passed
host validation. Hardware result pending.

## Checkpoint 40

Checkpoint 12 returned successfully on the phone. Checkpoint 40 stops after
ml_get_boot_cpu_number and assignment to master_cpu, before its nonnegative
assertion. The inspected helper loads the boot CPU topology pointer and then its
CPU number. This probe preserves single-CPU publication and does not start other
cores. Its kernel is identical to the successful checkpoint-12 payload, including
the diagnostic bootcpu-query bypass. Host checks verified image/payload structure,
hashes, unchanged FD/live DTB and kernel identity. Hardware result pending.

## Checkpoint 41

Checkpoint 40 returned successfully on the phone. Checkpoint 41 additionally
checks the boot CPU number range, records the number and stack pointers in
BootCpuData, and fills its CpuDataEntries physical/virtual pointers. It stops
before thread_bootstrap. The inspected code performs topology checks and RAM
updates; secondary CPUs remain unpublished. The payload kernel is unchanged
from successful checkpoint 40, including the diagnostic bootcpu-query bypass.
Host image/payload checks, unchanged FD/live DTB, hashes and kernel identity
passed. Hardware result pending.

## Combined remaining-stage probe: checkpoint 21

Checkpoint 41 returned successfully on the phone. At the user's request the
next diagnostic targets the final arm_init checkpoint 21 rather than stopping
at every remaining stage. Execution order after 41 is 42, 43, 44, 13, 14, 15,
16, 17, 18, 19, 20, 21. Intermediate checkpoint calls return without stopping.
The test includes thread/CPU bootstrap, early clocks, VM initialization, serial
and console initialization, and stops before machine_startup. It does not claim
an iOS boot or any intermediate success until a device result is available.

The kernel is identical to successful checkpoint 41, retaining only the fixed
current-core diagnostic bootcpu-query bypass. No new kernel instructions are
patched for this combined probe. Host checks verified the final checkpoint,
boot/payload structure, unchanged FD/live DTB, hashes and kernel identity.
Hardware result pending. A hang may require power-button recovery and a return
to individual checkpoints to identify the failing stage.

## Checkpoint 50: beyond arm_init

Checkpoint 21 returned successfully on the phone. Thus the combined remaining
arm_init stages reached their final stop; this does not establish a userspace
boot or peripheral support. Checkpoint 50 is a new diagnostic after machine_conf,
before kernel_bootstrap. The inspected machine_conf code updates RAM topology
and CPU-count globals. A guarded overlay redirects machine_startup's tail branch
at VA 0xfffffff007446d14 to unused entry padding containing mov w0,#50 followed
by a tail branch to loader_checkpoint. The existing current-core query bypass
is retained. All replaced bytes and the original kernel SHA are checked.
Host verification covered image/payload structure, unchanged FD/live DTB, hashes
and hook/stub disassembly. Hardware result pending. This extends the diagnostic
set beyond the original arm_init checkpoints.

## Checkpoint 51: vm_mem_bootstrap

Checkpoint 50 returned successfully on the phone. New checkpoint 51 enters
kernel_bootstrap, executes its initial print, scale_setup and early debug marker,
and stops immediately after vm_mem_bootstrap. A guarded hook at VA
0xfffffff00730fb28 branches to the same unused entry-padding stub mechanism as
50, reporting 51 through loader_checkpoint. The successful early bootcpu-query
bypass remains applied. This image does not include the checkpoint-50 hook,
which would otherwise stop progress before kernel_bootstrap. It stops before
ATM and later startup initialization. Original kernel SHA, hook opcode and zero
padding are checked. Image/payload structure, hashes, unchanged FD/live DTB and
hook/stub disassembly passed host validation. Hardware result pending.

## Combined kernel_bootstrap probe: checkpoint 52

After checkpoint 50 passed, the user requested a combined run through the next
bootstrap block rather than individual stops. Checkpoint 51 has not been reported
successful. Checkpoint 52 replaces the load_context call at VA
0xfffffff0073101cc with a branch to the guarded reporting stub, stopping after
kernel_bootstrap initialization and creation of kernel_bootstrap_thread, before
switching into that thread. It includes vm_mem_bootstrap and the intervening
startup routines without checkpoint-50/51 stops. It does not run the new thread
or claim an iOS boot. The existing diagnostic bootcpu-query bypass is retained.
Host checks verified the original hook, zero padding, image/payload structure,
unchanged FD/live DTB, hashes and hook/stub disassembly. Hardware result pending.
A failed combined run will require narrower checkpoints to locate the fault.

## Checkpoint 53: bootstrap thread before IOKit

Checkpoint 52 returned successfully on the phone. New checkpoint 53 allows
load_context to switch into kernel_bootstrap_thread and runs its initial block,
including idle-thread creation, scheduler startup, thread-daemon setup, early
services and randomization. The hook at VA 0xfffffff007a9f68c replaces the call
to PE_init_iokit with a branch to the guarded reporting stub. It stops before
that call and before the later explicit interrupt enable and BSD startup.
No checkpoint-50/51/52 hooks are included. The diagnostic early bootcpu-query
bypass remains. Original kernel SHA, hook opcode and unused padding are guarded.
Image/payload structure, unchanged FD/live DTB, hashes and hook/stub disassembly
passed host checks. Hardware result pending; a failure may require narrowing
this combined block and checking whether return from the new context works.

## Combined bootstrap-thread probe: checkpoint 54

The user requested another combined run after reporting checkpoint 52 successful;
checkpoint 53 has no reported device result. New checkpoint 54 redirects the
bsd_init call at VA 0xfffffff007a9f844 to the guarded reporting stub. It runs the
intervening bootstrap-thread path, including PE_init_iokit, explicit interrupt
enable, startup callbacks and machine/IOKit lockdown, without intermediate stops.
It does not execute bsd_init. This is a substantially broader probe, not evidence
that the intervening hardware drivers work. The early bootcpu-query diagnostic
bypass remains; no earlier startup checkpoint hooks are included. Image/payload
structure, unchanged FD/live DTB, hashes and hook/stub disassembly passed host
checks. Hardware result pending. Later protection/interrupt changes can also
affect the diagnostic return path, so a hang alone cannot locate the fault.

## Combined early BSD probe: checkpoint 56

Checkpoint 54 returned successfully on the phone. The user observed background
logs ending with GenericARMCPU registering CPU 0; the reported STOPPED result
establishes that the hook before bsd_init was reached, not a hang at that log.
A checkpoint-55 image was built and checked at an early BSD boundary but not
issued for testing, because the user requested a combined run.

Checkpoint 56 redirects IOFindBSDRoot at VA 0xfffffff00777f6a0. It executes the
early bsd_init block, including process-0 setup, VFS subsystem registration,
network initialization and IOKitBSDInit, then stops before root-device discovery
and vfs_mountroot. It does not execute those calls or launch userland. Intermediate
checkpoint-50 through 55 hooks are absent. The guarded startup reporting stub
and current-core early bootcpu-query bypass remain. Original SHA, hook opcode,
unused padding, boot/payload structure, unchanged FD/live DTB, hashes and
hook/stub disassembly passed host checks. Hardware result pending.

## RAM-only raw-syscall userland probe (mode 57)

Checkpoint 56 returned successfully on the phone. The observed lo0 legacy-TX
message is background network output; the STOPPED result shows the hook was
reached. This configuration supplies neither a RAM root nor a resident image,
and does not enable upstream's UFS block service. That service uses separate
firmware-serving and kernel cores, unlike this current-core diagnostic.

Mode 57 supplies upstream prebuilt/userland/hello as a mockfs RAM root, exposing
it as /sbin/launchd. SHA256 is pinned to
48a910940230506813aa0b75692e58d1c6698feccf7c6f7771c8790b6c97522d.
Source: userland/src/hello.c. This static raw-syscall program opens /dev/console,
prints a greeting, PID and UID, then intentionally exits PID 1. The expected
success signal is userland output followed by an init-exit panic and diagnostic
return, not STOPPED AT CHECKPOINT 57. An earlier panic/timer return is not success.

Only mode 57 permits a nonempty RAM disk up to 64 KiB in the fixed loader guard;
other modes still reject it. The packer accepts only the SHA-pinned executable.
No synthetic checkpoint hook is included; the payload kernel matches the
successful 21/10-r2 kernel, with only the early bootcpu-query bypass. Upstream
prepends rd=md0 to the fixed cpus=1 command line when supplying the RAM root.
No UFS flags, resident image, physical root, partition logger or writes are enabled.
The Android iosroot partition is not supplied to XNU. No iOS libraries are included.

Host checks verified exact root bytes/hash, LC_UNIXTHREAD, absence of dynamic
linker/library commands, boot/payload structure, unchanged FD/live DTB and hashes.
A local lld compilation attempt was discarded because -static was unsupported;
the reviewed upstream static prebuilt is used. Hardware result pending.

## RAM userland result

The user reports visible hello user-space output in the background, followed by
a panic, stackshot succeeded, and the MUNCH probe-finished return screen. This
matches the raw-syscall hello program's intentional PID-1 exit and demonstrates
execution of the supplied user-space program plus return from the panic path.
No photograph was available; the exact greeting, PID/UID values and panic text
were not transcribed, so those details are not independently verified. This is
an open Darwin/XNU RAM-userland result, not an iOS/launchd/SpringBoard boot. No
physical root partition was supplied to this image.

## First iOS 16.5 package: mode 58

Source members were fetched directly from Apple's CDN, checked against ZIP member
sizes/CRC32 and recorded with SHA256; BuildManifest identifies 16.5 / 20F66 for
iPhone10,3 and iPhone10,6. This is archive integrity verification, not a claim of
an independent Apple signature trust-chain validation or F4 runtime compatibility.
PREBUILT=1 REST=1 userland/build.sh produced a 2476732416-byte system.apfs and
4194304-byte root.apfs. Both passed apfsck -cw; the kernel APFS reader read back
all 62261 system entries and 458 helper entries with matching contents/metadata.

Mode 58 packages the helper with the SHA-pinned XNU kernel and local IPSW identity
using storage FD revision 3. ios-root.inc matches exactly one GPT iosroot partition,
checks geometry/RAM capacity, reads the exact host-built system size into the
claimed resident RAM area, and verifies NXSB and compiled CRC32 before handing
off to XNU. It never calls WriteBlocks/FlushBlocks. The root and helper become
md0 and md1 via upstream's resident-image path. The fixed command line is
-v cpus=1 loader_return_after=600. Other peripheral flags stay disabled.

The separate Android/recovery installer is generated from install-iosroot.sh.in
with pinned image size/SHA. It requires root, munch identity, iosroot -> sda35,
an unmounted/unheld target and sufficient capacity; only --apply writes, and a
full image-length SHA256 readback follows. Replacement of the old dedicated
partition was authorized. No bootloader flash command is used.

make-install-package.py creates the portable ZIP from local files only. Its ZIP
CRC check passed. The RAM boot image is 8284160 bytes, under the reported 120-MB
fastboot boot constraint. Hardware installation/boot are pending; no working iOS
interface, touch or GPU support is claimed. Reboot restores ordinary Android.

### Experimental FT3658U input

After the powered UEFI contact probe passes, add `--touch` to the verified
checkpoint-58 iOS build command. This compiles the bounded ABI-pinned overlay
from `../touch/xnu-ft3658u.c`; see `../TOUCH-BRINGUP.md` for its temporary nature
and power handoff. It requires the same iOS config and storage-enabled FD as
the working iOS RAM boot. Do not enable the upstream Redwood touch/button
boot arguments. No APFS reinstall is needed.

Before publishing the image, run:

```sh
python3 devices/poco-f4/touch/check-xnu-events.py
python3 devices/poco-f4/touch/check-power-guards.py
python3 devices/poco-f4/touch/verify-image.py /path/to/munch-ios165-touch-r1.img \
  --fd /path/to/munch-debug-storage-r3.fd --dtb /path/to/live.dtb
```

The independent image check verifies the Android boot header, size, exact FD
and DTB, payload hashes, helper APFS, and that no other kernel bytes changed
outside the bounded touch replacement and existing four-byte early-query fix.
The r1 image retains the existing 600-second kernel return timer. iOS input,
coordinate orientation and sustained reliability require device validation.

### Eight-core iOS experiment

Add `--cpus 8` to the iOS touch build to restore upstream CPU enumeration and
request `cpus=8 small_cores_after=0`. The current boot CPU remains the boot CPU;
other MPIDRs are 0x000..0x700, in 0x100 steps as captured on munch. Existing
PSCI AFFINITY_INFO filtering excludes processors firmware reports as in use;
CPU_ON, GICv3 redistributor selection and secondary shutdown use the kernel's
existing SMP implementation. The single-CPU checkpoint profiles remain the
default. The new profile also omits `-v` to avoid verbose framebuffer scrolling.

`munch-ios165-touch-8cpu-r1.img` requests up to eight processors. The actual
number registered/running is a device-test result, not a host-build guarantee.
The existing touch overlay, APFS installation and 600-second return timer are
retained. GPU acceleration and frequency control are not added by this change.
