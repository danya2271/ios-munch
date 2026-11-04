# QCA6390 endpoint DMA, granted MSI allocation and PCI HIF

## Implemented scope — not a runtime enablement

This stage adds the following callable software components:

- `hakios.dma.endpoint.c`: endpoint-specific DMA leases, coherent allocation
  admission, dedicated streaming bounce buffers, CPU/device ownership,
  SMMU-table/IOVA integration and retained failure cleanup.
- `hakios.dma.cache.c`: ARM64 PoC cache maintenance for exclusively owned,
  page-aligned streaming backing, with barriers. It is compiled but has not
  been exercised with device DMA.
- `hakios.dma.memory.c`: native wired VM backing, non-cacheable coherent CPU
  mappings through pmap, attribute restoration and an explicit endpoint
  constructor. See `WIRED-DMA-MEMORY.md`; no hardware owner is registered.
- `hakios.msi.pool.c`: serialized allocation of aligned 32-vector banks from
  **platform-granted** GIC routes, endpoint doorbell mapping and retryable
  teardown. This is not yet the GIC controller's hardware grant provider.
- `wifi/lx/src/lx_qca_hif.c`: native lease-based QCA register access, MSI and
  service mapping, CE/DP deferred dispatch and lifecycle/error barriers.

No component registers a boot-time provider or changes a phone. There is no
new Bus Master enable path. The existing RC0 ownership gate and `wifi=1`
behavior are unchanged. Hardware bring-up is still blocked by:

1. Proven RC0 handoff and a registered Kona host/endpoint owner.
2. An exclusive SMMU context/stream owner for SID `0x1c01`, connected to the
   native wired allocator with validated cache/TLB operations. No context-bank
   number is guessed or programmed by these new components; hardware coherence
   remains unvalidated.
3. A global GIC resource provider that arbitrates these grants against other
   users, attaches masked, implements bounded mask/unmask and synchronizes
   in-flight handlers. The current legacy `gic_v3_spi_attach` enables on
   attachment and is not a drop-in provider for this contract.
4. Complete LinuxKPI/network integration beyond the new bounded DMA/linear-SKB
   subset. `ATOMIC-DMA-RUNTIME.md` describes its preallocated, non-sleeping hot
   path, byte-range synchronization, quarantine and deferred-thread restriction.
5. Real provider implementation and validation for the new explicit runtime
   composition: it connects the arena/HIF/PCI adapter but does not acquire
   hardware resources or boot a controller without mandatory owner callbacks.

`lx_base.c` rejects `CONFIG_ATH11K_PCI` without `LX_QCA_NATIVE_DMA`: replacing
the AHB bus file alone must not accidentally retain its permanent DMA pool,
no-op frees or device-less `pmap_kextract` mapping. With the explicit define,
those legacy definitions are excluded and the new binding is required. The
ordinary AHB build is not switched to PCI. The explicit PCI profile now links
the native entrypoint, common ath11k helpers, HIF and runtime against this
binding. It deliberately does not import upstream `pci.c`, `pcic.c` or
`mhi.c`: those files depend on FreeBSD/LinuxKPI PCI ownership and cannot
replace the XNU native owner contract.

## Endpoint DMA and SMMU lifetime

Creation checks the native service's domain 0 / RID `0x100` / SID `0x1c01`
snapshot, address-mask bounds and page-table configuration. It takes a native
endpoint **hold**. PCI release returns `EBUSY` while DMA or MSI holds exist;
drop remains possible after an endpoint fault so cleanup can finish. Holds
are not permission to enable Bus Master or a replacement for host ownership.

The endpoint creates and exclusively owns its `hakios_dma_space`, synchronizes
the initially zeroed table pages, then invokes the mandatory owner's `attach`
with the validated endpoint resources and table geometry/root. The owner must
prove exclusive SID/context ownership and install the matching granule, TCR,
MAIR, table attributes and invalidation policy. Table callbacks must perform
real cache publication, hardware TLB invalidation and bounded completion.
Calling them successfully without that work is a contract violation, not a
supported fallback. Attach must leave device DMA disabled.

For each allocation the owner returns a retained token, CPU address, stable
array of distinct 4 KiB physical pages, page count and two properties:

- `coherent`: the returned CPU mapping really is coherent with the endpoint,
  through a proven uncached mapping policy or hardware coherency. Cached
  memory does not acquire this property because a caller requests it.
- `cacheable`: the normal-memory SMMU leaf attribute appropriate for that
  allocation. Non-cacheable coherent mappings use MAIR index 0; cacheable
  streaming backing uses index 1. CPU/SMMU aliases and attributes must agree
  with the platform's actual memory/coherency model.

The allocator owns pinning, PA accuracy, exclusivity, alias avoidance and
physical-page lifetime. Different active allocations must not share pages,
and backing must not alias table pages or unrelated kernel objects. The
core validates returned shape but cannot infer these properties from a token.
The explicit `hakios_dma_memory_create_endpoint` constructor supplies these
buffer callbacks through the native VM/pmap implementation; see
`WIRED-DMA-MEMORY.md`. Domain/table callbacks remain mandatory.
Callback arguments and table backing must remain alive until successful
endpoint destruction. Callbacks must not re-enter this endpoint.

Streaming maps use dedicated bounce backing, not whole-page mappings of an
arbitrary `skb->data` or `kmalloc` buffer. This avoids exposing adjacent kernel
objects and doing destructive cache invalidations on shared cache lines.
Unused backing bytes are zeroed before publication. TO_DEVICE maps copy to
backing and install read-only PTEs; FROM_DEVICE/BIDIRECTIONAL require writable
PTEs. ARM stage-1 does not provide a write-only leaf here, so FROM_DEVICE also
permits reads of the dedicated backing, never adjacent source allocations.

Map first publishes data-cache state, reserves IOVA, then publishes PTEs and
invalidates the SMMU TLB through the existing engines. The returned IOVA is
not a CPU PA. Streaming maps start device-owned. Whole-buffer `sync(false)`
requires the caller to have observed DMA completion, invalidates backing as
needed and copies device-written data back to the original CPU buffer.
`sync(true)` copies CPU changes for TO/BIDIRECTIONAL and transfers ownership
back. Repeating a streaming ownership transition returns `EALREADY`.
There is deliberately no partial-range or atomic-context API yet.

`hakios_dma_cache_streaming` accepts only complete, exclusively owned
4-KiB-aligned pages. It derives line size from `CTR_EL0`, uses `dc cvac` for
TO_DEVICE publication, `dc civac` before device writes and `dc ivac` after
observed device writes, surrounded by `dsb sy`. It must never operate on live
CPU-owned aliases or arbitrary unaligned skb storage. On non-ARM64 hosts it
returns `ENOTSUP`; passing host tests is not cache-coherency validation.

Successful unmap transfers remaining ownership back, removes PTEs, completes
TLB invalidation, frees backing and finally releases IOVA. Any uncertain
allocation/cache/map/unmap/free result faults the endpoint and retains the
affected backing/lease. No hidden retry replays a DMA operation. Allocator
free may update its mutable backing token after partial effects; it must be
idempotent and accept an empty backing. Successfully freed backing is not
freed again if later software cleanup fails.

Unmap is not itself a global DMA fence: the caller must already have proved
completion/no future use. In particular, a future LinuxKPI binding must not
blindly recycle coherent memory freed by upstream partial-init error paths
before the outer probe rollback can fence the device. It must retain such
allocations until owner-approved retirement/fencing. This software layer
alone does not prove that those upstream frees are safe.

`close` stops admission and requires the owner's actual endpoint fence.
After close, per-buffer operations are refused; final destruction reclaims
retained allocations, without copying aborted RX data to old source pointers.
Destroy requires fence, then SID/context detach, then table/address-space
destruction, backing release and endpoint hold drop. Table pages themselves
remain the owner's property. A failed constructor may return a non-NULL
endpoint; a failed destroy retains it. Keep that handle and retry destruction.

## MSI banks versus GIC ownership

Pool creation consumes an authoritative route array with distinct opaque
tokens, hardware INTIDs and message data. It verifies the Kona Linux binding:
DT SPI number plus 32 equals hardware INTID. Message data is validated as a
separate aligned 16-bit sequence supplied by the GIC/MSI owner; it is not
required to equal an INTID. It does not create either grant. For the supplied
Kona range, SPI 768–799 therefore validates INTIDs 800–831. The current Android
capability happens to contain payload base `0x0320` (800), but that observation
does not turn the value into a universal constant or an XNU allocation grant.

The allocator reserves aligned banks of 32 under a mutex. Each allocation
holds the endpoint lease and invokes the real provider's global claim and
DMA-map operations. The provider maps the doorbell **for this endpoint**, not
as a CPU address; the returned IOVA must retain the physical in-page offset.
The whole exposed page and its security implications must be granted by the
platform owner. This layer does not provide interrupt remapping or restrict
which payload a compromised endpoint could write to the doorbell.

Allocation returns either a valid bank or a retained failed handle. Partial
claim/map failures keep the bank unavailable. Teardown requires endpoint/MSI
quiescence and synchronized handlers, then doorbell unmap, route release and
hold drop. The bank cannot be reused until every stage succeeds. Pool destroy
refuses outstanding allocations. Lifecycle calls, including final destroy,
must be externally serialized against users of returned handles.

`allocation_config` supplies the existing MSI capability engine; `route`
supplies individual grant tokens for IRQ owners. The PCI adapter version is
now 2: native DMA and MSI-allocation handles are mandatory, and all 32 active
MSI messages must match that allocation's IOVA, payloads and INTIDs. Its fence
also verifies that the native DMA endpoint is closed and fenced. The current
adapter explicitly closes an optional native HIF before requesting the fence;
failure retains the driver rather than continuing its teardown. The current
GIC hardware driver is unchanged; no global hardware allocation is claimed.

## Native PCI HIF and deferred CE/DP

The owner can provide `lx_qca_hif_operations()` to the probe adapter and place
its HIF instance in the resource bundle. Its `attach_irqs` implementation must
call `lx_qca_hif_bind` after ath11k hardware parameters/CE pipes exist and
before firmware/core startup. HIF, DMA and MSI handles must outlive all driver
callbacks. HIF final destruction belongs before DMA/MSI resource destruction,
but after the driver has been fenced and freed.

Register accesses use serialized native BAR batches: high QCA offsets write
and verify window register `0x310c` before accessing the `0x80000` window.
Offsets at/above `0xfe0` require native wake handling. Invalid/wake/MMIO errors
never proceed as successful register operations. Void HIF writes and failed
reads latch a fault and notify the owner; reads return `UINT32_MAX`. Block
reads return an error and clear partial output. No raw BAR pointer is exported.

CE vectors start at 3 and skip `CE_ATTR_DIS_INTR` engines. More than ten active
engines is rejected without publishing partial state; a retry cannot inherit
stale CE vectors. DP groups use `14 + group`, only where a QCA ring-mask entry
requests work. MSI
metadata comes from the active native engine. Service-to-pipe mapping uses
the QCA hardware table, with missing/duplicate/out-of-range entries rejected.

The owner attaches IRQs masked, masks on arrival, and schedules a **sleepable**
call to `lx_qca_hif_service`. This entry serializes CE/DP work, calls the real
`ath11k_ce_per_engine_service` or `ath11k_dp_service_srng`, and returns whether
the DP budget was exhausted. Requeue instead of looping indefinitely when
`more=true`; rearm only after success and while that group remains enabled.
CE uses one upstream service call, whose internal loop has no budget argument.
This is a native polling boundary, not an implementation of Linux NAPI or
its IRQ registry. The current station adapter treats the NAPI argument as
opaque; changes to that boundary require revalidation before integration.
WAKE vector 13 and MHI vectors 0–2 belong to their respective native owners,
not the CE/DP dispatcher.

Mandatory callbacks perform actual power transitions, interrupt mask/unmask,
deferred-work drain and asynchronous fault handling. They cannot be success
stubs. Power/start/stop are serialized with HIF close so recovery cannot race
past closed admission. Driver register access uses a separate gate, allowing
CE/DP callbacks to read hardware without recursive service-lock acquisition.
Close masks and drains all configured CE/DP vectors and joins in-flight
service; cleanup failures retain the HIF. Fault callbacks must enqueue owner
recovery, not synchronously destroy or join their own service callback.

## Offline evidence

The endpoint/HIF stage covered thirteen host executables. Its coverage includes
real DMA PTE/IOVA integration, 36 callback failures, bounce data visibility,
permissions/cache attributes, false-coherent rejection, five MSI allocation
failure sites, exhaustion/retention and endpoint holds. HIF tests exercise
native BAR batches, vector/service mapping, CE/DP dispatch, six failure cases
plus failed-bind retry and power-up versus close serialization.
Driver/native-provider dependencies are explicitly mocked; they are not
hardware emulators. The wired-memory follow-up adds two VM-page geometries,
bringing the total to fifteen executables; see `WIRED-DMA-MEMORY.md`.

The real pinned-header object check compiles the HIF and PCI adapter, retains
PCI/AHB QMI checks, compiles the legacy DMA/SKB base in AHB mode, and rejects
both legacy bus and DMA files in PCI mode.
The native object/full-kernel runners now cover twenty-one C units and nineteen
headers. Concrete logs and validation limits are in `PCIE-VALIDATION.md`.
