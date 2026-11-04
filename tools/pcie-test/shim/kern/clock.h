#ifndef PCIE_TEST_CLOCK_H
#define PCIE_TEST_CLOCK_H

#include <stdint.h>
#include <time.h>

void delay(int usec);

static inline uint64_t
pcie_test_clock(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static inline void
clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
	*deadline = pcie_test_clock() + (uint64_t)interval * scale;
}

#endif
