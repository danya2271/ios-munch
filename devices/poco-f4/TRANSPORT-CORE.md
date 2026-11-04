# QCA6390 MSI, MHI/IPCR and QRTR components

## Scope and runtime gate

Six transport translation units and the per-endpoint QMI binding compile into XNU:

| Component | Implemented | Not implemented here |
| --- | --- | --- |
| `hakios.msi.c` | 32-message PCI MSI capability programming, verification, message layout and retryable restoration | IRQ allocation/attach, GIC routing, doorbell DMA mapping and a real RC0 owner |
| `hakios.mhi.c` | QCA IPCR transfer rings, channel-context/command encoding, ordered publication and completion accounting | Real channel/controller lifecycle and DMA owner |
| `hakios.mhi.control.c` | Command submission/completion and bounded control/data event consumption, credit recycling and retained teardown | Real IRQ worker and channel lifecycle owner |
| `hakios.mhi.boot.c` | BHI/BHIE programming, firmware vectors, context/aperture setup and bounded PBL-to-AMSS/M0 boot states | Real MMIO/DMA owner, runtime power/recovery machine and firmware execution on hardware |
| `hakios.qrtr.wire.c` | QRTR v1 transmit, v1/v3 receive, control messages and RESUME_TX encoding | Codec only; no routing actions |
| `hakios.qrtr.mhi.c` | Single-peer WLFW endpoint, HELLO, local lookup, flow windows, ACK/close/disconnect and per-handle QMI binding | Real IPCR DMA-buffer/worker bridge, general QRTR routing and ath11k integration |

These are functioning software components, **not a complete MSI/MHI/QRTR
runtime**. No caller registers a real RC0 provider, clears QCA DMA/MSI preflight,
sets Bus Master or starts them from `wifi=1`. LinuxKPI's MHI fail-closed patch
remains in effect. The existing GLINK QMI transport is not replaced. No device
access, boot flag changes or flashing were performed.

## MSI capability engine

`sys/hakios.msi.h` accepts an already-owned message block: endpoint-visible
doorbell IOVA, base data and 32 parent GIC hardware INTIDs. It does not turn
physical `0x17a10040` into a guessed DMA address or allocate IRQs from integers.
The future owner must retain the doorbell's DMA mapping and page offset for
SID `0x1c01`, exclusively own the capability and reserve/attach all routes.
Routes must initially be masked and the endpoint quiescent. Ownership remains
with the caller until cleanup and explicit resource release.

Kona's DT SPI 768–799 translates to hardware INTID 800–831. The Linux reference
uses the parent GIC hardware IRQ as payload and `dma_map_resource()`'s result
as message address. The corrected decode of the collected endpoint is 32-bit
address `0xa0000040` and payload base `0x0320` (800), with 32 enabled vectors.
Those values describe the resident Linux configuration, not an XNU ownership
grant; they must still be obtained from the future GIC/MSI owner at runtime.
The engine validates the supplied contiguous, aligned 32-vector payload block
and an independent INTID array; it does not manufacture either grant or
hardcode `800` as an allocated payload. A test uses different payload and INTID
blocks to detect accidental coupling.

The supported capability is conventional 64-bit MSI, MMC=5 (32 messages),
with or without per-vector mask bits. MSI-X, 32-bit address format and smaller
blocks return errors. Capability discovery/chain validation belongs to the
owner; the engine checks the supplied aligned offset, ID and extent. An
already enabled MSI capability is rejected, not taken over from firmware.

`create()` snapshots disabled control, address, data and optional mask without
writing. `start()` rechecks the snapshot, masks vectors where supported,
programs the IOVA/data, sets MME=5 and enable, unmasks device vectors and reads
back every programmed field before asking the IRQ owner to arm delivery.
The owner must handle pending delivery correctly; `arm()` is not a fake
allocation callback. All PCI transactions must use the serialized native
CFG0/iATU provider and its completion/abort guarantees. Control/data use native
16-bit accesses; no PCI command or adjacent status word is written.

Messages become available only after successful start:

| User | Relative vectors |
| --- | --- |
| MHI | 0–2 |
| CE | 3–12 |
| WAKE | 13 |
| DP | 14–31 |

Any start error faults the instance and clears message outputs. There is no
automatic start retry. `destroy()` first requires owner quiescence: stop/drain
interrupt sources, mask routes and synchronize handlers. It then restores the
disabled capability snapshot with readback. A failed quiesce, config access
or verification retains the handle and all external resource obligations for
another cleanup attempt. No route or doorbell mapping may be freed merely
because MSI programming failed. Successful destroy frees engine metadata;
the caller separately releases its masked IRQ routes and DMA resources.

The per-instance sleepable mutex serializes operations. One instance may own
one capability; independent instances must not share it. IRQ handlers and
owner callbacks must not reenter this API, including during `arm()` or
`quiesce()`. The owner already has its routing data and must not wait for a
handler that needs this mutex. Stop/join API users before destruction.

## MHI IPCR transfer core

Only QCA6390's IPCR channels are supported: channel 20 outbound, channel 21
inbound. Each ring contains 64 little-endian 16-byte TREs, with one slot kept
empty (63 outstanding transfers). Its 1024-byte IOVA range must be aligned
to ring size. The caller supplies exclusively owned pinned CPU storage and
its real DMA address. Creation zeros storage before any controller may use it;
it cannot import a live firmware ring.

The channel-context builder emits the 44-byte ABI: disabled state, burst mode
disabled (`2`), outbound/inbound type, event ring 1, base/length and initial
RP/WP. START/STOP/RESET command TRE builders encode the IPCR channel and opcode.
These builders **do not execute commands or mark channels started**. The
QCA profile still needs 128 channel contexts and event rings of 32/256 entries
using relative MHI MSI vectors 1/2; those controller resources are not created
by a transfer-ring helper.

Before queueing, a real controller owner must establish AMSS/M0, a successfully
started channel, pinned DMA buffers, and the required power/lifetime reference.
`status()` reports software health only. There is no ready flag that grants
access to a missing controller. The owner must synchronize initial ring/context
storage before attaching it; queueing only synchronizes changed TREs.

For each queue operation, the caller first records a nonzero buffer cookie and
keeps its DMA lease/backing storage alive. The core validates length (1–8192),
IOVA overflow, ring overlap, duplicate cookies and overlapping outstanding
IOVA intervals, then performs:

1. Data-buffer ownership/cache transition to the device through `sync_buffer`.
2. A single unchained transfer TRE with IEOT, then `sync_ring` for that TRE.
3. `publish(channel, next_wp)`, which the hardware owner must implement by
   updating/synchronizing context WP and ringing the channel doorbell with
   correct barriers and high-word-before-low-word MMIO ordering.

Callback completion is mandatory. A failed callback may have taken effect:
the ring faults and retains every cookie, buffer obligation and descriptor
slot until successful quiescence. Negative callback errors become `EIO`;
positive errno is preserved. Error codes alone are not proof of nonpublication.
Queue success means publication, not completed transfer or permission to
free the buffer. Physical aliases through distinct IOVAs remain an owner
responsibility; the ring cannot infer physical identity from IOVAs.

The event-ring engine synchronizes/snapshots an event TRE before its owner's
dispatch callback can pass a transfer event to `complete()`. The core validates event type,
channel, EOT/overflow completion code, aligned pointer inside the ring,
membership in the outstanding interval and returned length. It handles a
completion covering several queued TREs, including wraparound. Insufficient
result capacity returns `ENOSPC` without consuming anything; the worker must
not acknowledge/drop that event. All buffers transition back to CPU ownership
before completion results are released. Overflow is propagated to every
result covered by the event, matching the conservative Linux behavior.

Invalid/stale completion pointers and unsupported event codes fault the ring;
outputs are cleared and buffers remain retained. State/EE/command events must
be routed elsewhere, not passed to the transfer completion API. A reused slot
does not carry an epoch on the wire: the future controller must drain old
events before restarting/reusing storage. These checks cannot detect arbitrary
replayed events that happen to name a newly reused slot.

Destroy always requires owner quiescence, even for an empty ring: stop/drain
the channel, detach its ring and synchronize the event worker. Failure retains
the handle; retry is allowed. Success frees metadata only. All operations are
sleepable and serialized, not hard-IRQ/panic safe. Owner callbacks must not
reenter this ring or wait for its lock. Join API users before destruction.

## MHI command and event core

`hakios.mhi.control.c` implements the 128-slot primary command ring and the
32/256-slot control/data event rings. Commands complete strictly in order,
with at most one pending command per IPCR channel. Event polling validates
device pointers, bounds work to a producer snapshot, handles backpressure
without returning premature credits, and quarantines uncertain publication.
Dispatch can feed the existing command and transfer completion APIs; state/EE
events still need a real worker to schedule the boot/power logic. Context
builders and software start/status are not proof of firmware/power readiness.

See `MHI-CONTROL.md` for synchronization, initial and recycled WP semantics,
partial-progress reporting, lock ordering, failure retention and teardown.
These rings are not called by a real IRQ worker or QCA runtime yet.

## QRTR wire and control layer

The codec has no allocator, socket, global router or GLINK dependency. It
accepts unaligned byte buffers and uses explicit little-endian loads/stores.
Encoding produces QRTR v1's 32-byte header and zero-filled four-byte padding.
Decoding supports v1 and the 16-byte v2-layout header whose wire version is
**3**, including control-port normalization. Version-3 optional headers and
unknown flag/type combinations return an error rather than being ignored.

IPCR's 8192-byte transfer limit includes framing: v1 payloads are at most
8160 bytes, and v3 receive payloads at most 8176 bytes. No hidden fragmentation
or truncation is performed. A too-large v3 packet cannot be re-encoded as v1
without returning `EMSGSIZE`. Truncated, overlong, unaligned-length and
size-mismatched packets are rejected before exposing a payload pointer.

Supported controls are HELLO, BYE, NEW/DEL_SERVER, DEL_CLIENT, RESUME_TX and
NEW/DEL_LOOKUP. Controls require the 20-byte command body, matching header/body
type and appropriate destination port. Server instance encoding keeps the
version in the low 8 bits and instance in the upper 24 bits. EXIT, PING,
DEL_PROC and optional-header extensions are not implemented and do not return
success. Parsing a control does not perform its router action.

`wire_resume()` builds RESUME_TX only for DATA with `confirm_rx`, reversing
the route and identifying the consumed local node/port. The endpoint
sends it **after consumption**, not automatically on packet parsing. This
helper does not implement sender watermarks, blocking or retry policy.

Decoded payloads borrow the receive buffer; retain or copy them until the QMI
consumer is finished. Encoded frames must likewise remain DMA-owned until MHI
completion. There is no `hakios_qmi_set_transport()` call: the separate WLFW
endpoint binds an individual QMI handle with instance-aware open metadata.
See `MHI-BOOT-QRTR.md` for its restricted service scope, HELLO/flow control,
callback/close contracts and the still-required real IPCR DMA-buffer bridge.

## References and offline verification

Reference behavior was checked locally in the supplied kernel tree:

- `drivers/pci/controller/pci-msm-msi.c`: parent HW IRQ payload and DMA-mapped
  message address; `drivers/irqchip/irq-gic-v3.c`: DT SPI translation.
- `drivers/bus/mhi/core/mhi_internal.h`, `mhi_init.c`, `mhi_main.c`: context/TRE
  layouts, burst mode, descriptor publication and transfer completion.
- `net/qrtr/qrtr.c` and `include/uapi/linux/qrtr.h`: framing, control encoding
  and RESUME_TX direction.
- The ath11k QCA profile in the Linux release pinned by `tools/mk-ath11k.sh`:
  IPCR 20/21, ring sizes and MSI layout. The ignored extracted ath11k directory
  is a reference only, not a build/test input for these new components.

`tools/pcie-test/test_transport.c` compiles the real three C units against
memory-backed config/DMA models. It checks exact wire bytes, 32-vector layout,
maskable/non-maskable capability restoration, ignored writes, 378 injected
MSI callback failures, 36 MHI failures, full/wrapped rings, malformed/duplicate
events, four concurrent producers, QRTR payload lengths 1–8160 and malformed
frames. The IPCR model connects QRTR encoding, TX publication, RX completion
and RESUME_TX construction with separate CPU/device-visible buffers. It does
not emulate PCIe, GIC delivery, firmware boot or real QMI service discovery.

The separate `test_mhi_control.c` executable adds real command/event engines,
118 callback failures, credit/visibility checks, wrapped and bounded polling,
malformed device RP, backpressure, concurrent callers and event dispatch into
the actual command/transfer accounting APIs. It likewise uses no hardware.
