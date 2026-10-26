# F4 boot inputs reviewed on 2026-10-26

Source: `munch-boot-inputs-20261026-030047.tar.gz`, active slot B. All four
partition image SHA256 values match the archive's `SHA256SUMS`. The full images
and extracted vendor resources remain local under ignored workspace artifacts.
`boot-observed.json` contains sizes, hashes, and selected header fields.

## Android packaging

- Boot header v3, page size 4096, ARM64 Image kernel.
- Vendor boot header v3, DTB at file offset 8192, size 460338 bytes.
- DTBO v0 table with one overlay, size 441768 bytes.
- Base DTB plus overlay can be merged with `fdtoverlay`. The merged DT includes
  the FocalTech SPI node. Its root model remains the generic Kona model; the
  live DT additionally contains bootloader/runtime changes.

These are installed images and may contain root or ios-sweet changes. Android's
header version does not establish which temporary boot-image formats ABL accepts.
Do not package the UMI boot image unchanged or flash these dumps as part of bring-up.

## XBL findings

XBL is an ARM64 ELF containing a UEFI firmware volume. Its configuration describes:

| Region / parameter | Value |
| --- | --- |
| UEFI FD | `[0x9FC00000, 0x9FF00000)` |
| DXE heap | `[0x98900000, 0x9BC00000)` |
| Scheduler heap | `[0x9BC00000, 0x9C000000)` |
| Display reserved | `[0x9C000000, 0x9E400000)` |
| Primary stack | `[0x9FF90000, 0x9FFD0000)` |
| SecPagePoolCount | `0x980` |

XBL reserves more display memory than the live DT splash node. Preserve the
larger firmware reservation. Runtime EFI allocations still require inspection.

All 91 module GUIDs in the pinned Mu-Silicium alioth INF templates exist in this
XBL. This establishes module availability, not runtime protocol compatibility.
`extract-xbl.py` writes local firmware and a hash manifest from these GUIDs.

The original F4 TzDxe PE32 hash is
`2ff06cf1ccd084d73a5a18bf781dbf1a5c7851b31322825fa0a784dfa8ab637e`.
It differs from UMI's guarded revision and from the alioth patched template.
No UMI binary offset patch or foreign driver executable is used in the F4 overlay.
TzDxe initialization remains a device-test item.

## Reproduce inspection

```sh
python3 devices/poco-f4/inspect-boot.py /path/to/image-directory --slot b
```

The tool emits no kernel command lines. `uefi/README.md` documents the experimental
source overlay and local firmware extraction procedure.
