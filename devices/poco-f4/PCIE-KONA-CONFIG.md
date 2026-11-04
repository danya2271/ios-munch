# Kona RC0 CFG0 transaction engine

## Scope and activation gate

`hakios.pcie.kona.c` implements serialized CFG0/iATU transactions.
`hakios.pcie.kona.mmio.c` supplies bounded mappings, abort-safe native-width
config access, host-register access, CFG completion draining, and lazy BAR0
mapping/access. The backend is not registered with the native endpoint
service and has no boot-time caller. Mapping these resources does not grant
ownership or start Wi-Fi.

The backend requires an authorization token from a trusted platform owner
that already holds exclusive RC0 ownership. It does not establish that
ownership or implement firmware handoff, regulator/clock/reset/PHY sequencing,
DMA, MSI, or MHI. Offline host-model tests and ARM64 object compilation pass;
no device access has been performed. See `PCIE-VALIDATION.md`.

## Supported resource profile

Creation checks the original resource descriptors against the local Kona DT:

| Resource | Physical start | Resource size |
| --- | --- | --- |
| DBI (`dm_core`) | `0x60000000` | `0xf1d` |
| Unrolled iATU | `0x60001000` | `0x1000` |
| Config aperture | `0x60100000` | `0x100000` |

These are resource extents, not page-rounded mapping sizes. A mismatch returns
`ENOTSUP`. Matching constants is only a bounds/profile check, not evidence of
ownership. The callback mappings must actually represent those resources.

Only RC0 endpoint RID `0x100` (bus 1, device/function 0) is supported. Root-port
config access, CFG1, arbitrary BDFs, and viewport-style iATU are not supported.
Each transaction uses only the first 4 KiB of the aperture, mapped as CFG0
type `4` with target `0x01000000`. The resource is not treated as ECAM.

The saved/programmed register set is the seven region-0 words at offsets
`0x00` through `0x18`: control 1/2, lower/upper base, limit, lower/upper target.
This is the layout used by the local vendor `msm_pcie_iatu_config()`, not a
promise to support every DesignWare revision. The owner must confirm this
profile is sufficient and that no extended-limit or alternate mode affects
the mapping. Unverified layouts must not be attached. Other regions and any
extended registers are untouched; the owner must exclude overlapping routing.

## Owner callback contract

`create()` allocates an opaque context and copies the mandatory operations
table. It performs no MMIO. Operations return zero or positive XNU errno;
negative Linux-style errors become `EIO`. The callback code and owner context
must remain alive until successful destruction.

- `begin(context, &guard)` acquires exclusive config/iATU ownership against
  **all** RC0 users, including firmware and other CPUs, and pins the required
  power, clocks, link policy and mappings. It must not reprogram region 0.
  Success returns a non-null guard. On failure, a null guard means no cleanup
  remains; a non-null guard retains the resources needed for `end()`.
- The guard remains valid across failed accesses and cleanup retries. The
  owner must not asynchronously reset RC0, change region 0, unmap resources,
  or let firmware resume while it is retained. A mutex private to this
  engine is not firmware serialization or a handoff protocol.
- `read()` and `write()` use the named mapping plus the byte offset, not a
  physical address. They must provide real 8/16/32-bit little-endian config
  accesses with correct byte enables; no dword read/modify/write emulation.
  Host DBI/iATU accesses are always 32-bit. The owner must supply architectural
  MMIO ordering, fault containment and bounded error reporting, including
  handling a link disappearing after the software link check. Returning
  success after an external abort, or merely using a volatile pointer, does
  not implement this contract. Host writes must be ordered with subsequent
  iATU polling/recovery even when an error is returned.
- `config_drain()` must establish that every attempted config transaction
  has completed or has been safely aborted, including after a failed access.
  Only then can the translation be changed. It must not replay the config
  write. It is called after reads as well as writes; a CPU store barrier
  alone is not sufficient. On failure the engine keeps the mapping and guard
  instead of restoring iATU underneath a possibly outstanding transaction.
- `delay_us()` provides the requested bounded delay while preserving the
  guard. Polling uses at most 100 reads with 100 microseconds between reads.
  These are conservative software limits, not measured hardware timings;
  callback execution time is additional and must itself be bounded.
- `end()` releases the guard only after drain/restoration have completed.
  On error it must retain a valid, retryable guard and exclusivity, even if
  some cleanup steps already succeeded. It must not return success while
  ownership, ordering or power cleanup remains uncertain.

The engine mutex covers begin, all accesses, drain, restoration and end.
Callbacks must not reenter this engine, the native PCIe service, or QCA, or
wait for work needing those locks. Lock order is QCA -> native PCIe -> Kona
engine -> owner. This is sleepable thread-context infrastructure, not an
IRQ-safe CE/DP access path. Destruction requires the owner to stop and join
all callers first; the mutex does not make freeing a concurrently referenced
context safe. Request/output storage must not change concurrently.

## Cold-owner lifecycle scaffold

`hakios_pcie_kona_cold_owner_*()` is a software lifecycle coordinator, not a
registered Kona provider. The mapped MMIO backend supplies guarded access to
the relevant windows after an owner installs its token, but no production
cold-stage implementation supplies that token or sequences RC0 resources.
This API has no boot caller and does not make `lx_os_qca_platform()` succeed.

`hakios.pcie.kona.sid.c` implements the PARF requester-to-SID table stage for
RID `0x100`: it snapshots all 256 entries, installs the CRC8-hashed
`0x01000100` entry for PCIe SID 1 (SMMU SID `0x1c01`), verifies the table, and
restores the snapshot through a retained rollback cookie. It does not program
an SMMU stream match or context bank and is not wired into a production cold
owner.

`hakios.pcie.kona.platform.c` composes that lifecycle with the Kona CFG0
engine and native RC0 provider registration. Separate cold/config/BAR contexts
allow the mapped backend to bind without replacing the cold owner. The
adapter enforces the QCA6390 domain/RID/SID/identity/BAR profile and keeps
cold-host ownership across an endpoint release. Only explicit platform
deactivation unregisters RC0 and unwinds cold-init. It still has no boot
caller or firmware-exclusion proof.

Acquire serializes and requests exclusive ownership, then runs these stages
in order: assert PERST; enable regulators/GDSC and bus vote; enable clocks and
resets; configure PARF; initialize PHY and pipe resources; release PERST;
train link; configure the BDF-to-SID table; configure the controller; validate
endpoint configuration access. Release unwinds in reverse order and retains
the ownership token and remaining stage cookies if cleanup fails. `recover()`
retries that unwind. Destroy refuses active, uncertain, or poisoned state.

Each successful acquire stage returns a non-null rollback cookie. An acquire
error with a null cookie guarantees that the stage made no state change; an
error with a cookie means partial state changed and must be rolled back.
Rollback must be retryable: an error leaves the cookie valid and enough
progress state to safely resume cleanup. Successful release clears the
cookie. A success without a cookie, or ownership loss during revoke, poisons
the owner rather than guessing at cleanup.

`authorize()` follows the same partial-acquire rule: an error with no ownership
token made no claim; an error with a token is unwound through `revoke()`.

The mapped backend uses Kona reference extents for DBI/ELBI/iATU, the CFG0
page, PARF, PHY, and QCA BAR0. Creation maps host windows but performs no
register reads or writes. Guarded access uses the scoped abort-safe helpers,
validates every range/width, and retains authorization after an access fault.
BAR0 is mapped lazily only after endpoint acquisition. A wake-required access
needs a separately supplied wake callback; the backend never treats an
unsupported wake as success. These mechanisms do not establish
resident-loader exclusion or implement power/reset/clock sequencing.

The ARM64 exception path recognizes a dedicated recovery target for the
8-, 16-, and 32-bit `hakios_mmio_read*()` and `hakios_mmio_write*()` accessors.
It converts a synchronous external abort from those exact MMIO instructions
to `EFAULT`; aborts in other kernel code, asynchronous SError, and stage-1
page-table-walk external aborts retain the normal fatal behavior. A failed
MMIO write may already have reached the device, so callers must poison and
retain ownership on error. The helpers are wired into the mapped Kona backend,
but that backend has no production owner and has not been fault-injected on
hardware.

## Transaction and failure behavior

1. Validate RID, width, alignment, config bounds and write value before
   entering the owner or touching hardware. Invalid reads leave output zero.
2. Acquire the guard, check DBI `0x80` link-active bit 29, and read the entire
   seven-word snapshot before any iATU writes. An all-ones link status or
   control register is rejected, not accepted as an active controller.
3. Mark restoration required **before** the first write, since an MMIO write
   reporting failure might still have taken effect. Disable region 0, poll,
   program the CFG0 translation, enable/poll, and verify all seven words.
4. Recheck the link and mark config drain required before issuing the actual
   byte/halfword/dword access. No config write is automatically retried and
   the engine never sizes BARs or enables Bus Master on its own.
5. Drain config traffic, disable/poll region 0, restore all saved words,
   restore its original enable state, and verify exact readback. Release
   the guard only after restoration is confirmed. Publish a read result only
   after this entire sequence succeeds, including `end()`.

Any operational error faults the context and blocks subsequent config I/O;
invalid arguments do not. Cleanup errors take precedence over the original
transaction errno. An error does not imply a config write had no effect:
restoring iATU does not roll back endpoint registers.

`recover()` retries only pending drain, iATU restoration, and guard release,
in that order. It neither repeats the config transaction nor clears the
faulted state. Once cleanup succeeds, the owner may destroy the context;
recreating it requires the owner to reconcile the endpoint/controller state,
not simply retry a failed write. A native endpoint lease fault remains subject
to `PCIE-NATIVE-API.md` and is not cleared by this helper.

Failed drain/restoration/end retains the guard and snapshot. `destroy()`
returns `EBUSY` without freeing or clearing the caller's pointer while cleanup
is pending. Successful `begin()` with a null guard is a contract violation:
the context is quarantined, and recovery/destruction refuse to hide unknown
ownership. There is no force-free or ready-flag escape hatch.

## Source references and remaining integration

The resource geometry comes from
`arch/arm64/boot/dts/vendor/qcom/kona-pcie.dtsi` in
`/home/danya2271/xiaomi_sm8250_kernel`. The same tree's
`drivers/pci/controller/pci-msm.c` provides the unrolled offsets, link bit,
`msm_pcie_iatu_config()` and `msm_pcie_cfg_bdf()` target calculation. Its
`drivers/pci/controller/dwc/pcie-designware.c` also demonstrates native-width
config accesses; the vendor host's dword read/modify/write path is not copied.
The helper does not depend on ignored ath11k build output.

A future trusted provider can own one engine context and delegate its native
`config_read`/`config_write` callbacks to it. Its release path must recover
and destroy that context before relinquishing RC0, with the native owner
cookie retained on any failure. Actual MMIO/abort/completion primitives and
the firmware ownership path still need implementation and review. No dummy
provider or success-shaped callbacks have been added to bypass these gaps.
