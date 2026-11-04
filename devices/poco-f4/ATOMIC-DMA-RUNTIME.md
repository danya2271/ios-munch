# QCA6390 preallocated LinuxKPI DMA/SKB and runtime composition

## Scope and safety boundary

`hakios.dma.arena.c`, `wifi/lx/src/lx_qca_dma.c` and
`wifi/lx/src/lx_qca_runtime.c` supply a bounded, non-sleeping DMA/linear-SKB
binding and an explicit composition around the existing PCI adapter and HIF.
They do not implement or register the missing RC0/SMMU/GIC platform owner.
There is no boot-time caller, new Bus Master enable path, or change to Munch's
closed Wi-Fi gates. This is not a complete ath11k PCI library or working Wi-Fi.

The canonical sources and host tests live in the repository. Only the optional
pinned-ath11k object check uses the existing ignored Linux/FreeBSD extraction;
it is not an implicit dependency of the new source or host test runner.
`tools/mk-ath11k.sh` defaults to AHB for the Redwood build, but
`ATH11K_PROFILE=PCI tools/mk-ath11k.sh` now selects the native QCA6390 PCI
entrypoint, DMA/SKB binding and runtime object. The runtime is still not
hardware-enabled: the XNU platform hook returns `ENOTSUP` until RC0/SMMU/GIC/
MHI/IPCR ownership callbacks are registered.

## Sleepable setup, bounded hot path

Arena creation requires a healthy native endpoint and takes a dependency hold.
It preallocates a coherent DMA span, separate CPU-only storage, allocation
records and a per-byte ownership bitmap. Both pools are power-of-two sizes
between 4 KiB and 64 MiB; the owner must budget actual ring and packet demand.
The entire DMA span has aligned CPU and IOVA bases. The native wired allocator
uses `kernel_memory_allocate` with an alignment mask, Normal-NC pmap attributes
and scattered physical pages. This is not an identity mapping or a claim that
hardware coherency has been validated.

Allocation, mapping, range synchronization, unmapping and linear-SKB allocation
use only preallocated storage, spinlocks, copies and barriers. They perform no
VM allocation, sleepable endpoint operation or SMMU programming. Large copies
and zeroing run outside the lock while their records are marked busy. Pool
exhaustion returns an error/NULL/`DMA_MAPPING_ERROR`, with no sleeping fallback.
Both `GFP_ATOMIC` and normal requests use this path.

**Non-sleeping is not arbitrary hard-IRQ safety.** The spinlocks do not save
interrupt state; Linux driver work remains in deferred threads. This change
does not make the rest of LinuxKPI allocation, locks or workqueues atomic-safe.
Scans and copies are bounded by pool size, not constant-time latency guarantees.

## Mapping, ownership and frees

- Coherent allocations are zeroed, aligned to the next power-of-two page span,
  and DMA-visible. Noncoherent allocation uses the same stronger NC backing
  with the requested direction retained for validation.
- Streaming maps use private coherent bounce slots. SKB descriptors/shared
  metadata and original payload storage remain in the CPU-only pool.
- TO_DEVICE copies out; FROM_DEVICE copies in on CPU ownership transfer;
  BIDIRECTIONAL supports both. Unaligned ranges use a byte ownership bitmap,
  so repeated CPU synchronization does not overwrite CPU-owned modifications.
- Range synchronization validates the supplied mapping address and offset
  together. An offset cannot escape into a neighboring valid allocation.
  Overflow, zero size, wrong direction and crossing allocation bounds fail.
- Unmap requires the original address, exact length and direction. It copies
  back only device-owned bytes during healthy operation; faulted/closed cleanup
  deliberately does not deliver potentially invalid DMA data to the CPU.
- Coherent free quarantines the allocation until epoch retirement. Streaming
  slots can be reused after unmap: the caller must already have observed real
  completion. The software cannot prove a device has stopped using an address.
- CPU storage cannot be freed while any streaming mapping references it.
  Repeated coherent allocation/free cycles can exhaust quarantine; a completed
  teardown/recovery epoch, not opportunistic reuse, reclaims the span.

The registry admits one device. DMA APIs require its exact `struct device *`;
device-less SKB APIs use that one registered CPU pool. Registry references keep
the arena alive during wrappers. Close blocks new allocation/map/sync admission
but retains cleanup access. Active calls make close return `EBUSY`. Destroy
requires no references, busy records or live allocations, plus a closed and
fenced endpoint. The DMA span stays endpoint-owned until endpoint destruction.
An endpoint cannot be destroyed while its arena dependency exists.

Constructors may return a non-NULL retained handle on failure. Do not discard
it, retry over it, or release its backing early. Raw native arena callers must
serialize lifetime themselves; the Linux wrappers take registry references.

## LinuxKPI subset

The PCI-only `linux/dma-mapping.h` overlay redirects coherent/noncoherent
allocation/free, single map/unmap, whole/range sync and mask checks. Maximum
mapping size reports the bound arena capacity, not a fixed success value.
SG, page/resource mappings, managed DMA and unsupported map attributes fail
closed. Void operation failures latch an arena fault when a binding exists.
Foreign or absent devices never fall back to physical-address extraction.

Linear `alloc_skb`/`dev_alloc_skb` use the CPU pool, aligned descriptor/shared
info, headroom and refcounts. Last-reference free refuses mapped data,
fragments or external free callbacks and retains the object on error. This is
not fragment, clone/copy, external mbuf, socket-accounting or complete SKB API
support. Existing unsupported LinuxKPI facilities remain outside this subset.

In PCI mode, `lx_base.c` requires `LX_QCA_NATIVE_DMA` and excludes its legacy
radio pool, physical-address DMA, no-op sync/free and old SKB implementation.
AHB behavior is preserved. The explicit define is not a replacement for linking
the new binding into a future audited PCI library.

## Runtime owner contract

The owner must serialize `start`, `status`, `stop` and `recover`, including
pointer lifetime and simultaneous starts. These are sleepable control-plane
operations, never calls from the runtime's own IRQ/worker or fault callback.
Fault notification records the first error and calls the supplied notification
hook; that hook must schedule external work, not synchronously stop/recover.

`lx_qca_runtime_start` requires all versioned platform and HIF callbacks:

1. Initialize common LinuxKPI services. Ticker creation failure remains an
   error and does not mark initialization successful.
2. Platform `prepare` acquires actual host/endpoint, SID `0x1c01` DMA, granted
   MSI routes/doorbell, running MHI/IPCR and deferred-worker resources. It must
   track partial preparation for later close/fence/release. It must not supply
   an arena or HIF belonging to another caller.
3. Create/bind the arena and HIF, then use the existing adapter for actual
   ath11k core allocation, identity/resource checks, HAL/CE setup and QMI bind.
4. Bind HIF before platform IRQ attachment, then invoke core initialization.
   Resource identity includes the arena's endpoint and bound Linux device.

Stop first closes platform and arena admission, masks/drains HIF work, invokes
the mandatory platform fence and closes the endpoint. The existing adapter
validates worker/IPCR/DMA quiescence before driver cleanup. HIF destruction and
arena retirement precede platform resource release. The provider must preserve
the resources needed by cleanup and satisfy the existing adapter fence contract.
Failed close/fence/drain/release or outstanding allocations retain the runtime
handle for retry; they do not free it or start a replacement controller.

Recovery fully stops the previous epoch before asking the provider to prepare
fresh resources. This orchestrates lifetime; it does not implement the actual
hardware reset, clock/PHY sequencing, global grants or autonomous runtime PM.

## Offline verification

The host runner now has seventeen executables under GCC/Clang ASan/UBSan.
New tests execute the actual arena and Linux binding with modeled endpoint
backing: byte ranges, adjacent-mapping escape rejection, refcounts, direction
checks, pool/quarantine exhaustion, retained cleanup, allocation failures and
800 concurrent mapping/SKB lifetimes. Hot operations assert unchanged VM/
endpoint call counts. Wired-memory tests also exercise aligned endpoint
allocation and dependency holds with 4-KiB and 16-KiB VM pages.

The integration test links the actual runtime, PCI adapter, HIF, arena and
DMA/SKB binding, with mocked platform and upstream ath11k functions. It covers
prepare/core/attach failures, all existing adapter lifecycle failure sites,
arena allocation failures, missing/foreign resources, retained fencing,
IRQ/drain/endpoint-close failures, outstanding references/allocations and
recovery epochs. It is not execution of the full upstream driver or hardware.

The object check compiles the PCI binding/runtime/common base and pinned
upstream HAL/CE/DP sources, checking for legacy physical DMA imports. Native
ARM64 compilation and the isolated kernel build are separate checks; see
`PCIE-VALIDATION.md` for their exact artifacts and limits.
