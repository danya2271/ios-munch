#ifndef PCIE_TEST_MACHINE_ROUTINES_H
#define PCIE_TEST_MACHINE_ROUTINES_H

#include <stdbool.h>
typedef bool boolean_t;
extern _Thread_local bool pcie_test_interrupts_disabled;

static inline boolean_t
ml_set_interrupts_enabled(boolean_t enabled)
{
	bool previous = !pcie_test_interrupts_disabled;
	pcie_test_interrupts_disabled = !enabled;
	return previous;
}

#endif
