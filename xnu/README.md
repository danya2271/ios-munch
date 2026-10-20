# XNU inherited SWEET baseline in the Munch fork

This is not yet an XNU port for the POCO F4. The `MUNCH` repository branding
and patch filenames do not change the hardware implementation: patch contents,
staging scripts, and the known-good machine configuration remain `SWEET` for
Redmi Note 10 Pro / SM7150. No MUNCH/SM8250 kernel build or boot is validated.

Built on GitHub's macOS arm64 runners by `.github/workflows/xnu.yml`, using
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `MACOS_VERSION=13.5` -> xnu-8796 (same generation as iOS 16.6/16.7, the last iOS for iPhone 8).
- `patches/*.patch` are copied into the build script's patch dir and applied to the xnu tree with `git apply`.
- Output: `kernel.<config>.sweet` as a workflow artifact when built with the default `SWEET` configuration.

Fetch the latest build: `gh run download -R dipa-studio/ios-munch -n xnu-kernel -D xnu/out`
