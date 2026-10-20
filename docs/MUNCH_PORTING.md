# POCO F4 (`munch`) Porting Notes

This is a hardware bring-up guide for the POCO F4 (`munch`, SM8250). `sweet`
is the Redmi Note 10 Pro donor platform (SM7150), not this target device. The
current shim, XNU machine configuration, and ADT generator still describe the
donor; the Munch name does not make those values portable.

## Current safety boundary

The current device-test phase is RAM-only. Any test image must use a payload RAM
disk with `rd=md0`, a temporary `fastboot boot` image, and an ADT without an UFS
node. Do not read, back up, mount, or write `userdata`; the existing
non-userdata backups remain outside the repository. UFS is a possible later
boot mode, but it is not part of the current tests and needs its own reviewed
storage design before use.

The active shim source list excludes `shim/ufs.c`, and neither the shim entry
point nor the XNU loader calls its `test_ufs()` probe. That legacy probe is now
compile-time disabled unless `IOS_MUNCH_ALLOW_LEGACY_UFS_PROBE` is explicitly
defined. Static source inspection shows the probe programs UFSHCI registers,
submits SCSI read commands for LBA 0 and the GPT, then reads filesystem
superblocks from `userdata`, `cust`, and `cache` entries. It contains no SCSI
write command, but its controller takeover and storage reads violate the
current RAM-only boundary; do not enable it for Munch bring-up.

No local iOS root image is available in `research/`. The inherited root builder
defaults to a 16-GB raw HFS+ image, and `mkpayload.py` embeds that image
uncompressed as a resident RAM disk. That exceeds the 4-GB source-derived
candidate memory window. A RAM-sized root or demand-paged design is unresolved
and must be measured before proposing a phone boot.

## Live device evidence (2026-10-20)

Read-only ADB inspection identifies `ro.product.device=munch`, model
`22021211RG`, active slot `_b`, and `MemTotal=7815544 kB`. The reported Android
version (`16`, SDK `36`) conflicts with the version component (`13`) in the
fingerprint `POCO/munch_global/munch:13/RKQ1.211001.001/V816.0.9.0.ULMMIXM:user/release-keys`;
resolve this before treating the capture as a stable firmware baseline.

`/sys/firmware/fdt` is readable through root in the current Android boot. A
read-only capture on 2026-10-20 is FDT v17, 812778 bytes, with root model
`Qualcomm Technologies, Inc. xiaomi munch` and compatibles
`qcom,kona-mtp; qcom,kona; qcom,mtp`. The capture was kept in `/tmp` only; it
contains device-specific data in `chosen/bootargs` and must not be published or
added to Git. `tools/munch-fdt-pull.sh` reproduces this capture, refuses to
write inside the repository, and only reads `/sys/firmware/fdt`.

The reconstructed tree reports 8 PSCI CPUs, GICv3 at `0x17a00000` and
`0x17a60000`, UFS at `0x1d84000`, SDE at `0xae00000`, Focaltech touch on SPI at
`0x990000`, and RAM ranges `0x80000000+0x3bb00000` and
`0xc0000000+0x1c0000000` (ending at `0x280000000`). It also contains dynamic
reserved pools whose actual placement is not encoded as fixed addresses.
These are live Linux-tree observations, not proof that XNU can use the ranges.

The public device-specific EDK2 repository inspected for Munch is
`0xSelenicDove/edk2-msm-munch`; no other independent Munch-specific UEFI
repository was found in the public repository search. It has two Munch image
releases, both published on 2026-08-20: `v1.1.0` at source commit `1b11fb1`
and a later build at `d4b3760`. In the later source map, PSTORE is at
`0xb0000000+0x400000`, display ramdump at `0xb0400000+0x1000000`, and the
last conventional RAM descriptor is `0xffc20000+0x1003e0000`, ending at
`0x200000000` (8 GiB). Thus SWEET's current `PHYS_BASE=0xb0000000` overlaps
reserved regions. The live Linux tree reports RAM through `0x280000000` (10
GiB address), so the EDK2 source map omits the top 2 GiB of that address span.
The bundled Munch compatibility DTB carries the same two `/memory` ranges as
the live Android FDT. The published `boot-munch.img` is an Android boot-image
v1 containing a 3,946,361-byte UEFI payload; it is a UEFI artifact, not a
validated XNU handoff image. These are source and static-image observations,
not proof that this UEFI build is installed or that its runtime descriptors
match every firmware variant of this phone.

The working fork is now `danya2271/edk2-msm-munch`. Commit `cb48979` adds a
Munch-only `MUNCH_RAM_ONLY=TRUE` build profile: it removes UFS, SCSI,
partition/filesystem, USB mass-storage, Linux mass-storage and Shell modules
from the UEFI firmware file lists. Commit `3bb06c2` closes the remaining
`FvSimpleFileSystemDxe` FDF reference and makes the memory-map tracer a valid
DXE module. A local RELEASE build from `3bb06c2` succeeded with GCC 14 host
tools and produced `/tmp/edk2-munch-build/boot-munch.img` (1,536,000 bytes,
SHA-256
`dcddcc2f3898257d8c50ceb8e12df0a9a29374c661f0a7d256b6fa3f718d7da5`). The
artifact is only statically validated; it has not been sent to the phone.

The follow-up identity fix is pushed as `a8b755c`: the embedded Munch DTB now
matches the live Android FDT model and board ID (`0x32`). The resulting local
artifact `/tmp/edk2-munch-identity-build/boot-munch.img` has SHA-256
`b7153bc6de91227d25ac051452288a8763d093c753fd20ad5f501f3a5c2f417f`. It was
tested twice with temporary `fastboot boot` only; both runs returned to Android
without modifying `userdata`, `dtbo`, or any persistent partition. The phone
reported `munch`, `androidboot.dtbo_idx=0`, and `androidboot.dtb_idx=0` after
the tests. The framebuffer text visible in the supplied photographs is an
early UEFI banner/debug line; the photographs are too blurred to transcribe
reliably and do not establish an XNU panic.

**Candidate, not validated:** `[0x100000000, 0x200000000)` (4-8 GiB) lies
inside the later EDK2 source's conventional RAM descriptor and below its
reported end. It also lies in the upper range advertised by the reconstructed
Android Munch FDT; the saved-tree shim test accepts it and rejects listed
fixed/dynamic reservations. These are offline observations only. The public
UEFI source map is not relevant to direct `fastboot boot` through Android ABL,
and the reconstructed Linux FDT is not proof of what ABL supplies at runtime.
The shim now checks the selected window against that actual handoff FDT before
copying XNU. This fail-closed check is the runtime gate for the direct-ABL path;
it does not validate an EDK2 UEFI boot path. The candidate ADT may be generated
for offline inspection, but no phone payload or boot is authorized by this
offline check. The compatibility DTB bundled in the EDK2 source identifies as
`alioth`, so it is not a substitute for the live Munch tree.

The shim now performs a fail-closed preflight against the FDT supplied by the
Android bootloader. Before copying any XNU segment, it requires the payload's
`physBase..physBase+memSize` range to be contained in a `/memory` node, outside
the FDT reservation map and fixed `/reserved-memory` regions, and outside every
declared dynamic reservation allocation range. A `linux,usable-memory-range`
restriction is honored when present. This protects the current Android-ABL
RAM path from an unsafe ADT choice; it does not prove that an EDK2 UEFI memory
map, cache attributes, or XNU reservations will match.

The host-only check can be run without a phone or storage access:

```sh
gcc -O1 -Wall -Wextra -Werror -Ishim \
  -o /tmp/ios-munch-host-test \
  shim/console.c shim/fdt.c shim/fdt_memory.c shim/host_test.c
/tmp/ios-munch-host-test --fdt-range research/munch-live.dtb \
  0x100000000 0x100000000 1
```

On the saved Munch Linux FDT this accepts the source-derived 4-8 GiB
candidate, rejects the framebuffer at `0x9c000000`, rejects the low bank, and
rejects an address beyond the advertised RAM. The candidate remains a
research result only; no Munch ADT or phone payload is generated from it.

ADB reports `boot_b` as 192 MiB, `vendor_boot_b` as 96 MiB, `dtbo_b` as 32 MiB,
and `userdata` as about 224.8 GiB. Both boot headers are v3 while the published
UEFI boot image uses header v1. Only partition links, sizes, and the four-byte
header-version fields were read; no full partition was dumped and no partition
or GPT entry has been changed. `userdata` remains Android's data partition;
using it wholesale for the SWEET-style iOS root would not preserve Android
user data.

## Establish a trustworthy device tree

Do not use a DTB just because its filename or UEFI package says `munch`. The
public EDK2 compatibility DTB inspected for this port identifies itself as
`alioth` / `qcom,kona-mtp`, so it is not a trustworthy source for Munch hardware
addresses. The existence of two public EDK2 image releases is evidence that
images were published, not that either image has been validated on this phone
or that its runtime memory map matches the source descriptors.

Prefer a read-only DTB captured from the exact POCO F4 variant and installed
Android firmware. With Android booted and root access available, first check
whether the kernel exposes its original flattened tree at
`/sys/firmware/fdt`; if present, pull those bytes without changing device state:

```sh
adb exec-out su -c 'cat /sys/firmware/fdt' > /tmp/munch-live.dtb
```

If that path is absent, inspect `/sys/firmware/devicetree/base` or
`/proc/device-tree` read-only and reconstruct a DTB with a device-side or host
`dtc` that supports filesystem input. A reconstructed tree may omit original
blob metadata. A DTB extracted from Android boot artifacts is another option,
but use an unpacker that supports that artifact's actual header/container
version; `tools/unpack_bootv2.py` handles legacy Android boot headers v0-v2,
not `vendor_boot` v3/v4 containers.

Keep the exact model/region, Android build fingerprint, active slot, DTB source,
and SHA-256 with the local research notes. Do not put raw device dumps,
serials, or downloaded firmware in Git. Do not publish addresses as Munch facts
until the root `model`/`compatible` and the device provenance agree.

Run the inspector:

```sh
python3 tools/inspect-munch-dtb.py /tmp/munch-live.dtb --expect-munch
```

`--expect-munch` exits with status 2 if the root `model` and `compatible`
properties do not contain the Munch codename. Review memory and reserved-memory
first, then the GIC/timer, UFS, display, touch, and PMIC nodes. These are
discovery hints, not a ready-made XNU ADT: Linux DT properties and addresses
must be checked against the exact running firmware and XNU's required handoff.

## Port sequence and current boundary

1. Capture and validate the live/stock Munch DTB without reading `userdata`.
2. Build a Munch-specific hardware profile from verified memory carve-outs,
   GIC, timer, PSCI, framebuffer, touch, and power data. Kona's architected
   timer lists the virtual timer as PPI 11, which is GIC INTID 27; the Munch
   ADT generator now emits that value instead of SWEET's donor INTID 19. Keep
   the SWEET
   profile intact as the known-good reference.
3. Adapt the shim's boot handoff and XNU ADT generation to that profile; do not
   copy SWEET addresses such as its GIC, UFS, framebuffer, or RAM bounds.
4. Cross-compile and validate the shim/profile offline, then test only with a
   temporary `fastboot boot` image. Do not flash boot partitions or repartition.
5. Port touch separately from the inherited SWEET driver. Munch's live/kernel
   DTS places a FocalTech controller on QUPv3 SE4 SPI (CS0, up to 6 MHz), with
   IRQ GPIO 39, reset GPIO 33, and L10C/L1C supplies. The donor XNU driver is
   QUP I2C on a different serial engine and its reset, clock, pin, and power
   setup must not be called on Munch. The provided Munch kernel tree enables
   `CONFIG_TOUCHSCREEN_FOCALTECH_3658U` and contains the matching FT3658U SPI
   driver plus `fw_ft3658_k11.i`. The safe report-read reference is a single
   full-duplex SPI transfer: command `0x01`, read opcode `0xa0`, big-endian
   length, three dummy bytes, response data, then little-endian CRC-16/Kermit
   (initial value `0xffff`, reflected polynomial `0x8408`). Linux requests 90
   response bytes per report read (99 wire bytes including header, dummy bytes,
   and CRC). Its 63-byte local touch buffer includes the command marker; the
   on-wire touch portion is 62 bytes: two leading bytes plus ten six-byte
   contact slots. The remaining 28 requested bytes are reserved for gestures.
   Linux scales 12-bit contact coordinates by `10/16` for the DTS display
   extent `10800x24000`; a shorter 62-byte read has not been validated. Do not
   copy the Linux ID-detection sequence, reset sequence, configuration writes,
   or automatic firmware-upgrade path: they transmit controller commands and
   the latter can rewrite nonvolatile firmware. The XNU path must fail closed
   unless the runtime handoff proves SE4 is available, powered, clocked, and
   still configured for SPI; it must also establish safe panel-rail/reset and
   IRQ state without disturbing an ABL-owned resource. Linux's `FIFO_IF_DISABLE`
   bit selects FIFO versus GSI/DMA, and the ABL handoff value is not known from
   static DTS/source. Therefore no SE4 register programming or touch probe is
   currently enabled in XNU; the remaining work is a runtime-state observation
   and a Munch-specific transport, not adaptation of the SWEET I2C path. A
   read-only Android ADB snapshot confirmed the running kernel binds
   `990000.spi` to `spi_geni` and `spi1.0` to `fts_ts`; it does not establish
   what ABL leaves enabled or owned after `fastboot boot`.
6. Keep storage boot modes separate. Current phone tests use RAM only and leave
   `userdata` untouched. A later UFS boot may be explored after the RAM bring-up;
   it requires a verified root layout, explicit write policy, and a storage
   recovery plan. Do not let that future mode leak into RAM test images.

The device identity and live Linux-tree hardware observations are verified for
this handset, but the tree is reconstructed, Android-version metadata is
inconsistent, and the UEFI source map has not been confirmed at runtime. No
Munch XNU machine profile, Munch ADT, or POCO F4 boot result exists yet. The
4-8 GiB range is an offline candidate only; do not boot a payload until the
actual Android-ABL handoff passes the shim preflight and a RAM-root design is
available.

## CPU profile and XNU binding

The local Munch kernel DTS and reconstructed live tree agree on eight CPU nodes
with MPIDRs `0x000` through `0x700`. `kona.dtsi` groups cores 0-3 into
frequency domain 0, cores 4-6 into domain 1, and core 7 into domain 2, with
capacities 448, 1023, and 1024 respectively. The generated Munch ADT retains
all CPUs as one `cluster-type = 0` SMP cluster because the current XNU port does
not implement heterogeneous scheduling. All eight CPU nodes use
`compatible = "qcom,kryo"`; before patch 0082, `SweetCPU` matched only
`qcom,kryo470`, so the driver would not bind to the generated Munch CPUs. Patch
0082 adds the missing match without changing the SWEET DT compatibility entry.
The driver still reports the cosmetic name `Kryo 470`, and topology/power
management is not a Munch-specific implementation.

## XNU machine-config boundary

The workflow accepts a `machine_config` input, but that does not make `MUNCH` a
valid target: patch 0004 registers only `SWEET`, adds only
`MASTER.arm64.sweet`, and defines only `SWEET.h`. Several later patches also edit
that SWEET master config directly. A separate Munch target needs its own master
config and board header, plus a patch-series review so the donor-only settings
are not silently inherited.

Do not implement Munch by defining `SWEET` or
`ARM64_BOARD_CONFIG_SWEET` as aliases. Those guards enable donor platform
matching and code paths for display/framebuffer, the existing I2C touch driver,
keys and PMIC/SPMI access, UFS and persistent logging, among other features.
The Munch ADT's compatibility list currently retains `xiaomi,sweet` for that
donor matcher; it is scaffolding and must not be treated as proof that the
Sweet platform expert or its nubs are safe on Munch.

The following are static-source evidence, not runtime handoff guarantees:

- Kona DTS declares eight `qcom,kryo` CPU nodes, `enable-method = "psci"`,
  PSCI 1.0 through SMC, a GICv3, and the architected virtual timer at PPI 11
  (INTID 27). The current ADT carries those IDs and timer binding.
- The inherited CPU implementation embeds SWEET policy: one generic cluster,
  boot CPU 0, Aff1-based core IDs, a hard limit of eight, a hard-coded
  PSCI-SMC CPU_ON path, and a secondary entry that reconfigures EL2 under
  assumptions about the absence of a resident hypervisor and `CNTVOFF_EL2`.
  The `0..0x700` DTS IDs and SMC conduit are not enough to validate that entry
  against Android ABL's actual MPIDR, exception level, cache state, or PSCI
  behavior.
- Munch's DTS capacity groups are 4+3+1, unlike SWEET's 6+2 donor description.
  Treating every CPU as homogeneous may be acceptable for a first single-core
  bring-up, but it is not a validated final scheduler/topology configuration.
- GICv3 and timer wiring are known from DTS; the GIC security view, Group-1
  delivery, timer acknowledgement behavior, and exclusive CPU-interface state
  after ABL have not been measured on this Munch boot path. SWEET runtime GIC
  observations must not be carried over as Munch facts.

Patch `0083-munch-machine-config.patch` now provides that first isolated Munch
profile. It adds `MACHINE_CONFIG=MUNCH`, `MASTER.arm64.munch`, `MUNCH.h`, a
`xiaomi,munch` platform/`qcom,kryo` CPU personality, and shared RAM-root/crypto
guards. The Munch CPU driver matches only the boot CPU and reports one selected
CPU; it does not call PSCI `CPU_ON` for secondaries. Donor SWEET hardware paths
remain guarded out, including the I2C touch path, PMIC/SPMI, UFS/persistent
logging, USB bring-up and watchdog/battery services. The staging scripts now
also fetch the pinned libpthread, BearSSL and HFS inputs for a MUNCH CI build.

The MUNCH make configuration must stay plain `arm64`, not XNU's default
`arm64e`; the pthread, BearSSL AES/hash, and HFS staging templates are enabled
for both SWEET and MUNCH. Repository validation checks these build-plumbing
conditions, and the complete patch series still needs a fresh apply/build check.
No MUNCH CI artifact, local macOS compile, payload, or phone boot exists.
The generated Munch ADT still must be used without UFS and with `rd=md0`; the
runtime ABL memory/EL/GIC/PSCI contracts and a RAM-sized root remain blockers.
The ADT's `sweet-fbcon` entry currently points at the Munch continuous-splash
candidate (`0x9c000000`, size `0x2300000`) seen in the reconstructed Android
tree. This does not prove ABL ownership or framebuffer validity after
`fastboot boot`; all inherited early and normal framebuffer writes remain a
separate runtime-safety gate and must not be treated as verified Munch support.
