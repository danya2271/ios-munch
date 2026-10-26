#!/usr/bin/env python3
"""Test the actual PAS convention helper carried by the A650 source patch."""
from pathlib import Path
import re
import subprocess
import tempfile
patch = (Path(__file__).resolve().parent / 'umi-a650.patch').read_text()
added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
match = re.search(r'static uint64_t scm_pas_function\(uint64_t function\)\n\{.*?\n\}', added, re.S)
if not match:
    raise ValueError('Missing SCM width helper')
head = '''#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#define PAGE_SIZE 0x4000ull
#define SCM_64 (1ull << 30)
#define SCM_CALL(s, c) ((uint64_t)(s) << 8 | (c))
static uint64_t scm_width;
'''
region = re.search(r'static bool scm_region_valid\(uint64_t pa, uint64_t bytes\)\n\{.*?\n\}', added, re.S)
if not region: raise ValueError('Missing reservation geometry helper')
retry = re.search(r'static bool scm_retry_allowed\(unsigned calls, uint64_t nanos\)\n\{.*?\n\}', added, re.S)
if not retry: raise ValueError('Missing SCM resume policy')
tests = '''
int main(void) {
 assert(scm_retry_allowed(100,1000000));
 assert(scm_retry_allowed(4096,100000000));
 assert(scm_retry_allowed(1048575,4999999999ull));
 assert(!scm_retry_allowed(1048576,0));
 assert(!scm_retry_allowed(1,5000000000ull));
 assert(scm_region_valid(0xf8000000,0x10000));
 assert(scm_region_valid(0xffff0000,0x10000));
 assert(!scm_region_valid(0xffff4000,0x10000));
 assert(!scm_region_valid(0xf8001000,0x10000));
 assert(!scm_region_valid(0xf8000000,0x4000));
 assert(!scm_region_valid(0,0x10000));
 assert(!scm_region_valid(0x100000000ull,0x10000));
 assert(!scm_region_valid(0xffffffffffffc000ull,0x10000));
 scm_width=SCM_64;
 assert(scm_pas_function(0x42000201)==0x42000201);
 assert(scm_pas_function(0xc2000207)==0xc2000207);
 scm_width=0;
 assert(scm_pas_function(0x42000201)==0x02000201);
 assert(scm_pas_function(0xc2000207)==0x82000207);
 assert(scm_pas_function(0x42000601)==0x42000601);
 assert(scm_pas_function(0xc4000003)==0xc4000003);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='munch-scm-') as directory:
    work=Path(directory)
    (work/'test.c').write_text(head+match.group()+region.group()+retry.group()+tests)
    subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: SMC32/64 PAS selection, call-type preservation, availability/PSCI isolation reserved-buffer bounds and bounded resume beyond 100 calls')
