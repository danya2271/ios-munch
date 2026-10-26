/* SPDX-License-Identifier: MIT
 * Controlled temporary ENABLE votes for munch touch ldoc1/ldoc10 only.
 * DT: APPS RSC DRV2, sleep TCS0-1, active TCS2-4, stride 0x2a0.
 * No voltage/mode/firmware/flash writes. Both rails must start disabled at
 * recognized LDO types/valid voltage selectors; the PMIC state is read back.
 */
static uint32_t touch_rail_address[2];
static unsigned touch_rail_channel[2], touch_rail_changed[2], touch_rpmh_fault;
static unsigned touch_rpmh_error;
static uint32_t touch_rpmh_result;
static void touch_power_enable_end(void);

static void power_wr(uint64_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}
static uint16_t power_db16(uint64_t at)
{
    return (uint16_t)(*(volatile uint8_t *)(uintptr_t)at |
                     (uint16_t)*(volatile uint8_t *)(uintptr_t)(at + 1) << 8);
}
static uint32_t touch_resource(const char *name)
{
    const uint64_t db = 0x80860000, data = db + 144;
    const uint8_t magic[4] = {0xdb, 0x30, 3, 0x0c};
    uint32_t found = 0;
    for (unsigned i = 0; i < 4; i++)
        if (*(volatile uint8_t *)(uintptr_t)(db + 4 + i) != magic[i]) return 0;
    for (unsigned bank = 0; bank < 8; bank++) {
        uint64_t h = db + 8 + bank * 16;
        unsigned id = power_db16(h), off = power_db16(h + 2), n = power_db16(h + 6);
        if (id != 4 || (off & 3) || !n || n > 2048 || off + (uint64_t)n * 24 > 0x20000 - 144) continue;
        for (unsigned i = 0; i < n; i++) {
            uint64_t entry = data + off + i * 24;
            unsigned match = 1, end = 0;
            for (unsigned k = 0; k < 8; k++) {
                uint8_t expected = end ? 0 : (uint8_t)name[k];
                if (!expected) end = 1;
                if (*(volatile uint8_t *)(uintptr_t)(entry + k) != expected) match = 0;
            }
            if (!match) continue;
            uint32_t address = power_rd(entry + 16);
            if (found || !address || (address & 3) || ((address >> 16) & 15) != 4) return 0;
            found = address;
        }
    }
    return found;
}

/* One synchronous enable/disable vote, with an otherwise unused active slot.
 * Preserve firmware command state. Never overwrite a live or pending slot.
 */
static int touch_enable_vote(unsigned rail, unsigned enable)
{
    const uint64_t rsc = 0x18220d00;
    touch_rpmh_error = 0;
    if (rail > 1 || enable > 1 || !touch_rail_address[rail] || touch_rpmh_fault) { touch_rpmh_error = 1; return 0; }
    unsigned config = power_rd(0x1822000c), max_tcs = (config >> 12) & 63;
    if (max_tcs < 5 || ((config >> 27) & 31) < 1) { touch_rpmh_error = 2; return 0; }
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    unsigned slot;
    uint64_t base = 0;
    for (slot = 2; slot <= 4; slot++) {
        base = rsc + (uint64_t)slot * 0x2a0;
        if (power_rd(base + 0x18) && !power_rd(base + 0x1c) &&
            !(power_rd(rsc + 4) & (1u << slot))) break;
    }
    if (slot == 5) {
        touch_rpmh_error = 3;
        __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory"); return 0;
    }
    uint32_t old_irq = power_rd(rsc), control = power_rd(base + 0x14);
    uint32_t wait = power_rd(base + 0x10), message = power_rd(base + 0x30);
    uint32_t address = power_rd(base + 0x34), data = power_rd(base + 0x38);
    uint32_t mask = 1u << slot;
    power_wr(base + 0x14, control & ~(1u << 24));
    power_wr(base + 0x14, control & ~((1u << 24) | (1u << 16)));
    power_wr(rsc + 8, mask);
    power_wr(base + 0x30, 0x10108); /* WRITE, response requested, protocol message length */
    power_wr(base + 0x34, touch_rail_address[rail] + 4); /* ENABLE only */
    power_wr(base + 0x38, enable);
    power_wr(base + 0x10, 1); power_wr(base + 0x1c, 1);
    power_wr(rsc, old_irq | mask);
    power_wr(base + 0x14, 1u << 16);
    power_wr(base + 0x14, (1u << 16) | (1u << 24));
    unsigned ticks;
    for (ticks = 0; ticks < 2000; ticks++) {
        if (power_rd(rsc + 4) & mask) break;
        system_table->bs->stall(100);
    }
    uint32_t result = power_rd(base + 0x3c);
    touch_rpmh_result = result;
    int done = ticks < 2000;
    if (done || power_rd(base + 0x18)) {
        power_wr(base + 0x14, 0);
        power_wr(base + 0x1c, 0); power_wr(base + 0x10, wait);
        power_wr(base + 0x30, message); power_wr(base + 0x34, address); power_wr(base + 0x38, data);
        power_wr(rsc + 8, mask); power_wr(rsc, old_irq);
        power_wr(base + 0x14, control);
    } else {
        /* Do not rewrite a live timed-out command. Reboot will reset this state. */
        power_wr(rsc, old_irq & ~mask);
        touch_rpmh_fault = 1;
    }
    __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
    if (!done) touch_rpmh_error = 4;
    else if ((result & 0x10100) != 0x10100) touch_rpmh_error = 5;
    return !touch_rpmh_error;
}
static int touch_rail_ready(unsigned rail)
{
    uint8_t enable, status;
    for (unsigned n = 0; n < 100; n++) {
        if (!power_read(touch_rail_channel[rail], 0x46, &enable, 1) ||
            !power_read(touch_rail_channel[rail], 8, &status, 1)) return 0;
        if ((enable & status & 0x80) != 0) return 1;
        system_table->bs->stall(1000);
    }
    return 0;
}
static int touch_power_enable_begin(void)
{
    text("\r\nMUNCH TOUCH TEMPORARY POWER PROBE 5\r\n");
    text("Only ldoc1/ldoc10 ENABLE votes; voltage and mode are not written.\r\n");
    uint32_t version = power_rd(0x0c440000), count = power_rd(0x0c440004) & 0x7ff;
    if (version < 0x50000000u || version >= 0x70000000u) return 0;
    if (!count || count > 512) count = 512;
    if (power_rd(0x0f521004) & 1) { text("RESET is not LOW: refusing power sequence.\r\n"); return 0; }
    for (unsigned rail = 0; rail < 2; rail++) {
        uint8_t type[2], voltage[2], enable;
        unsigned ppid = rail ? 0x549 : 0x540;
        if (!power_find_channel(ppid, count, &touch_rail_channel[rail]) ||
            !power_read(touch_rail_channel[rail], 4, type, 2) ||
            !power_read(touch_rail_channel[rail], 0x40, voltage, 2) ||
            !power_read(touch_rail_channel[rail], 0x46, &enable, 1)) return 0;
        unsigned mv = voltage[0] | (unsigned)voltage[1] << 8;
        if (type[0] != 4 || (type[1] & 0xf0) != 0x70 || enable != 0 ||
            (rail == 0 ? mv != 1800 : (mv < 3000 || mv > 3312))) {
            text("Rail type/voltage/initial enable differs: NOT changed.\r\n"); return 0;
        }
        touch_rail_address[rail] = touch_resource(rail ? "ldoc10" : "ldoc1");
        field(rail ? "ldoc10 RPMh address: " : "ldoc1 RPMh address: ", touch_rail_address[rail]);
        if (!touch_rail_address[rail]) return 0;
    }
    if (touch_rail_address[0] == touch_rail_address[1]) return 0;
    field("RPMh DRV2 config: ", power_rd(0x1822000c));
    /* Android's order: I/O L1, 1 ms, analog L10. */
    for (unsigned rail = 0; rail < 2; rail++) {
        touch_rail_changed[rail] = 1; /* an unacknowledged request might still have reached hardware */
        if (!touch_enable_vote(rail, 1) || !touch_rail_ready(rail)) {
            text("POWER ENABLE/READY FAILED.\r\n");
            field("RPMh error stage (0=readiness): ", touch_rpmh_error);
            field("RPMh last command status: ", touch_rpmh_result);
            return 0;
        }
        system_table->bs->stall(1000);
    }
    text("TOUCH POWER: BOTH RAILS ENABLED AND READY\r\n");
    return 1;
}
static void touch_power_enable_end(void)
{
    if (!touch_rail_changed[0] && !touch_rail_changed[1]) {
        text("TOUCH POWER: NO ENABLE VOTES ISSUED\r\n"); return;
    }
    int ok = 1;
    for (int rail = 1; rail >= 0; rail--) {
        if (!touch_rail_changed[rail]) continue;
        if (!touch_enable_vote((unsigned)rail, 0)) ok = 0;
        uint8_t enable = 0xff;
        if (!power_read(touch_rail_channel[rail], 0x46, &enable, 1) || (enable & 0x80)) ok = 0;
    }
    text(ok ? "TOUCH POWER: ORIGINAL DISABLED STATE RESTORED\r\n"
            : "TOUCH POWER: RESTORE NOT CONFIRMED; REBOOT REQUIRED\r\n");
}
