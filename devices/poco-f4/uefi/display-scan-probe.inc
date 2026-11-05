/* SPDX-License-Identifier: MIT
 * Kona INTF1 observation, following the UMI display approach. No DSI/DPU
 * writes: only temporarily enable and restore the two register bus clocks.
 * Addresses match captured munch DT and Xiaomi alioth-r-oss clock sources.
 */
#ifndef MUNCH_SCAN_TEST
static uint32_t scan_read(uintptr_t at) { return *(volatile uint32_t *)at; }
static void scan_write(uintptr_t at, uint32_t value)
{
    *(volatile uint32_t *)at = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint64_t scan_ticks(void)
{
    uint64_t value; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value)); return value;
}
static uint64_t scan_frequency(void)
{
    uint64_t value; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value)); return value;
}
#endif
#define SCAN_GCC 0x0010b00cUL
#define SCAN_AHB 0x0af02080UL
#define SCAN_GDSC 0x0af03000UL
#define SCAN_INTF 0x0ae6b800UL
static int scan_clock_on(uintptr_t at, uint32_t saved)
{
    if (saved == UINT32_MAX) return 0;
    if (!(saved & 1)) scan_write(at, saved | 1);
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t v = scan_read(at);
        if (v != UINT32_MAX && (v & 1) && !(v & (1u << 31))) return 1;
        system_table->bs->stall(10);
    }
    return 0;
}
static void display_scan_probe(void)
{
    uint64_t freq = scan_frequency();
    uint32_t gcc = scan_read(SCAN_GCC), ahb = UINT32_MAX;
    int touched_ahb = 0, touched_gcc = 0;
    text("\r\nMUNCH DISPLAY SCAN PROBE 1\r\n");
    text("No panel mode change. Register bus clocks restored on exit.\r\n");
    field("GCC AHB before: ", gcc);
    if (freq < 1000000 || freq > 200000000) goto done;
    touched_gcc = gcc != UINT32_MAX && !(gcc & 1);
    if (!scan_clock_on(SCAN_GCC, gcc)) { text("GCC clock unavailable\r\n"); goto done; }
    uint32_t power = scan_read(SCAN_GDSC);
    field("MDSS power: ", power);
    if (power == UINT32_MAX || !(power & (1u << 31))) { text("Display power is off; INTF not read\r\n"); goto done; }
    ahb = scan_read(SCAN_AHB); touched_ahb = ahb != UINT32_MAX;
    field("DISP AHB before: ", ahb);
    if (!scan_clock_on(SCAN_AHB, ahb)) { text("Display AHB unavailable\r\n"); goto done; }
    field("INTF engine: ", scan_read(SCAN_INTF));
    field("TE enable: ", scan_read(SCAN_INTF + 0x284));
    field("TE config: ", scan_read(SCAN_INTF + 0x288));
    field("TE height: ", scan_read(SCAN_INTF + 0x28c));
    uint32_t initial = scan_read(SCAN_INTF + 0x294) & 0xffff;
    field("TE initial: ", initial);
    field("TE start: ", scan_read(SCAN_INTF + 0x2a0));
    field("TE auto: ", scan_read(SCAN_INTF + 0x2b4));
    uint64_t active_since = 0, completed_at = 0;
    int in_transfer = 0, saw_zero = 0, completed_valid = 0;
    uint32_t first_write = scan_read(SCAN_INTF + 0x2b0) & 0xffff;
    if (first_write > 0 && first_write < initial) {
        in_transfer = 1;
        active_since = scan_ticks();
        saw_zero = 1;
    }
    else if (first_write >= initial && initial) completed_at = scan_ticks();
    else saw_zero = 1;
    for (unsigned window = 0; window < 3; window++) {
        uint32_t lo = 65535, hi = 0, wlo = 65535, whi = 0, busy_last = 0, wraps = 0;
        uint32_t previous = scan_read(SCAN_INTF + 0x298) & 0xffff;
        uint64_t start = scan_ticks(), first = 0, last = 0, now = start;
        uint64_t active_min = UINT64_MAX, active_max = 0;
        uint64_t idle_min = UINT64_MAX, idle_max = 0;
        unsigned samples = 0, busy = 0, idle = 0;
        unsigned transfers = 0, idle_gaps = 0;
        while (now - start < freq / 5 && samples++ < 10000) {
            system_table->bs->stall(50);
            uint32_t raw = scan_read(SCAN_INTF + 0x298) & 0xffff;
            uint32_t written = scan_read(SCAN_INTF + 0x2b0) & 0xffff;
            now = scan_ticks();
            if ((scan_read(SCAN_INTF + 0x298) & 0xffff) < raw) { previous = raw; continue; }
            if (raw < previous) { if (!wraps++) first = now; last = now; }
            if (raw < lo) lo = raw;
            if (raw > hi) hi = raw;
            if (written < wlo) wlo = written;
            if (written > whi) whi = written;
            if (written > 0 && written < initial) {
                busy++;
                if (!in_transfer) {
                    if (completed_valid && completed_at) {
                        uint64_t gap = now - completed_at;
                        if (gap < idle_min) idle_min = gap;
                        if (gap > idle_max) idle_max = gap;
                        idle_gaps++;
                    }
                    active_since = now;
                    in_transfer = 1;
                    completed_valid = 0;
                }
                saw_zero = 0;
                if (raw >= initial && raw - initial > busy_last) busy_last = raw - initial;
            } else if (initial && written >= initial) {
                idle++;
                if (in_transfer) {
                    if (!saw_zero) {
                        uint64_t duration = now - active_since;
                        if (duration < active_min) active_min = duration;
                        if (duration > active_max) active_max = duration;
                        transfers++;
                    }
                    in_transfer = 0;
                    completed_at = now;
                    completed_valid = 1;
                }
                saw_zero = 0;
            } else if (written == 0) {
                saw_zero = 1;
            }
            previous = raw;
        }
        field("Window: ", window + 1);
        text("raw min/max "); hex(lo); text(" / "); hex(hi); text("\r\n");
        text("write min/max "); hex(wlo); text(" / "); hex(whi); text("\r\n");
        text("wraps/busy/idle/last-phase "); hex(wraps); text(" / "); hex(busy);
        text(" / "); hex(idle); text(" / "); hex(busy_last); text("\r\n");
        if (transfers) {
            field("Write active min us: ", active_min * 1000000 / freq);
            field("Write active max us: ", active_max * 1000000 / freq);
            field("Write active cycles: ", transfers);
        }
        if (idle_gaps) {
            field("Write idle min us: ", idle_min * 1000000 / freq);
            field("Write idle max us: ", idle_max * 1000000 / freq);
            field("Write idle gaps: ", idle_gaps);
        }
        if (wraps >= 3 && last > first) field("Measured frame period us: ", (last-first) * 1000000 / freq / (wraps-1));
    }
done:
    if (touched_ahb && !(ahb & 1)) scan_write(SCAN_AHB, ahb);
    if (touched_gcc) scan_write(SCAN_GCC, gcc);
    text("MUNCH DISPLAY SCAN PROBE FINISHED\r\n");
}
