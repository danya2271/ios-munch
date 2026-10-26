/* SPDX-License-Identifier: MIT
 * Temporary ABI-pinned XNU prototype. UEFI enables and verifies touch power.
 * Replaces the dormant Redwood touch entry/thread in the reviewed prebuilt.
 * Runtime SPI reads only: no firmware download, power or reset commands.
 */
#include <stdint.h>
#include "ft3658u-protocol.h"
extern uintptr_t ml_io_map(uintptr_t, uintptr_t);
extern int kernel_thread_start(void (*)(void *, int), void *, void **);
extern void thread_deallocate(void *);
extern void *current_thread(void);
extern int thread_terminate(void *);
extern void IOSleep(unsigned);
extern void delay_for_interval(unsigned, unsigned);
extern void hakios_touch_post(unsigned, unsigned, unsigned, unsigned);
extern int printf(const char *, ...);
struct state {
    uintptr_t gpio;
    unsigned mosi, miso, active, failures;
    uint16_t x[10], y[10];
};
/* Reuse an otherwise unreachable driver's 1032-byte BSS buffer. */
extern struct state munch_state;
static void touch_thread(void *, int);
__attribute__((section(".text.entry")))
void munch_touch_init(void)
{
    void *thread = 0;
    if (kernel_thread_start(touch_thread, 0, &thread) == 0) thread_deallocate(thread);
}
static uintptr_t cfg(unsigned pin) { return munch_state.gpio + (pin - 8) * 0x1000; }
static unsigned rd(uintptr_t at) { return *(volatile uint32_t *)at; }
static void wr(uintptr_t at, unsigned value)
{
    *(volatile uint32_t *)at = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void level(unsigned pin, unsigned high) { wr(cfg(pin) + 4, high ? 2 : 0); }
static void spi(const uint8_t *tx, uint8_t *rx, unsigned length)
{
    level(11, 0); delay_for_interval(2, 1000);
    for (unsigned i = 0; i < length; i++) {
        unsigned value = 0;
        for (int bit = 7; bit >= 0; bit--) {
            level(munch_state.mosi, (tx[i] >> bit) & 1);
            delay_for_interval(2, 1000);
            level(10, 1); delay_for_interval(2, 1000);
            value = (value << 1) | (rd(cfg(munch_state.miso) + 4) & 1);
            level(10, 0);
        }
        rx[i] = value;
    }
    level(11, 1); delay_for_interval(150, 1000);
}
static int read_reg(unsigned reg, uint8_t *data, unsigned length)
{
    uint8_t tx[80] = {0}, rx[80];
    if (!length || length > 62) return 0;
    tx[0] = reg; tx[1] = 0xa0; tx[3] = length;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        spi(tx, rx, length + 9);
        if ((rx[3] & 0xa0) || munch_ft_crc(rx + 7, length) !=
            (uint16_t)(rx[7 + length] | (uint16_t)rx[8 + length] << 8)) continue;
        for (unsigned i = 0; i < length; i++) data[i] = rx[7 + i];
        return 1;
    }
    return 0;
}
static void release(unsigned mask)
{
    for (unsigned id = 0; id < 10; id++) if (mask & (1u << id))
        hakios_touch_post(3, id, munch_state.x[id], munch_state.y[id]);
    munch_state.active &= ~mask;
}
static void dispatch(const uint8_t report[62])
{
    if ((report[1] & 15) > 10) return;
    unsigned seen = 0;
    for (unsigned slot = 0; slot < 10; slot++) {
        if ((report[4 + 6 * slot] >> 4) >= 10) break;
        struct munch_ft_contact contact;
        if (!munch_ft_contact(report, slot, &contact)) continue;
        unsigned id = contact.id, mask = 1u << id;
        if (contact.flag == 1) continue;
        seen |= mask;
        unsigned x = (unsigned)contact.x * 65535 / 1079;
        unsigned y = (unsigned)contact.y * 65535 / 2399;
        if (!(munch_state.active & mask) || x != munch_state.x[id] || y != munch_state.y[id])
            hakios_touch_post(munch_state.active & mask ? 2 : 1, id, x, y);
        munch_state.x[id] = x; munch_state.y[id] = y;
        munch_state.active |= mask;
    }
    release(munch_state.active & ~seen);
}
static void touch_thread(void *unused, int result)
{
    (void)unused; (void)result;
    munch_state.gpio = ml_io_map(0x0f908000, 0x4000);
    munch_state.active = munch_state.failures = 0;
    if (!munch_state.gpio) goto failed;
    for (unsigned wiring = 0; wiring < 2; wiring++) {
        munch_state.mosi = wiring ? 9 : 8; munch_state.miso = wiring ? 8 : 9;
        for (unsigned pin = 8; pin <= 11; pin++) wr(cfg(pin), 0);
        level(11, 1); level(10, 0); level(munch_state.mosi, 0);
        for (unsigned pin = 8; pin <= 11; pin++) if (pin != munch_state.miso) wr(cfg(pin), 1u << 9);
        IOSleep(1);
        uint8_t hi = 0, lo = 0;
        if (read_reg(0xa3, &hi, 1) && read_reg(0x9f, &lo, 1) && hi == 0x56 && lo == 0x52) {
            printf("MUNCH FT3658U: ID 5652 verified, MOSI%u MISO%u; input ready\n", munch_state.mosi, munch_state.miso);
            for (;;) {
                uint8_t report[62];
                if (read_reg(1, report, 62)) { munch_state.failures = 0; dispatch(report); }
                else if (++munch_state.failures == 5) release(munch_state.active);
                IOSleep(16);
            }
        }
    }
failed:
    printf("MUNCH FT3658U: link failed; input thread stopped\n");
    thread_terminate(current_thread());
}
