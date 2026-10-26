/* SPDX-License-Identifier: MIT */
#include "ft3658u-protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void encode(uint8_t *report, unsigned id, unsigned flag, unsigned x, unsigned y)
{
    memset(report, 0xff, 62);
    report[1] = flag == 1 ? 0 : 1;
    report[2] = (uint8_t)((flag << 6) | (x >> 12));
    report[3] = (uint8_t)(x >> 4);
    report[4] = (uint8_t)((id << 4) | (y >> 12));
    report[5] = (uint8_t)(y >> 4);
    report[6] = (uint8_t)((x << 4) | (y & 15));
}
int main(void)
{
    const uint8_t check[] = "123456789";
    assert(munch_ft_crc(check, 9) == 0x6f91);
    assert(munch_ft_crc(check, 0) == 0xffff);
    uint8_t report[62]; struct munch_ft_contact contact;
    encode(report, 0, 0, 0, 0);
    assert(munch_ft_contact(report, 0, &contact) && !contact.x && !contact.y && !contact.flag);
    encode(report, 9, 2, 1079 * 16 + 9, 2399 * 16 + 11);
    assert(munch_ft_contact(report, 0, &contact) && contact.x == 1079 && contact.y == 2399 && contact.id == 9 && contact.flag == 2);
    assert(!munch_ft_contact(report, 10, &contact));
    assert(!munch_ft_contact(report, 1, &contact));
    encode(report, 0, 1, 300 * 16, 400 * 16);
    assert(munch_ft_contact(report, 0, &contact) && contact.flag == 1);
    encode(report, 0, 0, 1080 * 16, 0);
    assert(!munch_ft_contact(report, 0, &contact));
    encode(report, 0, 0, 0, 2400 * 16);
    assert(!munch_ft_contact(report, 0, &contact));
    encode(report, 10, 0, 0, 0);
    assert(!munch_ft_contact(report, 0, &contact));
    encode(report, 0, 0, 0, 0); report[1] = 11;
    assert(!munch_ft_contact(report, 0, &contact));
    puts("FT3658U CRC, extended coordinates, release, limits and malformed reports: PASS");
}
