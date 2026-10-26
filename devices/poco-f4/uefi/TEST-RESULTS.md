# F4 UEFI device tests

## Diagnostic image 1, 2026-10-26

- Image: `munch-uefi-diagnostic.img`.
- SHA256: `d464ef14c87932ec4741b339694c9d0fda99783f217fd7e9b3498ab7d042078b`.
- UEFI build: RELEASE, original F4 vendor modules.
- User report: black screen followed by a return to Android after approximately
  5–10 seconds. No visible diagnostic output was reported.
- Full fastboot output and reset reason are not yet available.

This does not establish that the boot shim or UEFI entry point was reached.
The diagnostic application's programmed 90-second reset does not explain the
reported timing. Boot-image acceptance, shim execution, early UEFI failure,
firmware initialization, and display state remain possible investigation areas.
No specific cause has been identified.

## Next diagnostic

Prepared `munch-uefi-debug.img` (1824768 bytes), SHA256
`6023055f719e66dd963bbbcee6ad406697f614688a4e6d6139fe0e1ef7c4181a`.
The DEBUG FD built successfully. Its boot header, compressed payload, exact FD,
live DTB, build-target metadata, and image hash were checked on the host.
The user subsequently supplied a photograph of DEBUG output. It shows memory
HOB processing and multiple DXE driver load messages, ending after the
`FeatureEnablerDxe.efi` load line with `EFI_ERROR (Status = Device Error)`.
The full assertion file/line is not legible. The user reports the dark upper
part of the image occurred as the phone began rebooting.

This establishes boot-shim execution, UEFI SEC/DXE progress, and visible
framebuffer debug output. It does not establish BDS, GOP protocol installation,
USB protocol availability, or execution of the diagnostic EFI application.
The last driver load line is evidence for investigation, not proof of causality.

## Diagnostic revision 2

Exclude `FeatureEnablerDxe` from both the FDF DXE list and APRIORI order.
All remaining vendor executable bytes and DEBUG assertions are unchanged.

Supporting evidence:

- Pinned Mu-Silicium Device-Binaries `alioth/README.md` documents a patch for
  FeatureEnablerDxe because re-enabling existing display clocks can lose the
  framebuffer. Its solution is not applied to the different F4 binary.
- The F4 binary contains error strings for FeatureEnabler TA loading, missing
  QSEE protocol, display-clock lookup, and feature execution failures.
- The diagnostic path uses inherited display state and SimpleFbDxe; those source
  components do not directly reference the FeatureEnabler protocol.

Omitting the driver is an isolation test. Whether other firmware components
require its protocol or the original failure has another cause remains unverified.

Prepared `munch-uefi-debug-r2.img`, SHA256
`2296cc1a1386fb2e2276acdc56b17c20f4b5256d15993b9f546688b942161651`.
The host build succeeded. Parsed both DEBUG firmware volumes and confirmed
the only removed firmware file GUID is FeatureEnablerDxe
(`1c04f529-0612-4319-9315-b2e75c16810a`), with no added GUIDs. Verified the exact
FD/DTB inside the packaged image and its checksum.

The user supplied a photograph showing successful `LoadImage` and execution of
`MUNCH UEFI DIAGNOSTIC 1`. Visible results:

- GOP status: success; width `0x438` (1080), height `0x960` (2400).
- Stride: `0x438` pixels; pixel format: 1 (BGR reserved 8-bit per channel).
- Framebuffer base: `0x9C000000`.
- Qualcomm USB protocol lookup: success.
- Memory map query: success; zero conventional-memory overlaps with the checked
  DDR-hole and XNUP reservation intervals.
- Large conventional-memory ranges include `[0xC0000000, 0xF0000000)` and
  `[0xF4000000, 0x200000000)`.
- The application reached its programmed 90-second reboot-wait message. Actual
  reset timing and return have not been separately reported for this run.

The photo also contains a partly obscured `ConvertPages` memory-type message in
the background. Its full text and significance are not established. The probe's
zero-violation result checks only the two stated intervals, not the entire map.

This confirms the temporary boot format, UEFI BDS, inherited framebuffer output,
GOP queries, and EFI application handoff on this phone. Protocol lookup alone
does not establish USB enumeration or transfers. Excluding FeatureEnablerDxe
allowed progress to the probe; the internal cause of its earlier error remains
unverified.

## USB probe

Package the same revision-2 DEBUG FD with `--usb-probe`. It starts the Qualcomm
USB device protocol with a fastboot interface, then accepts only product/version/
probe queries, `oem ping`, and `reboot`. Unknown commands receive FAIL. No storage,
download, or kernel-execution command is implemented. The session lasts at most
300 seconds if firmware event calls return, then attempts a cold reset.

The command whitelist was tested on the host, including rejection of flash,
erase, download, kernel-run, and prefix-match attempts. Runtime USB behavior
requires the next device test.

Prepared `munch-uefi-usb-probe.img`, 1824768 bytes, SHA256
`5f1809d2e01f93e35592b0be7b1b6969e9a31819af39983dd9dc5b1a5f579ffa`.
Verified that the FD, boot shim, and live DTB are byte-identical to revision 2;
only the XNUP application differs. ARM64 PE format, XNUP entry, metadata, and
image checksum were checked.

The user reports Windows cannot recognize the device and fastboot waits without
a reply. A phone photograph shows successful USB PHY initialization followed by
repeated Full-speed and detach/attach messages. A read-only Windows PnP query
finds VID `18D1`, PID `D00D`, problem code 43, and no assigned device service.
The user supplied the specific Windows error: invalid USB BOS descriptor.

## USB probe revision 2: BOS correction

The first USB probe introduced a transcription error in its BOS initializer:
only 41 bytes were explicitly present in a 42-byte array. The SSP capability
lost its attributes value `0x01` and one zero byte relative to the UMI source,
shifting subsequent fields; the C compiler silently padded the final byte.
The resulting reserved fields and sublink attribute count were invalid.

Restore the exact 42-byte UMI BOS, infer its C array length, and assert its size
at compile time. Add pre-packaging checks for descriptor bounds, total lengths,
capability counts, SSP reserved fields, and sublink attribute count. This change
does not modify the UEFI FD, USB driver binaries, endpoint configuration, or
event loop. The invalid BOS is an established defect; whether fixing it is
sufficient for enumeration and transfers requires another device run.

Prepared `munch-uefi-usb-probe-r2.img`, SHA256
`87c877dee55ece21c3ee83c6f030fd90f56dcfa9482ebba532f03559ba6e7f05`.
Validated the corrected BOS in the actual compiled EFI payload and unchanged
FD/shim/DTB against USB probe 1. The checker rejects both the original short
initializer and its explicitly padded malformed SSP form.

The user reports successful Windows fastboot command round trips:

```text
getvar product -> product: munch-usb-probe (0.001 s)
oem ping       -> OKAY (0.001 s)
```

This confirms enumeration, usable host driver binding, command reception, and
reply transmission for this probe. The Windows CLI did not display a `pong`
payload; only its reported OKAY result is recorded. Large uploads, continuous
sessions, XNU coexistence, and reconnect behavior are not yet tested.

Windows BOS validation reference:
https://techcommunity.microsoft.com/blog/microsoftusbblog/usb-2-1-2-0-1-1-device-enumeration-changes-in-windows-8/270775

Build the same F4 overlay with `BUILD_TARGET=DEBUG`. Mu-Silicium's DEBUG target
enables framebuffer serial output and assertion messages; RELEASE uses null
debug libraries. Preserve the original image for comparison and use a distinct
output name for the DEBUG probe. This is diagnostic instrumentation, not a fix.

Collect Android reset reason properties and the pstore directory listing after
the failed run. Such records may be absent or may describe another boot stage;
interpret their contents before drawing conclusions.

## Checkpoint 10 hang and intermediate checkpoint 6

The user reports checkpoint 10 remains at the loader handoff and does not
reboot. Checkpoint 10 has not passed. Checkpoints 1–5 previously returned.
A diagnostic checkpoint 6 now stops immediately before the boot CPU enters
arm_init; it is pending hardware testing. The image payload, unchanged FD and
live DTB, kernel patch guards, hashes and relocated instructions were checked
on the host. No partition writes are involved.

The user subsequently confirmed checkpoint 6 returned successfully. Checkpoint 7
has been prepared to stop at arm_init entry before its stack prologue, with a
physical-alias transition before the MMU-off return. Host checks passed; hardware
result pending.

Checkpoint 7 was confirmed successful by the user. Checkpoint 8 now probes the
arm_init prologue and initial memory accesses, stopping before arm_set_kernel_tbi.
Host image and disassembly checks passed; hardware result pending.

Checkpoint 8 was confirmed successful by the user. Checkpoint 9 now stops after
copying boot arguments, before the C checkpoint-10 call. Host validation passed;
hardware result pending. It uses the assembly return rather than loader_jump.

Checkpoint 9 was confirmed successful by the user. Checkpoint 10 revision 2
bypasses only the early PE bootcpu query in loader_jump; suspected initialization
dependency, not a confirmed root cause. Four-byte patch and image checks passed.
Hardware result pending.

Checkpoint 10 revision 2 was confirmed successful by the user. Checkpoint 11
retains the same kernel overlay and targets completion of early Platform Expert
initialization. Host validation passed; hardware result pending.

Checkpoint 11 was confirmed successful by the user. Prepared checkpoint 31 after
wfe_timeout_configure; checkpoint 30 adds only a bare-RET timer helper and is not
separately tested. Kernel identity with successful 11 and image checks passed.
Hardware result pending.

Checkpoint 31 was confirmed successful by the user. Prepared checkpoint 32 after
wfe_timeout_init, including its CNTKCTL_EL1 update. Image checks and kernel
identity with successful 31 passed; hardware result pending.

Checkpoint 32 was confirmed successful by the user. Prepared checkpoint 34 after
early SecureDT/platform-stall handling. The two helpers before checkpoint 33 are
bare RETs and are not separately tested. Host validation passed; hardware pending.

Checkpoint 34 was confirmed successful by the user. Prepared checkpoint 35 after
early page-size bookkeeping, before CPU-topology parsing. Image and kernel
identity checks passed; hardware result pending.

Checkpoint 35 was confirmed successful by the user. Prepared checkpoint 12 after
CPU-topology parsing, retaining single-CPU publication. Host image and kernel
identity checks passed; hardware result pending.

Checkpoint 12 was confirmed successful by the user. Prepared checkpoint 40 after
boot CPU number retrieval and assignment. Host validation passed; hardware pending.

Checkpoint 40 was confirmed successful by the user. Prepared checkpoint 41 after
BootCpuData and CpuDataEntries setup, before thread_bootstrap. Host validation
passed; hardware result pending.

Checkpoint 41 was confirmed successful by the user. The user requested trying
all remaining stages together. Prepared checkpoint 21, the final arm_init stop
before machine_startup; intermediate stops are skipped. Host checks passed;
hardware and all intervening stages remain unconfirmed.

Checkpoint 21 was confirmed successful by the user, covering the remaining
arm_init path. Prepared new checkpoint 50 after machine_conf, before entering
kernel_bootstrap. Guarded overlay and host image/disassembly checks passed;
hardware result pending.

Checkpoint 50 was confirmed successful by the user. Prepared checkpoint 51 after
vm_mem_bootstrap in kernel_bootstrap. Guarded hook and host image/disassembly
checks passed; hardware result pending.

The user requested a combined run beyond 50 rather than reporting a 51 result.
Prepared checkpoint 52 immediately before load_context at the end of
kernel_bootstrap. It includes intervening stages; none are yet confirmed.
Host image/disassembly validation passed; hardware pending.

Checkpoint 52 was confirmed successful by the user. Prepared checkpoint 53 in
kernel_bootstrap_thread immediately before PE_init_iokit. Host image and
disassembly validation passed; context switch and intervening stages remain
unconfirmed until the device result.

The user requested a combined run beyond 52; no checkpoint-53 result was reported.
Prepared checkpoint 54 immediately before bsd_init, including earlier IOKit and
lockdown stages. Host image/disassembly checks passed; hardware result pending.

Checkpoint 54 was confirmed successful by the user, with CPU-0 registration
visible in the background console. Prepared checkpoint 56 before IOFindBSDRoot,
combining early BSD stages at the user's request. Checkpoint 55 was host-built
but not device-tested. Host validation passed; 56 hardware result pending.

Checkpoint 56 passed per user; last background message mentioned lo0 legacy TX.
Prepared mode 57: pinned upstream static hello as a mockfs RAM root, bypassing
checkpoint stops. No physical root is supplied. Expected success: hello/PID 1
output, then intentional init-exit panic. Host image and static-Mach-O checks
passed; hardware pending.

## RAM userland result

The user reports visible hello user-space output in the background, followed by
a panic, stackshot succeeded, and the MUNCH probe-finished return screen. This
matches the raw-syscall hello program's intentional PID-1 exit and demonstrates
execution of the supplied user-space program plus return from the panic path.
No photograph was available; the exact greeting, PID/UID values and panic text
were not transcribed, so those details are not independently verified. This is
an open Darwin/XNU RAM-userland result, not an iOS/launchd/SpringBoard boot. No
physical root partition was supplied to this image.

## Root-storage preparation

The user reported RAM hello user-space output followed by the intentional panic
and MUNCH probe-finished screen. They authorized downloading the intended iOS
system and are willing to replace the old iosroot installation. iOS 16.5 20F66
source-image download was started using upstream sysimage/fetch.sh against the
Apple CDN. The separately downloaded BuildManifest confirms version/build.
Full downloads, archive checks and APFS image construction are still pending.

The user can read their 16-GB NTFS USB drive in Android and reports roughly
122 MB / 130 seconds for fastboot boot, limiting practical boot images to 120 MB.
Prefer a small RAM boot loader and images transferred separately via the USB
drive, with the dedicated iosroot partition as staging for read-only UEFI loads.
No partition write has been performed or installation script issued yet.

Built an EFI-only --root-probe that matches exactly one GPT iosroot partition,
checks block geometry/alignment and reads only its first 64 KiB. It reports status
and NXSB presence without printing contents or UUIDs. No write/flush method is
called. Build and host image/manifest/FD/DTB checks passed. Hardware pending.

## Storage probe revision 2 / FD revision 3

The user reports only the partition enumeration status, possibly 0xE, followed
by the normal 90-second pause. No read-PASS message was observed. This does not
establish a partition read failure: the original diagnostic FD omitted UFSDxe,
DiskIoDxe and PartitionDxe entirely. MUNCH_STORAGE=1 now includes those three
unmodified modules from the pinned F4 XBL, with metadata diagnostic_revision=3.
FeatureEnablerDxe stays omitted. Default builds retain the earlier storage-free
configuration. The successful revision-2 FD was saved and restored at its usual
path; the storage FD is kept separately in local artifacts.

The revised read-only probe reports Block I/O enumeration and handle count,
connects those controllers recursively to bind partition drivers, then performs
the same exact-name, geometry-guarded 64-KiB iosroot read. Build/FV manifest
checks confirm all three modules are present. Host checks verified the EFI-only
payload, image/EFI hashes, storage FD and live DTB. Hardware result pending.

The iOS download stalled after 3.94 GB. Replaced the active fetch with a local
wrapper that uses 4-MiB stored-member ranges, 20-second socket timeouts, strict
HTTP 206/Content-Range checks, resumable partial files and progress reporting.
The existing partial file was retained. Metadata archive CRC/size verification
passed and the local identity header was generated; full DMG checks remain
pending while downloads continue.

## Storage and first iOS package status

The user reports IOSROOT READ: PASS (64 KiB) with storage FD revision 3, despite
some ConnectController statuses possibly ending in 0xE. This confirms successful
partition discovery/read, not success of every controller connection.

All five downloaded source members passed ZIP CRC/size checks; SHA256 values
are in local artifacts. Both APFS images passed clean structural checks and full
content/metadata comparison through the kernel's host APFS reader: 62261 system
entries and 458 helper entries. The installer ZIP passed its CRC check. The
8284160-byte mode-58 boot image passed host header/payload/FD/live-DTB/hash checks;
its only kernel-byte change is the tested early bootcpu-query bypass. The RAM
loader CRC routine passed standard and empty test vectors. Installation and
full iOS runtime remain untested. No device partition write was performed here.

## First reported iOS interface and touch investigation

The user supplied successful installer --check output for a 2476732416-byte
system image and a 17179869184-byte /dev/block/sda35. They subsequently report an
Apple logo, an iOS interface, repeated CPU-monitor entitlement messages, and no
touch response. An --apply/readback transcript and exact runtime logs were not
provided. Do not treat the entitlement text alone as proof of a fatal failure.

Prepared the read-only Android touch collector and inspected Xiaomi's primary
munch driver references. The existing upstream FocalTech transport is hardcoded
for redwood; munch additionally needs different TLMM tiles, SPI SE4, supplies,
reset/IRQ and confirmed IC/firmware. No touch-enable image has been issued yet.

## FT3658U runtime-read probe

Android touch archive 20261026-123312 confirms ID 5652, firmware text 28, SPI
mode 0/6 MHz, reset/IRQ TLMM33/39 and 10800x24000 Android coordinates. No external
firmware file was found. Prepared an EFI-only GPIO SPI runtime-read probe, without
reset, regulator, boot-ROM or firmware writes. It verifies runtime ID/CRC and
shows green contact marks, then restores GPIO8–11. No XNU or iOS is included.
Protocol/coordinate tests passed under address/undefined sanitizers; host image,
FD/live DTB and EFI payload checks passed. Hardware result pending.

The first touch runtime-read probe did not obtain a verified ID; the user reports
Failed reads = 1. Prepared revision 2 with both data-line assignments and compact
RESET/input-latch and raw-reply summary, retaining no-reset/no-power/no-firmware
operation and pin restoration. Host image checks passed; hardware pending.

Photo of touch probe r2 confirms RESET GPIO33 cfg=1/IO=0, IRQ GPIO39 IO=0,
SPI configs all 1, A/B replies all zero, 0 CRC-valid and 4 failed register reads.
Prepared opt-in touch reset-release r3 (GPIO33 HIGH, Android pull-up/8-mA config,
200-ms settling, original state restored at exit); no regulator or firmware
commands. Host image verification passed; hardware result pending.

Photo of touch reset-release r3 confirms GPIO33 cfg=0x2c3 and input/latch HIGH,
but SPI replies remain zero with 0 valid/4 failed register reads. Prepared r4
power-state inspection via bounded SPMI observer reads of candidate SIDs 4/5
L1/L10 peripherals. No PMIC write opcode, reset, GPIO or firmware writes exist.
Host build/image checks passed; hardware pending.

User-reported r4 power results: SID4 L1/L10 no channels; SID5 L1 type04/sub71,
mode02/en00/status03/Vraw0708; L10 type04/sub approximately70, mode07/en00/status03/
Vraw0cf0. Prepared opt-in r5 with guarded, temporary ldoc1/ldoc10 ENABLE votes,
readiness checks, then reset-release/SPI contact test and reverse-order cleanup.
Voltage and mode selectors are never written. Actual C guard/slot/parser tests
passed with mocked host MMIO; build/image checks passed. Hardware pending.
