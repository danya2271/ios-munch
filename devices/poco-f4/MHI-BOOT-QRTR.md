# QCA6390 firmware boot and WLFW/QMI endpoint

## Implemented scope

`hakios.mhi.boot.c` implements a bounded, poll-driven **cold-boot state
machine** and actual BHI/BHIE register programming through owner callbacks.
`hakios.qrtr.mhi.c` implements a point-to-point QRTR endpoint for QCA6390's
WLFW service, including HELLO, service presence, flow control, delivery and
close/disconnect handling. `hakios_qmi_handle_init_endpoint()` binds an
existing native QMI handle to that specific endpoint without selecting a
global transport or replacing WPSS/MPSS GLINK.

Neither component is started by the QCA runtime. There is still no real RC0
provider, endpoint DMA/cache owner, GIC/MSI allocation or MHI IRQ worker.
`hakios.ipcr.c` supplies the buffer/ring/worker bridge; its hardware DMA and
asynchronous scheduler owner is still missing. See `IPCR-RUNTIME.md`.
No firmware was executed, device changed, image flashed or boot flag enabled.
The host models are not hardware emulators or evidence of working Wi-Fi.

## Firmware buffers and ownership

The boot configuration supplies an image and **already owned** pinned DMA
storage: up to 32 segments of at most 512 KiB and a separate vector table.
Creation checks lengths, IOVA aperture bounds, alignment and pairwise CPU/IOVA
overlaps before copying anything. It also excludes the three context arrays
from firmware IOVA ranges. Physical aliases through distinct IOVAs, context
CPU mappings, transfer/event-ring overlaps and pinning remain owner duties.

The entire image is copied into caller storage. Every BHIE vector is encoded
as a little-endian 64-bit IOVA and 64-bit byte length. The original image may
then be released; the copied segments/table must remain alive until successful
destruction, including after boot failure or apparent transfer success.
There is no in-kernel filesystem loader or invented physical/DMA address.

BHI receives the first `min(image_size, 512 KiB)` bytes; BHIE subsequently
receives a vector table for the **whole** image, not just the remainder. The
Android `amss20.bin` length 4,364,432 yields nine vectors / 144 table bytes.
Tests use synthetic bytes of that size, not an authenticated firmware run.
`m3.bin`, board data and regulatory data are not separate BHI stages; their
later WLFW/ath11k handling is not implemented by this boot engine.

`activate()` is mandatory. It must verify exclusive cold-boot ownership,
legal MMIO/power access, correct SID `0x1c01` mappings for **all** published
storage and safe DMA/interrupt readiness. Returning success without these
grants is a provider bug, not a way to satisfy preflight. The core does not
write PCI command/Bus Master, RC0/iATU, SMMU or GIC registers.

All callbacks are synchronous, return zero or a positive errno, and execute
under the instance's sleepable mutex. Negative errors become `EIO`. They must
not reenter the boot API, wait on its mutex or hide bus aborts. Register reads
must report access failures explicitly: `0xffffffff` can be a legitimate low
word of an address limit, so it is not universally treated as a failed read.

## Cold-boot states

The supported path is:

```text
NEW / PBL -> BHI transfer -> RESET cleared + READY
          -> contexts and apertures -> request M0 -> SBL + M0
          -> BHIE transfer -> AMSS + M0 -> ONLINE
any uncertain operation, device error or deadline -> FAULT
```

Start discovers BHI/BHIE offsets inside the 1 MiB BAR, rejects overlapping or
out-of-range blocks and requires PBL, MHI RESET/BHI state and no active reset
request. An already running firmware is rejected; there is no live-ring
adoption, force reset or implicit firmware handoff.

All image segments and the vector table are synchronized before publishing
BHI. The engine clears/verifies BHI status, writes the image address high word
before low word, size and nonzero 30-bit session ID. Address pairs are read
back. BHI completion must report success, not merely a changed status.

On READY, it synchronizes and programs the 128-channel, two-event and one-command
context-array addresses, sets two software event rings / zero hardware event
rings while preserving channel fields, and writes the owner-granted inclusive
control/data apertures. Address and event-count readback failures are fatal.
`ready()` must initialize the real doorbell/event-ring/wake side before the
engine requests M0. The owner must keep wake/power access valid; merely
returning success without arming the real rings is not supported.

Only SBL **and** M0 allow BHIE publication. The vector address is written
high-before-low, then byte size and a new sequence. Transfer success must match
that sequence; an old success cannot advance boot. ONLINE requires both AMSS
and M0 after the matching BHIE success. It does not imply that IPCR channels
have completed START commands, that QMI has discovered WLFW or that WLAN is up.

`step(now_ms)` does bounded work without busy waiting or internal sleeps.
The owner supplies monotonic time and drives it from a serialized worker.
Each phase has a deadline (1–120,000 ms configured timeout); time regression,
timeout, SYS_ERR/RDDM, bad/unsupported execution environments and callback
errors retain the instance in FAULT. `EAGAIN` normally means pending progress;
an owner callback returning it still faults the engine, so also inspect status.

This boot engine is **not the full MHI runtime power machine**. Without an
explicit runtime handoff, ONLINE polling rejects a departure from AMSS/M0.
`hakios.mhi.runtime.c` can exclusively lease ONLINE, then drive conservative
M0/M3 suspend/resume, wake references and failed-epoch retirement. While leased,
boot polling/status return `EALREADY` without MMIO and destroy returns `EBUSY`.
Autonomous M1/M2, warm reset/restart and RDDM collection remain unsupported.

The worker may be scheduled by state/EE events, but do not invoke boot steps
inline from a locked event-ring dispatch callback: boot `ready()` can acquire
event-ring locks. Stop/join worker/API users before destruction. `quiesce()`
must guarantee no remaining device access or callback before any buffer or
mapping is released. Failure retains the handle for cleanup retry. No failed
publication is automatically repeated, rolled back or declared harmless.

## QRTR endpoint and QMI binding

The scope is deliberately narrow: one local port, one direct peer and WLFW
service `0x45`, version 1, instance 1, as used by QCA6390. The caller supplies
exclusive local node/port IDs; the peer ID is learned from HELLO, not guessed
from the unrelated RC2 modem. There is no forwarding, wildcard service lookup,
multiple-client registry or generic QRTR nameservice daemon.

`start()` sends HELLO. A valid peer HELLO is replied to once and establishes
the link. NEW/DEL_SERVER, DEL_CLIENT and BYE update service presence; duplicate
announcements are idempotent. Unrelated well-formed services are consumed but
not registered. Foreign routes, malformed framing and unsupported control
actions return errors rather than silently claiming support. `lookup()` is
a local filter over peer announcements, not a fabricated remote subscription.
Only the exact supported WLFW lookup succeeds.

`hakios_qrtr_mhi_endpoint()` supplies instance-aware open metadata plus the
existing port operations. Use `hakios_qmi_handle_init_endpoint()` with it.
The old `hakios_qrtr_ops` layout and legacy initialization API are preserved.
The adapter never calls `hakios_qmi_set_transport()`. Cached service presence
is delivered by the owner's subsequent `poll()`; future announcements trigger
callbacks on receive. The owner must keep driving poll, not assume a hidden
kernel thread exists.

The required `transmit()` callback must copy/retain the complete frame into
owner-controlled IPCR channel-20 DMA storage before returning success. It may
not borrow the codec scratch buffer, invoke RX/completion callbacks inline or
claim completion merely because it has an address. The actual transfer-ring
owner must retain that storage through DMA completion. `EAGAIN`/`ENOSPC` mean
**nothing was queued**; any uncertain/published MHI queue error must instead
be reported as fatal. In particular, consult the underlying ring's health
before forwarding an `ENOSPC` callback error as ordinary backpressure.

Channel-21 completions feed `receive()` only after CPU ownership is restored.
The payload is borrowed until the callback returns. If receive returns
`EAGAIN` before delivery, the worker must retain the frame and retry rather
than recycle/drop it. This DMA-buffer/worker binding is still an owner
integration requirement, not a new runtime path supplied by the router.

DATA uses QRTR v1 with a maximum payload of 8160 bytes. The endpoint requests
confirmation at packet five and stops at ten unacknowledged packets for the
server. Only matching peer/server/local route RESUME_TX can reset the window;
unsolicited credits before a confirmation request are rejected. Controls do
not consume DATA flow credits. The wire has no flow epoch/ACK sequence, so
arbitrary replay after a reused window cannot be detected locally.

RX RESUME_TX is scheduled **after** the application consumes DATA. A deferred
ACK is retained through TX backpressure; while one is pending, later RX frames
are not delivered. `poll()` flushes that ACK, then pending DEL_CLIENT notices,
without redelivering DATA. A fatal callback error prevents further transmission.

Callbacks are serialized by a separate dispatch mutex, but run without the
router state mutex. Sending/lookup from callbacks is allowed. Close from
another thread waits for current dispatch; native port self-close prevents
further callbacks without deadlocking. Recursive receive/poll/disconnect is
rejected. This does not relax QMI handle lifetime requirements: do not release
the QMI handle itself from its own callback; stop its transaction/users first.

Close permanently retires this endpoint's one port and schedules DEL_CLIENT.
Reopen requires a fresh endpoint/epoch. `disconnect()` lets the MHI worker
mark a lost/reset link offline and notify service removal without inventing a
received BYE; it does not stop DMA itself. BYE/disconnect cannot be undone by
another HELLO. Close handles, stop/join workers, then destroy; failed owner
quiescence retains resources. Pending ACK/close notices block normal destroy
until flushed, but a disconnected/faulted link can proceed to owner quiescence.

## Offline validation and next integration

The source tests run without a phone or downloaded kernel sources:

- `test_mhi_boot.c`: separate CPU/device image bytes, real 44-byte context-array
  sizes, high/low register ordering/readback, 1-byte and 4,364,432-byte synthetic
  images, invalid overlaps, stale BHIE sequence, timeouts, device errors and
  600 injected callback/quiescence failures with before/after effects.
- `test_qrtr_mhi.c`: HELLO retry, cached/dynamic WLFW service discovery, five/ten
  flow limits, deferred ACK, disconnect, malformed/foreign messages, fatal TX,
  failed teardown, close-vs-callback concurrency and a real native QMI encoded
  request/decoded response against a memory-backed peer.
- The existing canonical QMI source also passes its 59 no-ath11k regression
  checks. Tests verify the new endpoint does not replace the global transport.

The peer test is not an actual WLFW firmware exchange. The real DMA/interrupt
owner and scheduler, channel START/STOP/RESET sequencing, platform power/reset
recovery, ath11k WLFW/M3/board/calibration transactions and PCI probe/remove
integration remain required. `IPCR-RUNTIME.md` describes the implemented
buffer bridge, conservative PM core and explicit PCI-QMI binding. Existing
runtime gates and LinuxKPI's MHI fail-closed patch stay unchanged.

## Reference locations

- Supplied kernel: `drivers/bus/mhi/core/mhi_boot.c`, `mhi_init.c`,
  `mhi_internal.h` and `include/linux/mhi.h` for BHI/BHIE programming, context
  setup, execution environments and device states.
- Supplied kernel: `net/qrtr/qrtr.c` for DATA confirmation watermarks and
  RESUME_TX meaning.
- The locally available Linux release pinned by `tools/mk-ath11k.sh`:
  ath11k `mhi.c`, `core.c`, `pci.c` and `qmi.h` for the QCA6390 FBC and WLFW
  profile; `net/qrtr/ns.c` for local lookup/HELLO/service announcements.
  Neither the ignored source extraction nor its archive is a dependency of
  these production units or offline tests.
