# Munch touch bring-up

The user reports an Apple logo and an iOS interface with the mode-58 APFS RAM
boot image. Touch does not respond. This is a user-reported UI result, without
an exact runtime log capture; it does not validate the other peripherals.
The supplied command line intentionally has no touchscreen flag.

## Why the existing touch flag cannot be enabled yet

The current XNU hakios.goodix.c supports a FocalTech variant as well as Goodix,
but its pin, clock, SPI, RPMh and power definitions are for redwood. It contains
reset, regulator and firmware-recovery operations. It is not a device-neutral
FocalTech driver and must not be applied to munch by adding touchscreen=1.
The buttons driver also defaults on with that flag and has its own platform
assumptions. F4 tests must keep buttons disabled until separately adapted.

The live munch DT records SPI SE4 at 0x00990000, CS0, max 6 MHz; GPIO33 reset,
GPIO39 IRQ; GPIO8–11 muxed to qup4; analog supply PM8150A L10 and I/O L1.
Display coordinate bounds are 0,0,10800,24000, requiring comparison with actual
Android input-axis/report scaling before emitting iOS touches.

The Xiaomi munch-s-oss pinctrl-kona.c assigns GPIO8–11 to NORTH (offset 0x900000)
and GPIO33/39 to SOUTH (offset 0x500000), relative to DT's TLMM base 0x0f000000.
Thus the GPIO configuration addresses are 0x0f908000, 0x0f909000, 0x0f90a000,
0x0f90b000, 0x0f521000 and 0x0f527000. IO registers are at configuration+4.
One replacement TLMM base cannot handle both sets of pins. QUP4 is mux function
1 in that pinctrl table. No MMIO or regulator writes based on this document
have been performed on the device.

## Primary Android references

The Xiaomi source branch has both focaltech_3658u and focaltech_spi variants;
the generic DT compatible focaltech,fts does not identify which IC is fitted.
The 3658u configuration selects FT3658U, mapping runtime IDs 0x56/0x52 and ROM
0x56/0xb2, and uses SPI mode 0 with SPI_V2 framing. This is a candidate, not a
confirmed identification of this phone's controller.

- https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/munch-s-oss/drivers/input/touchscreen/focaltech_3658u
- https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/munch-s-oss/drivers/input/touchscreen/focaltech_spi
- https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/munch-s-oss/drivers/pinctrl/qcom/pinctrl-kona.c
- https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/munch-s-oss/drivers/spi/spi-geni-qcom.c

Local reference copies stay outside the port source. The vendor driver is GPL-2.0;
inspect protocol/register behavior with attribution, preserving applicable
licensing for any derived implementation. Do not reuse another IC's firmware.

## Next device input

collect-touch-info.sh runs as root in Android and only reads existing sysfs/proc
attributes, input metadata, filtered touch logs, pin/clock/regulator summaries
and matching firmware/module files from the owner's device. It writes a new
regular-file archive in Download. It never writes a control attribute, toggles a
GPIO, resets/flashes the controller or installs anything. The driver-info sysfs
attribute can provide the runtime IC ID, reset/IRQ pins, bus mode and frequency.
The archive is required to confirm the active variant and firmware before
implementing the device-specific power/reset and transport path.

## Android archive: 20261026-123312

The owner's archive confirms runtime IC ID 0x5652, driver version Focaltech V3.2
20200422, firmware-version text 28, SPI mode 0 and max 6000000 Hz. Driver reset/IRQ
numbers 1133/1139 correspond to TLMM 33/39 with Linux's 1100 GPIO base. Input axes
are 0..10799 and 0..23999, with ten slots. No separate matching firmware/module
file was found by the collector; no debugfs pin/clock/regulator dumps were present.
The IC ID and configuration match the FT3658U primary Android source.

The vendor report decoder reconstructs extended 16-bit positions using the
additional nibbles in the former pressure byte, converts to Android units by
raw*10/16, then to display pixels by /10. The portable independent decoder in
touch/ft3658u-protocol.h uses the equivalent raw/16 scaling, bounds checks and
per-contact flags/IDs. Packet CRC starts at 0xffff with reflected polynomial
0x8408. Host tests with address/undefined sanitizers passed CRC vectors, fractional
extended coordinates, screen edges, releases and malformed/out-of-range reports.

## Initial runtime-read diagnostic

munch-touch-read-probe.img is EFI-only and does not boot iOS. It saves GPIO8–11
configuration/output state, temporarily configures GPIO SPI mode 0, and issues
only register reads with control 0xa0, checking status and CRC on every reply.
It does not alter GPIO33/reset, GPIO39/IRQ, regulators, GENI clocks, controller
firmware or flash. Initial data-line roles for this diagnostic are MOSI GPIO8,
MISO GPIO9, SCLK GPIO10, CS GPIO11; a failed read must not be treated as proof of
a defective controller because power/handoff state and line assignment remain
hardware questions. No automatic data-pin swap or boot-ROM recovery is attempted.

Expected success: TOUCH LINK: PASS (ID 5652, CRC checked), then contact positions
and green marks while touching the screen for roughly 30–35 seconds. Before
returning, the four SPI pin states are restored, followed by the normal UEFI
90-second reset delay. A NO VERIFIED response is a diagnostic result, not a
working touch driver. Host image/FD/DTB/payload/hash checks passed; phone pending.

## Runtime-read probe revision 2

The user reports Failed reads = 1 and NO VERIFIED FT3658U RESPONSE with revision
1. That counter records a failed register read after retries; the short-circuit
ID condition prevented the second ID read once the first failed. It does not
identify power, pin direction, reset or protocol state as the cause.

Revision 2 tests MOSI8/MISO9 and then MOSI9/MISO8, releasing both data lines
before changing their direction and using minimum 2-mA GPIO drive. It reads both
ID registers independently and prints compact ten-byte replies. It stops at the
first CRC-verified 5652 ID; otherwise restores the original pins and prints RESET
input level, RESET output latch and both SPI-A3 replies together in a summary.
There are still no reset, regulator, flash or boot-ROM writes. This supersedes
the revision-1 choice not to swap data pins. Host image/FD/DTB/payload/hash checks
passed; hardware result pending.

## Photo evidence and reset-release probe revision 3

The user's photo of revision 2 shows GPIO33 config 0x1 (GPIO input with weak
pull-down), IO 0; GPIO39 IO 0; all four SPI pad configs 0x1. Both SPI directions
return ten zero bytes for A3 and 9F. CRC-valid reads are 0, failed register reads
4. This is not a confirmed wiring or firmware failure. RESET is observed low,
but supply state is unknown and GPIO33 is not configured as an active output.

Revision 3 is a separate opt-in --touch-reset-probe. If RESET input is low, it
saves GPIO33's config/output latch, preloads HIGH, selects GPIO output with
pull-up and 8-mA drive (0x2c3, from Android's active pinctrl), waits 200 ms, and
prints the applied config/input/latch. It does not first drive LOW. It then runs
the same read-only SPI ID/contact tests. Original GPIO33 state is restored with
the SPI pins on all normal completion/failure paths, so the old low state may
be reinstated at exit. No regulator, boot-ROM, flash or firmware commands exist.
Primary driver reference: fts_reset_proc in Xiaomi's focaltech_3658u/core uses a
1-ms low, high, then caller-selected settling delay; this probe only tests release
of the already-low line. GPIO39 and both regulators remain untouched.

Host build and image/EFI/hash/FD/live-DTB checks passed. Firmware and partition
contents are not modified. Hardware response and supply state remain pending.

## Reset-release result and power-read probe revision 4

The user's photo of r3 shows GPIO33 applied config 0x2c3 with input and latch
HIGH. Both candidate SPI wirings still return zero bytes; CRC-valid=0 and failed
reads=4. The pad write/readback path works; reset release alone did not produce
a runtime reply. Regulator state and SPI transport remain unresolved.

The live DT names the touch rails pm8150a_l1 (resource ldoc1, fixed 1800000 uV)
and pm8150a_l10 (resource ldoc10, 3000000..3312000 uV). The third PMIC node pair
is pm8150l at SIDs 4/5; PMIC naming/resource aliases do not by themselves prove
which SID/peripheral implements a rail. Revision 4 inspects candidate L1/L10
peripherals (0x40/0x49) on both SIDs through read-only SPMI observer channels.

Arbiter core 0x0c440000, config 0x0c40a000, observer 0x0e600000 and EE0 match
live DT. Only the v5/v6 layout is accepted; channels are found through PPID and
owner tables, never guessed. Transfers use extended-register READ LONG, bounded
to 2 ms with local IRQ masking/restoration. The sole MMIO store submits the
observer read command; no PMIC write opcode, GPIO change or regulator operation
is included. It prints peripheral type/subtype, mode, enable, status and raw
voltage selectors. Enable/ready labels are conditional on LDO type 4; raw voltage
is deliberately not converted until subtype/mapping is verified. Firmware and
partition contents are untouched.

Build and host image/hash/EFI-payload/FD/live-DTB checks passed. Hardware pending.

## Power-state report and temporary-enable probe revision 5

The user reports no observer channels for SID4 candidates. SID5 L1 is type 04,
subtype 71, mode 02, enable 00, status 03, voltage raw 0708; SID5 L10 is type 04,
subtype approximately 70 (the user was unsure of the final digit), mode 07,
enable 00, status 03, voltage raw 0cf0. Both are disabled/not ready. In this PMIC5
LDO register format, selectors represent 1800 and 3312 mV, consistent with live
DT rail constraints. Runtime guards still check the actual values before any vote.

The opt-in --touch-enable-probe resolves only ldoc1 and ldoc10 in the bounded
command database at 0x80860000, requiring the expected magic, unique names and
VRM address class 4. It requires recognized SID5 LDO type/subtype family,
initially-zero enable bytes, L1 selector exactly 1800 and L10 in 3000..3312.
RESET must initially read low. It does not write voltage or mode selectors.

ENABLE votes use APPS RSC DRV2 at 0x18220000, offset 0xd00, stride 0x2a0, only
active TCS2-4 from this phone's DT (TCS0-1 are sleep slots and never touched).
A slot must be idle, have no enabled commands and no pending completion. IRQ,
command and control state are saved/restored while local IRQs are masked. Each
request is bounded to 200 ms and its issued/completed flags are checked. A live
timed-out command is not overwritten; further requests are blocked and a reboot
is requested. This is a temporary prototype, not a full RPMh framework.

Following Android's power order, it enables L1, waits 1 ms, then L10, checks both
PMIC enable/ready bits, and invokes the reset-release/contact probe. Normal exit
restores GPIO state first, then sends disable votes in reverse order and verifies
the original disabled state. Failures attempt the same cleanup; unconfirmed
restoration is explicitly reported and the normal reboot follows. No controller
firmware/flash or device partition writes are included.

Host tests extract the actual C parser/vote functions with mocked MMIO: address
and value limits, slot state preservation, pending-slot avoidance, timeout lockout
and command DB validation passed. EFI compilation and image/FD/DTB/payload/hash
checks passed. Hardware power enable and touch response remain pending.
Primary behavior references: Xiaomi munch-s-oss drivers/soc/qcom/rpmh-rsc.c,
cmd-db.c and drivers/regulator/qcom-rpmh-regulator.c, plus its FocalTech power order.

## Powered contact probe: device result

The r5 photograph `5402504418520932709.jpg` shows `TOUCH CONTACTS: PASS`,
0x154 decoded contact positions and 0x25b CRC-valid reads. The test drew green
7x7 marks at sampled coordinates; it does not interpolate lines. Two failed
reads were reported across both wiring candidates and polling. The final
message confirms `TOUCH POWER: ORIGINAL DISABLED STATE RESTORED`.
The SID5 L10 subtype reads 0x7d in this photograph. Coordinate reception in
UEFI is confirmed; this alone does not establish iOS event delivery.

## XNU touch prototype r1

`touch/xnu-ft3658u.c` is an independent runtime-read-only GPIO SPI driver.
For this first integration, the loader reuses the proven r5 rail enable and
reset release, verifies chip ID 5652 with CRC, and retains the powered link
across the XNU handoff. It restores pads, reset and rail votes after XNU returns.
A failed power or ID check prevents the handoff.

`touch/kernel-overlay.py` compiles the driver into the dormant Redwood touch
entry/thread region of the exact SHA-pinned prebuilt. It checks Mach-O symbols,
limits code to `[0xfffffff0074a4b80, 0xfffffff0074a5e2c)`, and reuses only that
old driver's 1032-byte SPI TX BSS buffer. This is a temporary binary overlay,
not a complete kernel rebuild. The original prebuilt remains unchanged.
The old touch initialization and firmware recovery path are bypassed entirely.
No `touchscreen=1` flag is added, so the unrelated Redwood button defaults
remain disabled.

The new thread maps only GPIO8..11, checks both data-line directions, verifies
ID 5652, reads CRC-checked reports, and posts normalized down/move/up events
through `hakios_touch_post`. It sleeps 16 ms between polls. Five consecutive
failed reads release held fingers; invalid point counts do not dispatch events.
No touch firmware is downloaded and no controller write commands are sent.
The existing installed APFS image and userland injection remain unchanged.

Host checks: `python3 touch/check-xnu-events.py` verifies event ordering,
duplicate suppression, invalid point counts, missing contacts and coordinate
endpoints under ASan/UBSan. `touch/check-protocol.c` and
`python3 touch/check-power-guards.py` cover packet parsing and power guards.
The first iOS touch response and orientation still require a device test.
