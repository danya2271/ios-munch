# Native wired DMA backing for QCA6390

## Scope

`xnu/files/bsd/dev/hakios.dma.memory.c` provides real XNU VM/pmap calls for the
endpoint memory callbacks. It does not register an RC0/SMMU/GIC owner, enable
Bus Master, attach ath11k or change any boot-time Wi-Fi gate. Host models and
ARM64 compilation are not hardware coherency validation.

The exported APIs are:

- `hakios_dma_memory_allocate`: exclusive wired storage and a stable array
  of physical 4-KiB subpages, with a coherent or streaming CPU mapping.
- `hakios_dma_memory_sync`: coherent barriers or streaming cache maintenance.
- `hakios_dma_memory_free`: restore CPU cache attributes before releasing VM.
- `hakios_dma_memory_create_endpoint`: explicitly select this implementation
  for an otherwise fully supplied native endpoint/domain owner.

The last entry takes the existing endpoint operation structure with
`allocate`, `sync` and `free` unset. It rejects supplied memory callbacks,
fills those three with the wired implementation and calls the real endpoint
constructor. `attach`, `fence`, `detach`, table publication and hardware TLB
invalidation remain mandatory platform operations. No missing hardware
operation is replaced with success. The constructor preserves the existing
RID/SID checks, endpoint holds and retained-failure semantics.

## Allocation and CPU aliases

Storage comes from `kernel_memory_allocate(kernel_map, ..., alignment_mask,
KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT)`. Coherent allocations
request next-power-of-two alignment for the native-page-rounded size; aligned
endpoint allocation also checks CPU alignment and reserves an aligned IOVA.
These are dedicated wired VM pages, not
arbitrary kmalloc/SKB pages. The implementation records the full rounded VM
size and releases it with the matching `kmem_free` call. It does not require
physically contiguous memory or convert a CPU physical address into an IOVA.

Native VM pages may be 4 or 16 KiB while the SMMU table granule remains 4 KiB.
The allocator walks each native page with `kvtophys`, validates its alignment
and page-number representation, then expands its physical subpages. Padding
in the last native page stays exclusively owned and zeroed, even if the
endpoint maps only the requested subpages.

Streaming backing keeps the default write-back CPU attributes. Coherent
backing changes each exclusively allocated native page to `VM_WIMG_WCOMB`
through `pmap_set_cache_attributes`, checking the reported attributes after
each call. In the pinned XNU ARM pmap this is Normal non-cacheable memory;
the pmap operation updates the physical aperture and tracked CPU mappings,
performs TLB maintenance and synchronizes the attribute transition. The
allocator does **not** create a second uncached alias with `ml_io_map` while
leaving a cached allocation active. `coherent=true, cacheable=false` selects
the existing endpoint's Normal-NC SMMU leaf attributes, not a cached mapping.

The implementation uses the internal pmap APIs already defined in pinned
XNU 8796.121.2 (`osfmk/arm/pmap/pmap.c`). Their exact prototypes are declared
locally because the common header hides them from BSD compilation. No global
compiler/private-header flags are changed. Their behavior on this port's
actual physical aperture still needs device validation.

## Cache and teardown contracts

Each successful allocation starts zeroed. Coherent synchronization uses
`dsb sy`; streaming synchronization calls `hakios_dma_cache_streaming` over
the entire exclusively owned, native-page-rounded range. This does not flush
adjacent allocations or use the original unaligned SKB as a cache operand.
Unexpected CPU cache attributes or a failed cache operation stop further
synchronization on that backing.

The functions are **sleepable**, not a GFP_ATOMIC/interrupt-context allocator.
Callers must serialize operations on a backing and retain its unique token,
CPU address, count and physical-page array without editing or copying it into
another independent lifetime. The endpoint layer already performs that
serialization. Independent backing allocations use the XNU VM/pmap APIs.

A failure before VM allocation leaves an empty descriptor. A failure after
VM allocation may return a retained non-empty descriptor; it must be passed
to `free`, not discarded. The endpoint constructor/map path already retains
such allocations when its callback fails.

Before free, the caller must establish completed DMA or an actual endpoint
fence. Free is not itself a DMA fence. It stops synchronization, restores
each changed native page to its original CPU attributes, checks readback and
only then releases VM/metadata. Partial restoration failures retain the
allocation and already-completed restoration state for a retry. An unexpected
attribute change on a page this allocator did not change also retains memory
rather than returning it to the VM allocator. Successful free clears the
descriptor; repeating free of an empty descriptor is harmless.

With `hakios_dma_memory_create_endpoint`, the established endpoint teardown
still fences the device, detaches SID/context ownership, destroys the DMA
address space, then releases buffer memory. Table storage remains the domain
owner's responsibility and may also be allocated by this helper; it must not
be freed while SMMU tables or their CPU pointers remain in use.

## What remains

This replaces the missing wired/coherent **buffer implementation**, not proof
of hardware ownership or a complete Linux DMA API. Required work still includes:

1. Verified RC0 handoff and the actual registered native host owner.
2. Exclusive SID `0x1c01` SMMU programming, hardware cache/TLB callbacks and
   an authorized IOVA aperture/doorbell mapping.
3. Global GIC grants, masked attachment, bounded mask/unmask and synchronized
   IRQ teardown.
4. Full LinuxKPI/network integration beyond the bounded DMA/linear-SKB subset
   in `ATOMIC-DMA-RUNTIME.md`. Its arena preallocates this sleepable backing;
   GFP_ATOMIC operations do not call the VM/endpoint allocator directly.
   Correct hardware completion ordering remains a caller/provider requirement.
5. Real provider integration with the explicit runtime composition and validation of firmware boot,
   interrupts, association, traffic and power/recovery transitions.

## Offline checks

The host suite includes the actual memory implementation and real endpoint,
IOVA and page-table engines, using a modeled XNU VM/pmap and domain owner.
It runs twice, with 4-KiB and 16-KiB native pages. Each geometry checks
scatter-backed subpages, padding, coherent/streaming policy, exact frees,
21 failure scenarios, before/after attribute-update effects, retained cleanup
and the explicit wired endpoint constructor with real bounce-copy lifetimes.
The cache instruction helper is modeled here; its ARM64 implementation is
compiled separately. These checks do not exercise physical DMA or prove
that a real SMMU/GIC provider exists. See `PCIE-VALIDATION.md` for artifacts.
