# Munch RC0 ownership and native endpoint contract

## Scope and current gate

This is a source-level contract for QCA6390 HW2.0 on Kona RC0, not a claim
that firmware has transferred any resources. Authorized local compilation and
host tests are recorded in `PCIE-VALIDATION.md`; no device operations have
been performed for this work.

`hakios_qca6390_preflight(required, missing)` is a software-only prerequisite
query. It returns `EINVAL` for a missing output, an empty request, or unknown
bits; `ENODEV` for another platform; and `ENOTSUP` while any requested
prerequisite is missing. RC0 can be reported ready only while QCA holds a
validated, healthy native endpoint lease. Registering a provider alone does
not make it ready. DMA/MSI always remain missing. There is currently no Kona
provider, so every valid Munch request still fails. The query itself performs
no MMIO, DMA allocation, interrupt programming, or firmware calls. The weak
`lx_os_qca_platform()` fallback invokes this preflight, logs the missing gate
mask, and returns its error; it does not fabricate a runtime owner.

The loader deliberately keeps the IPSW identity at the device-tree root
(`iPhone10,3`) so iOS selects the matching userspace and dyld cache. Hardware
identity comes from the loader-generated `/arm-io` node instead: `device_type`
is `munch`, and `ranges` is `{ 0, 0x00400000, 0x3fc00000 }`. XNU's QCA/Wi-Fi
platform discriminator uses those loader values, not the iOS product model.
This identifies the platform only; it is not an RC0 ownership or handoff grant.

| Requirement | Needed before | Currently provided |
| --- | --- | --- |
| `HAKIOS_QCA6390_RC0_HANDOFF` | RC0 mapping/access, CFG0, BAR claim | No |
| `HAKIOS_QCA6390_DMA_DOMAIN` | SMMU programming or endpoint DMA mapping | No |
| `HAKIOS_QCA6390_MSI_ROUTES` | PCI MSI programming, IRQ attach, returning a usable MSI message | No |

This query is not an ownership token. There is no boot argument, DT flag,
or readiness setter that clears a requirement. `hakios.pcie.c` now supplies
the native provider/lease boundary, but not a Kona hardware owner. Runtime
access goes through that boundary rather than inline QCA RC0 mappings.
Resolving RC0 handoff alone must not enable DMA/MSI. The API and provider
obligations are documented in `PCIE-NATIVE-API.md`.

The loader's read-only PCI inventory counts UEFI
`PciHostBridgeResourceAllocation`, `PciRootBridgeIo`, and `PciIo` protocol
handles and publishes those counts under `/chosen` for `wifi_qca_status=1`.
This discovers whether a standard firmware PCI interface exists. It does not
prove that its root bridge owns Kona RC0, that QCA6390 is behind it, or that
the interface remains callable/exclusive after XNU starts on another core.
Handle counts never clear the RC0 gate.

## What the boot sources establish

The checked sources are `xnuloader/loader.c`,
`devices/poco-f4/loader/checkpoint-loader.patch`, the Munch UEFI platform
driver lists, and `xnu/files/osfmk/arm64/loader_return.c`.

- `run_kernel()` leaves UEFI resident. It saves the loader context, cleans
  firmware memory, and enters XNU without `ExitBootServices()`.
- The actual return mechanism patches the `XNULDRST` slot in the loaded
  kernel with a return-stub address, checkpoint, and slide. The introductory
  comment in `loader_return.c` still mentions `/chosen/loader-return`; that
  comment is not evidence of a current PCIe handoff property.
- The loader can remain on another CPU while XNU runs. Its return path
  explicitly takes back UFS with `hc_take_back()`. No analogous RC0/QCA6390
  transfer and reclaim protocol was found in those sources.
- The Munch DXE/APRIORI lists include `XhciPciEmulationDxe`. That name does
  not establish ownership or initialization of the WLAN PCIe root port.
  Binary firmware may retain state not described by those lists.

Neither an Android WLAN session, a live link, an empty-looking SMMU context,
nor an absent visible UEFI WLAN driver authorizes XNU to change RC0. Disabling
the loader-return timer does not terminate firmware ownership. Do not add
`ExitBootServices()` as a shortcut: the loader and other device paths depend
on resident UEFI.

The r53 diagnostic boot performed only UEFI protocol-database lookups before
entering XNU. `PciHostBridgeResourceAllocation`, `PciRootBridgeIo`, and
`PciIo` each returned `EFI_NOT_FOUND` (`0x800000000000000e`) with zero handles;
XNU received and printed those values from `/chosen`. The Munch DXE/APRIORI
lists likewise expose only `XhciPciEmulationDxe`, not a PCI host bridge.
Therefore this firmware image offers no standard EFI PCI owner for a loader
lease. This does not prove that opaque firmware code never touched RC0, and it
does not authorize XNU cold initialization. The next ownership path must be
explicitly designed as either a platform-verified loader quiescence protocol
that excludes every other RC0 user, or a complete native Kona host owner with
documented power/reset/PHY/link sequencing. The bounded MMIO/config/BAR backend
now exists, but its authorization token is only as trustworthy as the owner
that supplies it. Do not enable the RC0 provider until ownership and cold-init
are proven.

## Resource inventory, not inherited state

Reference tree: `/home/danya2271/xiaomi_sm8250_kernel/`.
Resource source: `arch/arm64/boot/dts/vendor/qcom/kona-pcie.dtsi`.
Board overrides and the booted DT still need reconciliation before use.

| Resource | Kona RC0 reference |
| --- | --- |
| PARF | `0x01c00000`, size `0x3000` |
| PHY | `0x01c06000`, size `0x1000` |
| DBI / dm_core | `0x60000000`, size `0xf1d` |
| ELBI | `0x60000f20`, size `0xa8` |
| iATU | `0x60001000`, size `0x1000` |
| Config aperture | `0x60100000`, size `0x100000`; not ECAM |
| Memory window | `0x60300000`, size `0x03d00000` |
| Endpoint | `0000:01:00.0`, `17cb:1101`, revision `01`, class `028000` |
| Observed endpoint BAR0 | `0x60300000`, size `0x00100000` |
| PERST / wake | TLMM GPIO79 / GPIO81 in this DT |
| SMMU requester mapping | RID `0x000` -> SID `0x1c00`; RID `0x100` -> SID `0x1c01` |
| MSI doorbell physical address | `0x17a10040` |
| MSI interrupt specifiers | `GIC_SPI 768` through `GIC_SPI 799`, rising edge |

The same node lists regulators, PCIe GDSC, clocks, reset controls, pinctrl,
and a PHY sequence. Those dependencies belong to the host owner's lifecycle;
copying their addresses is not initialization. Reference enable, enumerate,
and disable paths are `msm_pcie_enable()`, `msm_pcie_enumerate()`, and
`msm_pcie_disable()` in `drivers/pci/controller/pci-msm.c`.

## Required native API boundaries

The provider/lease layer implements part of these boundaries; the hardware
provider, DMA/MSI owners, and MHI runtime remain missing. FreeBSD LinuxKPI PCI
objects do not supply XNU ownership.

1. **Host acquire/release.** A trusted boot/platform owner must transfer
   exclusive RC0 resources or provide a lease excluding firmware access for
   its entire lifetime. Establish power/clock/PHY/link state and previous DMA/
   IRQ quiescence through that owner. Keep shared clock/regulator references
   and a return-to-loader policy. DT addresses and model strings are not grants.
2. **Endpoint acquire/release.** Bind identity and BAR resources to domain 0,
   RID `0x100`, and SID `0x1c01`. Revalidate identity through the owned host.
   Keep Bus Master Enable clear until the DMA domain and device lifecycle are
   ready. Do not resize or relocate a live BAR as a probe.
3. **Config read/write.** Accept an endpoint lease, offset, width, and value.
   Use real byte/halfword/dword accesses with bounds/alignment checks. Serialize
   against all host config users, not just QCA callers. Bus 1 uses target
   `0x01000000` and CFG0. Preserve the owned iATU region across the complete
   transaction, bound enable/disable waits, and verify restoration. A restore
   failure must poison the transaction path and require owner recovery, not
   silently continue. The scoped MMIO abort helper covers only its own load/
   store instructions; the provider still needs completion/drain and an owner.
4. **BAR access.** Return a lifetime-bound owned-BAR mapping, not a raw physical
   mapping service. QCA6390 register access must serialize `0x310c` window
   selection with the subsequent access, handle wake/power requirements, and
   reject out-of-range offsets.
5. **DMA domain/map/unmap.** Bind SID `0x1c01` to owned stream matches, a context
   bank, page tables, and an IOVA range. A context-bank number is not a SID;
   finding an unused-looking bank is not permission to take it. Provide coherent
   and streaming APIs, cache transitions, TLB completion, rollback, and teardown.
6. **MSI allocate/release.** Reserve actual IRQ routes and obtain hardware
   message data from the GIC owner. DMA-map the doorbell for this endpoint,
   program its MSI capability, and implement mask/attach/detach lifecycles.
   Return usable messages only after those operations succeed.
7. **Stop/return.** Stop new work, quiesce MHI/CE/DP, prevent further DMA, and
   mask/synchronize interrupts before dismantling mappings. An unconfirmed
   stop or TLB timeout must retain/quarantine affected resources, not release
   them for reuse or return to firmware as though idle. Coordinate panic and
   timeout returns separately; mutex-based cleanup is not automatically safe.

The Kona source includes a serialized cold-owner lifecycle coordinator for
PERST, power, clocks/resets, PARF, PHY/pipe, link, SID, controller setup, and
endpoint validation. The mapped MMIO backend supplies access for a future
stage implementation and retains uncertain accesses. A rollback-safe PARF
SID-table primitive now maps RID `0x100` to SID `0x1c01`; it is not an SMMU
context mapping and is not wired into a cold-stage owner. The backend does not
sequence power/clock/reset/PHY stages or prove resident-loader exclusion. No
cold-stage owner is registered, so the coordinator remains unreachable from
QCA runtime and does not clear the RC0 preflight gate.

The provider adapter composes the cold-owner, CFG0 transaction engine, and
native endpoint lease, and can bind distinct mapped-MMIO contexts for config
and BAR access. It preserves RC0 ownership when the QCA endpoint lease is
released, and only explicit platform deactivation unwinds the host. It remains
unregistered and has no boot caller until a real Kona resource owner exists.

The ARM64 exception path now has a narrowly tagged recovery for synchronous
external aborts from the dedicated MMIO read/write instructions. It returns
`EFAULT` only when the saved PC matches that dedicated recovery entry and the
fault is not a stage-1 page-table walk; unrelated external aborts still take
the normal fatal path. These accessors now back the guarded MMIO mapping
service, but this does not grant RC0 ownership, make accesses safe while
another owner controls the block, or handle asynchronous SError.

## MSI derivation from the local Linux sources

1. `kona-pcie.dtsi` assigns RC0 MSI the 32 SPI specifiers 768-799.
2. `drivers/irqchip/irq-gic-v3.c:gic_irq_domain_translate()` adds 32 for a
   DT SPI. These are hardware INTIDs 800-831, not Linux virtual IRQ numbers.
3. `drivers/pci/controller/pci-msm-msi.c` obtains parent mappings with
   `irq_of_parse_and_map()`. Despite its name, `msi->irqs[].hwirq` at that
   point holds a Linux IRQ number, not directly the MSI payload.
4. `msm_msi_irq_compose_msi_msg()` looks up the parent IRQ data and uses its
   GIC hardware IRQ as `msg->data`. The message address is `client->msi_addr`,
   returned by `dma_map_resource(client->dev, msi->msi_addr, PAGE_SIZE, ...)`.
   Preserve page offset and mapping lifetime; a physical address or guessed
   IOVA is not a substitute.

For allocation of this full contiguous block, ath11k QCA6390 HW2.0 expects:

| User | Vector indices | Reference hardware INTIDs |
| --- | --- | --- |
| MHI | 0-2 | 800-802 |
| CE | 3-12 | 803-812 |
| WAKE | 13 | 813 |
| DP | 14-31 | 814-831 |

The MSI payload is a separate 16-bit field. The current Android evidence reads
the capability as 32-bit message address `0xa0000040`, payload base `0x0320`
(800), and 32 enabled vectors; `/proc/interrupts` exposes Linux child mappings
`524288..524319`. That state is useful evidence about the resident Linux
configuration, but it does not prove an XNU ownership grant. The provider must
obtain and retain the actual payload and endpoint-specific doorbell IOVA; no
code may derive payload from a guessed DT value or from a Linux virtual IRQ.

This is a topology derivation, not proof of XNU routing. A Kona MSI owner now
reserves the expected GIC bank, installs handlers masked, maps the doorbell
through an explicit endpoint callback, programs the PCI MSI capability, and
restores those resources on teardown. It has no boot-time caller and does not
acquire RC0, provide the doorbell DMA mapping, or prove resident-firmware
exclusion. `hakios_qca6390_msi()` remains fail-closed until an authorized owner
activates and registers the provider.

## DMA groundwork audit before enabling it

The independent `hakios.dma.pagetable.c` component now addresses software
table geometry, map/unmap preflight and failure retention for a bounded
4 KiB/39-bit stage-1 profile; see `DMA-PAGETABLE.md`. It does not replace the
hardware owner or validate the legacy QCA SMMU register programming listed
below. Its root-info/status APIs must not be used to clear DMA preflight.
The separate `hakios.dma.space.c` layer adds bounded IOVA allocation and lease
retention around that engine (`DMA-SPACE.md`); it likewise supplies no actual
SID/context ownership or endpoint buffer/cache implementation.

Keep `HAKIOS_QCA6390_DMA_DOMAIN` missing until these issues are resolved:

- Validate IDR geometry, context-bank addressing, granule and level count,
  physical/IOVA widths, TCR/TCR2, descriptor permissions, MAIR indices, and
  walker coherency against the actual SMMU owner and hardware.
- The mapper accepts contiguous physical memory and advances a bump IOVA
  cursor. It lacks allocation ownership, streaming sync, unmap, and IOVA reuse;
  it must not become the general ath11k DMA API unchanged.
- Partial page-table population and allocation failures need rollback. Failed
  TLB invalidation must not leave apparently reusable IOVAs or physical pages.
- `qca_smmu_restore()` bounds its TLB wait but does not propagate timeout.
  `dma_stop()` retains `found`, saved registers, tables, and the IOVA cursor.
  Restart must not reuse that ownership snapshot blindly.
- Endpoint and SMMU state use different locks. Map, release, direct stop, and
  MSI setup need one documented lifetime/lock order; checks outside it do not
  exclude concurrent teardown.
- Native endpoint release now refuses an active legacy SMMU context rather
  than tearing it down first. It delegates resource restoration to the actual
  provider and propagates failure. A complete DMA/MHI shutdown path is still
  required before bus mastering becomes possible.

## Next implementation gate

Choose and implement the RC0 ownership path with the resident loader/firmware,
or a native host cold-initialization path with an explicit resource grant.
Describe actually transferred resources, quiescence, and return policy; a
versioned DT flag or absence of standard EFI PCI protocols alone is
insufficient. Implement provider config transactions under the native lease
and review abort handling and exact restore semantics. DMA, MSI, MHI, IPCR
channels 20/21, and ath11k PCI remain subsequent gates.

The user has authorized local builds, compilation and host tests. Device
changes and hardware testing still require separate explicit permission.
Do not add `wifi=1` or `wifi_qca_probe=1` to a Munch image as part of this work.
