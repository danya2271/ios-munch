/* SPDX-License-Identifier: MIT
 * Passive TE cadence measurement. Live munch DT: selected l11r panel,
 * qcom,platform-te-gpio = <tlmm 66 0>; Kona GPIO66 uses the NORTH tile.
 * No pad configuration, display clock, DSI, regulator or partition writes.
 */
static uint64_t te_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}
static void te_decimal(uint64_t value)
{
    char s[24]; unsigned n = 23; s[n] = 0;
    do { s[--n] = '0' + value % 10; value /= 10; } while (value && n);
    text(s + n);
}
static void panel_te_probe(void)
{
    const uint64_t cfg_at = 0x0f942000, io_at = cfg_at + 4;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    uint32_t cfg = *(volatile uint32_t *)(uintptr_t)cfg_at;
    text("\r\nMUNCH PANEL TE READ-ONLY PROBE 1\r\n");
    text("Selected panel DT supports 60/120 Hz. This probe does not switch modes.\r\n");
    field("GPIO66 config: ", cfg);
    if (frequency < 1000000 || frequency > 200000000) {
        text("Unsupported timer frequency; no measurement.\r\n"); return;
    }
    if ((cfg & 0x3c) != 4 || (cfg & 0x200)) {
        text("GPIO66 is not confirmed as mdp_vsync input. No configuration changed.\r\n"); return;
    }
    for (unsigned window = 0; window < 3; window++) {
        uint64_t periods[256], last = 0, begin = te_ticks(), now = begin;
        unsigned count = 0, edges = 0, polls = 0;
        unsigned previous = *(volatile uint32_t *)(uintptr_t)io_at & 1;
        while (now - begin < frequency / 2 && polls++ < 40000000) {
            unsigned high = *(volatile uint32_t *)(uintptr_t)io_at & 1;
            now = te_ticks();
            if (high && !previous) {
                if (last && count < 256) periods[count++] = now - last;
                last = now; edges++;
            }
            previous = high;
        }
        text("Window "); te_decimal(window + 1); text(": rising edges "); te_decimal(edges);
        if (now - begin < frequency / 2) { text("; poll limit reached\r\n"); continue; }
        if (count < 8) { text("; insufficient TE samples\r\n"); continue; }
        for (unsigned i = 1; i < count; i++) {
            uint64_t value = periods[i]; unsigned j = i;
            while (j && periods[j - 1] > value) { periods[j] = periods[j - 1]; j--; }
            periods[j] = value;
        }
        uint64_t median = periods[count / 2];
        if (!median) { text("; invalid period\r\n"); continue; }
        uint64_t hz100 = frequency * 100 / median;
        text("; median cadence "); te_decimal(hz100 / 100); text(".");
        if (hz100 % 100 < 10) text("0");
        te_decimal(hz100 % 100); text(" Hz\r\n");
    }
    text("TE measurement only; panel mode, scanout synchronization and frame delivery are separate.\r\n");
    text("MUNCH PANEL TE PROBE FINISHED\r\n");
}
