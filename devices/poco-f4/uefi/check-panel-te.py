#!/usr/bin/env python3
"""Exercise the actual TE estimator with synthetic GPIO edges and timer reads."""
from pathlib import Path
import re
import subprocess
import tempfile
source = Path(__file__).with_name('panel-te-probe.inc').read_text()
source = re.sub(r'static uint64_t te_ticks\(void\)\n\{.*?\n\}', 'static uint64_t te_ticks(void) { ticks += 200; return ticks; }', source, count=1, flags=re.S)
source = source.replace('__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));', 'frequency = test_frequency;')
source = source.replace('*(volatile uint32_t *)(uintptr_t)cfg_at', 'test_cfg')
source = source.replace('*(volatile uint32_t *)(uintptr_t)io_at', '(test_period && ticks % test_period < 1000)')
head = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint64_t ticks, test_period, test_frequency=19200000;
static uint32_t test_cfg=4;
static char output[4096];
static void text(const char *s) {assert(strlen(output)+strlen(s)<sizeof(output));strcat(output,s);}
static void field(const char *s,uint64_t x) {(void)s;(void)x;}
'''
tail = r'''
int main(void) {
 test_period=320000;panel_te_probe();assert(strstr(output,"60.00 Hz"));
 output[0]=0;ticks=0;test_period=160000;panel_te_probe();assert(strstr(output,"120.00 Hz"));
 output[0]=0;ticks=0;test_period=0;panel_te_probe();assert(strstr(output,"insufficient TE samples"));
 output[0]=0;test_cfg=0;panel_te_probe();assert(strstr(output,"not confirmed"));
 output[0]=0;test_frequency=0;panel_te_probe();assert(strstr(output,"Unsupported timer"));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='munch-te-test-') as directory:
    work=Path(directory);(work/'test.c').write_text(head+source+tail)
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=address,undefined',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: real estimator, 60/120-Hz synthetic TE, absent edges, wrong mux and invalid timer')
