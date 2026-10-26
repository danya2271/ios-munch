# Munch TE staging experiment

The passive GPIO66 probe measured 60.69, 60.68 and 60.68 Hz in three
500-ms windows. The captured panel DT includes 60/120-Hz modes, but this
experiment does not change panel, DSI, DPU or clock registers.

The app-switcher video shows horizontal discontinuities. The existing
framebuffer compositor writes horizontal stripes concurrently into the live
front buffer. This is a plausible contributor; the video alone does not
establish the complete cause.

`munch_te=1` enables a separate composition buffer on the exact munch
1080x2400/4320-byte-stride configuration. Workers finish before a single
serial front-buffer copy. GPIO66 is read without changing its mux or IRQ.
The initial measured TE period must be 15–18 ms. Presentation attempts a
14-ms offset derived from the stock 60-Hz DT transfer-time budget. This is
an experimental phase assumption, not a verified blanking interval or
atomic page flip. A missing signal, late frame, or excessive measured copy
time falls back to serial staging; repeated missing pulses disable polling.
It may reduce frame rate and does not guarantee tear-free output.

Build with `gpu/stage-kernel.py --te-staging`. The source manifest and image
configuration must both enable staging. Normal staging without that option
restores the baseline framebuffer implementation.

Validation: the native kernel build target passed with BUILD_DSYM=0;
`check-te-frame.c` passed with AddressSanitizer and UndefinedBehaviorSanitizer
for geometry, period, copy-window, lateness and overflow policy. The packed
image passed the independent image verifier. Hardware results are pending.

`munch-ios165-gpu-te-r1.img` retains the existing Mesa helper and iOS root,
requests the reviewed 587-MHz GPU selection, keeps software refresh at 60 Hz,
and disables the deliberate 600-second loader return. It does not install
or rewrite the iOS root. Keep the previous boot image for comparison.

## Device feedback and rollback

The TE-staging r1 image felt slower and did not visibly resolve tearing.
It is not the preferred presentation path. The touch4 r1 image restores
the original framebuffer source, including its existing frame pacing, and
changes only the native touch loop's sleep from 16 to 4 ms relative to the
session r1 profile. The GPIO SPI transaction timings and CRC/retry checks
are unchanged. Four milliseconds is the sleep interval, not a measured
250-Hz sample rate or an end-to-end latency claim. More polling can use
more CPU and power. Device latency and stability testing is pending.

## UMI command-mode scan adaptation: measurement first

The UMI `hk_display.c` command-mode path checks `te_initial == 2340`,
calibrates TE counter wraps, and checks the write-pointer counter before
copying. Munch's selected panel is 2400 lines tall. Its captured live DT
lists INTF offsets 0x6b000/0x6b800/0x6c000/0x6c800 at MDP base 0xae00000,
so the first DSI interface is 0xae6b800, as in UMI/Kona.

`munch-display-scan-r1.img` observes INTF1 TE configuration and three
200-ms windows of raw and write-counter ranges, wraps, busy/idle samples,
and the last observed busy phase. It does not boot iOS or modify DSI/DPU
registers. GCC display AHB at 0x10b00c and DISPCC AHB at 0xaf02080 may be
temporarily enabled, then restored on every ordinary return path. MDSS
power at 0xaf03000 must already be on; the probe does not power it up.
Clock/power guards precede INTF reads. This does not provide recovery
from a hardware bus hang or an unexpected reset.

The counter semantics and offsets were checked against Xiaomi's
[alioth-r-oss sde_hw_intf.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/techpack/display/msm/sde/sde_hw_intf.c),
[gcc-kona.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/drivers/clk/qcom/gcc-kona.c),
[dispcc-kona.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/alioth-r-oss/drivers/clk/qcom/dispcc-kona.c),
and the GDSC offset against Linux v6.12
[dispcc-sm8250.c](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/dispcc-sm8250.c).
These references establish register layout, not a proven safe copy interval
on this handset. No UMI 2340-line calibration is assumed valid for munch.

`uefi/check-display-scan.py` executes the actual probe with mocked MMIO
and time. ASan/UBSan checks cover period measurement, power/clock guards,
clock timeout, restoration and an invalid timer. Hardware results are
required before enabling controller-based presentation in the iOS image.
