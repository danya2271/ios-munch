# Linux XNU build: DriverKit and export validation

The Munch DEVELOPMENT kernel uses the generic `ARM64_BCM2837` configuration.
Build the full target, including symbol sets, rather than stopping at the kernel
link:

```sh
MUNCH_STAGE=1 ./xnu-build.sh -j8 -k
python3 tools/check-xnu-exports.py
```

The kernel is `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
Successful compilation/linking alone is not the success criterion: the complete
command must exit zero. The generated kernel is not an Android boot image.

## DriverKit compatibility generator

The Linux `iig` replacement uses the pre-generated SDK headers and the checked-in
`xnu-deps/iig-impl/OSObject.iig.cpp`. It does not implement user-space DriverKit
RPC. Native wrappers with an existing `_Impl` forward to that implementation;
unsupported RPC dispatch/invoke entries retain the error-returning compatibility
behavior. The presence of these symbols is not DriverKit hardware-driver support.

The generator must include exported generated methods as well as the linker's
undefined references. An exported method can be unused inside the kernel and
still be required by the KPI contract. The typed
`OSAction_IOUserClient_KernelCompletion` also needs its metaclass and vtable.
Unknown C++ classes and Apple IOMMU functions must not become generic stubs.

To regenerate into a temporary directory for review:

```sh
source ./xnu-env.sh
"$(xcrun -sdk iphoneos -find nm)" -n \
  "$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped" \
  > /tmp/xnu-kernel-symbols.txt
python3 xnu-deps/gen-iig-stubs.py \
  xnu-deps/iig-impl/undefined-all.txt /tmp/xnu-iig \
  /tmp/xnu-kernel-symbols.txt --exports-dir "$XNU_SRC/config"
diff -u xnu-deps/iig-impl/OSObject.iig.cpp /tmp/xnu-iig/OSObject.iig.cpp
```

The symbol input is mandatory when collecting exports, so regeneration cannot
silently turn native `_Impl` forwarding into unsupported operations. The patched
DriverKit makefile tracks both the shim and its implementation files as generated
header dependencies; updating the checked-in implementation triggers regeneration
and recompilation without a manual `touch` or stale generated source.

## Generic ARM64 private ABI

`Private.arm64.bcm2837.exports` is selected only for `ARM64_BCM2837`. It retains
the generic private ARM64 API but does not advertise the 35 Apple-only PPL IOMMU,
DART, SART, NVMe-PPL and UAT exports absent from this port. The original Apple
`Private.arm64.exports` remains unchanged and is still selected for other machine
configurations. Public KPI lists remain unchanged.

The generic platform defines `pmap_cs_configuration()` as zero and
`pmap_has_iofilter_protected_write()` as false, following the unsupported-platform
semantics of these capability queries. Zero is not a claim that code-signing/PPL
security is implemented. Qualcomm SMMU support does not provide Apple's private
DART/UAT API; a kext requiring that API must not be treated as supported.

Both `check_all_exports` invocations consume exactly the selected component
lists. They write real `checked-public.symbolset` and `checked-private.symbolset`
files because the symbol-set writer seeks while generating Mach-O data and Linux
`/dev/null` cannot satisfy the expected seek offset. No `-sect`, `-diff`, ignored
exit code, or missing-symbol filtering is used to bypass validation. Duplicate
exports are still checked during output generation.

The regression script tests generation/forwarding, both machine selections,
reproducibility against the linked kernel, patch application to the original XNU
sources, and the real validator's rejection of missing and duplicate exports.

## SDE status

On October 28, 2026 the full DEVELOPMENT build and export regressions passed with
the integrated L11r SDE presenter. This is host/build validation, not a device
boot or proof of safe presentation. The backend remains opt-in via
`devices/poco-f4/loader/pack-iosroot.py --sde-backend`; see
`devices/poco-f4/display/README.md` for runtime qualification and rollback rules.
