# Native DMA page-table core

## Implemented scope

`xnu/files/bsd/dev/hakios.dma.pagetable.c` and
`xnu/files/bsd/sys/hakios.dma.pagetable.h` implement an ARM64 stage-1 LPAE
page-table engine. It supports a 4 KiB granule, a 39-bit input address space
starting at level 1, and output widths of 32/36/40/42/44/48 bits. Mapping takes
a vector of physical page addresses; physical contiguity is not assumed.

This is one component of a future endpoint DMA implementation, **not** a
complete DMA API or an SMMU hardware owner. It does not select context banks,
claim stream matches, allocate IOVAs, allocate/pin data buffers, synchronize
data-buffer caches, configure registers, or enable PCI Bus Master. No QCA
runtime calls it yet. The former direct QCA SMMU path has been removed; this
component does not configure a replacement hardware owner. RC0, DMA and MSI
gates are unchanged; the eventual domain must still belong to endpoint SID
`0x1c01`.

`hakios.dma.space.c` now builds software IOVA allocation and mapping-lease
accounting around a private instance of this engine; see `DMA-SPACE.md`.
It retains reservations on uncertain translation/teardown failures but still
requires the actual hardware owner, pinned buffers and cache-maintenance
callbacks. Neither component is invoked by the QCA runtime.

## Table storage and creation

The owner supplies 1–512 exclusively owned, pinned 4 KiB table pages, each
with a CPU address and a physical address. XNU's CPU page size need not be
4 KiB: an owner may subdivide a larger allocation only if each supplied
4 KiB physical interval is valid, pinned and independently accounted for.
This module does not derive physical addresses by adding to the first page's
address or infer physical ownership from matching numbers.

`create()` validates all pages before changing any supplied table memory:
alignment, output-address bounds, non-null/nonzero addresses and duplicate
CPU/physical pages. It allocates/copies only metadata, then zeros the table
pages. Failure before that point leaves the supplied pages unchanged. The
operations table is copied, but callback code, owner state and table buffers
must remain alive until successful destruction. No MMIO occurs in creation.

The caller must not supply live firmware tables, an apparently free context's
tables, or memory already used by a walker. Supplied metadata and operation
arguments must remain stable for each call. Physical-page input vectors must
not alias the managed table storage. After creation, only this engine may
write the table descriptors; inspecting or freeing them concurrently is not
supported.

## Descriptor geometry and permissions

Indices are bits `[38:30]`, `[29:21]` and `[20:12]`. Only table descriptors and
4 KiB page descriptors are supported, not section/block mappings. Child
descriptors must identify a known owned table of the correct level; malformed
descriptors fault the engine rather than being followed as CPU pointers.

Leaf permissions are explicit:

- `READ` is mandatory; `WRITE` is optional. Stage-1 AP bits cannot express
  write-only access, so a write-only request fails instead of silently gaining
  read permission. Read-only leaves use AP `11`; read/write uses AP `01`.
- All data mappings are execute-never, access-flagged, non-global and outer
  shareable. The optional nonsecure profile sets both NS and NSTable bits.
- Default memory uses MAIR index 0 (`0x44`, normal noncacheable); `CACHE` uses
  index 1 (`0xff`, normal write-back); `MMIO` uses index 2 (`0x04`, device).
  `CACHE | MMIO` and unknown flags are rejected. The reported MAIR0 is
  `0x0004ff44`; Qualcomm upstream/LLC hints are not implemented.
- Mapping any supplied table page as endpoint data is rejected, including
  currently unused table pages. A data mapping must not expose the endpoint's
  translation tables to the endpoint itself.

`get_info()` reports root PA, MAIR0, input/output widths, granule and starting
level only after successful table synchronization. Outputs are zero on error.
The owner must use matching SMMU geometry and separately establish supported
walker coherency, ASID, TCR/TCR2 settings, stream routing and ownership. The
page-table format does not prove that this profile is correct for the live
Kona SMMU or that its Qualcomm coherency quirks can be ignored.

## Map/unmap and publication

Each map or unmap is limited to 16,384 pages (64 MiB). Map validates the whole
range, all physical pages, existing mappings and the required intermediate
table count before changing descriptors. Alignment/overflow errors, overlaps
and insufficient table capacity do not leave a partly installed range.
Unmap similarly checks the entire range first; a hole does not partially
remove neighboring mappings. Existing mappings cannot be overwritten in
place. Permission changes require explicit unmap, completed invalidation and
remap; there is no implicit break-before-make shortcut.

Intermediate tables are retained until domain destruction. This avoids
reusing a table page while a walker could still reference it. Unmapped leaf
slots can be reused, but spreading mappings over new prefixes can exhaust
the fixed arena even when older leaves are empty. An aligned 64 MiB range in
one 1 GiB prefix needs 34 pages: one root, one level-2 table, and 32 leaves.

Before the first successful `get_info()`, the root is unpublished: table
changes are synchronized, but no TLB callback is needed. The owner must not
attach the root by obtaining its address from the supplied arena instead.
Successful `get_info()` permanently marks it published for lifetime accounting;
this is **not** a readiness or ownership flag. After publication, both map and
unmap require synchronization followed by completed invalidation.

## Mandatory owner callbacks

- `sync(owner, page)` makes all descriptor stores in that page visible to the
  SMMU walker, with correct cache maintenance and store ordering. Dirty pages
  are synchronized children before parents. A coherent walker still needs
  the required ordering; the callback cannot simply assert success without
  implementing the appropriate memory contract.
- `invalidate(owner, iova, size)` must cover changed translations and cached
  walks/negative entries and wait for completion. The owner may invalidate
  the entire owned ASID/context if necessary. It must issue the required
  barriers after table writes. Returning while TLBSYNC is pending is failure,
  not a usable mapping or a reusable IOVA.
- `quiesce(owner)` must stop new endpoint DMA, drain outstanding requests,
  detach every walker from this root, and complete required invalidation
  before table/data storage can be reclaimed. Clearing an SMMU enable bit,
  guessing that Bus Master is off, or a timed-out poll is insufficient. On
  error the owner must retain/account for outstanding resources and support
  a later cleanup retry. No buffers may be freed on a failed teardown.

Callbacks return positive XNU errno; negative errors become `EIO`. They run
under the engine's sleepable mutex and must not reenter it or wait on work
requiring that mutex or upstream owner locks. The owner must coordinate the
endpoint's DMA lifetime, power and domain references outside this mutex.
This is not a hard-IRQ or panic-context interface.

## Failure and lifetime rules

A sync/invalidation failure after descriptor changes poisons the engine. The
changed entries may already be visible; the module deliberately does not
claim an automatic rollback or free any backing storage. Later map/unmap and
root-info calls fail. `status()` is a software-only health query and does not
publish the root or touch hardware. It is needed because a callback can return
the same errno as preflight validation: errno alone cannot distinguish a
rejected request from a partially visible, faulted transaction.

For a faulted published table, the owner retains **all affected data pages,
IOVAs and table pages** until successful quiescence. A failed unmap can leave
an old DMA translation cached even when the CPU and walker-visible PTE are
zero. The tests explicitly model that case. There is no flag-based fault
clear or automatic retry of a mapping operation.

`destroy()` calls quiesce for every published root, including healthy ones.
Failure preserves the handle and metadata for another cleanup attempt.
An unpublished root cannot have been attached under this contract and can be
destroyed without a hardware callback. Successful destruction frees engine
metadata only; the owner then releases its own pinned table/data allocations.
The owner must stop and join all API callers before destruction; the internal
mutex does not protect a pointer concurrently being freed.

## References and verification

Geometry, AP/XN/shareability and MAIR encoding were checked against
`/home/danya2271/xiaomi_sm8250_kernel/drivers/iommu/io-pgtable-arm.c`, notably
its descriptor definitions, `arm_lpae_prot_to_pte()` and
`arm_64_lpae_alloc_pgtable_s1()`. The implementation does not depend on ignored
ath11k output or import Linux allocator/locking code.

`tools/pcie-test/test_dma_pagetable.c` uses an independent memory walker,
separate CPU/walker-visible copies and a stale-translation cache model. It
checks boundaries, all supported output widths, permissions, a scatter-backed
64 MiB range, reuse, preflight atomicity, allocation failures, 24 injected
sync/invalidation failures, failed root publication/quiescence, corrupted
descriptors and four concurrent mapping threads. These are offline model
tests, not evidence of real SMMU coherency or working Wi-Fi.
