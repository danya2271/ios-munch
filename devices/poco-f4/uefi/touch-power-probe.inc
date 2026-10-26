/* SPDX-License-Identifier: MIT
 * Read-only PMIC observer diagnostic for munch. No SPMI write opcode exists.
 * Arbiter v5 register layout: Linux spmi-pmic-arb and upstream hakios.buttons.c.
 * Addresses and EE0 are from the phone's live DT. Candidate PMIC SIDs 4/5 are
 * the third PMIC pair; peripheral identity is printed, not assumed from a label.
 */
static uint32_t power_last_status;
static uint32_t power_rd(uint64_t address) { return *(volatile uint32_t *)(uintptr_t)address; }
static void power_hex8(unsigned value)
{
    static const char digits[] = "0123456789ABCDEF";
    char s[3] = {digits[(value >> 4) & 15], digits[value & 15], 0};
    text(s);
}
static int power_find_channel(unsigned ppid, unsigned count, unsigned *chosen)
{
    int found = 0;
    for (unsigned n = 0; n < count; n++) {
        uint32_t map = power_rd(0x0c440900ULL + 4 * n);
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        if (!found || !(power_rd(0x0c40a700ULL + 4 * n) & 7)) {
            *chosen = n; found = 1;
        }
    }
    return found;
}
static int power_read(unsigned channel, unsigned reg, uint8_t *data, unsigned size)
{
    if (channel >= 512 || reg > 255 || !size || size > 4 || reg + size > 256) return 0;
    uint64_t base = 0x0e600000ULL + 0x80ULL * channel, daif;
    __asm__ volatile ("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    /* Extended register READ LONG. Only the observer command word is written. */
    *(volatile uint32_t *)(uintptr_t)base = (1u << 27) | (reg << 4) | (size - 1);
    __asm__ volatile ("dsb sy" ::: "memory");
    unsigned ticks;
    for (ticks = 0; ticks < 2000; ticks++) {
        power_last_status = power_rd(base + 8);
        if (power_last_status & 1) break;
        system_table->bs->stall(1);
    }
    uint32_t value = power_rd(base + 0x18);
    __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
    if (ticks == 2000 || (power_last_status & 15) != 1) return 0;
    for (unsigned i = 0; i < size; i++) data[i] = (uint8_t)(value >> (8 * i));
    return 1;
}
static void power_ldo(unsigned sid, unsigned ldo, unsigned channels)
{
    unsigned channel = 0, peripheral = 0x40 + ldo - 1;
    uint8_t type[2], voltage[2], mode_enable[2], status;
    text(sid == 4 ? "SID4 " : "SID5 "); text(ldo == 1 ? "L1: " : "L10: ");
    if (!power_find_channel((sid << 8) | peripheral, channels, &channel)) {
        text("NO OBSERVER CHANNEL\r\n"); return;
    }
    if (!power_read(channel, 4, type, 2) || !power_read(channel, 8, &status, 1) ||
        !power_read(channel, 0x40, voltage, 2) || !power_read(channel, 0x45, mode_enable, 2)) {
        text("READ FAILED status="); hex(power_last_status); text("\r\n"); return;
    }
    text("type="); power_hex8(type[0]); text(" sub="); power_hex8(type[1]);
    text(" mode="); power_hex8(mode_enable[0]); text(" en="); power_hex8(mode_enable[1]);
    text(" status="); power_hex8(status); text(" Vraw="); power_hex8(voltage[1]); power_hex8(voltage[0]);
    if (type[0] == 4) {
        text(mode_enable[1] & 0x80 ? " ENABLED" : " DISABLED");
        text(status & 0x80 ? " READY" : " NOT_READY");
    } else text(" (peripheral type not confirmed as LDO)");
    text("\r\n");
}
static void touch_power_probe(void)
{
    text("\r\nMUNCH TOUCH POWER READ PROBE 4\r\n");
    text("No GPIO, regulator, firmware or partition writes.\r\n");
    uint32_t version = power_rd(0x0c440000), count = power_rd(0x0c440004) & 0x7ff;
    field("SPMI arbiter version: ", version); field("Reported channels: ", count);
    if (version < 0x50000000u || version >= 0x70000000u) {
        text("Unsupported arbiter layout: no transfers attempted.\r\n"); return;
    }
    if (!count || count > 512) count = 512;
    text("--- COPY THESE POWER RESULTS ---\r\n");
    power_ldo(4, 1, count); power_ldo(4, 10, count);
    power_ldo(5, 1, count); power_ldo(5, 10, count);
    text("Vraw is an unconverted register value; supply mapping still under verification.\r\n");
    text("TOUCH POWER READ PROBE FINISHED\r\n");
}
