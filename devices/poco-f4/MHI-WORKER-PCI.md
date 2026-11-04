# QCA6390 MHI IRQ worker and ath11k PCI lifecycle

## Status and boundaries

`hakios.mhi.worker.c` implements a native XNU IRQ-to-thread-call scheduler.
`wifi/lx/src/lx_qca_pci.c` implements an ath11k **core lifecycle adapter** for
QCA6390 HW2.0. Neither has a boot-time caller. This is **not a complete PCI
backend, a Linux MHI compatibility implementation, or working Wi-Fi**.

The adapter calls the pinned ath11k core/HAL/CE initialization and destruction
functions, rather than reporting success for unsupported operations. It
requires a PCI HIF and a native owner. These hardware integrations are not implemented:
RC0 handoff/provider, endpoint-specific pinned DMA/SMMU ownership,
global GIC/MSI grants, CE/DP IRQ ownership and their runtime coordination.
`tools/mk-ath11k.sh` keeps AHB as its default, while
`ATH11K_PROFILE=PCI tools/mk-ath11k.sh` builds the native QCA entrypoint,
runtime, HIF and DMA/SKB binding. The Munch runtime gate still requires the
XNU platform-owner hook; with no RC0/SMMU/GIC/MHI/IPCR owner it returns
`ENOTSUP` before hardware access.

In particular, `wifi/lx/src/lx_base.c` still has one legacy AHB radio-memory
pool, no real endpoint-specific DMA map/unmap/cache lifetime and no proper
coherent-buffer reclamation. Supplying that pool as the adapter's DMA owner
would be incorrect. Merely adding upstream `pci.c`, `pcic.c`, and `mhi.c` to
the build would not provide XNU PCI enumeration or a working native backend.

Follow-up: `ENDPOINT-DMA-HIF.md` describes the new endpoint DMA/cache core,
allocator for platform-granted MSI banks and native HIF/CE/DP dispatch.
They still require the actual hardware owners. The old DMA file rejects PCI
builds unless the explicit native-DMA define excludes its legacy implementation.
The probe adapter is version 2:
its resource bundle requires native DMA and MSI-allocation handles, and it
cross-checks the complete active MSI configuration against that allocation.
`WIRED-DMA-MEMORY.md` adds the native VM/pmap buffer implementation and an
explicit endpoint constructor. `ATOMIC-DMA-RUNTIME.md` adds the preallocated
atomic/range-aware DMA/linear-SKB subset and runtime composition around the
adapter/HIF. Neither supplies a real SMMU/GIC grant or boot-time caller.

## Native worker

Creation allocates an XNU `thread_call` with `THREAD_CALL_OPTIONS_ONCE` and
kernel priority. It accepts an existing control event ring, data event ring,
IPCR bridge, nonzero epoch, budgets and retry interval. All objects must remain
alive until successful worker destruction. Their geometry and channel/ring
identity must match the controller configuration; the scheduler does not
infer or repair them. It validates distinct ring pointers and IPCR epoch.

Three distinct, externally granted IRQ tokens and hardware INTIDs are
mandatory. No vector allocation is fabricated from an INTID constant:

| MSI vector / work bit | Processing |
| --- | --- |
| 0 / STATE | Owner's control/state-machine poll |
| 1 / CONTROL | Native control event ring, then control poll |
| 2 / DATA | Native data event ring, then IPCR worker |
| software / IPCR | IPCR TX/RX/deferred-ACK work |

Provider `attach` must leave the interrupt masked, including while installing
the handler. Failed attachment is treated as potentially effectful: the
attempted token is retained for mask/synchronize/detach. Provider `mask` and
`unmask` execute under the worker's spin gate with local interrupts disabled.
They must be IRQ-safe, bounded, non-sleeping and must not synchronously invoke
the handler, call back into the worker or acquire a conflicting lock. Attach,
synchronize and detach run in sleepable lifecycle context. All callbacks use
positive native errno values; invalid/error-shaped progress is not success.

The IRQ handler masks its vector, verifies the delivered INTID, latches a bit
and schedules the thread call. It does not poll rings, allocate, wait, decode
QMI or take a sleepable lock. Work runs outside the spin gate. The control
callback runs after event polling releases its locks; `retry=true` requests
a delayed state poll. Its error return is fatal, not a progress indication.
IPCR's `schedule` callback can call `hakios_mhi_worker_kick` with the current
epoch and the IPCR work bit. Before start/after quiesce, `EAGAIN` means closed
admission, not a reason to spin. Old epochs return `ESTALE`.

Per-run budgets bound event and IPCR processing. Full budgets and legitimate
backpressure schedule delayed retries; concurrent new work takes priority
over the delay. Ring/bridge health is checked before treating retry-shaped
errnos as normal backpressure. A fault closes admission, attempts to mask all
attached vectors and reports the error once outside the gate. Mask failures
do not make resources safe to release: explicit quiescence remains mandatory.

`quiesce` closes admission before cancel-and-wait, then masks and synchronizes
all attempted attachments. `resume` requires successful quiescence, all three
attachments and no fault; it schedules a fresh bounded poll before unmasking.
`destroy` detaches only after successful quiescence and retains its handle on
any failure, including an unexpected failure to free the thread call. Resume
cannot reanimate a partially detached instance. External lifecycle callers
must retain the object and serialize final destruction against all users.
Quiesce/resume/start/destroy from a running worker callback return `EDEADLK`;
fault handling must enqueue teardown on a separate owner thread, not join itself.

The provider must drain stale hardware IRQs before reusing an epoch or grant.
Hardware MHI events do not carry the software epoch. The worker alone does
not fence DMA, stop channels, control clocks or authorize RC0 access.

## PCI adapter contract

The adapter's public functions return Linux-style negative errno. Its native
owner operations return positive errno. Calls to probe/remove must be
externally serialized and must not originate in an ath11k, QMI or MHI callback
that removal would join. The device and owner must outlive successful remove.
`lx_qca_pci_owner(ab)` is for bases allocated by this adapter, not arbitrary
upstream PCI-private objects.

Mandatory owner operations:

- `prepare`: acquire or retain endpoint, MSI, IPCR and worker resources and
  install a real device-specific DMA implementation before any ath11k
  allocation. All supplied objects, grants, mappings and HIF callbacks must
  belong to the same endpoint/epoch. The adapter can check identities and
  epochs but cannot prove this cross-object ownership. Partial preparation
  must remain cleanable even if it returns an error.
- `attach_irqs`: attach all required CE/WAKE/DP interrupts and their deferred
  handlers. MHI uses its own worker; no legacy `hakios_lx_irq` routing is implied.
  Failed attachment can have side effects and still requires detach.
- `close_admission`: permanently close new I/O, IRQ enable, power-up/recovery,
  DMA publication and work production. HIF operations after closure must not
  re-enable the endpoint. This barrier precedes the hardware fence and must
  coordinate with already executing owner operations. It must also work after
  partially failed preparation. An error retains the entire driver instance.
- `fence`: drain active owner operations and all device DMA, mask/join every
  IRQ class, stop/join the MHI worker, and stop/quiesce IPCR. It must keep the
  endpoint inaccessible to later recovery requests, not merely clear Bus
  Master once. Successful worker and IPCR status checks are additional
  necessary conditions, not proof that the device has stopped DMA.
- `detach_irqs`: release attempted IRQ attachments only after the fence and
  driver callback drain. Failure retains the driver/DMA allocation state.
- `release`: release the remaining native resources last, after driver memory
  destruction. All cleanup callbacks must support retry after partial effects.

Fence stops and joins these resources but must not destroy the returned worker,
IPCR or MSI objects: their handles remain valid until `release`. Close kick
producers and quiesce the worker before stopping IPCR, then destroy the worker
only once no IPCR/IRQ callback can submit another kick. Destroying a scheduler
while a bridge still holds a raw scheduler pointer is not safe.

The real DMA implementation must quarantine buffers/mappings until safe
revocation is proven. Upstream CE/HAL/QMI initialization can free partially
allocated resources before returning failure to the adapter. An outer fence
after that return cannot retroactively protect a prematurely reused buffer.
Void HIF callbacks cannot communicate shutdown failure; they must retain
faults in the owner and cannot replace the explicit fence.

Probe performs these real stages:

1. Validate required HIF/owner callbacks, allocate a retained state handle,
   prepare native resources and allocate an `ATH11K_BUS_PCI` core instance.
2. Read the native service's provider-validated endpoint snapshot and config
   identity: domain 0, RID `0x100`, SID `0x1c01`, BAR0 `0x60300000`/1 MiB,
   `17cb:1101`, class/revision `0x02800001`. The snapshot itself performs no I/O.
3. Read QCA hardware version at BAR offset `0x224`, require major 2; check the
   IPCR/worker epoch and health and all 32 granted MSI messages. Populate the
   MHI 0–2 / CE 3–12 / WAKE 13 / DP 14–31 layout from the active MSI engine,
   not a guessed payload base or physical doorbell address.
4. Run `ath11k_core_pre_init`, `ath11k_hal_srng_init`,
   `ath11k_ce_alloc_pipes`, populate CE/QMI tables and bind the WLFW instance-1
   handle to this IPCR bridge before `ath11k_core_init` opens it.
5. Attach remaining IRQ handlers and run `ath11k_core_init`, including the
   supplied HIF power-up. Zero means asynchronous probe completed, **not**
   firmware-ready, registered radio, associated station or usable networking.

Remove and failed-probe rollback mark `UNREGISTERING`, close admission and
require the explicit fence before destroying anything. They cancel QMI event
work when initialized, reset/dump/restart/11d work and the RX-replenish timer;
then use normal core deinit or the pinned driver's `QMI_FAIL` cleanup. A QMI
handle left open by a failed core initialization is explicitly released.
QMI unbind precedes IRQ detach, CE/HAL/firmware/core destruction and final
owner release. Partial HAL initialization frees only its allocated config,
not an uninitialized lockdep context. CE allocation failures use upstream's
own partial-allocation cleanup.

On any cleanup failure, probe/remove returns that error and leaves a non-NULL
state handle. The caller must retain it and retry `lx_qca_pci_remove`; it must
not free the owner, forget the handle or start another probe. Successfully
completed teardown stages are not repeated. The IPCR router is never replaced
by a global WPSS/GLINK fallback.

## Offline checks

`tools/pcie-test/run.sh` tests the real worker with a pthread-backed XNU
thread-call shim, real native event rings and the real IPCR bridge. It checks
IRQ deferral, bounded draining, 10,000 coalesced kicks, delayed retries,
cross-thread cancellation, self-join rejection, allocation failures,
resume-after-partial-destruction rejection and 56 injected callback failures.

The same runner tests the actual PCI adapter against explicitly mocked
ath11k/native-owner APIs: successful and QMI_FAIL removal, resource/version/
epoch/MSI mismatches, partial HAL/CE/QMI errors, all MSI vectors, 60 injected
lifecycle failures, retained cleanup and retry with no double free. These
mocks are not a DMA provider or an executable ath11k data path.

`tools/pcie-test/compile-ath11k-pci.sh` separately compiles the adapter against
the real pinned ath11k/LinuxKPI headers and actual `qmi.c` in both profiles.
It checks that the AHB adapter unit exports no PCI entry points, PCI IPCR
imports, unchanged AHB QMI imports and rejection of the
AHB bus in PCI mode. It requires pre-existing pinned extraction/generated
headers, records their release identifiers and does not modify those inputs.
This is an object check, **not** a complete PCI library build or link.
Native worker object/kernel checks are documented in `PCIE-VALIDATION.md`.
