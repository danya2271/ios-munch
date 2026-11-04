# Munch Wi-Fi bring-up

## Current state

Latest software stage: `hakios.mhi.worker.c` supplies the native IRQ/deferred
worker; `wifi/lx/src/lx_qca_pci.c` supplies a conditional ath11k core
probe/remove adapter with retained, fenced rollback. Endpoint DMA/cache,
granted-MSI allocation and native PCI HIF/CE/DP software are now present; see
`ENDPOINT-DMA-HIF.md`. A preallocated, range-aware LinuxKPI DMA/linear-SKB
binding and explicit runtime composition now exist; see `ATOMIC-DMA-RUNTIME.md`.
`tools/mk-ath11k.sh` now has explicit `AHB` and `PCI` profiles; the latter links
the native entrypoint and runtime composition. There is still no real RC0/SMMU/GIC
owner, so the XNU platform hook returns `ENOTSUP` and Wi-Fi remains disabled.
See also `MHI-WORKER-PCI.md`.

The native wired/coherent buffer provider now uses XNU VM/pmap, including
attribute restoration and an explicit endpoint constructor. This removes the
need for a fake pinned-memory callback, not the missing platform grants or
hardware coherency validation. See `WIRED-DMA-MEMORY.md`.

The phone uses a QCA6390 HW2.0 WLAN device on the Kona PCIe/MHI path. The
existing `wifi/lx/src/lx_bus.c` backend is the Redwood WCN6750 AHB path: it
selects `ATH11K_BUS_AHB`, WCN6750 hardware parameters, and fixed SoC register
windows. That path must not run on Munch. `hakios.wifi.c` now identifies the
Munch device-tree model and refuses the legacy path even if
`wifi_wcn6750=1` is supplied.
The exported `hakios_lx_start()` boundary refuses the Munch model when the
linked object is the AHB profile, so a direct caller cannot bypass the boot-time
check and start the Redwood bus. The PCI profile reaches its native entrypoint
but stops at the mandatory platform-owner hook. `hakios_lx_prepare()` applies
the same profile guard before allocating the legacy backend's radio DMA pool.
The opt-in `wifi_qca_probe=1` entry point now refuses
the probe with `ENOTSUP` before mapping or accessing RC0, because no verified
host ownership provider exists. Ordinary `wifi=1` also does not touch RC0.
Probe and claim now acquire an endpoint lease through `hakios.pcie.c`, not
through raw RC0 mappings in the QCA driver. The platform model and an
apparently live link are not ownership evidence.
`tools/mk-ath11k.sh` now treats unresolved ath11k functions as a hard error
instead of generating success-shaped fallback stubs. Use
`ATH11K_PROFILE=PCI tools/mk-ath11k.sh` for the native QCA object; the default
`AHB` profile remains the Redwood build.

The former inline RC0/iATU mapping path has been removed from the QCA driver.
The native endpoint service serializes access through a provider, validates
identity/BAR resources/RID/SID, rejects acquisition with Bus Master already
enabled, and uses a 16-bit PCI command access to enable memory decode. It
retains faulted ownership if release fails, and old lease identifiers cannot
be reused. A separate Kona CFG0/iATU transaction engine now saves/restores
region 0, bounds polling, preserves native config access widths, and retains
ownership after uncertain drain/restoration. A mapped Kona backend now
provides abort-safe config/BAR accesses and CFG completion draining behind an
explicit owner token. No concrete Kona owner is registered: firmware handoff,
power/clock/reset/PHY sequencing, SMMU context ownership, and MSI ownership
remain missing. The CRC8 RID `0x100` -> SID `0x1c01` PARF table operation is
implemented with full snapshot/restore, but has no cold-owner caller. The
components have no boot-time caller; see `PCIE-KONA-CONFIG.md` for their
contracts and cleanup lifecycle.
See `PCIE-HANDOFF.md` and `PCIE-NATIVE-API.md` for the contract and limits.

Claim validates the owned 1 MiB BAR0 and the MSI capability without enabling
Bus Master. It is not wired into `wifi=1`. QCA register reads/writes now have
errno-returning entry points, with serialized window selection at `0x310c`
and mandatory provider wake handling for offsets at or above `0xfe0`.
No raw BAR pointer is exposed. DMA-domain ownership and MSI routing remain missing
preflight requirements: resolving RC0 ownership alone must not enable the
unverified SMMU or MSI groundwork. DMA/MSI requests currently fail and clear
their output addresses/data; the LinuxKPI boundary refuses Munch radio-memory
allocation before allocating memory and refuses IRQ attachment before GIC
access. Its raw physical register-mapping API also refuses Munch. No BAR, MSI,
or DMA path has been validated on-device under XNU.
The old MSI helper no longer manufactures a message from a mapped page and
an expected INTID; it returns failure until real allocation/routing exists.

Independent transport components now implement checked 32-message MSI
capability programming/restoration, IPCR 20/21 MHI transfer rings, primary
command and control/data event rings, completion accounting, a BHI/BHIE
cold-boot state machine, and a QRTR WLFW endpoint with per-handle QMI binding. They
are not wired into this runtime: the MSI engine requires already-owned IRQ
routes and a DMA-mapped doorbell, the MHI ring core requires an actual running
controller. A new IPCR DMA-buffer/deferred-worker bridge connects those rings
to QRTR, with conservative M0/M3 runtime ownership, failed-epoch retirement
and an explicit per-handle ath11k PCI QMI hook. A native thread-call scheduler
now provides IRQ/deferred-worker dispatch, but actual DMA/IRQ/controller owners,
platform power/reset recovery, real firmware execution and full ath11k PCI
integration remain missing. See `TRANSPORT-CORE.md`,
`MHI-CONTROL.md`, `MHI-BOOT-QRTR.md`, `IPCR-RUNTIME.md` and `MHI-WORKER-PCI.md` for exact contracts and
offline coverage; these helpers do not clear any QCA readiness gate.

A separate native stage-1 DMA page-table core now provides scatter-page
map/unmap, explicit AP/MAIR/XN encoding, child-before-parent synchronization,
and retained storage on failed TLB completion/quiescence. Its offline tests
include a stale-TLB case and a 64 MiB arena. This is not wired into the legacy
QCA SMMU path and does not satisfy DMA preflight. An independent address-space
manager now allocates aligned/boundary-limited IOVAs, tracks non-reused mapping
leases, and prevents address reuse after uncertain map/unmap or teardown.
Actual SID/context ownership, a granted IOVA aperture and hardware callbacks
are still missing. Wired/coherent buffer implementation is now available but
not integrated into a registered owner or validated on hardware. See
`DMA-PAGETABLE.md`, `DMA-SPACE.md` and `WIRED-DMA-MEMORY.md` for the profile
and lifecycle.

The firmware staging and name mapping are prepared for ath11k's QCA6390
directory. The three expected device files are `amss20.bin`, `m3.bin`, and
`bd_l11rgl.elf`; the last is exposed to ath11k as `board.bin`. The QCA6390
HW2.0 profile in the pinned ath11k source does not request `regdb.bin`, although
the Android CNSS firmware loader does request it. Firmware preparation alone
does not make the device usable: `wifi=1` remains unsupported until the
PCIe/MHI transport is implemented.

## Android observations

The supplied Android sysfs output identifies RC0 function `0000:01:00.0` as
Qualcomm `17cb:1101`, revision `01`, class `028000`. This matches the QCA6390
PCI ID in the pinned ath11k source and the vendor CNSS ID table. BAR0 is
`0x60300000-0x603fffff` (1 MiB); sysfs reports resource flags `0x140204`, and
the remaining BARs are empty. The boot log places it in IOMMU group 16 with
reserved-memory node `cnss_wlan_region`. CNSS loads `qca6390/amss20.bin`,
`m3.bin`, `regdb.bin`, and `bd_l11rgl.elf`; the vendor `qcacld` driver creates
`wlan0`. These observations establish the Linux/vendor path only; they do not
establish that XNU inherits a live PCIe link, BAR ownership, SMMU mappings, or
MSI routing.

The files on the phone are `amss20.bin` 4,364,432 bytes, `m3.bin` 266,684
bytes, `bd_l11rgl.elf` 58,000 bytes, and `regdb.bin` 19,348 bytes. In the pinned
ath11k QCA6390 HW2.0 profile, `supports_regdb` is false, so ath11k does not
request the regulatory blob; retain it as a vendor-path reference rather than
making it a required ath11k image input.

The same log shows a different endpoint on RC2 (`0002:01:00.0`) under MHI,
requesting `sdx55m/sbl1.mbn`. Do not treat that RC2 MHI endpoint as the WLAN
transport. The RC0 QCA6390 has its own MHI controller inside its PCIe BAR; the
pinned ath11k `mhi.c` configures that controller against the same PCI device.

## RC0 access requirements

Kona describes RC0's configuration aperture at `0x60100000`, but it is not a
flat ECAM window. The Kona `pci-msm.c` host driver programs iATU region 0 for
the bus-1 configuration aperture and, after enumeration, assigns regions by
endpoint index to configuration pages at `axi_conf->start + index * 4 KiB`.
Transactions target `(bus << 24) | (devfn << 16)` with CFG0 for bus 1 and CFG1
for later buses. The new native Kona engine implements region-0 save/restore,
bounded polling and readback verification for RID `0x100` only; it deliberately
does not expose CFG1 or arbitrary BDFs. The future provider must still supply
real ownership, completion and hardware-abort handling; a raw read from
`0x60100000` is not a valid endpoint probe. Neither the native service nor
this transaction engine implements the missing firmware handoff.

The vendor `iommu-map` assigns requester ID `0x000` to SID `0x1c00` and
requester ID `0x100` to SID `0x1c01` (`qcom,smmu-sid-base = <0x1c00>`).
The live log's IOMMU group confirms that Linux attached the endpoint, but does
not prove which translation context XNU may reuse. Likewise, the PCIe MSI node
lists SPIs 768-799, while ath11k's QCA6390 assignment divides its 32 vectors
into MHI 0-2, CE 3-12, WAKE 13, and DP 14-31. The existing Redwood AHB MSI
helper is not evidence that those PCIe routes or message data are configured
for XNU. The local Linux GICv3 translation adds 32 to each DT SPI number,
yielding hardware INTIDs 800-831 for this node. `pci-msm-msi.c` composes message
data from the parent GIC hardware IRQ, not a Linux virtual IRQ or the MSI
vector index. The corrected decode of the collected endpoint contains payload
base `0x0320` (800) and 32-bit address `0xa0000040`; these are resident Linux
configuration values, not an XNU allocation grant. Its message address comes
from `dma_map_resource()` for the endpoint; it is not necessarily the physical
doorbell address. This source derivation explains the expected INTID topology
but does not allocate or validate XNU IRQ routes, PCI MSI configuration, or the
doorbell IOVA.

The log also shows WLAN link retraining/reset activity and a CNSS QMI server
disconnect/restart. Thus successful Linux enumeration and `wlan0` creation
are not proof of a stable firmware session, much less a successful XNU port.

## Required backend work

The QCA6390 path needs all of these before it can safely share the current
mac80211/station layer:

1. Discover the endpoint through the active PCIe host bridge and validate the
   QCA6390 vendor/device and revision IDs. Do not assume Android left the link,
   BARs, or endpoint power state in a reusable configuration.
2. Implement RC0 endpoint/config-space ownership and BAR mapping, then
   QCA6390 windowed register access, MHI controller startup/shutdown,
   channel/event rings, doorbells, state transitions, and error recovery.
   QCA6390 uses BAR register `0x310c` for the window mapping; the pinned ath11k
   PCI profile assigns 32 MSI vectors across MHI (3), CE (10), WAKE (1), and
   DP (18). The current LinuxKPI MHI patch fails unsupported operations
   closed; it is not an MHI implementation. The imported LinuxKPI PCI API is
   backed by FreeBSD `device_t` and cannot be used as XNU's endpoint-
   enumeration layer.
3. Provide DMA allocation and mapping with the correct SMMU stream IDs and
   cache ownership, plus PCIe MSI allocation/routing and interrupt teardown.
   Existing AHB radio-memory and GIC-MSI helpers are not a substitute for these
   contracts.
4. Supply the real IPCR DMA/IRQ owner and complete the PCI HIF/CE/DP backend
   required by the native worker and ath11k core probe/remove adapter for
   QCA6390 HW2.0. The native IPCR bridge and PCI-QMI
   binding are separate from the existing WPSS/GLINK transport; neither
   redirects its global router nor creates a native PCI bus by itself. Retain the existing
   WMI/HTC/CE/data-path pieces only after their PCIe dependencies are real.
   Unknown MHI, PCI, DMA, or interrupt operations must return errors, never
   success-shaped stubs.
5. Resolve the PCIe handoff and resource-ownership contract before any hardware
   access, including the diagnostic probe. Live observation or validation of
   clocks, resets, regulators, GPIOs, and SMMU state requires separate user
   authorization; no device operations are part of the current source work.

The vendor tree has Kona PCIe and QCA6390 reference material, but the XNU
kernel still lacks an active Munch PCIe owner and an MHI controller backend
for this driver. `kona-mtp.dtsi` and the vendor connectivity driver are references, not
proof that the live Munch endpoint has those exact resources or is powered.

## Validation gate

Keep the Munch path disabled until a QCA6390-specific backend can prove
enumeration and resource ownership, then boot firmware and pass the ath11k
QMI/HTC initialization sequence, then transmit and receive traffic, and finally
survive stop/restart and suspend/resume. None of those runtime checks has been
completed. Do not use the legacy `wifi_wcn6750=1` diagnostic switch on Munch.
The current `wifi_qca_probe=1` implementation fails closed; this is not
permission to enable that option in an image or run it on the phone. Authorized
offline PCIe, DMA and transport tests pass under GCC/Clang with ASan/UBSan,
including 376 PCIe, 24 DMA-table, 54 DMA-address-space, 378 MSI, 36 MHI transfer,
118 MHI command/event, 600 MHI boot, 30 runtime-PM and 160 IPCR callback-failure
cases per run. IOVA
model checks, concurrent mapping/queue/event tests, QRTR flow/lifetime tests,
a native QMI request/response model and 59 legacy QMI regressions also pass.
All sixteen checked native files compile
for ARM64. The latest isolated full kernel build completes successfully
against the current staged tree, superseding the
earlier snapshot's post-link export-check failure. This does not validate
hardware handoff, DMA coherency or Wi-Fi runtime. The actual pinned ath11k
`qmi.c` also compiles separately for PCI with the new IPCR import and for AHB
with its unchanged legacy imports; this is not a complete PCI driver build.
See `PCIE-VALIDATION.md` for results, the corrected QCA allocation tag, and
remaining limitations. No device test has been run.
