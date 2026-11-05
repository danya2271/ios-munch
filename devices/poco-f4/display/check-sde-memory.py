#!/usr/bin/env python3
"""Run the generated loader's memory layout with the shared SDE reservation contract."""
import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path


root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_iosroot', root / 'devices/poco-f4/loader/pack-iosroot.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)
program = r'''
#include <assert.h>
#include <stdio.h>
#include "munch-sde-memory.h"
#define PAGE_SIZE_MAX 0x4000ULL
#define RESIDENT_RD_OFFSET 0x08000000ULL
#define print(...) ((void)0)
static unsigned writes;
static void *zero_scm(void *address, int value, uint64_t size)
{
    assert((uint64_t)address < 0x100000000ULL && value == 0 && size == 0x10000);
    writes++;
    return address;
}
#define memset zero_scm
static uint64_t saved_pool, saved_helper, saved_root, saved_top;
static int layout(uint64_t kern_hi, uint64_t win_base, uint64_t win_size,
    uint64_t helper, uint64_t root_size, bool resident_argument)
{
    struct { uint64_t ramdisk_size, resident_rd; } pi = { helper, resident_argument ? root_size : 0 };
    uint64_t phys_base = win_base, resident_rd_size = resident_argument ? 0 : root_size;
__LAYOUT__
    (void)rd_resident;
    saved_helper = rd_pa;
    saved_root = rd1_pa;
    saved_top = top;
__SAVE_POOL__
    return 1;
}
int main(void)
{
    (void)zero_scm;
    for (unsigned argument = 0; argument < 2; argument++) {
        assert(layout(0xf8260000, 0xf4000000, 0x180000000ULL, 0x400000, 0x93900000, argument));
        assert(saved_helper == 0xf8260000ULL + __SCM_SIZE__ + __POOL_SIZE__);
        assert(saved_root == 0xfc000000 && saved_top > (1ULL << 32));
        assert(saved_helper + 0x400000 <= saved_root);
__POOL_ASSERT__
    }
    assert(writes == (__GPU__ ? 2u : 0u));
    uint64_t pool = 0, helper = 0;
    assert(munch_sde_pool_plan(0xf8270000, 0xf4000000, 0x180000000ULL, 0xfc000000, 0x400000, &pool, &helper));
    assert(pool == 0xf8270000 && helper == pool + MUNCH_SDE_POOL_SIZE);
    assert(munch_sde_pool_valid(pool, MUNCH_SDE_POOL_SIZE, 0xf4000000, 0x194000000ULL));
    for (unsigned scenario = 0; scenario < 11; scenario++) {
        uint64_t after = 0xf8270000, base = 0xf4000000, size = 0x180000000ULL;
        uint64_t resident = 0xfc000000, bytes = 0x400000;
        switch (scenario) {
        case 0: after = 0; break;
        case 1: after += 4096; break;
        case 2: after = 1ULL << 32; break;
        case 3: size = UINT64_MAX; break;
        case 4: after = 0xf0000000; break;
        case 5: size = 0; break;
        case 6: resident = after + MUNCH_SDE_POOL_SIZE - 0x4000; break;
        case 7: resident = after + MUNCH_SDE_POOL_SIZE; break;
        case 8: bytes = UINT64_MAX - 0x3fff; break;
        case 9: bytes = 1; break;
        case 10: resident = base; break;
        }
        pool = helper = UINT64_MAX;
        assert(!munch_sde_pool_plan(after, base, size, resident, bytes, &pool, &helper));
        assert(pool == UINT64_MAX && helper == UINT64_MAX);
    }
__FAILURE_ASSERT__
    printf("Generated loader GPU=%u SDE=%u: disjoint kernel/SCM/pool/helper/root, static low DMA, bounds passed\n",
        __GPU__, __SDE__);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='munch-sde-memory-') as directory:
    work = Path(directory)
    config = {'system': {'size': 0x93900000, 'crc32': '12345678'}}
    for gpu in (False, True):
        for sde in (False, True):
            packer.patch_loader(root / 'xnuloader/loader.c', work, config, False, gpu, sde)
            loader = (work / 'loader.c').read_text()
            assert ('"munch-sde-buffers"' in loader) == sde
            assert ('"munch-scm-metadata"' in loader) == gpu
            if sde:
                assert (work / 'munch-sde-memory.h').read_bytes() == (root / 'xnu/files/iokit/Kernel/munch-sde-memory.h').read_bytes()
            start = loader.index('\tuint64_t scm_pa = kern_hi' if gpu else '\tuint64_t rd_pa = kern_hi;')
            end = loader.index('\n\t/* --- load segments', start)
            test = program.replace('__LAYOUT__', loader[start:end])
            test = test.replace('__SAVE_POOL__', '    saved_pool = sde_pa;' if sde else '    saved_pool = 0;')
            test = test.replace('__SCM_SIZE__', '0x10000' if gpu else '0')
            test = test.replace('__POOL_SIZE__', 'MUNCH_SDE_POOL_SIZE' if sde else '0')
            test = test.replace('__POOL_ASSERT__', '        assert(saved_pool == 0xf8260000ULL + __SCM_SIZE__);' if sde
                                else '        assert(!saved_pool);')
            test = test.replace('__SCM_SIZE__', '0x10000' if gpu else '0')
            test = test.replace('__FAILURE_ASSERT__', r'''
    assert(!layout(0xfc000000 - MUNCH_SDE_POOL_SIZE, 0xf4000000, 0x180000000ULL, 0x400000, 0x93900000, false));
    assert(!layout(0xff800000, 0xf4000000, 0x180000000ULL, 0x400000, 0, false));
    assert(!layout(0x100000000ULL, 0xf4000000, 0x180000000ULL, 0x400000, 0, false));
''' if sde else '')
            test = test.replace('__GPU__', str(int(gpu))).replace('__SDE__', str(int(sde)))
            source = work / 'check.c'
            source.write_text(test)
            binary = work / 'check'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', '-I' + str(root / 'xnu/files/iokit/Kernel'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
