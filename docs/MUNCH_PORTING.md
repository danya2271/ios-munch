# POCO F4 (`munch`) Porting Notes

This is a hardware bring-up guide for the Munch fork. The current shim, XNU
machine configuration, and ADT generator still describe Redmi Note 10 Pro
(`sweet`, SM7150); the Munch name does not make those values portable.

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
5. Treat Android dual boot as a separate storage/boot-menu task. The existing
   SWEET flow uses all of `userdata` for the iOS root; it cannot preserve
   Android user data as-is. Keep Android partitions untouched until a distinct
   storage design has been implemented and explicitly approved.

No verified Munch DTB, Munch XNU machine profile, or POCO F4 boot result is
currently in this repository. The first board-specific port is blocked on
trustworthy device-tree and firmware evidence; inventing constants would make
the port less safe, not more complete.
