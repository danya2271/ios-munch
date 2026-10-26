/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include "xnu-ft3658u.c"
struct state munch_state;
static struct { unsigned type, id, x, y; } events[32];
static unsigned count;
void hakios_touch_post(unsigned type, unsigned id, unsigned x, unsigned y)
{
    assert(count < 32);
    events[count].type = type; events[count].id = id;
    events[count].x = x; events[count++].y = y;
}
uintptr_t ml_io_map(uintptr_t p, uintptr_t n) { (void)p; (void)n; return 0; }
int kernel_thread_start(void (*f)(void *, int), void *a, void **t) { (void)f; (void)a; (void)t; return 1; }
void thread_deallocate(void *t) { (void)t; }
void *current_thread(void) { return 0; }
int thread_terminate(void *t) { (void)t; return 0; }
void IOSleep(unsigned n) { (void)n; }
void delay_for_interval(unsigned n, unsigned scale) { (void)n; (void)scale; }
static void report(uint8_t *r, unsigned x, unsigned y, unsigned id, unsigned flag)
{
    memset(r, 0xff, 62); r[1] = flag == 1 ? 0 : 1;
    x *= 16; y *= 16;
    r[2] = (flag << 6) | (x >> 12); r[3] = x >> 4;
    r[4] = (id << 4) | (y >> 12); r[5] = y >> 4;
    r[6] = ((x & 15) << 4) | (y & 15);
}
int main(void)
{
    uint8_t r[62];
    report(r, 0, 0, 2, 0); dispatch(r);
    assert(count == 1 && events[0].type == 1 && events[0].id == 2);
    dispatch(r); assert(count == 1);
    report(r, 1079, 2399, 2, 2); dispatch(r);
    assert(count == 2 && events[1].type == 2 && events[1].x == 65535 && events[1].y == 65535);
    r[1] = 15; dispatch(r); assert(count == 2 && munch_state.active == 4);
    report(r, 1079, 2399, 2, 1); dispatch(r);
    assert(count == 3 && events[2].type == 3 && !munch_state.active);
    report(r, 500, 1000, 0, 0); dispatch(r);
    memset(r, 0xff, 62); r[1] = 0; dispatch(r);
    assert(count == 5 && events[4].type == 3 && !munch_state.active);
    return 0;
}
