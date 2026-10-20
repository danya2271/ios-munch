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

No local iOS root image is available in `research/`. The inherited root builder
defaults to a 16-GB raw HFS+ image, and `mkpayload.py` embeds that image
uncompressed as a resident RAM disk. That exceeds the 4-GB source-derived
candidate memory window; the candidate itself is not a verified runtime map.
A RAM-sized root or demand-paged design is unresolved and must be measured
before proposing a phone boot.

## Live device evidence (2026-10-20)

Read-only ADB inspection identifies `ro.product.device=munch`, model
`22021211RG`, active slot `_b`, and `MemTotal=7815544 kB`. The reported Android
version (`16`, SDK `36`) conflicts with the version component (`13`) in the
fingerprint `POCO/munch_global/munch:13/RKQ1.211001.001/V816.0.9.0.ULMMIXM:user/release-keys`;
resolve this before treating the capture as a stable firmware baseline.

`/sys/firmware/fdt` is absent. The tree was reconstructed read-only from
`/proc/device-tree` using root access in the global mount namespace. It is FDT
v17, 812030 bytes, with root model `Qualcomm Technologies, Inc. xiaomi munch`
and compatibles `qcom,kona-mtp; qcom,kona; qcom,mtp`. SHA-256 is
`7ac4e2551f5d76d1f46d0ef6d53a3246bd5cd498417a466b23e1eb0cf985025c`. The local
copy is `research/munch-live.dtb` (ignored by Git, mode 0600); it contains a
device serial in `chosen/bootargs` and must not be published or added to Git.

The reconstructed tree reports 8 PSCI CPUs, GICv3 at `0x17a00000` and
`0x17a60000`, UFS at `0x1d84000`, SDE at `0xae00000`, Focaltech touch on SPI at
`0x990000`, and RAM ranges `0x80000000+0x3bb00000` and
`0xc0000000+0x1c0000000` (ending at `0x280000000`). It also contains dynamic
reserved pools whose actual placement is not encoded as fixed addresses.
These are live Linux-tree observations, not proof that XNU can use the ranges.

The public EDK2 repository has two Munch image releases, both published on
2026-08-20: `v1.1.0` at source commit `1b11fb1` and a later build at
`d4b3760`. In the later source map, PSTORE is at
`0xb0000000+0x400000`, display ramdump at `0xb0400000+0x1000000`, and the
last conventional RAM descriptor is `0xffc20000+0x1003e0000`, ending at
`0x200000000` (8 GiB). Thus SWEET's current `PHYS_BASE=0xb0000000` overlaps
reserved regions. The live Linux tree reports RAM through `0x280000000` (10
GiB address), so the EDK2 source map omits the top 2 GiB of that address span.
This is an address-limit comparison, not proof of 2 GiB of missing physical
capacity or of the map installed/running on this phone.

**Candidate, not validated:** `[0x100000000, 0x200000000)` (4-8 GiB) lies
inside the later EDK2 source's conventional RAM descriptor and below its
reported end. It avoids the listed low-memory carve-outs and is a plausible
region to test for XNU placement after confirming the UEFI runtime memory map
on this handset. It must not yet be used to build or boot a Munch payload: the
released image's successful boot and its runtime descriptors are unverified.
The compatibility DTB bundled in that source still identifies as `alioth`,
so it is not a substitute for the live Munch tree.

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
   GIC, timer, PSCI, framebuffer, touch, and power data. Keep the SWEET
   profile intact as the known-good reference.
3. Adapt the shim's boot handoff and XNU ADT generation to that profile; do not
   copy SWEET addresses such as its GIC, UFS, framebuffer, or RAM bounds.
4. Cross-compile and validate the shim/profile offline, then test only with a
   temporary `fastboot boot` image. Do not flash boot partitions or repartition.
5. Port touch separately from the inherited SWEET driver. Munch's live/kernel
   DTS places a FocalTech controller on QUPv3 SE4 SPI (CS0, up to 6 MHz), with
   IRQ GPIO 39 and reset GPIO 33. The donor driver's FocalTech path is QUP I2C;
   it cannot be reused as-is. The provided Munch kernel tree enables
   `CONFIG_TOUCHSCREEN_FOCALTECH_3658U` and contains a matching FT3658U SPI
   driver plus `fw_ft3658_k11.i`, which provides a concrete protocol/report
   reference. Port the SE4 SPI controller and GPIO/regulator sequencing into
   XNU, then adapt its report path. Verify whether firmware upload is needed
   when booting outside the Android kernel.
6. Keep storage boot modes separate. Current phone tests use RAM only and leave
   `userdata` untouched. A later UFS boot may be explored after the RAM bring-up;
   it requires a verified root layout, explicit write policy, and a storage
   recovery plan. Do not let that future mode leak into RAM test images.

The device identity and live Linux-tree hardware observations are verified for
this handset, but the tree is reconstructed, Android-version metadata is
inconsistent, and the UEFI source map has not been confirmed at runtime. No
Munch XNU machine profile, Munch ADT, or POCO F4 boot result exists yet. The
4-8 GiB range is a source-derived candidate only; do not set `physBase` or
`memSize` until the actual boot handoff and reservations are verified.
