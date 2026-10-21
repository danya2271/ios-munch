# ios-munch

**An experimental iOS 16 port targeting the POCO F4 (`munch`).**

> **Port status:** `munch` (POCO F4, SM8250 / Snapdragon 870) is the target device. `sweet` (Redmi Note 10 Pro, SM7150 / Snapdragon 732G) is the upstream donor platform whose working XNU port is being used as a reference. XNU for the isolated plain-arm64 `MUNCH` target built successfully in Actions run `39597312881`. A RAM-only diagnostic image was accepted by `fastboot boot` on 2026-10-21, but no successful Munch XNU boot is confirmed. The phone is currently in OrangeFox Recovery, temporarily booted with `fastboot boot`. During the 2026-10-21 storage test, the GPT was changed, `userdata` was reformatted as F2FS, and a 28-GiB `iosroot` partition was created and formatted as F2FS. Only selected shared-storage folders have partial archives; there is no full userdata backup. Do not use inherited storage/boot steps or perform further device writes; see `NOTES.md` for the handoff. Donor touch, PMIC, UFS and watchdog paths remain SWEET-only.

The inherited `sweet` bring-up baseline boots Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) with the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and stock apps. As of **2026-10-18**, that Redmi Note 10 Pro baseline reaches the iOS home screen, supports touch, hardware buttons, and home/app-switcher gestures. Rendering runs entirely on the CPU; there is no GPU driver. These results have not been reproduced on `munch`.

> The `sweet` baseline is experimental, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. Its legacy installation writes to `userdata` and erases Android user data. Do not follow those instructions on a POCO F4.

## Inherited `sweet` baseline: what works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi bootloader → `fastboot boot` of our shim → XNU. Nothing is flashed to the boot partitions |
| CPU | All 8 cores (six Cortex-A55s and two Cortex-A76s) via PSCI, 16K pages, GICv3 interrupts, timers |
| Memory | About 3.95 GiB of the upper DDR bank |
| Storage | SWEET baseline uses a UFS root; Munch bring-up is RAM-only and has no root volume yet |
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

## Intended Munch test flow

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img              (temporary; nothing is flashed to boot)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> virtual RAM disk (`rd=md0`)
              -> iOS userspace, if a RAM-sized root can be prepared
```

This remains an unvalidated Munch OS boot path. MUNCH XNU has completed CI, and a minimal RAM userspace has run for 150 seconds in QEMU without panic. No full-root userspace boot is available yet. A local iPhone X 16.7.16 root image is present, but the current payload format embeds the entire uncompressed image in RAM; that root exceeds the candidate memory window. A RAM-sized root, demand-paged root, or later UFS-root design remains unresolved. Device storage has already been modified: the original `userdata` filesystem was reformatted, and only selected shared-storage folders have archives. Do not perform additional userdata or partition writes until a recovery plan is reviewed.

## How to do it yourself

> These steps document offline preparation for Munch RAM-only bring-up. They do not install anything and do not read, back up, mount, or write `userdata`.

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

### 3. Munch RAM boot status

The `MUNCH` kernel artifact from Actions run `39597312881` has been packed into
a diagnostic, RAM-only image and sent through temporary `fastboot boot` on
2026-10-21. ABL accepted the image, but a successful XNU boot is not confirmed.
The QEMU run reached CPU/GIC setup, platform registration and AppleCLCD, then
waited for an HFS root device because this diagnostic intentionally contains no
root disk. A future payload with a RAM root must use `rd=md0` and omit UFS from
the ADT; start with `cpus=1`. Never flash this image. No further `userdata`
or partition writes are allowed until the current storage state is reviewed.

The following SWEET-only boot arguments are historical reference, not a Munch
recipe:

| Boot-arg | Effect |
| --- | --- |
| `rd=md0` | Root on the payload's RAM disk; no UFS access |
| `cpus=8` | Start all eight cores on the inherited sweet baseline; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1` | SWEET console option; Munch diagnostics must not use persistent storage |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |

### 4. Device testing boundary

Device tests are limited to temporary `fastboot boot` images. Do not flash
partitions or access `userdata` during this RAM-only phase. The 4–5 minute
boot-to-lock-screen result refers to SWEET only.

### 5. Read offline logs

```sh
tools/qemu-munch.py /tmp/payload.bin /tmp/qemu-run --secs 45 --fail-on-panic
```

The QEMU output contains the framebuffer and kernel logs without touching the phone. A device test must use the same RAM-only payload and a temporary `fastboot boot`; no storage root is supported by this workflow.

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
