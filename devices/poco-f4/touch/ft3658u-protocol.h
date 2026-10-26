/* SPDX-License-Identifier: MIT
 * FT3658U SPI_V2 packet CRC and extended-coordinate decoding.
 * Protocol reference: Xiaomi munch-s-oss focaltech_3658u; see TOUCH-BRINGUP.md.
 * No firmware, bus access or controller writes.
 */
#ifndef MUNCH_FT3658U_PROTOCOL_H
#define MUNCH_FT3658U_PROTOCOL_H
#include <stdint.h>
#define MUNCH_FT_REPORT_BYTES 62
struct munch_ft_contact { uint16_t x, y; uint8_t id, flag; };

static inline uint16_t munch_ft_crc(const uint8_t *bytes, unsigned size)
{
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0x8408 : 0));
    }
    return crc;
}

static inline int munch_ft_contact(const uint8_t report[MUNCH_FT_REPORT_BYTES],
                                   unsigned slot, struct munch_ft_contact *out)
{
    if (slot >= 10 || (report[1] & 15) > 10) return 0;
    unsigned off = 6 * slot, id = report[4 + off] >> 4, flag = report[2 + off] >> 6;
    if (id >= 10 || flag > 2 || (!(report[1] & 15) && flag != 1)) return 0;
    unsigned raw_x = ((report[2 + off] & 15) << 12) | (report[3 + off] << 4) | (report[6 + off] >> 4);
    unsigned raw_y = ((report[4 + off] & 15) << 12) | (report[5 + off] << 4) | (report[6 + off] & 15);
    unsigned x = raw_x / 16, y = raw_y / 16;
    if (x >= 1080 || y >= 2400) return 0;
    out->x = (uint16_t)x; out->y = (uint16_t)y;
    out->id = (uint8_t)id; out->flag = (uint8_t)flag;
    return 1;
}
#endif
