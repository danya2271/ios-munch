# Native PCIe endpoint layer for Munch

## Implemented boundary, missing hardware owner

`xnu/files/bsd/sys/hakios.pcie.h` and `xnu/files/bsd/dev/hakios.pcie.c`
define a native, single-RC0/single-endpoint service. It does not enumerate a
FreeBSD PCI bus and does not infer ownership from the device tree. No Kona
provider is registered anywhere in the current port, so acquisition returns
`ENOTSUP` without mapping or accessing hardware.

The service and Kona engine now pass local host-model tests and ARM64
compilation; see `PCIE-VALIDATION.md` for the limits and build artifacts.
This is not a hardware-validated host controller, an MHI
implementation, or a DMA/MSI implementation.

The separate `hakios.pcie.kona.c` CFG0 engine now implements bounded region-0
programming, snapshot/restore and cleanup retention for a future provider.
It is not registered or called by this service automatically. Its mandatory
cold-owner authorization and power/clock/reset/PHY sequencing still need a
real platform implementation. `hakios.pcie.kona.mmio.c` now supplies the
abort-safe access and config-drain callbacks, but no trusted owner token or
provider registration exists; see `PCIE-KONA-CONFIG.md`.

## Provider contract

Dependent native DMA and MSI allocations now take endpoint holds via
`hakios_pcie_endpoint_hold`. Release returns `EBUSY` until their matching
`hakios_pcie_endpoint_drop` calls complete. A hold requires a healthy lease;
drop permits cleanup of the same faulted lease but rejects stale or unbalanced
calls. This prevents host release before dependent resource teardown, not
DMA publication before proven ownership. See `ENDPOINT-DMA-HIF.md`.

Only a trusted native platform driver that implements the ownership protocol
in `PCIE-HANDOFF.md` may call `hakios_pcie_rc0_register()`. There is no boot-arg
registration path. The service copies the operations table; the context and
callback code must remain alive until successful unregister. Registration
alone neither acquires the hardware nor makes QCA preflight succeed.

All callbacks return zero on actual success or a positive XNU errno. Negative
Linux-style errors are treated as `EIO`, not mistaken for successful results.

### Acquire and release

- `acquire()` must obtain exclusive host/endpoint ownership, power/clock/PHY
  references, IRQ and DMA quiescence, and any required mappings. It returns
  a non-null owner cookie and the actual domain, requester ID, stream ID,
  and allocated BAR0 resource. These fields describe owned resources, not
  values copied from the request without verification.
- The cookie must retain enough state to restore every modified resource,
  including PCI command changes made through the service. Bus Master must
  already be clear. The service rejects a live Bus Master rather than
  assuming it may stop the previous driver's DMA.
- On failed acquisition, a null cookie means the provider has left nothing
  for this service to release. If cleanup remains necessary, return the cookie
  even with an error. The service calls `release()`; if cleanup fails, it
  retains the cookie, blocks new work/unregister, and allows the provider to
  retry cleanup through `hakios_pcie_rc0_recover(context)`.
- A success result with a null cookie is a provider-contract violation. The
  service quarantines the host; there is no flag-based recovery from unknown
  ownership. Recovery requires resolving the broken provider/reset path.
- `release()` is responsible for quiescence, restoring the provider's snapshots,
  dropping mappings/references, and the agreed firmware return policy. It must
  not report success until those operations complete. On error the cookie and
  ownership must remain valid for a later retry; partially cleaned resources
  must stay accounted for. This callback is also used after config/BAR faults.
- A failed release of a published lease retains that lease in the faulted
  state. Its client retries `hakios_pcie_endpoint_release(lease)`; provider
  recovery cannot steal an outstanding client lease.

### Config and BAR operations

- Config callbacks operate only on the endpoint represented by the cookie.
  They implement real 8/16/32-bit accesses, including correct write byte
  enables. Kona CFG0/iATU serialization, resource bounds, bounded polling,
  snapshot/restore verification, and MMIO-abort handling belong here. The
  removed QCA-local raw mapping path is not a fallback.
  A Kona provider may delegate the transaction algorithm to the new engine,
  but must supply its real owner callbacks and preserve its context through
  failed cleanup. The engine is not a substitute for the provider contract.
- Provider callbacks must serialize against firmware and other host users,
  not merely this service. Losing that exclusivity invalidates the contract.
- `access_begin(wake_required)` obtains a power-access reference. If wake is
  required but unsupported, return `ENOTSUP` before any BAR transaction. A
  failed begin must not leak a wake reference. Zero means accesses really are
  permitted for the duration, including any required boot-phase power hold.
- Every successful begin is paired with exactly one `access_end()`, even if
  a BAR operation fails. An end failure faults the lease. Full release must
  account for any power state left uncertain by a failed end.
- Before dropping that power reference, the provider must complete the
  required posted-write ordering/drain using a hardware-safe mechanism. A CPU
  store barrier alone is not a promise that a PCIe posted write has reached
  the endpoint. The generic service cannot choose a harmless flush register.
- BAR callbacks access the owned BAR0 mapping, preserve MMIO ordering, and
  report hardware faults. They do not expose a raw mapping pointer to QCA.

### Locking and execution context

All native service operations and provider callbacks are serialized by a
sleepable mutex. The QCA client takes its own mutex first. Callbacks must not
re-enter the native service or QCA client, nor wait for work requiring those
same locks. In particular, do not wire a recursively accessing MHI wake
routine into `access_begin()`. A future asynchronous MHI power lifecycle and
interrupt-safe data path need their own reviewed pinning/locking design.

The current interface is for thread-context bring-up, not hard IRQ, panic,
or final CE/DP hot-path use. Register batches keep one native lock and power
reference from window selection through the data operation. Client request
and operation buffers must not be mutated concurrently during a call.

## Endpoint lifecycle

Acquisition validates resources against the request, then reads PCI identity,
class/revision, BAR0 and, for a 64-bit BAR, its upper half. It rejects I/O and
reserved BAR types. BAR size comes from the owner's resource table, never an
all-ones sizing write. BAR0 address/size and SID must match the request.

If needed, the service enables PCI memory decode with a 16-bit command write,
then checks memory decode is on and Bus Master remains off. Public config
writes are limited to the conventional capability area `0x40..0xff`; command,
status and BAR registers cannot be changed through that API. There is no Bus
Master enable API. A validation failure releases the owner or quarantines the
failed cleanup; it never publishes a lease.

Published leases use nonzero monotonically increasing identifiers, which
are not reset on unregister. Old identifiers return `ESTALE`, and exhaustion
returns `EOVERFLOW` before acquisition. Config/BAR callback failures and
verification failures poison the active lease: subsequent I/O fails, while
release remains available. Release failure does not discard the identifier.

`hakios_pcie_bar0_transfer()` validates the entire bounded operation list
before beginning access. It supports read, write, and masked readback-check
operations. Failure stops the batch and clears all read results; it does not
claim to roll back writes already issued. Such a partial hardware transaction
requires owner recovery, not silent continuation.

## QCA6390 adapter

Probe/claim request domain 0, RID `0x100`, SID `0x1c01`, identity `17cb:1101`,
revision `01`, class `028000`, and the observed BAR0 at `0x60300000`, size
1 MiB. They also walk the MSI capability list with cycle/alignment checks.
This does not configure MSI. Probe acquires and releases a temporary lease
unless QCA already holds one; it is not inherently a read-only hardware probe.

`hakios_qca6390_release()` now returns errno and keeps failed ownership
visible. Provider-owned DMA and MSI resources are torn down before the
endpoint is released. The unverified legacy DMA groundwork remains behind an
independently unsatisfied prerequisite.

The QCA adapter accepts explicit DMA and MSI providers through
`sys/hakios.qca6390.h`. `hakios.msi.kona.c` composes the GIC bank lease, MSI
capability engine and route pool: it derives INTIDs/payloads from the Kona DT
parent mapping, installs routes masked, and maps the doorbell through an
owner-supplied endpoint callback. It has no boot-time owner or doorbell mapper
registered. Runtime therefore remains fail-closed until a trusted owner proves
RC0, SID `0x1c01`, DMA/cache behavior and the live GIC route. The historical
direct SMMU scaffold is not a runtime mapping path.

`hakios_qca6390_read32()` / `write32()` replace the raw BAR-pointer accessor:

- Register offsets must be dword-aligned and below `0x02000000`.
- Offsets below `0x80000` use direct BAR0 access.
- Higher offsets use window `(offset >> 19) & 0x3f`, selected through BAR0
  register `0x310c` with enable bit `0x40000000`. One batch writes and checks
  the selector, then accesses `0x80000 + (offset & 0x7ffff)`. A selector
  mismatch prevents the data access. There is no stale window cache.
- Offsets at or above `0xfe0` always request a power/wake reference, including
  before an MHI runtime exists. Unsupported wake does not fall through into
  MMIO. This deliberately avoids pretending the endpoint is already awake.
- Reads return errno separately from the value; failed reads clear the output.

The register geometry comes from the local pinned ath11k `pci.c`, `pcic.c`,
and `pcic.h`. These files are ignored build inputs, not an implementation
dependency of the native service. Only offline validation has been performed;
there have been no device accesses, and `wifi=1` remains disconnected from
this adapter.
