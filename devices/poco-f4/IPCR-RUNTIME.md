# QCA6390 IPCR bridge, runtime ownership and ath11k QMI boundary

## Scope and safety gate

`hakios.ipcr.c` now connects the native MHI transfer rings to the native QRTR
endpoint using caller-owned DMA buffers and a deferred, bounded worker entry
point. `hakios.mhi.runtime.c` implements a conservative, explicit M0/M3 power
lifecycle and retires failed epochs. The PCI variant of the LinuxKPI QMI
header uses a per-handle IPCR binding rather than the global GLINK router.

These are callable implementations, **not a running QCA controller**. No real
RC0 provider, SID `0x1c01` DMA/cache owner, MSI/GIC allocation or native MHI
interrupt owner is registered. A native deferred scheduler and a conditional
ath11k core probe/remove adapter now exist; see `MHI-WORKER-PCI.md` for their
contracts and missing HIF/DMA/IRQ integration. The owner callbacks below are
mandatory, not success stubs. There are no new boot-time callers, device
operations, boot flags, Bus Master writes or readiness shortcuts. Full ath11k
PCI probe/remove, CE/DP integration and hardware recovery remain unfinished.

## Buffer ownership and deferred delivery

Creation takes two distinct 1024-byte MHI rings, channel 20 TX / 21 RX, and
1–63 externally pinned **8192-byte buffers per direction**. It checks ring
alignment, range overflow and pairwise CPU/IOVA overlap across both rings and
all buffers. Buffer descriptors are copied; actual backing storage remains
the owner's until successful destruction. Physical aliases, cache-line
isolation, overlap with contexts/events/firmware, SID assignment and memory
pinning cannot be proved from these descriptors and remain owner obligations.

`start()` schedules work; it does not issue channel START commands. Before
calling it, the owner must have completed AMSS/M0 startup, event-ring setup,
both channel START commands and the associated wake/DMA/IRQ grants.

- `transmit()` copies QRTR's borrowed scratch frame into a free TX buffer and
  queues the real channel-20 ring. Storage remains occupied until EOT.
- `sync_buffer(channel, index, for_device, length)` transfers cache ownership
  in both directions. `sync_ring()` publishes the modified TRE, then
  `publish()` updates the device context WP and channel doorbell with the
  required cache maintenance/barriers. These callbacks must not fabricate
  DMA coherency or execute completions inline.
- No free TX buffer means `EAGAIN` and no publication. A queue callback
  returning `EAGAIN`/`ENOSPC` after making the MHI ring uncertain is converted
  to a fatal error, not retried as backpressure.
- `complete(epoch, event)` runs in a **sleepable event worker, not hard IRQ
  context**. It performs ring completion/CPU synchronization, releases TX
  slots or enqueues RX buffer indices. It never invokes QMI delivery inline.
- `work(budget, &received)` posts free RX buffers, starts HELLO, polls QRTR
  control traffic and delivers completed frames in order. It holds no bridge
  mutex across router/application callbacks; callbacks may transmit again.
- RX `EAGAIN`/`ENOSPC` retains the same CPU-owned frame without delivery or
  recycling. Deferred ACKs and DEL_CLIENT are polled; a consumed RX buffer is
  reposted only after callback return. Later faults never replay that callback.

The worker is serialized with a busy admission flag. Recursive/concurrent
work, pause or stop cannot wait on a callback that is waiting on them;
they return `EBUSY`. Status reports the retained first fault and buffer counts.
These counts describe software bookkeeping, not a proof that DMA has stopped.

## Scheduler and teardown contract

`schedule(owner)` is a **nonblocking, asynchronous, coalescing kick**. It may
be called under bridge/router mutexes. It must not invoke `work()` inline,
wait for it, acquire reverse-order locks or lose a kick while work is already
running. The owner supplies the actual XNU thread/work mechanism and lifetime
reference. Completion, start, budget exhaustion, cached lookup, close and
resume can request another pass. QRTR operations version 2 adds this optional
notification; the IPCR adapter supplies it. Standalone routers may still poll.

When no progress is possible, wait for a completion/service event or an
owner-controlled retry; do not busy-loop on `EAGAIN`. On a fatal return the
owner must schedule control-path recovery, not another transfer submission.
Native bridge/runtime APIs return positive errnos; the QMI initializer follows
the existing Linux-compatible QMI API and returns negative errnos.

`stop()` closes producer/worker admission, disconnects QRTR outside the bridge
mutex and invokes the mandatory owner `quiesce()`. Quiescence must stop/reset
channels as appropriate, fence DMA, drain old event callbacks/IRQs and drain
scheduled worker invocations before success. Do not synchronously join the
current control caller. If quiescence fails, all storage and handles survive;
stop can be retried, but the epoch cannot be restarted. Lookup/close after
stop cannot enqueue a new worker. Work scheduled before start is also drained.

Destruction additionally requires closing/releasing the QRTR/QMI client and
removing its explicit binding. Do not destroy while other API calls or cached
endpoint users retain references: lifecycle destruction is externally
serialized, not a general-purpose object reference-counting API.

Epoch numbers are nonzero owner-issued identities. A stale epoch returns
`ESTALE` before touching rings. Hardware EOT records contain **no epoch**:
IRQ/event work must retain its original epoch, not label every event with the
current one. Retiring an epoch therefore still requires physical DMA/IRQ/event
drain; incrementing a counter or clearing ring indices is not recovery.

## Conservative runtime power lifecycle

Creation takes an exclusive runtime lease from a boot engine that has reached
ONLINE. Boot `step()`/status then return `EALREADY` with phase RUNTIME, without
MMIO; boot destruction returns `EBUSY`. The runtime owner and boot owner must
refer to the **same controller, mappings and wake ownership**. Public take /
retire boot helpers are lifecycle internals, not an alternative safety gate.

```text
ONLINE boot -> runtime NEW -> verify AMSS/M0, assert wake, rearm -> ACTIVE
ACTIVE -> zero wake references + drain -> request M3, release wake
       -> SUSPENDING -> observed AMSS/M3 -> link_suspend -> SUSPENDED
SUSPENDED -> link_resume, assert wake, request M0 -> RESUMING
          -> observed AMSS/M0, rearm -> ACTIVE
error / timeout / SYS_ERR / RDDM -> FAULT -> quiesce -> RETIRED
```

The policy keeps device wake asserted throughout ACTIVE: **autonomous M1/M2
is not implemented**. Those states fault rather than silently approving
doorbells in an unsupported power state. Explicit M3 suspend/resume is not
automatic system suspend integration or a promise of working power saving.

- `get()` grants a wake reference only in ACTIVE; `put()` releases one, even
  after failure/retirement. A pending transition returns `EAGAIN` without a
  grant. It does not wake hardware implicitly. Every successful get requires
  exactly one put; final destruction requires zero references.
- `drain()` must atomically close all IPCR **and CE/DP** producer admission
  and verify idle ownership. It can use `hakios_ipcr_pause()`, but that alone
  is not controller-wide quiescence. `EBUSY`/`EAGAIN` requires no surviving
  admission change; rollback a partial multi-client pause before returning.
- IPCR pause checks pending transfers, ready RX, worker activity and pending
  QRTR controls/notifications. RX arriving after pause is retained through
  resume. It is not discarded or mistaken for safe DMA teardown.
- `request_state()` must perform the actual serialized MHICTRL update and
  report MMIO failures. `read_state()` returns device state/EE and must report
  SYS_ERR/status and access failures, not just mask those bits away.
- `link_suspend()` runs only after observed M3. `link_resume()` completes
  link/register-access restoration before wake or M0 programming. No register
  read occurs while SUSPENDED. There is no assumed right to toggle RC0 power.
- `rearm()` must restore event/channel doorbells and actual client admission
  after M0; for IPCR this includes start/resume as appropriate. A generic
  success return cannot stand in for that work.
- Monotonic time and bounded deadlines apply to both transitions. Callback
  failures after potential effects are fatal, including retry-shaped errnos
  except the explicitly non-consuming `drain()` backpressure case.

Callbacks run outside the runtime mutex with a busy guard. Reentrant APIs
return `EBUSY` instead of deadlocking; do not wait for a concurrent operation
that needs this guard. `fault()` must immediately close platform/client
admission and schedule recovery without inline teardown. This does not itself
prove DMA stopped. `quiesce()` must work even after partially completed link
suspend/resume and stop/drain every old client and event source.

`retire()` retries real quiescence and retains the boot lease on failure.
Only successful retirement and zero references permit runtime destruction
and release of the boot lease. Firmware/ring/DMA storage still follows its own
destruction contract. Recovery creates new boot/ring/router/QMI instances
only after this fence and an owner-proven cold PBL/reset state. There is no
automatic warm reset, RDDM collection, in-place index reset or firmware restart.

## ath11k integration boundary

For a future native PCI bus object, the lifetime sequence is:

1. Allocate/validate a QCA6390 HW2.0 `ath11k_base` using native endpoint, DMA
   and MSI ownership; complete MHI startup/channel commands and create IPCR.
2. Bind its zero-initialized `&ab->qmi.handle` with `hakios_ipcr_qmi_bind()`
   before `ath11k_qmi_init_service()`. This pins the bridge lifetime until
   explicit unbind; one handle and one port are supported per bridge epoch.
3. Compile that driver's QMI unit with `CONFIG_ATH11K_PCI`. The tracked
   LinuxKPI QMI wrapper maps `qmi_handle_init()` to the bound IPCR initializer.
   Missing binding returns **`-ENXIO`**, even if a global GLINK router exists.
   Requests use native `qmi_send_request()`, not Redwood's mission-mode hook.
4. On removal/failure, close admission, notify disconnect, stop producer
   work, deinitialize/release QMI and unbind. Wait for quiescence before
   destruction/reuse. Never release a QMI handle from its own callback.

AHB still uses its previous QMI initializer and mode interception. Mixed PCI /
AHB configurations and compiling `lx_bus.c` as PCI are explicit build errors.
`mk-ath11k.sh` permits only the specific new native initializer as an unresolved
kernel import; it still builds AHB and does **not** claim to build PCI/MHI.
The LinuxKPI MHI fail-closed patch and Munch runtime guards remain in place.

No ignored Linux source was modified or promoted into a tracked dependency.
The source extraction remains reproducible through the existing pinned
`mk-ath11k.sh` archive workflow. Host tests use only tracked sources, including
the actual PCI QMI wrapper, and do not require that extraction.

## Offline verification

The new IPCR model uses separate CPU/device copies and drives real native MHI,
QRTR and QMI code, not a replacement codec. Coverage includes borrowed TX
storage, delayed RX, ACK backpressure, lookup wakeups, ring wrap, retained
quiescence failure, before-start shutdown, stale epochs, callback reentry,
worker/stop concurrency, allocation failure, 160 callback failures and a
QMI transaction through the PCI header hook.

The boot model now also exercises the runtime lease, delayed M0/M3 completion,
no MMIO while asleep, wake reference gating, RDDM/SYS_ERR, unsupported M1/M2,
clock regression/deadlines, retired references and 30 runtime callback failures.
These checks do not validate real cache maintenance, scheduling/IRQ context,
power sequencing or firmware behavior. See `PCIE-VALIDATION.md` for run logs.

Reference behavior was compared with the supplied kernel's
`drivers/bus/mhi/core/mhi_pm.c` and `net/qrtr/mhi.c`, and the pinned ath11k
`qmi.c` initialization/deinitialization paths. The implementation remains
owner-driven native XNU code, not a substituted FreeBSD PCI ownership API.
