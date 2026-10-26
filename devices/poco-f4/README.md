# POCO F4 (munch): bring-up preparation

Status: the phone has executed XNU through early BSD initialization and the
static RAM-root hello program. The user reports hello output, the program's
intentional PID-1 exit panic, and return to UEFI. Storage FD revision 3 has also
found iosroot and read its first 64 KiB through vendor Block I/O. The user subsequently reports an Apple logo and iOS interface in mode 58;
touch input is now confirmed with the powered FT3658U XNU prototype. The latest
image requests eight CPUs; their actual online count has not been captured.
Adreno 650 hardware drawing is confirmed with the r4 diagnostic. The user confirms Mesa/freedreno-backed iOS UI and a large smoothness improvement.
Intermittent rendering glitches while swiping remain under investigation.
Audio, Wi-Fi and modem support are not established. See
TOUCH-BRINGUP.md and GPU-BRINGUP.md for the current driver work.

A first iOS 16.5 (20F66) attempt is prepared in loader/: a verified local APFS
system image staged in iosroot, a 4-MiB helper RAM disk, and a small RAM boot
image. Source IPSW member integrity and both APFS images passed host checks;
the installer check and a subsequent user-reported iOS interface have been
observed. Exact launch/runtime logs have not yet been captured.

observed.json records selected facts from rooted Android's live DT. Raw captures,
vendor firmware and downloaded/generated iOS files stay outside git.

## Reproduce the device summary

From the repository root:

```sh
python3 devices/poco-f4/inspect-dt.py /path/to/device-tree.tar > /tmp/munch-observed.json
```

The inspector reads a sysfs device tree archive without extracting it. It emits
selected hardware properties and resolves touchscreen GPIO and supply references.
It excludes device-specific boot arguments except for the selected panel name.

## Comparison with UMI

| Component | Observed on this F4 | Implication |
| --- | --- | --- |
| RAM banks | `[0x80000000, 0xBCC00000)`, `[0xC0000000, 0x200000000)` | The low-bank gap differs from UMI's map. |
| CPU MPIDRs | `0x000` through `0x700`, step `0x100` | UMI's core numbering is a useful reference. |
| CPU capacity | 448 for cores 0–3, 1023 for cores 4–6, 1024 for core 7 | Slow-core mask candidate: `0x0f`; scheduling remains untested. |
| GICv3 | Distributor `0x17A00000`, redistributors `0x17A60000` | Matches the UMI loader constants. |
| PSCI / timer | SMC; 19.2 MHz timer, virtual timer PPI 11 / INTID 27 | Matches the expected handoff convention; runtime unverified. |
| Splash reservation | `[0x9C000000, 0x9E300000)` | Active scanout address, stride, and pixel format still require verification. |
| Display metadata | `qcom,mdss_dsi_l11r_38_08_0a_dsc_cmd` | Select this panel's timings rather than other panels present in the DT. |
| Touch | FocalTech SPI, 6 MHz maximum, reset GPIO33, IRQ GPIO39 on TLMM | UMI's ST I2C driver cannot be used directly. |
| Touch supplies | PM8150A L10: 3.0–3.312 V; L1: 1.8 V | Verify voltage selection and sequencing against the Android driver. |

Memory intervals above describe DT banks, not free memory. Subtract all firmware
and device reservations and obtain the actual EFI memory map before allocating
the XNU window. Do not infer safe FD or payload addresses from bank coverage.

## UEFI sources reviewed

- UMI fork `6b6fb2b`, `uefi/umi/`: useful RAM payload hook and Qualcomm USB path.
  Its extraction script requires an exact UMI TzDxe binary hash; it cannot be
  repurposed by changing the device name.
- Mu-Silicium `4cba43c90e02cdbefbcd9fcdbda33d2328deaec0`: no munch platform in
  this revision. Its alioth package provides an SM8250-AC reference, not an F4 port.
- `https://github.com/0xSelenicDove/edk2-msm-munch`, commit `d4b3760`:
  contains a munch package and a different EDK2 framework. Its FD is placed at
  `0xCE000000` with size `0x02000000`, unlike UMI's `0x9FC00000` / `0x00300000`.
  Its memory map also differs from this device's live DT. This is a reference
  requiring audit, not a validated HackiOS boot path. No external binaries or
  installer scripts have been executed.

The current upstream loader and packer still select Redwood-specific behavior
in several places. Adding this directory alone does not make them device-neutral.
An F4 port must explicitly address those paths before any boot image is offered.

## System staging

The earlier ios-sweet installation was reported as iOS 16.7. Initial diagnostics
preserved it. The owner subsequently authorized replacing the dedicated iosroot
contents for a fresh iOS 16.5 attempt. The generated installer targets only the
verified munch mapping iosroot -> /dev/block/sda35, checks image hashes and target
state/capacity, and verifies a readback. It does not target Android boot/system
or userdata. The boot loader reads the staged system into RAM; no persistent
iOS data configuration is enabled.

The active-slot images have now been collected and reviewed; see `BOOT-INPUTS.md`
and `boot-observed.json`. The experimental UEFI overlay is in `uefi/`.

For a future refresh, run `collect-boot-inputs.sh` as root on the target phone to collect the active
slot's XBL, boot, vendor_boot, and DTBO images. The script only reads partitions
and saves regular files under `/data/local/tmp`, then packages them in Download.
These are the currently installed images; root or previous modifications may
mean they differ from stock firmware.

XBL supplied this device's `uefiplat.cfg` and firmware modules.
Boot/vendor_boot/DTBO establish the packaging and DT inputs from the same slot.
The pinned UMI/Mu-Silicium source package is the initial diagnostic baseline.
Its F4 loader integration and RAM handoff still require implementation and testing.
Storage drivers, partition writes, and the dual-boot installer are outside the
initial bring-up path.
