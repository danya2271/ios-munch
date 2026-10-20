# POCO F4 (`munch`) Porting Notes

This is a hardware bring-up guide for the Munch fork. The current shim, XNU
machine configuration, and ADT generator still describe Redmi Note 10 Pro
(`sweet`, SM7150); the Munch name does not make those values portable.

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

The public EDK2 Munch map reserves PSTORE at `0xb0000000+0x400000` and display
ramdump at `0xb0400000+0x1000000`, directly invalidating SWEET's current
`PHYS_BASE=0xb0000000`. Its high RAM descriptor ends at `0x200000000`, whereas
the live tree describes RAM up to `0x280000000`. Establish the exact UEFI map
and reconcile this 2 GiB span difference before choosing XNU memory bounds.
The bundled UEFI compatibility DTB identifies as `alioth`, not `munch`; a
successful CI build or release is not evidence of a successful phone boot.

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
addresses. A UEFI build or release is also not evidence that it boots on the
phone.

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

1. Capture and validate the live/stock Munch DTB and read the partition layout
   without writing to the device.
2. Build a Munch-specific hardware profile from verified memory carve-outs,
   GIC, timer, PSCI, UFS, framebuffer, touch, and power data. Keep the SWEET
   profile intact as the known-good reference.
3. Adapt the shim's boot handoff and XNU ADT generation to that profile; do not
   copy SWEET addresses such as its GIC, UFS, framebuffer, or RAM bounds.
4. Cross-compile and validate the shim/profile offline, then test only with a
   temporary `fastboot boot` image. Do not flash boot partitions or repartition.
5. Treat Android dual boot as a separate storage/boot-menu task. Although
   `userdata` is large, the existing SWEET flow uses the whole partition for
   the iOS root and would destroy Android's data filesystem. A dual-boot design
   needs a verified way to store and locate an iOS root without overwriting
   Android data; keep Android partitions untouched until that design exists.

The device identity and live Linux-tree hardware observations are now verified
for this handset, but the tree is reconstructed, the Android-version metadata
is inconsistent, and the UEFI memory map does not agree with the live RAM
extent. No Munch XNU machine profile, Munch ADT, or POCO F4 boot result exists
yet. The first implementation remains blocked on a trustworthy, reconciled
memory/firmware handoff; do not invent `physBase` or `memSize` values.
