/* SPDX-License-Identifier: MIT
 * Munch FT3658U diagnostic. Mode-0 GPIO SPI, runtime READ commands only.
 * No power/reset/flash/boot-ROM commands. Original SPI pin states restored.
 * Pin tiles and wire format are documented in ../TOUCH-BRINGUP.md.
 */
#define TOUCH_NORTH 0x0f900000ULL
#define TOUCH_SOUTH 0x0f500000ULL
#define TOUCH_PIN_MOSI 8
#define TOUCH_PIN_MISO 9
#define TOUCH_PIN_CLK 10
#define TOUCH_PIN_CS 11
static const unsigned touch_pins[4] = {8, 9, 10, 11};
static uint32_t touch_saved_cfg[4], touch_saved_io[4];
static uint32_t touch_good, touch_bad;
#ifdef MUNCH_TOUCH_RELEASE_RESET
static uint32_t touch_saved_reset_cfg, touch_saved_reset_io;
static unsigned touch_reset_changed;
#endif
static unsigned touch_mosi = TOUCH_PIN_MOSI, touch_miso = TOUCH_PIN_MISO;
static uint8_t touch_last_rx[10], touch_attempt_rx[2][10];

static void touch_show_bytes(const char *label, const uint8_t bytes[10])
{
    static const char digits[] = "0123456789ABCDEF";
    text(label);
    for (unsigned i = 0; i < 10; i++) {
        char word[4] = {digits[bytes[i] >> 4], digits[bytes[i] & 15], ' ', 0};
        text(word);
    }
    text("\r\n");
}

static uint32_t touch_rd(uint64_t address) { return *(volatile uint32_t *)(uintptr_t)address; }
static void touch_wr(uint64_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}
static uint64_t touch_cfg(unsigned pin) { return TOUCH_NORTH + 0x1000ULL * pin; }
static void touch_level(unsigned pin, unsigned high) { touch_wr(touch_cfg(pin) + 4, high ? 2 : 0); }
static void touch_delay(unsigned us) { system_table->bs->stall(us); }

#include "../touch/ft3658u-protocol.h"

static void touch_spi(const uint8_t *tx, uint8_t *rx, unsigned size)
{
    touch_level(TOUCH_PIN_CS, 0);
    touch_delay(2);
    for (unsigned i = 0; i < size; i++) {
        unsigned value = 0;
        for (int bit = 7; bit >= 0; bit--) {
            touch_level(touch_mosi, (tx[i] >> bit) & 1);
            touch_delay(2);
            touch_level(TOUCH_PIN_CLK, 1);
            touch_delay(2);
            value = (value << 1) | (touch_rd(touch_cfg(touch_miso) + 4) & 1);
            touch_level(TOUCH_PIN_CLK, 0);
        }
        rx[i] = (uint8_t)value;
    }
    touch_level(TOUCH_PIN_CS, 1);
    touch_delay(150);
}

static int touch_read(unsigned address, uint8_t *data, unsigned size)
{
    uint8_t tx[80] = {0}, rx[80] = {0};
    if (!size || size > 62) return 0;
    tx[0] = (uint8_t)address; tx[1] = 0xa0;
    tx[2] = (uint8_t)(size >> 8); tx[3] = (uint8_t)size;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        touch_spi(tx, rx, size + 9);
        for (unsigned i = 0; i < 10; i++) touch_last_rx[i] = rx[i];
        if ((rx[3] & 0xa0) || munch_ft_crc(rx + 7, size) !=
            (uint16_t)(rx[7 + size] | ((uint16_t)rx[8 + size] << 8))) continue;
        for (unsigned i = 0; i < size; i++) data[i] = rx[7 + i];
        touch_good++;
        return 1;
    }
    touch_bad++;
    return 0;
}

static void touch_restore(void)
{
    touch_level(TOUCH_PIN_CS, 1);
    for (unsigned i = 0; i < 4; i++) {
        touch_wr(touch_cfg(touch_pins[i]) + 4, touch_saved_io[i] & 2);
        touch_wr(touch_cfg(touch_pins[i]), touch_saved_cfg[i]);
    }
    text("SPI pin configuration restored.\r\n");
#ifdef MUNCH_TOUCH_RELEASE_RESET
    if (touch_reset_changed) {
        touch_wr(TOUCH_SOUTH + 33 * 0x1000 + 4, touch_saved_reset_io & 2);
        touch_wr(TOUCH_SOUTH + 33 * 0x1000, touch_saved_reset_cfg);
        text("RESET33 configuration restored.\r\n");
    }
#endif
}

static void touch_probe(efi_system_table *table, efi_gop *gop)
{
    uint8_t high = 0, low = 0, version = 0;
    unsigned positions = 0, last_x = ~0u, last_y = ~0u;
#ifdef MUNCH_TOUCH_POWER_ENABLE
    text("\r\nMUNCH FT3658U POWERED LINK PROBE 5\r\n");
    text("Power ENABLE votes active. No firmware commands.\r\n");
#elif defined(MUNCH_TOUCH_RELEASE_RESET)
    text("\r\nMUNCH FT3658U RESET-RELEASE PROBE 3\r\n");
    text("RESET33 release only. No regulator or firmware commands.\r\n");
#else
    text("\r\nMUNCH FT3658U RUNTIME READ PROBE 2\r\n");
    text("No reset, regulator or firmware commands.\r\n");
#endif
    field("Reset GPIO33 cfg: ", touch_rd(TOUCH_SOUTH + 33 * 0x1000));
    field("Reset GPIO33 level: ", touch_rd(TOUCH_SOUTH + 33 * 0x1000 + 4));
    field("IRQ GPIO39 level: ", touch_rd(TOUCH_SOUTH + 39 * 0x1000 + 4));
    for (unsigned i = 0; i < 4; i++) {
        touch_saved_cfg[i] = touch_rd(touch_cfg(touch_pins[i]));
        touch_saved_io[i] = touch_rd(touch_cfg(touch_pins[i]) + 4);
        field("SPI GPIO cfg before: ", touch_saved_cfg[i]);
    }
    uint32_t reset_io = touch_rd(TOUCH_SOUTH + 33 * 0x1000 + 4);
#ifdef MUNCH_TOUCH_RELEASE_RESET
    touch_saved_reset_cfg = touch_rd(TOUCH_SOUTH + 33 * 0x1000);
    touch_saved_reset_io = reset_io;
    if (!(reset_io & 1)) {
        /* Preload HIGH before enabling output: never introduce a LOW pulse.
         * Android DT specifies GPIO mode, pull-up, 8 mA; wait 200 ms as its driver.
         */
        touch_reset_changed = 1;
        touch_wr(TOUCH_SOUTH + 33 * 0x1000 + 4, 2);
        touch_wr(TOUCH_SOUTH + 33 * 0x1000, 0x2c3);
        touch_delay(200000);
    }
    reset_io = touch_rd(TOUCH_SOUTH + 33 * 0x1000 + 4);
    field("RESET33 applied cfg: ", touch_rd(TOUCH_SOUTH + 33 * 0x1000));
    text(reset_io & 1 ? "RESET33 input during test: HIGH\r\n" : "RESET33 input during test: LOW\r\n");
    text(reset_io & 2 ? "RESET33 latch during test: HIGH\r\n" : "RESET33 latch during test: LOW\r\n");
#endif
    int linked = 0;
    for (unsigned wiring = 0; wiring < 2; wiring++) {
        touch_mosi = wiring ? 9 : 8;
        touch_miso = wiring ? 8 : 9;
        text(wiring ? "SPI B: MOSI9 MISO8 CLK10 CS11\r\n" : "SPI A: MOSI8 MISO9 CLK10 CS11\r\n");
        /* Release both data lines before changing the candidate direction. */
        for (unsigned i = 0; i < 4; i++) touch_wr(touch_cfg(touch_pins[i]), 0);
        touch_level(TOUCH_PIN_CS, 1); touch_level(TOUCH_PIN_CLK, 0); touch_level(touch_mosi, 0);
        for (unsigned i = 0; i < 4; i++)
            if (touch_pins[i] != touch_miso) touch_wr(touch_cfg(touch_pins[i]), 1u << 9);
        touch_delay(1000);
        high = low = 0;
        int high_ok = touch_read(0xa3, &high, 1);
        for (unsigned i = 0; i < 10; i++) touch_attempt_rx[wiring][i] = touch_last_rx[i];
        touch_show_bytes("A3 RX: ", touch_last_rx);
        int low_ok = touch_read(0x9f, &low, 1);
        touch_show_bytes("9F RX: ", touch_last_rx);
        if (high_ok && low_ok && high == 0x56 && low == 0x52) { linked = 1; break; }
        touch_level(TOUCH_PIN_CS, 1);
    }
    if (!linked) {
        touch_restore();
        text("--- TOUCH DIAGNOSTIC SUMMARY ---\r\n");
        text(reset_io & 1 ? "RESET33 input: HIGH\r\n" : "RESET33 input: LOW\r\n");
        text(reset_io & 2 ? "RESET33 output latch: HIGH\r\n" : "RESET33 output latch: LOW\r\n");
        touch_show_bytes("SPI A RX: ", touch_attempt_rx[0]);
        touch_show_bytes("SPI B RX: ", touch_attempt_rx[1]);
        field("CRC-valid reads: ", touch_good); field("Failed reads: ", touch_bad);
        text("TOUCH LINK: NO VERIFIED FT3658U RESPONSE\r\n");
        return;
    }
    text("TOUCH LINK: PASS (ID 5652, CRC checked)\r\n");
    if (touch_read(0xa6, &version, 1)) field("Firmware version: ", version);
#ifdef MUNCH_TOUCH_HANDOFF
    touch_handoff_ready = 1;
    text("TOUCH HANDOFF: verified link retained for XNU\r\n");
    return;
#else
    text("Touch the screen for 30 seconds. Green marks indicate received contacts.\r\n");
    for (unsigned tick = 0; tick < 600; tick++) {
        uint8_t report[62];
        if (touch_read(0x01, report, sizeof(report)) && (report[1] & 15) <= 10 && (report[1] & 15)) {
            for (unsigned slot = 0; slot < 10; slot++) {
                unsigned off = 6 * slot, id = report[4 + off] >> 4, flag = report[2 + off] >> 6;
                if (id >= 10) break;
                if (flag != 0 && flag != 2) continue;
                struct munch_ft_contact contact;
                if (!munch_ft_contact(report, slot, &contact)) continue;
                unsigned x = contact.x, y = contact.y;
                positions++;
                if (positions < 8) { field("Touch X: ", x); field("Touch Y: ", y); }
                last_x = x; last_y = y;
                if (gop && gop->mode && gop->mode->info && gop->mode->fb_base == 0x9c000000 &&
                    gop->mode->info->width == 1080 && gop->mode->info->height == 2400 &&
                    gop->mode->info->pixels_per_scanline == 1080) {
                    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)gop->mode->fb_base;
                    for (unsigned dy = 0; dy < 7 && y + dy < 2400; dy++)
                        for (unsigned dx = 0; dx < 7 && x + dx < 1080; dx++) fb[(y + dy) * 1080 + x + dx] = 0x00ff00;
                }
            }
        }
        table->bs->stall(50000);
    }
    field("Contact positions: ", positions); field("Last X: ", last_x); field("Last Y: ", last_y);
    field("CRC-valid reads: ", touch_good); field("Failed reads: ", touch_bad);
    touch_restore();
    text(positions ? "TOUCH CONTACTS: PASS\r\n" : "TOUCH CONTACTS: NONE OBSERVED\r\n");
#endif
}
