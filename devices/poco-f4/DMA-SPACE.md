# Native DMA address-space manager

## Implemented scope

`xnu/files/bsd/dev/hakios.dma.space.c` manages IOVA reservations and their
mapping lifetimes on top of the real `hakios.dma.pagetable.c` engine. It owns
its page-table instance privately, so callers cannot bypass the reservation
accounting through a raw page-table handle. Both components are compiled into
the kernel; neither is attached to the QCA runtime.

This is not an endpoint DMA owner, buffer allocator or LinuxKPI DMA API. It
does not allocate/pin physical data or table pages, manage data-cache ownership,
program SMMU registers, stop endpoint requests, route SID `0x1c01`, enable Bus
Master or provide a firmware handoff. Those remain independent requirements.
The old QCA SMMU groundwork remains gated off and is not used by this manager.

## Aperture and allocation

Creation requires an explicit, exclusively owned IOVA interval
`[iova_base, iova_limit)`, with page-aligned endpoints inside the page-table
engine's 39-bit input range. The first page is excluded: `iova_base` must be at
least 4096. There is no default aperture and no hardcoded identity mapping.
The owner must establish that the entire supplied interval is available to
this domain, with no firmware mappings, other allocator or reserved holes.
This interface does not support importing another owner's mappings.

Reservations use a sorted first-fit allocator, at most 4096 live reservations
per space. Each reservation is 1–16,384 pages (up to 64 MiB); alignment is a
power of two from 4 KiB through 512 GiB. A zero boundary disables segment
boundary restrictions; a nonzero boundary is a power of two in the same range,
and the entire reserved interval must fit inside one such segment. Alignment,
boundary and length are checked before allocation, without address wraparound.
All values are bytes except the explicit page counts.

Successful release makes the whole reservation reusable. Neighboring free
intervals coalesce implicitly because only live reservations are stored.
Intermediate page-table pages remain allocated under the lower-level engine's
contract: freeing an IOVA does not reclaim empty intermediate tables. IOVA
capacity and table-page capacity are distinct, and either may return `ENOSPC`.
This bounded linear allocator is a correctness-first implementation, not a
claim of packet-path performance.

## Lease protocol

The public API is `xnu/files/bsd/sys/hakios.dma.space.h`:

1. `reserve()` claims an interval and returns a nonzero lease, not a DMA
   address. No PTEs or hardware state are changed. Leases use a globally
   serialized monotonic generation across all spaces; released, foreign and
   previous-instance leases cannot refer to new reservations. Generation
   exhaustion returns `EOVERFLOW`, never wraps or reuses a token.
2. The caller records its pinned backing pages against the lease **before**
   calling `map()`. The physical-page vector must have exactly the reserved
   page count and remain stable for the call. Its contents are consumed by the
   page-table engine, not retained as an array pointer by this manager.
3. `map()` installs the PTEs and returns the allocated IOVA only after the
   lower layer succeeds, including any required table synchronization and TLB
   completion. Output IOVA is zero on every error. A second map of a mapped
   reservation returns `EBUSY`; permissions cannot be changed in place.
4. After the caller stops/drains all uses of that mapping, `unmap()` removes
   it and completes required invalidation. The reservation is retained; the
   caller may map new backing pages into the same lease or release it.
5. `release()` accepts only an unmapped reservation in a healthy space. It
   clears the caller's lease on success and otherwise preserves it. Releasing
   a live mapping returns `EBUSY`; stale or foreign leases return `ESTALE`.

Unmap is **not** an endpoint stop operation. A descriptor still containing an
old IOVA could issue a new request after that IOVA has been reused. The owner
must stop/drain producers, handle completion and perform data-buffer cache
transitions before recycling pages or addresses. Table invalidation alone
does not establish those conditions. Physical aliases between separate
leases also remain the caller's pinning/cache-ownership responsibility.

An address returned by `map()` describes a successfully installed translation,
not an attached or running DMA domain. `get_info()` publishes root geometry
under the page-table lifetime contract; it grants no hardware ownership and
does not satisfy the QCA DMA preflight requirement.

## Faults and teardown

Reserving before mapping gives the caller an already-known lease even when
mapping fails after partially visible descriptor writes. An ordinary preflight
failure leaves the reservation unmapped and the space healthy, so it can be
retried or released. A callback can return `EINVAL` too: the manager checks
the lower engine's health instead of interpreting errno as proof of rollback.
An unexpected existing PTE in an unmapped lease, or a missing PTE in a mapped
lease, also quarantines the entire space: disagreement between page tables
and reservation accounting must not allow a range to be recycled.

On a table synchronization/invalidation fault, the **entire space** becomes
faulted. All reservations, including previously unused ones, remain retained;
valid reserve/map/unmap/release/root-publication calls fail without further
mapping callbacks. The caller must retain all possibly affected backing pages,
including pages supplied to a failed map. Clearing the output IOVA does not
mean that those pages were never exposed. `status()` is a software-only health
query, not a recovery or readiness operation.

`destroy()` delegates to the table engine's mandatory quiesce callback for a
published root. The callback must stop/drain DMA, detach all walkers and
complete invalidation. If it fails, the space, table, leases and IOVA intervals
are retained and normal operations remain blocked; destruction may be retried.
A failure after the owner already detached hardware still retains software
state until a successful, idempotent cleanup retry. No implicit rollback or
fault-clear switch exists.

Unpublished roots have never been attached under the contract and do not need
a hardware callback at destruction. Successful destruction frees metadata
only. The owner may then release its pinned table/data resources and discard
all remaining lease records. This manager never frees physical backing pages
on behalf of the owner.

## Concurrency and verification

All per-space operations serialize under a sleepable mutex. Lock order is
space then page-table mutex for translation operations, or space then global
lease-generation mutex for token allocation. No callback executes under the
global generation lock. Owner callbacks must not reenter either space or table
APIs or wait on work requiring their locks. Callers must stop/join all users
before destruction; an internal mutex cannot protect an already freed handle.
This API is not safe in hard-IRQ or panic context.

`tools/pcie-test/test_dma_space.c` links both real production components with a
separate walker/cache model. It covers alignment/boundary constraints,
fragmentation and first-fit reuse, aperture and reservation limits, separate
table exhaustion, stale/foreign/recreated leases, allocation failures, failed
publication and destruction, and a stale translation after failed unmap.
It also injects otherwise valid but unexpected/missing PTEs to check accounting
mismatch quarantine; this does not claim detection of arbitrary corruption.
The fault sweep injects 54 published map/unmap failures with positive and
negative errno before and after callback effects. A deterministic 10,000-step
sequence is checked against a separate page-occupancy model; four threads run
400 complete reserve/map/unmap/release lifetimes and check for overlapping
active ranges. These are offline model tests, not SMMU or Wi-Fi device tests.
