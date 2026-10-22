# ios-munch

**An experimental iOS 16 port targeting the POCO F4 (`munch`).**

> **Port status:** `munch` (POCO F4, SM8250 / Snapdragon 870) is the target device. `sweet` (Redmi Note 10 Pro, SM7150 / Snapdragon 732G) is the upstream donor platform. A RAM-only Munch XNU boot reached a live diagnostic PID 1 on 2026-10-21; this is not the iOS userland. The Munch UFS-root path now builds in Actions run `39668666918`, but has not yet been tested on the phone. The local 28-GiB `iosroot` partition is currently F2FS; a separate HFS+ install candidate exists on the host, with a 16-GiB HFS+ volume padded to the 28-GiB partition size. The GPT was changed and `userdata` was reformatted as F2FS during the 2026-10-21 storage test. Only selected shared-storage folders have partial archives; there is no full userdata backup. Do not perform device writes without a fresh confirmation. See `NOTES.md` for the active handoff.

The inherited `sweet` bring-up baseline boots Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) with the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and stock apps. As of **2026-10-18**, that Redmi Note 10 Pro baseline reaches the iOS home screen, supports touch, hardware buttons, and home/app-switcher gestures. Rendering runs entirely on the CPU; there is no GPU driver. These results have not been reproduced on `munch`.

> The `sweet` baseline is experimental, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. Its legacy installation writes to `userdata` and erases Android user data. Do not follow those instructions on a POCO F4.

## Inherited `sweet` baseline: what works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi bootloader → `fastboot boot` of our shim → XNU. Nothing is flashed to the boot partitions |
| CPU | All 8 cores (six Cortex-A55s and two Cortex-A76s) via PSCI, 16K pages, GICv3 interrupts, timers |
| Memory | About 3.95 GiB of the upper DDR bank |
| Storage | SWEET baseline uses a UFS root; Munch has a CI-built, untested UFS `iosroot` path |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`) services; CoreAnimation renders in software; frames are copied into the bootloader's framebuffer. Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center, Settings |
| Touch | Goodix GT9896 over I²C (GENI), rail enabled through RPMh, delivered as HID digitizer events |
| Buttons | Power, volume up/down (PMIC through SPMI) as HID consumer keys; lock/wake works |
| Battery | IOPMPowerSource service (static 100 %, charging icon; real fuel-gauge readout prepared) |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| Activation | Native "Hactivation" path of mobileactivationd, chosen through device-tree security status. No Apple servers involved |

## Inherited baseline: what does not work yet

- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS or SEP / Face ID.** Daemons for them start and fail or keep relaunching.
- **No GPU.** Everything is drawn by the CPU at roughly 20–30 fps.
- **Few apps.** The image has only what is on the system volume. There is no App Store, no Apple ID, and some Settings pages are empty.
- **Slow boot:** about 4 minutes on the Apple logo.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.

## Intended Munch UFS test flow

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot-ufs-iosroot.img  (temporary; nothing is flashed)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU UFS driver -> GPT partition `iosroot` (`rd=ufsroot0`)
              -> HFS+ root -> iOS userspace
```

The Munch test boot starts the shim and XNU from a temporary fastboot boot image; XNU is intended to mount `/dev/ufsroot0` from the dedicated `iosroot` GPT partition. The current UEFI build has UFS/SCSI/GPT and FAT support but no HFS+ or APFS filesystem driver, so it cannot directly launch an HFS+ root from its boot manager. The read-only XNU UFS probe and HFS+ host image are prepared, but neither the UFS handoff nor iOS userspace has been verified on the phone. The phone's `iosroot` is still F2FS, and no image has been installed. The HFS+ volume in the host image is 16 GiB inside a 28-GiB container. Keep all device writes gated on explicit confirmation; never use `userdata` as the iOS root.

## How to do it yourself

> These steps document offline Munch preparation. They do not install anything or access device partitions.

The inherited Redmi Note 10 Pro (`sweet`) workflow used a destructive UFS root and is retained only as historical reference. Do not apply those storage steps to Munch. Bootloader, modem, and persist partitions were not intentionally changed; the GPT and `userdata` were changed during the 2026-10-21 storage test. OrangeFox was temporarily booted and was not flashed.

### 0. Preserve non-userdata state

Keep the existing non-userdata backups and the six partial shared-storage archives outside git. They are not a full `userdata` backup. Do not dump or write `userdata` for this RAM-only workflow. The shim's first device test is allowed only as a temporary `fastboot boot` image after the runtime UEFI memory map is verified.

The shim needs the stock DTB. Unpack your phone's stock `boot.img`:

```sh
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
```

For a read-only live-DTB build without storing the dump in the repository, set
`DTB_PATH` to an external file, for example
`cd shim && DTB_PATH=/tmp/munch-live.dtb ./build.sh`.

### 1. Build the kernel

The XNU workflow builds either the inherited `SWEET` reference or the isolated `MUNCH` target on GitHub's macOS runners with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). It fetches xnu-8796, applies the patch series in order, and stages the pinned open-source libpthread, HFS and BearSSL sources. MUNCH kernel build run `39597312881` succeeded; its artifact is available from that Actions run. To start another build:

```sh
gh workflow run xnu.yml -R YOU/ios-munch
gh workflow run xnu.yml -R YOU/ios-munch -f machine_config=MUNCH
gh run download RUN_ID -R YOU/ios-munch -n xnu-kernel -D xnu/out
```

To check a patch locally before spending a CI run, `tools/xnu-cc.py` replays the compile commands of a previous CI log for single files.

### 2. Legacy SWEET root image (reference only)

This creates the inherited on-disk SWEET root; it is not part of Munch RAM
testing. Do not write this image to the phone during the current phase.

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. Then, on Linux, with `apfs-fuse`, `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images from the IPSW (see BuildManifest.plist for their names)
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg iosroot.img 28G
sudo fsck.hfsplus -fy iosroot.img
sudo mount -t hfsplus -o loop,force iosroot.img /mnt/iosroot
sudo python3 tools/munch-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg iosroot.img iosroot.simg
```

`mkiosroot.sh` copies the OS volume and both cryptexes into one case-sensitive journaled HFS+ volume. It adds the dyld cache links, the plain system keybag and the log directories the daemons expect. `munch-root-tweaks.py` applies the bring-up changes: it skips Setup, seeds the activation state, removes the baseband daemon, disables the watchdogs that kill slow apps and the crash-looping media paths, and so on. The script's header explains each change.

The device tree builder copies `/product` from the real iPhone X device tree, which MobileGestalt needs. Extract it once, without downloading the whole IPSW:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

### 3. Munch Boot Status

The RAM-only Munch kernel reached a live diagnostic PID 1 on 2026-10-21. CI run
`39668666918` then built the Munch UFS-root kernel. A read-only UFS test image
is prepared at
`~/Загрузки/ios-munch-install-20H392/shim-boot-ufs-iosroot.img`; boot it only
with temporary `fastboot boot`. The current partition is F2FS, so this probe
can verify UFS and GPT discovery but is expected to fail HFS mounting. It does
not install iOS. Do not flash the root candidate until separately confirmed.

The UFS-root diagnostic payload is built read-only (`iosrootrw=0`). A future
full iOS boot needs explicit `iosrootrw=1`; the kernel port restricts GPT lookup
and writes to the `iosroot` partition. Do not enable writes until the
read-only UFS probe succeeds and the user confirms the install/boot step.

The following SWEET-only boot arguments are historical reference, not a Munch
recipe:

| Boot-arg | Effect |
| --- | --- |
| `rd=md0` | Historical RAM-root test; omit UFS from the generated ADT |
| `cpus=8` | Start all eight cores on the inherited sweet baseline; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1` | SWEET console option; Munch diagnostics must not use persistent storage |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |

### Build a temporary MUNCH RAM boot image

For the diagnostic RAM-only path, use one script. It generates an upper-bank
Munch ADT, packs a small RAM root, embeds the `MUNCHXNU` payload as the Android
ramdisk, and verifies the unpacked image:

```sh
tools/mkram-boot.sh \
  --kernel xnu/out/RUN_ID/kernel.development.munch \
  --dtb /tmp/munch-live.dtb \
  --out /tmp/munch-ram-8cpu \
  --cp-macho /tmp/munch-init.macho \
  --cpus 8
fastboot boot /tmp/munch-ram-8cpu/shim-boot.img
```

`--cpus 1` is the safe fallback. The script prints SHA-256 hashes and refuses to
overwrite an existing image. It never runs `fastboot` and never touches
partitions or `userdata`.

### 4. Device testing boundary

Device tests are limited to temporary `fastboot boot` images. Do not flash a
partition without a separate, explicit confirmation. The UFS-root probe uses
only the dedicated `iosroot` GPT partition; it never selects or accesses
`userdata`. The 4–5 minute boot-to-lock-screen result refers to SWEET only.

### 5. Read offline logs

```sh
tools/qemu-munch.py /tmp/payload.bin /tmp/qemu-run --secs 45 --fail-on-panic
```

The QEMU output contains the framebuffer and kernel logs without touching the phone. QEMU `virt` has no Munch UFS controller, so the UFS-root path requires a temporary hardware boot; the first test keeps `iosroot` read-only.

### QEMU

`tools/qemu-munch.py` boots the same kernel on QEMU `virt` with a test RAM-disk root. It is useful for kernel-only changes; see `NOTES.md`.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2 |
| `xnu/patches/` | The XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the inherited sweet-hardware Apple Device Tree and iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from the IPSW images |
| `tools/munch-root-tweaks.py` | Bring-up changes to the root (Setup skip, activation, daemons) |
| `tools/munch-log-pull.sh` | Disabled fail-closed stub; it never reads `userdata` |
| `tools/munch-log.py` | Offline parser for an already-approved local log capture |
| `tools/munch-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-munch.py` | QEMU test boots with framebuffer text decoding |
| `tools/adt.py`, `remotezip.py`, `unpack_bootv2.py`, `sparsediff.py` | Helpers |
| `NOTES.md` | Detailed engineering log: every boot, crash and decision, plus hardware facts |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, inherited SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services. Apple's IOHIDFamily-1915.140.3 user client and event queue, matching the iOS 16.7 userland |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | Power and volume keys over SPMI; touch over GENI I²C |
| 0061 | Present the display as 1125×2436 |
| 0062 | RPMh regulator vote for the touch rail; battery power source |
| 0083 | Isolated MUNCH machine config: SM8250 board header, Munch platform/CPU match, shared RAM-root/crypto guards, and boot-CPU-only startup |
| 0063 | GENI FIFO packing fix and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary A55 cores via PSCI CPU_ON |
| 0066–0067 | Fault-region crash info; console mute |
| 0068 | IOSurface lookup semantics for already-mapped surfaces |
| 0069 | Batched persistent-log flushes |

## Key hardware facts

- RAM: payloads use the upper bank, `0x100000000` + `0xfcc00000` (about 3.95 GiB), above Qualcomm's protected carve-outs.
- GICv3 with security extensions: the timer goes through Group 1 IRQ (virtual timer INTID 19), not FIQ as on Apple silicon.
- PSCI through `smc`, 19.2 MHz timer, ARMv8.2 without pointer authentication. MPIDR uses Aff1 for the core number.
- Panel: 1080×2400 DSC video mode, continuous splash from ABL, no refresh kick needed.
- Touch: Goodix GT9896 at I²C `0x5d` on QUP SE7 (`0xa84000`), powered by PM6150L LDO11 through RPMh.
- Buttons: power and volume down on the PM6150 PON block, volume up on PM6150L GPIO2 (SPMI SID 4).

More in [`NOTES.md`](NOTES.md).

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). iOS itself is Apple's and is not distributed here.
