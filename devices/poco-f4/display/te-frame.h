/* SPDX-License-Identifier: MIT
 * Pure policy for the opt-in munch 60-Hz TE presentation experiment.
 */
#ifndef MUNCH_TE_FRAME_H
#define MUNCH_TE_FRAME_H
#include <stdint.h>
static inline int munch_te_geometry(uint32_t w, uint32_t h, uint32_t stride)
{
    return w == 1080 && h == 2400 && stride == 4320;
}
static inline int munch_te_period_valid(uint64_t ns)
{
    return ns >= 15000000 && ns <= 18000000;
}
/* Return a start time only if a bounded copy budget fits before the next TE.
 * All arguments use the same monotonic timer units. Zero means resync/fallback.
 */
static inline uint64_t munch_te_copy_time(uint64_t edge, uint64_t now,
                                         uint64_t period, uint64_t delay, uint64_t budget)
{
    if (!edge || !period || now < edge || edge > UINT64_MAX - period ||
        delay >= period || !budget || budget >= period - delay) return 0;
    uint64_t start = now > edge + delay ? now : edge + delay;
    if (start >= edge + period || budget >= edge + period - start) return 0;
    return start;
}
#endif
