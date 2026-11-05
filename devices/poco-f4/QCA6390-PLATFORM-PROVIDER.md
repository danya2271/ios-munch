# QCA6390 platform-provider contract

The QCA adapter now has explicit provider hooks for the resources that cannot
be safely inferred by a generic XNU client. No provider is registered by the
current Munch port, so the runtime remains fail-closed.

The PCI-profile entrypoint obtains its complete callback bundle through the
weak XNU `lx_os_qca_platform()` hook. The fallback performs a software-only
preflight and logs which of RC0 handoff, DMA domain, and MSI routes are missing;
it returns the preflight error while any gate is missing. It does not acquire
hardware or fabricate a callback bundle. A platform owner may supply a strong
implementation only after it has acquired and validated RC0, SID `0x1c01`,
DMA, GIC/MSI, MHI and IPCR ownership. The hook must not turn a partial resource
bundle into success.

## DMA provider

`hakios_qca6390_dma_register()` accepts a provider with:

- `status(endpoint)`: proves that the endpoint owns the exact SID `0x1c01`,
  configured stage-1 tables, cache policy, and a live TLB/invalidation path.
- `stop(endpoint)`: quiesces device DMA, invalidates outstanding translations,
  and restores or retires the provider's state. Failure retains RC0 ownership.

The former direct SMMU programming path has been removed from
`hakios.qca6390.c`. The runtime's byte-range mappings are owned by the
`hakios_dma_endpoint`/arena APIs; this provider only proves the active domain
and performs retryable quiescence. Registering it is the only path that can
satisfy `HAKIOS_QCA6390_DMA_DOMAIN`.

## MSI provider

`hakios.msi.kona.c` implements the provider callbacks over the XNU GIC bank
lease API, `hakios.msi.pool.c`, and the PCI MSI capability engine. The platform
owner supplies only the endpoint doorbell map/unmap callbacks and must activate
the provider after its RC0/DMA owner is established. The provider does not
initialize the GIC, acquire RC0, or prove that resident firmware relinquished
these resources.

`hakios_qca6390_msi_register()` accepts a provider with an allocation and a
per-vector lifecycle. `allocate()` receives the validated PCI MSI capability
offset and must complete all of the following before returning:

1. reserve the exact 32-vector GIC bank;
2. use the hardware INTIDs derived from the Kona parent interrupt mapping;
3. map physical doorbell `0x17a10040` through the endpoint's SID, returning
   its DMA IOVA rather than assuming the physical address is usable. The live
   QCA6390 capability is 32-bit MSI, so this IOVA must also be below 4 GiB;
4. program and verify the PCI MSI capability without enabling vectors;
5. return a handle plus the message address, base data, and first hardware
   INTID.

The Kona provider reserves hardware INTIDs `800..831`, derived from DT
`GIC_SPI 768..799` plus the GIC SPI translation. It attaches all 32 trampoline
handlers while masked before enabling the PCI MSI capability; the per-vector
ath11k callbacks are published before each vector is unmasked. The adapter
never treats the DT SPI or a Linux virtual IRQ as MSI payload.
`hakios_msi_create()` supports both 32-bit and 64-bit PCI MSI layouts and rejects
a 32-bit capability paired with an IOVA above `UINT32_MAX`.

`attach()` is called for MHI, CE, WAKE, and DP vectors after allocation. It
must install each handler masked and must mask the vector before invoking the
handler from interrupt context; the XNU bridge deliberately does not take its
sleepable ownership mutex from a hard IRQ. `enable(false)` masks a vector and
`detach_all()` waits for handlers. The
provider's `release()` must perform, in order: mask, synchronize handlers,
quiesce GIC, disable/restore PCI MSI, unmap the doorbell, and release the GIC
bank. Any failed step keeps the handle and endpoint ownership live for retry.

## Ownership rules

- Registration does not claim hardware and does not clear any preflight bit.
- The callbacks must not re-enter the QCA adapter or wait on its mutex.
- Serialize Kona MSI provider activation/deactivation against QCA start/stop.
  Deactivate only after the QCA MSI allocation and endpoint lease are released.
- Doorbell map/unmap callbacks must use the exact endpoint's SID and retain a
  retryable mapping token; they must not fall back to a physical/identity DMA
  address or re-enter QCA while its provider callback is active.
- A successful callback must return a non-null handle where the API requires
  one; malformed success is converted to `EPROTO` and cleaned up.
- `hakios_qca6390_release()` tears down MSI and DMA providers before releasing
  the endpoint. A failure leaves the endpoint retained.
- No boot argument, DT property, Android sysfs result, or live Linux link state
  is accepted as an ownership grant.
