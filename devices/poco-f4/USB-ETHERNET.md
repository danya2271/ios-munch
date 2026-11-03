# POCO F4 USB Ethernet and Live XNU Logs

This guide builds and boots the tested CDC-ECM USB Ethernet logger for the
POCO F4 (`munch`). The host gets `192.168.7.1`; XNU uses `192.168.7.2`.
Console log datagrams are sent to UDP port `5140`.
The one-shot autoboot profile and live log delivery have been confirmed on the
phone.

Use `fastboot boot`; these instructions do not flash or modify a phone
partition.

## Build the Autoboot Image

Run commands from the repository root. The packer expects the local verified
iOS configuration, the reviewed Munch UEFI firmware, and the captured live DTB
at the paths shown below. `artifacts/munch-build/ios-config.json` is prepared by
the existing root-image build.

Rebuild XNU after changing kernel sources:

```sh
MUNCH_STAGE=1 \
MUNCH_STAGE_REPORT="$PWD/artifacts/munch-build/munch-stage.json" \
  ./xnu-build.sh -j8 -k
```

If the source-built kernel is already current, this step can be skipped. Pack
the one-shot USB-autoboot image:

```sh
python3 devices/poco-f4/loader/pack-iosroot.py \
  --fd Mu-Silicium/Build/munchPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
  --dtb research/munch-live.dtb \
  --kernel xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  --ios-config artifacts/munch-build/ios-config.json \
  --output /tmp/munch-iosroot-usb-autoboot.img \
  --cpus 8 \
  --usb-autoboot \
  --extra-cmdline 'loader_return_after=600'
```

Touchscreen and GPU are enabled by default. `--no-touch` and `--no-gpu` disable
them for isolated diagnostics. `--cpus` accepts `1` or `8`.

`--usb-autoboot` makes the one-shot UEFI loader start and stop the Qualcomm USB
protocol before handing DWC3 to XNU. Do not add `--usb-debug` to the same
command; the two profiles are mutually exclusive. The default
`loader_return_after=600` returns from XNU after ten minutes, after which the
one-shot loader reboots the phone. Use `loader_return_after=0` to disable this
timer for a longer run.

## Boot and Capture Logs

Start the host listener first, in one terminal:

```sh
sudo tools/munch-usbnet.sh logs/munch-live.log
```

The script waits for the CDC Ethernet interface, assigns the host address
`192.168.7.1/24`, and listens on UDP `5140`. It waits indefinitely by default.
To use a 120-second wait instead:

```sh
sudo env MUNCH_USB_TIMEOUT=120 tools/munch-usbnet.sh logs/munch-live.log
```

In another terminal, boot the image:

```sh
fastboot devices
fastboot boot /tmp/munch-iosroot-usb-autoboot.img
```

Do not run `fastboot oem run` with this profile. The loader starts XNU
automatically. The USB interface appears after XNU takes ownership; the host
side ECM MAC is `02:00:5e:77:ee:01` and the phone MAC is
`02:00:5e:77:ee:02`. The listener recognizes either MAC.

Keep the listener running while XNU is up. `logs/munch-live.log` should grow as
kernel console messages arrive. When the timed return or a reboot ends XNU, USB
Ethernet goes away; a ping attempted after that point does not test the
previous session.

For USB enumeration diagnostics, start this in a separate terminal before
booting:

```sh
sudo dmesg -wT | grep --line-buffered -Ei 'usb|xhci|xnuloader|dwc3'
```

## Interactive Fastboot Profile

Use `--usb-debug` instead of `--usb-autoboot` when the loader must stay in its
fastboot command loop:

```sh
python3 devices/poco-f4/loader/pack-iosroot.py \
  --fd Mu-Silicium/Build/munchPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
  --dtb research/munch-live.dtb \
  --kernel xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  --ios-config artifacts/munch-build/ios-config.json \
  --output /tmp/munch-iosroot-usb-debug.img \
  --cpus 8 \
  --usb-debug \
  --extra-cmdline 'loader_return_after=600'
```

Start the host listener as above, then run:

```sh
fastboot boot /tmp/munch-iosroot-usb-debug.img
fastboot oem run
```

This profile requires `fastboot oem run`; after XNU returns, the loader
restarts fastboot. Staged logs can then be retrieved with:

```sh
fastboot getvar result
fastboot get_staged /tmp/munch-usb-staged.log
```
