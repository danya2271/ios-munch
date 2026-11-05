# Offline native PCIe validation

These checks compile the canonical sources in `xnu/files`, not stale copies
of the Wi-Fi files in `xnu-src`. They do not access a phone, firmware, PCIe,
GIC, or an SMMU. Host tests are not evidence of working Wi-Fi or a safe
firmware handoff. See `devices/poco-f4/PCIE-VALIDATION.md` for recorded results.

## Host tests

From the repository root:

```sh
bash tools/pcie-test/run.sh
CC=clang bash tools/pcie-test/run.sh
```

Requirements: a C11 compiler with AddressSanitizer/UndefinedBehaviorSanitizer,
POSIX threads and `fork()`. No SDK, kernel build, ath11k download, or ignored
build input is needed. The runner uses a fresh temporary directory and removes
only that directory on exit. The native PCIe service, Kona CFG0/lifecycle
engine, and Kona provider adapter are linked into the test executable.
The shim replaces only kernel allocation, mutexes and basic library headers.
A separate memory-backed test links the read-only QCA SMMU diagnostic and
checks SID matches, translated/bypass routes, extended-SID mode, CB aperture
bounds, abort handling and unmapping.
A second executable links the real `hakios.dma.pagetable.c` implementation
against an independent walker/cache model. It exercises 4 KiB/39-bit geometry,
permissions and output-width limits, scatter-backed 64 MiB mapping, atomic
preflight, map/unmap reuse, sync/TLB failures, stale translations, teardown
retention, and concurrent maps. See `devices/poco-f4/DMA-PAGETABLE.md` for the
implemented scope and the still-missing real DMA owner.

A third executable links `hakios.dma.space.c` with the page-table engine. It
checks IOVA first-fit allocation against an independent 10,000-step model,
alignment/boundary limits, stale/foreign leases, separate IOVA/table capacity,
54 injected map/unmap failures, retained cleanup and four concurrent callers.
See `devices/poco-f4/DMA-SPACE.md` for reservation and backing-page ownership.

A fourth executable compiles the real MSI capability, MHI IPCR transfer-ring
and QRTR wire/control components. It covers 378 MSI and 36 MHI callback
failures, capability restoration, ring wrap/full conditions, completion
validation, concurrent producers and an offline QRTR/IPCR pipeline. It is not
a real MSI allocator, MHI controller or QMI service test. See
`devices/poco-f4/TRANSPORT-CORE.md` for remaining controller/router work.

A fifth executable links the real MHI command/event engines with the transfer
core. It checks credit recycling, bounded polling, ring wrap, stale and
out-of-order completions, backpressure, invalid device pointers, 118 injected
failures, failed/retried teardown, four concurrent submitters/consumers and
dispatch into command/transfer completion accounting. See
`devices/poco-f4/MHI-CONTROL.md` for the owner and partial-progress contracts.

The sixth executable checks BHI/BHIE boot programming, firmware vectors,
context/aperture publication, timeouts and 600 injected failures. The seventh
tests the single-peer WLFW QRTR endpoint, flow control, callback/close lifetime
and a real native QMI request/response using a memory-backed peer. The eighth
runs the existing 59 no-ath11k QMI regressions against the canonical QMI source,
not a stale staged copy. See `devices/poco-f4/MHI-BOOT-QRTR.md` for supported
boot states, endpoint restrictions and missing hardware/worker integration.

The boot executable also checks conservative M0/M3 runtime power and boot-lease
ownership with 30 runtime callback failures. A ninth executable connects the
real IPCR buffer/worker bridge, MHI, QRTR and QMI through the PCI LinuxKPI
header hook. Separate CPU/device copies exercise buffer retention, deferred
delivery/ACKs, backpressure, wrap, shutdown/worker concurrency, stale epochs,
allocation failure and 160 injected callback failures. It does not need an
ath11k extraction. See `devices/poco-f4/IPCR-RUNTIME.md` for remaining owners
and the deliberately unsupported autonomous M1/M2 and warm-reset paths.

Two further executables exercise the real native MHI worker and the actual
ath11k core probe/remove adapter. The worker uses a pthread-backed XNU
thread-call shim with real event/IPCR engines, 56 injected callback failures,
IRQ deferral, delayed retries, coalescing and cross-thread cancellation. The
adapter uses explicitly mocked ath11k/native APIs, with 60 lifecycle failures,
identity/version/epoch/MSI checks, retained fencing and retry. These are not
hardware interrupt tests or execution of the complete ath11k PCI driver.
Together with the existing QMI regression executable, the runner now builds
eleven host executables at that stage. See `devices/poco-f4/MHI-WORKER-PCI.md`.

The endpoint-resource and native HIF tests bring the total to thirteen. They
cover 36 DMA failures, real PTE/IOVA integration with bounce/cache ownership,
five granted-MSI allocation failure sites, endpoint holds, six HIF failures,
native window batches, QCA CE/DP layout, failed-bind retry and power/close
serialization. Actual
hardware providers and ath11k CE/DP execution are not supplied by the mocks.
See `devices/poco-f4/ENDPOINT-DMA-HIF.md` for the remaining platform and
LinuxKPI DMA/SKB requirements. ARM64 cache instructions are compiled only;
the host cache helper returns `ENOTSUP`.

The wired-memory tests bring the total to fifteen executables. The actual
native memory/endpoint/IOVA/table implementation runs with modeled XNU VM/pmap
and a modeled domain, once with 4-KiB native pages and once with 16-KiB pages.
Each geometry exercises 21 failures, including before/after cache-attribute
effects, partial-restoration retention and explicit wired endpoint/bounce
integration. This does not execute ARM cache instructions or real DMA. See
`devices/poco-f4/WIRED-DMA-MEMORY.md` for native APIs and remaining blockers.

The arena, Kona MSI, and common-runtime tests bring the current total to eighteen.
They link the real preallocated DMA/linear-SKB binding and runtime/PCI/HIF
composition with modeled endpoint backing and upstream driver functions.
Coverage includes exact byte-range ownership, adjacent-mapping escape refusal,
coherent alignment/quarantine, pool exhaustion, mapped-SKB/refcount retention,
800 concurrent lifetimes without hot-path allocator/endpoint calls, partial
prepare and foreign-resource rejection, rollback, IRQ/drain/fence failures
and recovery epochs blocked by outstanding references/allocations. The native
memory tests also check aligned endpoint allocations and dependency holds.
See `devices/poco-f4/ATOMIC-DMA-RUNTIME.md`; this does not validate hard IRQ
execution, real ownership grants or working Wi-Fi.

The RAM-backed owner model checks ordering and tracks config traffic pending
completion. It refuses iATU changes before drain, requires exclusive guards,
models native config widths and PCI status write-one-to-clear behavior, and
can fail callbacks before or after a write takes effect. It is not a hardware
emulator or a platform owner implementation.

Coverage includes:

- Missing provider; resource/SID/identity/BAR mismatch; rejecting active Bus
  Master; memory-decode writes that preserve the adjacent PCI status word.
- Config widths, alignment and aperture bounds; initially enabled/disabled
  region snapshots; link down; enable/disable timeout; delay failure;
  translation readback mismatch and invalid narrow read values.
- Failure at every callback on a successful config-read/write path, with
  both write-side-effect possibilities and positive/negative callback errno.
  Recovery never replays a config write or silently clears a faulted context.
- Persistent drain/restoration failure, retained ownership, refused destroy,
  eventual cleanup, native lease cleanup across a Kona failure, and quarantine
  for a provider reporting success without an ownership cookie.
- Stale leases; partial acquisition cleanup; failed release/retry; complete
  BAR-batch prevalidation; failed verification stopping data access; clearing
  partial read outputs; unsupported wake; failed access-end handling.
- Four concurrent callers, separately for Kona config access, native config
  access and write/verify/read BAR batches. Callback yields increase the
  opportunity for interleaving; this is not a formal concurrency proof or a
  substitute for IRQ-context testing. Contract-violation cases run in a child
  process because their quarantined state intentionally cannot be discarded.

## ARM64 object compilation

```sh
bash tools/pcie-test/compile-kernel.sh
```

Requires the local XNU SDK/compiler tools and an existing configured
`DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT` object tree. `XNU_SRC` can select
that source tree. The runner uses `make -n` to obtain its BSD compile flags,
with the same `arm64` override as `xnu-build.sh`, then compiles the canonical
PCIe, Kona, DMA page-table/address-space/endpoint/cache/wired-memory/arena, MSI engine/pool,
MHI transfer/control/boot/runtime/worker, IPCR,
QRTR wire/endpoint, QMI, QCA6390, Wi-Fi
and LinuxKPI files into a separate `xnu-deps/build/pcie-kernel.*` directory.
The original sources, object
files and kernel are not replaced. Compiler argv and per-file logs remain in
the output directory. This check is not a full kernel link and still uses the
configured tree's generated headers and the existing cross-compiler wrapper.

## Isolated full kernel build

```sh
JOBS=4 bash tools/pcie-test/build-kernel.sh
```

Requires a previously prepared XNU source tree, its external build dependencies
and `rsync`. This copies the existing source tree, excluding `.git` and `BUILD`,
into a fresh `xnu-deps/build/pcie-full-kernel.*` directory. Only that copy receives
the canonical QCA Wi-Fi/PCIe/DMA/transport source lists and any missing BSD
manifest entries. All unrelated staged source changes are retained in the
copy, so this is an integration check against that snapshot, not a pristine
release build or a reproducibility claim.

The runner invokes `xnu-build.sh` with `MUNCH_STAGE=0` and an isolated `XNU_SRC`;
no display staging or source synchronization back to the working tree occurs.
It retains `build.log` and returns failure if any build stage fails, even when
the kernel link itself succeeds. The existing build's optional ath11k object
is used as-is; it is not rebuilt or converted from AHB to PCI. No image is
packed, flashed, selected for boot or executed, and no Wi-Fi boot flag changes.

## Pinned ath11k PCI adapter object check

```sh
bash tools/pcie-test/compile-ath11k-pci.sh
```

Requires the previously prepared pinned ath11k/FreeBSD extraction and generated
headers from `tools/mk-ath11k.sh`; those ignored inputs are not supplied by a
fresh checkout. The check does not download or patch them. It compiles the
actual lifecycle adapter and upstream QMI source in PCI/AHB profiles, checks
the expected QMI imports and the adapter's empty AHB profile, and verifies
that the Redwood bus and its AHB DMA/SKB storage refuse PCI mode without
the native binding. It also compiles the native PCI HIF, DMA/SKB binding,
common runtime, explicitly isolated PCI common base, and actual upstream
HAL/CE/DP sources. It checks these objects for legacy physical-DMA imports
and checks that PCI common-base definitions exclude the old DMA/SKB backend.
Commands, release identifiers, objects and diagnostics remain in a new
`xnu-deps/build/ath11k-pci-check.*` directory. This is not a full PCI backend
link and does not replace the ordinary AHB library.
