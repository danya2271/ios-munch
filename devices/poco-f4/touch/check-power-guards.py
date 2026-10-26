#!/usr/bin/env python3
"""Exercise the actual parser/vote C functions with host-only mocked MMIO."""
from pathlib import Path
import re,subprocess
s=(Path(__file__).resolve().parents[1] / 'uefi/touch-power-enable.inc').read_text()
f=s[s.index('static uint32_t touch_resource('):s.index('static int touch_rail_ready(')]
f=re.sub(r'__asm__ volatile \("mrs %0, daif\\nmsr daifset, #2" : "=r"\(daif\) :: "memory"\);','daif = 0;',f)
f=re.sub(r'__asm__ volatile \("msr daif, %0" :: "r"\(daif\) : "memory"\);','(void)daif;',f)
head=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t regs[0x10000/4], touch_rail_address[2]={0x40000,0x40020};
static unsigned touch_rpmh_fault, touch_rpmh_error, writes, issued, inject_busy;
static uint32_t touch_rpmh_result;
static void fake_stall(unsigned long x) {(void)x;}
static struct {void (*stall)(unsigned long);} fake_bs={fake_stall};
static struct {typeof(fake_bs)*bs;} fake_st={&fake_bs}, *system_table=&fake_st;
static uint16_t power_db16(uint64_t x) {return *(uint16_t*)(uintptr_t)x;}
static uint32_t power_rd(uint64_t x) {
 if(x>=0x80860000 && x<0x80880000) return *(uint32_t*)(uintptr_t)x;
 assert(x>=0x18220000 && x<0x18230000);return regs[(x-0x18220000)/4];
}
static void power_wr(uint64_t x,uint32_t v) {
 assert(x>=0x18220d00 && x<0x18230000); writes++;
 if(x==0x18220d08) {regs[0xd04/4]&=~v;return;}
 regs[(x-0x18220000)/4]=v;
 for(unsigned n=2;n<=4;n++) {
  uint64_t b=0x18220d00+n*0x2a0;
  if(x!=b+0x14 || !(v&(1u<<24)))continue;
  assert(power_rd(b+0x34)==touch_rail_address[0]+4 || power_rd(b+0x34)==touch_rail_address[1]+4);
  assert(power_rd(b+0x38)<=1 && power_rd(b+0x30)==0x10108); issued++;
  if(inject_busy) regs[(b+0x18-0x18220000)/4]=0;
  else {regs[0xd04/4]|=1u<<n;regs[(b+0x3c-0x18220000)/4]=0x10100;}
 }
}
static void reset(void) {
 memset(regs,0,sizeof(regs)); writes=issued=touch_rpmh_fault=inject_busy=0;
 regs[0xc/4]=(8u<<12)|(16u<<27);regs[0xd00/4]=0x55;
 for(unsigned n=2;n<=4;n++)regs[(0xd00+n*0x2a0+0x18)/4]=1;
}
'''
tail=r'''
int main(void) {
 reset();uint32_t before[sizeof(regs)/sizeof(regs[0])];memcpy(before,regs,sizeof(regs));
 assert(touch_enable_vote(0,1) && issued==1 && !touch_rpmh_fault);
 for(unsigned n=0;n<sizeof(regs)/sizeof(regs[0]);n++) {
  if(n==(0xd00+2*0x2a0+0x3c)/4)continue;assert(before[n]==regs[n]);
 }
 reset(); assert(touch_enable_vote(1,0) && issued==1);
 reset();regs[0xc/4]=0;assert(!touch_enable_vote(0,1) && !writes);
 reset();for(unsigned n=2;n<=4;n++)regs[(0xd00+n*0x2a0+0x1c)/4]=1;
 assert(!touch_enable_vote(0,1) && !writes);
 reset();assert(!touch_enable_vote(2,1) && !writes);assert(!touch_enable_vote(0,2) && !writes);
 reset();regs[0xd04/4]=1u<<2;assert(touch_enable_vote(0,1));assert(regs[0xd04/4]&(1u<<2));
 reset();inject_busy=1;assert(!touch_enable_vote(0,1) && touch_rpmh_fault && issued==1);
 unsigned w=writes;assert(!touch_enable_vote(0,0) && writes==w);
 unsigned char *db=mmap((void*)0x80860000,0x20000,PROT_READ|PROT_WRITE,
  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(db!=(void*)-1);
 const unsigned char magic[]={0xdb,0x30,3,0x0c};memcpy(db+4,magic,4);
 *(uint16_t*)(db+8)=4;*(uint16_t*)(db+14)=2;
 memcpy(db+144,"ldoc1",6);*(uint32_t*)(db+160)=0x40000;
 memcpy(db+168,"ldoc10",7);*(uint32_t*)(db+184)=0x40020;
 assert(touch_resource("ldoc1")==0x40000 && touch_resource("ldoc10")==0x40020);
 assert(!touch_resource("ldoc2"));
 memcpy(db+168,"ldoc1\0\0\0",8);assert(!touch_resource("ldoc1"));
 *(uint16_t*)(db+10)=1;assert(!touch_resource("ldoc1"));
 *(uint16_t*)(db+10)=65532;assert(!touch_resource("ldoc1"));
 *(uint16_t*)(db+10)=0;db[4]=0;assert(!touch_resource("ldoc1"));
 munmap(db,0x20000);puts("RPMh address/value guards, slot preservation, pending-slot skip, timeout lockout and CMD DB validation: PASS");
}
'''
p=Path('/tmp/munch-test-power-guards.c');p.write_text(head+f+tail)
subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(p),'-o','/tmp/munch-test-power-guards'],check=True)
subprocess.run(['/tmp/munch-test-power-guards'],check=True)
