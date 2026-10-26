#!/usr/bin/env python3
"""Execute the actual probe with simulated clocks, counters and UEFI stalls."""
from pathlib import Path
import subprocess
import tempfile
stub = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#define MUNCH_SCAN_TEST 1
static uint64_t now, timer_hz = 1000000;
static uint32_t gcc, ahb, power = 0x80000000;
static unsigned reads, writes, periods, deny_ahb;
static void text(const char *s) {(void)s;}
static void hex(uint64_t x) {(void)x;}
static void field(const char *s, uint64_t x) {
 if (!strcmp(s,"Measured frame period us: ")) { assert(x>=16000 && x<=17000); periods++; }
}
static void stall(unsigned us) {now+=us;}
static struct {void (*stall)(unsigned);} bs = {stall};
static struct {void *unused; typeof(bs) *bs;} table = {0,&bs}, *system_table = &table;
static uint64_t scan_ticks(void) {return now;}
static uint64_t scan_frequency(void) {return timer_hz;}
static uint32_t scan_read(uintptr_t at) {
 if(at==0x10b00c) return gcc;
 assert(gcc & 1);
 if(at==0xaf03000) return power;
 if(at==0xaf02080) return deny_ahb ? 0x80000001 : ahb;
 assert(power & 0x80000000); assert(ahb & 1); reads++;
 switch(at-0xae6b800) {
 case 0x294:return 2400;
 case 0x298:return 2400+(now%16500)/6;
 case 0x2b0:return now%16500<12000 ? (now%16500)/5 : 2400;
 default:return 0;
 }
}
static void scan_write(uintptr_t at,uint32_t x) {
 writes++;
 if(at==0x10b00c)gcc=x;
 else {assert(at==0xaf02080 && (gcc&1));ahb=x;}
}
'''
test = r'''
int main(void) {
 display_scan_probe();assert(reads && periods==3 && gcc==0 && ahb==0 && writes==4);
 reads=writes=0;power=0;display_scan_probe();assert(!reads && gcc==0 && writes==2);
 reads=writes=0;power=0x80000000;deny_ahb=1;
 display_scan_probe();assert(!reads && gcc==0);
 deny_ahb=0;timer_hz=0;reads=writes=0;
 display_scan_probe();assert(!reads && !writes);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p/'test.c').write_text(stub + Path(__file__).with_name('display-scan-probe.inc').read_text() + test)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: counter measurement, power/clock guards, timeout, restoration, invalid timer')
