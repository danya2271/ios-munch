# QCA6390 MHI command and event rings

## Scope

`xnu/files/bsd/dev/hakios.mhi.control.c`, with the public API in
`sys/hakios.mhi.h`, implements native command accounting and event consumption.
It uses the same byte-oriented little-endian ABI as `hakios.mhi.c`. It has no
MMIO, physical-address mapping, IRQ registration, firmware loader, timer or
controller power implementation. There is **no runtime caller** and no newly
satisfied QCA preflight requirement. This is not evidence of working Wi-Fi.

The reference is the supplied `xiaomi_sm8250_kernel` tree:

- `drivers/bus/mhi/core/mhi_internal.h`: 44-byte contexts, 16-byte TREs,
  one primary command ring with 128 elements, command/event encodings.
- `mhi_init.c`: initial RP/WP at ring base, valid event contexts and MSI fields.
- `mhi_pm.c`: initial event WP at the last slot, leaving one slot unavailable.
- `mhi_main.c`: event RP is device-produced; host WP returns credits;
  command completions must name the oldest outstanding command.
- The QCA6390 profile in ath11k `mhi.c`: 32 control events, 256 data events,
  relative MHI IRQs 1/2, moderation values 0/1, IPCR channels 20/21.

The ignored extracted ath11k directory is a reference, not a required source
or test dependency. Production code and host tests live outside that directory.

## Ownership and contexts

Creation requires exclusive, sufficiently sized, pinned CPU storage and a
valid owner-issued DMA IOVA. The device must not access the storage yet.
The constructors zero it; they **cannot import a live firmware-owned ring**.
The base must be nonzero, aligned to the complete ring's byte size and not
overflow the IOVA integer. CPU storage must be at least 16-byte aligned.
The owner must also reject physical aliases and overlapping DMA allocations
across all channel, command, event, context and data-buffer allocations.

Context builders produce the 44-byte packed ABI with base, byte length and
initial RP/WP. The command context's first 12 bytes remain zero. Event
contexts encode valid type, relative MSI vector and moderation. Both initial
pointers equal base; `event_start()` publishes the last slot separately.
Builders neither install contexts nor claim hardware resources. Context
storage and shared RP/WP cache-line ownership are the controller's job.

The event and command operations are version 1. All operations run under a
per-instance sleepable mutex; none may be called from hard IRQ context. Owner
callbacks must not reenter their ring or wait for a thread needing its lock.
Event dispatch may call a **different** command/transfer ring, with lock order
event -> command/transfer. The reverse dependency, including from publication
or quiescence callbacks, is forbidden. Join API users before destruction.

## Command submission and completion

Only START (`0x12`), STOP (`0x11`) and RESET (`0x10`) for IPCR channels 20/21
are accepted. Cookie zero is invalid; a pending duplicate cookie returns
`EEXIST`. A second outstanding command for the same channel returns `EBUSY`.
The 128-element ABI keeps one slot empty, but this restricted profile allows
at most **two pending commands**, one per supported channel.

Submission reserves metadata, writes the TRE, calls `sync_ring()` and then
`publish(next_wp)`. The owner must already have installed the command context
and established legal register/power access. Publication must synchronize the
context WP, apply DMA/MMIO ordering and ring the command doorbell, upper word
before lower word. A successful submission is not command completion or
proof that a channel is running. `command_status()` reports software health.

Completion requires event type `0x21` and a pointer exactly equal to the head
of the outstanding queue. Channel and opcode are recovered from retained host
metadata, not the command event's unused channel byte or device-mutated ring
storage. Success code 1 returns the cookie/channel/opcode and retires one slot.
Wrong, duplicate, empty, unaligned, out-of-order or foreign pointers fault the
ring. All non-success command codes also fault it without reporting completion
or releasing the pending metadata. A future controller must log the raw code
and perform actual error recovery; no automatic retry is safe here.

The core does not implement command timeouts or channel state transitions.
After a timeout, owners must stop submissions and quiesce/reset/drain before
destruction/recreation. A RESET completion alone is not proof that old transfer
events have been drained or that backing pages may be freed.

## Event consumption and credits

Control ring 0 contains 32 elements (512 bytes); data ring 1 contains 256
elements (4096 bytes). `event_start()` synchronizes the whole initialized ring
for the device, reads the initial device RP and requires it to equal base,
then publishes WP at `base + length - 16`. This grants 31 or 255 credits.
Calling start twice returns `EALREADY`. An initial RP check does not establish
ownership; a live ring must not reach creation/start in the first place.

`event_poll()` takes a nonzero budget smaller than the element count and
samples the device RP once. `read_pointer()` must synchronize the shared
context and provide an ordered, consistent RP snapshot after the device's TRE
writes. The core validates alignment, bounds and producer advance against
remaining credits before accessing any element. It processes only that bounded
snapshot, even if firmware produces more events during dispatch.

Each event follows this order:

1. Synchronize its 16-byte slot for CPU access, then copy it into local storage.
2. Validate routing: state `0x20`, command `0x21` or execution-environment `0x40`
   on control ring 0; transfer `0x22` for channel 20/21 on data ring 1.
3. Call `dispatch()` with the snapshot. The owner must reserve delivery space
   before consuming downstream state, then handle or durably enqueue the event.
4. On success, advance the host consumer, return cache ownership for that slot
   and publish the next credit WP, which names the **just-consumed slot**, not
   the new consumer RP. The previous sentinel becomes available to the device.

After initial zeroing the CPU only reads event memory. Runtime synchronization
must not write stale cache lines over device-produced neighboring events.
Partial-slot ranges may share cache lines: real DMA owners must use coherent
memory or correct direction-aware synchronization, not naive clean/writeback.
This also applies to packed shared contexts where device RP and host WP can
share a line. The host model is not an ARM coherency validation.

Only `dispatch()` may return `EAGAIN`/`ENOSPC` as backpressure: it must have
made **no externally visible progress**, and the core retains that event
without returning a credit. An undersized transfer-completion output array
is tested through this path. State/EE events are only routed; the owner must
validate values and implement the actual state machine. Unknown/unimplemented
types and wrong-ring events fault rather than being silently discarded.

`dispatched` counts successful handler returns, including those **before a
later error**. A cache-return or WP-publication failure can return nonzero
`dispatched` with an error; handlers must not be replayed. The ring faults and
retains all storage, even if a failing publication already reached the device.
A failing handler can also have unknown effects: outside the strictly defined
backpressure contract it is never retried. Other callback failures, including
`EAGAIN`/`ENOSPC` from sync/RP/publication, are fatal; query status rather than
assuming every such errno is safe backpressure.

The ring tracks previously observed producer progress and unread events to
reject observed overruns/regressions. A pointer-only ABI cannot detect every
malicious in-range pointer, an unobserved complete lap or a replay naming a
reused slot. Hardware isolation, producer ordering and reset-epoch draining
remain mandatory owner responsibilities.

## Failure and teardown

No operation automatically clears a fault or rolls back a partially published
descriptor. Only teardown may proceed. The controller must stop producers
and dependent submissions after any ring fault; another ring's healthy status
does not make the controller safe. `event_status()` returns `EAGAIN` before
start, zero after software start and `EIO` after a fault, not firmware readiness.

Destroy always calls owner `quiesce()`, even if empty or never started. The
owner must stop/drain device access, detach ring contexts/doorbells, prevent
new dispatch and synchronize workers without violating the lock order.
Pending command/transfer lifetimes and completed-but-uncredited events must
be resolved before releasing DMA mappings. A failed quiesce retains the handle
and memory obligations for a later cleanup attempt. Successful destroy frees
only host metadata, not caller-owned storage or DMA mappings.

## Offline checks and remaining work

`tools/pcie-test/test_mhi_control.c` links the real command/event and transfer
engines. Its independent model separates CPU/device-visible TRE bytes, enforces
publication ordering and event credit limits, and checks:

- Packed/unaligned context bytes, rejected arguments and allocation failures.
- Command wraparound, channel serialization, stale/out-of-order completions
  and command errors; four concurrent submitters.
- Full, wrapped and bounded-snapshot event consumption, backpressure without
  early credit return, invalid RP/type/channel and observed overrun/regression.
- 118 injected callback failures per run, including before/after effects,
  negative errno normalization, partial progress and failed/retried teardown.
- Control events into real command accounting, data events into real batched
  transfer completions, insufficient result capacity and duplicate events.
- Four concurrent event consumers with no duplicate delivery.

These ring tests do not register a controller, deliver MSI or execute firmware.
`MHI-BOOT-QRTR.md` describes the separate BHI/BHIE boot-state engine and WLFW
endpoint now available. Next work still requires RC0 ownership and endpoint
DMA/cache/IRQ providers, IPCR buffer and channel lifecycle integration, a
power/error/IRQ worker and ath11k PCI lifecycle. Runtime gates and the LinuxKPI
MHI fail-closed patch remain unchanged.
