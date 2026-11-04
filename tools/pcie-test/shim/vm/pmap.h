#ifndef PCIE_TEST_PMAP_H
#define PCIE_TEST_PMAP_H

#include <mach/vm_param.h>

unsigned pmap_cache_attributes(ppnum_t page);
void pmap_set_cache_attributes(ppnum_t page, unsigned attributes);

#endif
