/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include "te-frame.h"
int main(void) {
    assert(munch_te_geometry(1080,2400,4320));
    assert(!munch_te_geometry(1080,2400,4352));
    assert(munch_te_period_valid(16480000));
    assert(!munch_te_period_valid(8333333));
    assert(munch_te_copy_time(1000000,2000000,16480000,14000000,2000000)==15000000);
    assert(!munch_te_copy_time(1000000,17000000,16480000,14000000,2000000));
    assert(!munch_te_copy_time(1000000,2000000,16480000,14000000,3000000));
    assert(!munch_te_copy_time(UINT64_MAX-10,UINT64_MAX-5,16480000,14000000,2000000));
    assert(!munch_te_copy_time(0,0,16480000,14000000,2000000));
    assert(!munch_te_copy_time(100,99,16480000,14000000,2000000));
    return 0;
}
