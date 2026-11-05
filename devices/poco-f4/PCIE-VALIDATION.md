# Offline Wi-Fi/PCIe validation — October 29, 2026

The user authorized builds and local tests. Device changes, flashing and
hardware tests remain outside that authorization. None were performed.

## Results

| Check | Result | Scope |
| --- | --- | --- |
| GCC host tests, ASan + UBSan | PASS | Real PCIe, Kona, DMA, MSI, MHI rings/boot, QRTR codec/endpoint and QMI C code against memory-backed models |
| Clang host tests, ASan + UBSan | PASS | Same sources and scenarios under a second compiler |
| PCIe callback failure sweep | PASS, 376 cases per run | Read/write paths, both initial iATU enable states, failures before/after write effects |
| Additional lifecycle/bounds/polling/BAR/concurrency cases | PASS | Includes retained cleanup through native and Kona layers |
| DMA table geometry/attributes/preflight | PASS | 4 KiB/39-bit profile, six PA widths, scatter-backed 64 MiB mapping, table-page protection and no partial changes on rejected ranges |
| DMA callback failure sweep | PASS, 24 cases per run | 18 published map/unmap failures and six unpublished sync failures, before/after effects |
| DMA lifecycle/cache/concurrency | PASS | Failed publication/quiescence, stale translations, retained state and 800 map/unmap operations across four threads |
| DMA address-space allocation/lifetime | PASS | Alignment, segment boundaries, first-fit reuse, 4096-reservation cap, stale/foreign/recreated leases and separate table exhaustion |
| DMA address-space fault sweep | PASS, 54 cases per run | Published map/unmap failures with three errno values before/after callback effects; leases cannot be released or reused |
| DMA address-space model/concurrency | PASS | 10,000 model-checked operations and 400 complete mapping lifetimes across four threads; no overlapping active ranges |
| DMA address-space cleanup/accounting | PASS | Failed publication, unpublished mapping failure, stale TLB, failed/retried destruction and unexpected/missing PTE quarantine |
| Endpoint DMA backing/ownership | PASS, 36 callback failures | Real page-table/IOVA engines with modeled pinned pages and SMMU owner; bounce copies, zeroed padding, permissions/cache attributes, false-coherent rejection and retained cleanup |
| Endpoint dependency holds | PASS | DMA/MSI holds prevent early PCI release; stale/unbalanced calls rejected and faulted leases can drop holds for teardown |
| Native wired/coherent memory | PASS for 4-KiB and 16-KiB VM pages | Actual memory implementation with modeled VM/pmap; scatter subpages, zeroed padding, NC attributes, exact frees and explicit integration with real endpoint/IOVA/table engines |
| Wired-memory fault retention | PASS, 21 scenarios per geometry | Metadata/VM allocation, PA/attribute checks, before/after attribute-update effects, partial restoration, cache errors and foreign attribute-change quarantine |

## Cold-owner lifecycle scaffold — November 4, 2026

| Check | Result | Scope |
| --- | --- | --- |
| GCC host PCIe suite | PASS | 376 existing PCIe callback fault cases plus cold-owner acquire/release, partial-stage rollback, failed cleanup/retry and ownership-revoke recovery; memory-backed model only |
| Native ARM64 object compilation | PASS, 22 files | Kona cold-owner API and dependent PCIe/DMA/MSI/MHI/QCA translation units; no kernel image or device boot |
| Provider/hardware activation | NOT IMPLEMENTED | No trusted resident-loader ownership grant or real Kona cold-init backend exists; QCA preflight remains closed |

The cold-owner coordinator in `hakios.pcie.kona.c` serializes the Linux-
referenced RC0 enable stages and retries reverse-order cleanup while retaining
ownership. Its callbacks are an explicit platform boundary, not a hardware
implementation. Stage callbacks must provide rollback cookies and retryable
cleanup; guessed resource writes are intentionally absent. The host suite
validates lifecycle bookkeeping, not clocks, regulators, PHY training, SID
hardware, link behavior, or safe MMIO fault recovery.

## Kona provider adapter — November 4, 2026

| Check | Result | Scope |
| --- | --- | --- |
| GCC and Clang host PCIe suites | PASS | Native RC0 provider adapter, exact QCA endpoint profile, host lease retained across endpoint release, explicit deactivate, rollback and cleanup failures |
| ARM64 object compilation | PASS | `hakios.pcie.kona.platform.c` plus the PCIe/DMA/MSI/MHI/QCA dependency set |
| Full XNU build and symbol-set generation | PASS | New Kona adapter linked into the complete kernel; required the existing stage report to safely refresh stale XNU overlay copies |
| MMIO abort containment | PASS, compile/link scope | Only dedicated synchronous external-abort load/store recovery entries return `EFAULT`; no hardware abort injection or device access was attempted |

The provider adapter composes the cold-owner and CFG0 engines but has no
hardware callback implementation or boot-time caller. It does not change
`lx_os_qca_platform()` and does not clear any preflight bit.

## Scoped ARM64 MMIO recovery — November 5, 2026

| Check | Result | Scope |
| --- | --- | --- |
| Recovery policy | PASS, source inspection | Exact recovery-table target, synchronous external-abort FSC only, rejects S1 page-table-walk aborts and interrupt-context recovery |
| ARM64 MMIO assembly compile | PASS | `machine_routines_asm.o` contains read/write helpers for 8, 16, and 32 bits plus the abort target |
| Current isolated full kernel build | BLOCKED | Stops at the independently modified `hakios.cpufreq.c:223` (`thread_terminate` undeclared), before kernel link; no image produced |
| Runtime/device fault injection | NOT RUN | No exclusive RC0 owner is registered; no real MMIO instruction was exercised |

The 8-, 16-, and 32-bit `hakios_mmio_read*()` and `hakios_mmio_write*()`
accessors return `EFAULT` for a matching synchronous external abort. A write
error is ambiguous because the endpoint may have accepted the posted write;
owner code must quarantine the transaction. The fixup does not handle
asynchronous SError or prove controller ownership, powered state, or firmware
exclusion.

## Read-only QCA SMMU snapshot — November 5, 2026

| Check | Result | Scope |
| --- | --- | --- |
| GCC/Clang host model, ASan + UBSan | PASS | SID/context selection, translated/bypass routes, extended SID, abort cleanup, RC0 snapshot and guarded CFG0 admission |
| ARM64 object compilation | PASS, 27 units | Includes `hakios.qca6390.diag.c` and the `wifi_qca_status=1` call site |
| Device snapshot | PASS | `logs/qca-smmu-r58.log`; SID `0x1c01` absent, no RC0 snapshot in this pre-RC0 diagnostic image |
| RC0 diagnostic device snapshot | PENDING | Status image records link/iATU/PARF/SID/GPIO/clock/reset state without programming hardware |

The diagnostic reports the SMMU ID registers and global fault state, matching
SMR/S2CR entries for RID `0x100` / SID `0x1c01`, and referenced translation
contexts. It uses the synchronous-abort-safe MMIO reader, checks IDR geometry
against the 1 MiB DT aperture before reading a context, and unmaps its view on
every mapped path. It does not acknowledge faults, program SMMU registers or
clear a QCA readiness gate. The RC0 snapshot reads an existing CFG0 aperture
only when its live iATU registers already match the QCA configuration window.
Host tests use a memory-backed register window and do not establish ownership
or live hardware behavior.
The RC0 mock verifies that a nonmatching iATU window skips CFG0 entirely.

## Resource-backed Kona MMIO/config/BAR backend — November 5, 2026

| Check | Result | Scope |
| --- | --- | --- |
| Host model | PASS, GCC and Clang | Exact resource mapping, ownership token/guard lifecycle, native-width CFG0, iATU restore, RID/SID CRC8 programming and full table restoration, lazy BAR mapping, wake rejection/callback and map-failure rollback |
| ARM64 object compilation | PASS, 25 files | Includes the Kona mapped backend, SID stage, platform context split, PCIe/DMA/MSI/MHI/QMI/QCA and LinuxKPI units |
| Runtime/device validation | NOT RUN | The backend has no production cold-owner caller; the token is not an ownership proof |

`hakios.pcie.kona.mmio.c` supplies the config and BAR callbacks and guarded
accesses to DBI/iATU/ELBI/PARF/PHY. `hakios.pcie.kona.sid.c` snapshots and
programs the exact QCA requester mapping with rollback. Host tests run the real
CFG0 coordinator and SID stage against memory-backed windows. They do not test
electrical link state, actual abort delivery, CFG completion timing, firmware
exclusion, an SMMU context, or hardware resource sequencing.
`lx_os_qca_platform()` remains fail-closed; DMA/MSI ownership and MHI runtime
are still not registered.
| Platform-granted MSI pool | PASS, five callback failure sites | Aligned 32-vector banks, authoritative route/payload checks, endpoint doorbell mapping, exhaustion and failed-cleanup retention; not a global hardware GIC allocator |
| MSI callback failure sweep | PASS, 378 cases per run | Constructor/start/teardown, maskable and non-maskable capabilities, three errno values before/after effects |
| MSI programming/lifetime | PASS | All 32 vectors, no hardcoded payload base, rejected active/unsupported capabilities, ignored writes and exact restoration |
| MHI callback failure sweep | PASS, 36 cases per run | Queue/completion failures, retained handles and failed/retried quiescence |
| MHI ring accounting | PASS | 63-transfer capacity, wraparound, batched completion/overflow, invalid and duplicate events, four concurrent producers |
| MHI command/event fault cases | PASS, 118 cases per run | Before/after callback effects, negative errno, partial-progress retention, non-dispatch backpressure errors and retryable quiescence |
| MHI command/event accounting | PASS | Packed contexts, ordered command completions, bounded event snapshots, full/wrapped rings, credit recycling, invalid producer pointers and four concurrent submitters/consumers |
| MHI event dispatch integration | PASS | Real command and batched transfer completion paths, insufficient output capacity without early credit return, duplicate-event rejection |
| MHI firmware/boot fault sweep | PASS, 600 cases per run | Every callback on the boot/teardown path, before/after effects, positive/negative errno and retryable quiescence |
| MHI boot states and image publication | PASS | Synthetic 1-byte, multi-segment and 4,364,432-byte images; vector bytes, contexts/apertures, high-before-low ordering, readback, stale BHIE sequence, early AMSS, timeouts and retained failures |
| QRTR wire/control and IPCR model | PASS | v1/v3, payload sizes 1–8160, malformed packets, server instance packing, TX/RX visibility and RESUME_TX generation |
| QRTR WLFW endpoint and native QMI | PASS | HELLO retry, cached/service removal callbacks, five/ten flow window, delayed ACK, disconnect, callback/close serialization and an encoded request/decoded response against a memory-backed peer |
| IPCR bridge / PCI QMI integration | PASS, 160 callback failures | Separate CPU/device buffers, TX retention, bounded/deferred RX, ACK backpressure, lookup scheduling, stale epochs, wrap, reentrant sends, stop/worker concurrency and retained shutdown |
| Conservative MHI runtime PM | PASS, 30 callback failures | Exclusive boot handoff, M0/M3 request/observation order, no suspended MMIO, wake references, deadlines, faults and failed-epoch retirement |
| Native MHI IRQ/thread-call worker | PASS, 56 callback failures | Real event/IPCR engines, deferred IRQ processing, budgeted retries, 10,000 coalesced kicks, cancellation/join, allocation failures and retained destruction |
| ath11k PCI lifecycle adapter | PASS, 60 injected failures | Actual adapter with mocked ath11k/native owner; DMA/MSI resource validation, HIF close before fence, partial initialization, QMI_FAIL removal, retained rollback and no double free |
| Native PCI HIF / CE / DP boundary | PASS, six fault cases plus lifecycle scenarios | Native BAR/window batches, MSI/service mapping, mocked CE/DP dispatch, failed-bind retry without stale vectors and power-up/close serialization |
| Kona MSI provider | PASS, 32-vector lifecycle and partial rollback | GIC bank lease, masked route attachment, endpoint doorbell map, PCI MSI capability restore and release retry; no live GIC/RC0 access |
| Atomic/range DMA and linear SKB | PASS | Actual arena and Linux binding, exact byte ownership, adjacent-mapping bounds, alignment/quarantine, pool exhaustion, refcounts, allocation failures and 800 concurrent lifetimes; modeled endpoint backing |
| Runtime composition | PASS | Actual runtime/PCI/HIF/arena/binding with mocked platform and ath11k; partial prepare, foreign resources, IRQ/drain/fence/release failure retention and epoch recovery |
| Actual adapter / HIF / DMA / runtime / pinned ath11k ARM64 objects | PASS | Compiles the binding, isolated PCI common base and upstream HAL/CE/DP against real pinned headers; not a full PCI driver link or execution of upstream CE/DP |
| Legacy AHB isolation | PASS | AHB DMA/SKB base still compiles; PCI rejects the legacy bus and requires native-DMA isolation; inspected PCI HAL/CE/DP objects have no legacy physical-DMA imports |
| Actual pinned ath11k QMI ARM64 object | PASS for PCI and AHB | PCI imports the explicit IPCR initializer/native request; AHB retains its original initializer/mode hook; not a full PCI driver link |
| Legacy QMI regression suite | PASS, 59 checks | Canonical QMI source, no optional ath11k tables; legacy transport API remains functional and default transport is not changed by the new endpoint |
| Clang static analyzer with host shim | No diagnostics | Earlier checks plus wired memory under both VM-page geometries; not an ARM/hardware verification |
| ARM64 static analysis, new DMA/runtime work | No changed-source diagnostics | Native endpoint/memory/arena clean; four reports of the same pre-existing dead store in imported FreeBSD `linux/pci.h:1159` for PCI translation units; not an entirely warning-free analysis |
| ARM64 object compilation | PASS, 26 units | Includes `hakios.msi.kona.c` and capability-only PCI config writes; isolated objects against configured XNU headers |
| Isolated full kernel compilation/link | PASS | PCIe/DMA/transport/QCA symbols present in the resulting unstripped kernel |
| Overall full-build command | PASS for the latest snapshot | Command exits zero; public/private and IOKit/Libkern symbol-set artifacts generated |
| Device boot, firmware boot, association, traffic, suspend/resume | NOT RUN | No working Wi-Fi claim |

The eighteen host executables use `-Wall -Wextra -Werror`, AddressSanitizer and
UndefinedBehaviorSanitizer. They cover error-path ownership and serialization,
not real MMIO faults, bus transactions, memory ordering on ARM, DMA coherency,
MSI delivery or controller power sequencing. Four-thread tests exercise 400
transactions each for direct Kona config, native config and atomic BAR batches.
The DMA suite separates CPU and walker-visible table memory and models a cached
translation surviving a failed invalidation. It also injects allocation
failures and checks software health without publishing the root. None of this
validates real SMMU registers, endpoint SID routing or data-buffer coherency.
The test owners use only ordinary process memory; they grant no real resources.

The independent table engine is documented in `DMA-PAGETABLE.md`. It is not
called by the QCA runtime, does not validate the old SMMU groundwork and does
not clear the DMA preflight gate. `DMA-SPACE.md` describes the new IOVA and
mapping-lease layer around that engine. It likewise has no QCA runtime caller,
hardware context or pinning provider. A failed map retains
its previously reserved lease; callers must retain the affected backing pages.
`ENDPOINT-DMA-HIF.md` describes the endpoint/bounce layer and ARM64 cache helper
above it. The cache instructions are compiled, not exercised with real DMA.
SMMU attachment remains a mandatory owner callback. `WIRED-DMA-MEMORY.md`
adds actual XNU VM/pmap buffer allocation and an explicit endpoint constructor;
its VM/pmap and domain dependencies are modeled in the host tests, not run
against a device. `hakios.msi.kona.c` now reserves the XNU GIC bank, but its
doorbell map is still supplied by an external endpoint owner and it has no
boot-time registration caller.

`TRANSPORT-CORE.md` describes the MSI capability engine, IPCR transfer-ring
core and QRTR codec. The new test connects these software components using
ordinary memory and simulated callbacks; it does not deliver a real GIC IRQ,
boot MHI firmware or register a QRTR/QMI service. The existing LinuxKPI MHI
fail-closed implementation and all QCA runtime gates remain unchanged.
`MHI-CONTROL.md` documents the command/event engines and their owner callbacks.
Their separate test dispatches real command and transfer completions through
memory-backed event rings; state/EE delivery is not a controller state-machine
or firmware execution test.
The boot model drives the real register-programming/state code but does not
run firmware instructions. The new endpoint model performs real native QMI
encoding/transaction completion against a synthetic peer, not WLFW firmware.
See `MHI-BOOT-QRTR.md` and `IPCR-RUNTIME.md` for the missing DMA/IRQ/worker owner
integration, the conservative M0/M3 policy and unsupported autonomous M1/M2,
warm reset and RDDM collection paths.

## Compilation correction

The first ARM64 compile found `VM_KERN_MEMORY_IO` in the legacy QCA SMMU
groundwork, which is not a valid allocation tag in this XNU tree. It now uses
`VM_KERN_MEMORY_IOKIT`, matching adjacent device allocations. This fixes the
compilation error only. It does not validate page-table geometry, attributes,
cache maintenance, rollback, teardown, or SMMU ownership, and the independent
DMA preflight requirement remains unsatisfied.

The initial object-check runner also inherited the generated Makefile's
`arm64e` default, causing stock-Clang pointer-authentication qualifier errors.
The runner now explicitly passes `ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64`,
as the repository's `xnu-build.sh` already does. No production pointer-
authentication header or unrelated compiler setting was weakened to hide it.

## Latest atomic DMA/runtime artifacts

These are generated ignored outputs, not fresh-checkout dependencies:

- Current 26-unit native ARM64 object compile, including the Kona MSI provider:
  `xnu-deps/build/pcie-kernel.FNimY6bA/`.
- Twenty-two native ARM64 objects and compiler commands:
  `xnu-deps/build/pcie-kernel.rvgeyjg8/`.
- Actual pinned-header PCI DMA/SKB/runtime/base/HAL/CE/DP object check:
  `xnu-deps/build/ath11k-pci-check.5m6P25xD/`.
- Fresh isolated complete XNU build, command exits zero:
  `xnu-deps/build/pcie-full-kernel.qnTmajeB/build.log`.
- GCC/Clang ASan/UBSan logs for eighteen executables, ARM64 analysis with
  the imported-header warning retained, source hashes, canonical-overlay
  verification, patch parsing and nineteen linked arena/alignment/hold APIs:
  `xnu-deps/build/atomic-dma-runtime-analysis.iaPaubeX/`.

The prior full-kernel artifact predates the Kona MSI provider and capability
write API. The current 26-unit compile covers those sources as isolated
objects, but does not link a new full kernel. The nineteen APIs are present in
that earlier unstripped kernel. This full
build still uses the existing AHB ath11k object: it does not link the new PCI
runtime/binding into a complete PCI library or give it a boot-time caller.
No kernel image was packed, flashed, booted or approved for device testing.

## Earlier wired-memory artifacts

All paths are generated ignored outputs, not source dependencies:

- Twenty-one-source ARM64 compilation:
  `xnu-deps/build/pcie-kernel.wrMc19uA/`.
- Fresh isolated full kernel build, command exits zero:
  `xnu-deps/build/pcie-full-kernel.nMSbcn5X/build.log`.
- Linked kernel, **not approved for device use**:
  `xnu-deps/build/pcie-full-kernel.nMSbcn5X/src/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
- Final GCC/Clang ASan/UBSan logs for fifteen executables, empty static-analyzer
  diagnostics for both VM-page geometries, source hashes, overlay comparisons,
  patch statistics and the four linked native-memory APIs:
  `xnu-deps/build/wired-memory-analysis.9ro4PkUK/`.

All twenty-one overlaid C files and nineteen headers match the canonical
files. The native memory provider is linked, but has no runtime caller. The
kernel still uses the existing AHB ath11k object; the previous separate PCI
adapter/HIF object check is not a complete PCI library link. No hardware
ownership, real DMA/cache behavior or working Wi-Fi is inferred from this
successful build. The initial memory-object check exposed pmap declarations
hidden from BSD headers; the implementation now declares the exact pinned
internal APIs locally, without weakening compiler flags or changing headers.

## Previous endpoint DMA / MSI pool / PCI HIF artifacts

All paths are generated ignored outputs, not source dependencies:

- Twenty-source ARM64 compilation:
  `xnu-deps/build/pcie-kernel.sm0tUh8o/`.
- Fresh isolated full kernel build, command exits zero:
  `xnu-deps/build/pcie-full-kernel.YKQsG2pP/build.log`.
- Linked kernel, **not approved for device use**:
  `xnu-deps/build/pcie-full-kernel.YKQsG2pP/src/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
- GCC/Clang ASan/UBSan logs for thirteen host executables, empty static-analyzer
  diagnostics, linked-symbol inventory and parsed patch statistics:
  `xnu-deps/build/endpoint-analysis.qxYnPu3h/`.
- Actual PCI adapter/HIF, PCI/AHB QMI, legacy AHB DMA/SKB-base objects,
  compiler commands, release identifiers, imports and PCI isolation checks:
  `xnu-deps/build/ath11k-pci-check.gFkVbLHu/`.

All twenty overlaid C files and eighteen headers matched the canonical files
at that checkpoint.
The linked kernel contains the seventeen new endpoint-DMA/cache, MSI-pool and
PCI dependency-hold APIs. The kernel still links the existing **AHB** ath11k
object. Native PCI adapter/HIF objects are checked separately and have no
production caller; neither these objects nor the full kernel prove a complete
PCI driver link. Hardware SMMU/GIC ownership, LinuxKPI per-device DMA/SKB
binding and controller orchestration remain absent. See `ENDPOINT-DMA-HIF.md`
for completion/quarantine contracts and the remaining runtime blockers.

## Previous worker / PCI adapter artifacts

All paths are generated ignored outputs, not source dependencies:

- Seventeen-source ARM64 compilation:
  `xnu-deps/build/pcie-kernel.AtXxAvK2/`.
- Fresh isolated full kernel build, command exits zero:
  `xnu-deps/build/pcie-full-kernel.kPSob2pe/build.log`.
- Linked kernel, **not approved for device use**:
  `xnu-deps/build/pcie-full-kernel.kPSob2pe/src/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
- GCC/Clang ASan/UBSan logs for eleven host executables, empty static-analyzer
  diagnostics, linked-symbol inventory and parsed patch statistics:
  `xnu-deps/build/worker-pci-analysis.1lUsp6Bz/`.
- Actual adapter and pinned PCI/AHB QMI objects, commands, release identifiers,
  imports and AHB-as-PCI rejection:
  `xnu-deps/build/ath11k-pci-check.SGZ9E5ew/`.

All seventeen overlaid C files and sixteen headers matched the canonical files
at that checkpoint.
All seven worker APIs and the endpoint-resource snapshot API are present in
the linked kernel. This kernel still uses the existing AHB ath11k object;
the conditional PCI adapter has no production caller and is only compiled
separately. These checks do not implement the missing RC0/DMA/GIC/HIF owner or
exercise firmware on hardware. `MHI-WORKER-PCI.md` records the contracts and
the remaining full-PCI integration blockers.

## Previous IPCR/runtime artifacts

All paths are generated ignored outputs, not source dependencies:

- Final sixteen-source ARM64 compilation:
  `xnu-deps/build/pcie-kernel.9aQD0ygw/`.
- Fresh isolated full build, command exits zero:
  `xnu-deps/build/pcie-full-kernel.qt2PjO9h/build.log`.
- Linked kernel, **not approved for device use**:
  `xnu-deps/build/pcie-full-kernel.qt2PjO9h/src/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
- GCC/Clang ASan/UBSan host logs, empty static-analyzer diagnostics, symbol
  inventory and parsed patch statistics:
  `xnu-deps/build/ipcr-runtime-analysis.iuANGrpf/`.
- Isolated compilation of the actual pinned ath11k `qmi.c` for both profiles,
  compiler commands, import lists and the AHB-as-PCI rejection check:
  `xnu-deps/build/ipcr-ath11k-qmi.1QsnJMaO/`.

All sixteen overlaid C files and fifteen headers match the canonical files.
The unstripped kernel contains the thirteen IPCR APIs, ten runtime-PM APIs
and both boot-lease APIs. The ordinary AHB ath11k object is not replaced by
either QMI-only object; neither output is a complete QCA6390 PCI driver.
Host tests do not depend on the ignored Linux extraction. It is needed only
for the additional real-driver object check and is fetched by the existing
pinned ath11k build workflow.

## Earlier local artifacts

These are generated, ignored outputs, not inputs required in a fresh checkout:

- Successful object compilation and per-file logs:
  `xnu-deps/build/pcie-kernel.J4bEzCuR/`.
- Isolated integration-build log:
  `xnu-deps/build/pcie-full-kernel.ZFQeYTFW/build.log`.
- Final incremental integration log in the same isolated tree:
  `xnu-deps/build/pcie-full-kernel.ZFQeYTFW/rebuild-final.log`.
- Linked kernel, **not approved for boot/device testing**:
  `xnu-deps/build/pcie-full-kernel.ZFQeYTFW/src/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
- MHI command/event static analysis, no diagnostics:
  `xnu-deps/build/mhi-control-analysis.91sj4vdy/analysis.log`.
- Final GCC/Clang logs and boot/QRTR endpoint static analysis:
  `xnu-deps/build/mhi-boot-qrtr-analysis.LANp6RHe/`.

That earlier isolated full build and its incremental integration builds
all exit zero, including kernel link, strip and export checks. The final
fourteen overlaid sources and thirteen headers matched their canonical files
at that checkpoint; later source counts are recorded with their own artifacts.
`llvm-nm` confirms four MSI, twenty-four MHI, thirteen QRTR and the new
`hakios_qmi_handle_init_endpoint` API symbol in the `.unstripped` kernel alongside
the earlier PCIe/DMA/QCA code. The isolated output also contains
`checked-public.symbolset`, `checked-private.symbolset`,
`IOKit.symbolset`, `Libkern.symbolset` and `Private.symbolset`.

An earlier snapshot in `xnu-deps/build/pcie-full-kernel.g3DkiT1z/` linked but
failed export checks with `exported name not in import list` diagnostics.
That result is superseded by the latest integration snapshot, not reclassified
as a pass. The runner copies the current staged tree, including unrelated
concurrent changes; success is not attributed to the DMA core fixing IOKit.
This Wi-Fi work does not disable export checks or change unrelated IOKit code.
A linked file alone still does not constitute a successful complete build.

The original dirty kernel tree and user changes were not overwritten. For
repeatable commands and test coverage, see `tools/pcie-test/README.md`.

## Runtime gate remains closed

There is still no registered real RC0 ownership provider. The new narrowly
scoped MMIO abort fixup is not wired into a Kona resource owner or config
completion/drain implementation. Integration of the native memory provider
with SMMU ownership for SID `0x1c01`, global GIC route grants/masked attachment,
LinuxKPI/network coverage beyond the new bounded DMA/linear-SKB subset,
the real MHI channel/IRQ/scheduler owner, platform power/reset recovery and full
ath11k PCI/WLFW integration remain required. The endpoint resource layers,
PCI HIF, buffer bridge, conservative PM core and PCI-QMI hook do not provide
those platform grants or a running controller. The new runtime composition
requires mandatory providers and has no boot-time caller. The native
service still refuses acquisition without an owner. These local checks do not
authorize `wifi=1`, `wifi_qca_probe=1`, enabling Bus Master or using the legacy
AHB backend on Munch.
