# XNU inherited SWEET baseline in the Munch fork

This is an incomplete XNU port for the POCO F4. The patch series retains the
working `SWEET` donor target and now also defines an isolated `MUNCH` target in
patch 0083. `MUNCH` enables the SM8250 board header, Munch platform/CPU
matching, shared RAM-root/crypto support and boot-CPU-only startup; it does not
enable the donor touch, PMIC, UFS, persistent-log or watchdog drivers. No
MUNCH/SM8250 kernel build or phone boot is validated yet.

Built on GitHub's macOS arm64 runners by `.github/workflows/xnu.yml`, using
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `MACOS_VERSION=13.5` -> xnu-8796 (same generation as iOS 16.6/16.7, the last iOS for iPhone 8).
- `patches/*.patch` are copied into the build script's patch dir and applied to the xnu tree with `git apply`.
- Output: `kernel.<config>.sweet` for `SWEET` or `kernel.<config>.munch` for `MUNCH`.

Run the initial Munch build manually with `machine_config=MUNCH`. Keep the
default `SWEET` build unchanged until the Munch compile has passed on CI.

Fetch the latest build: `gh run download -R dipa-studio/ios-munch -n xnu-kernel -D xnu/out`
