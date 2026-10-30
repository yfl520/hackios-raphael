#!/usr/bin/env python3
"""Exercise actual RPMh read transaction and timeout quarantine with MMIO mock."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-rpmh.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
static struct { uintptr_t base; } rsc;
#define RSC_TCS(n) (0xd00+672*(n))
#define TCS_CONTROL 0x14
#define TCS_STATUS 0x18
#define TCS_ENABLED 0x1c
#define TCS_COMMAND(c) (0x30+20*(c))
#define tell printf
static uint32_t regs[0x10000/4];
static unsigned writes, sleeps, scenario;
static bool expect_write;
static uint32_t rd(uintptr_t p) {assert(p<sizeof(regs)&&!(p&3));return regs[p/4];}
static void wr(uintptr_t p,uint32_t v) {
    assert(p<sizeof(regs)&&!(p&3));writes++;
    if(p==RSC_TCS(0)+8) {regs[(RSC_TCS(0)+4)/4]&=~v;return;}
    regs[p/4]=v;
    if(p==RSC_TCS(1)+TCS_CONTROL&&(v&(1u<<24))) {
        assert(rd(RSC_TCS(1)+TCS_COMMAND(0))==(expect_write?0x10108u:0x108u));
        assert(rd(RSC_TCS(1)+TCS_COMMAND(0)+4)==0x40100);
        assert(rd(RSC_TCS(1)+TCS_COMMAND(0)+8)==(expect_write?3100u:0u));
        assert(rd(RSC_TCS(1)+TCS_ENABLED)==1);
        if(scenario!=2) {
            regs[(RSC_TCS(0)+4)/4]=2;
            regs[(RSC_TCS(1)+TCS_COMMAND(0)+12)/4]=scenario==1?0x100:0x10100;
            regs[(RSC_TCS(1)+TCS_COMMAND(0)+16)/4]=0x1234;
        }
    }
}
static void settle(void) {}
static void sleep_us(unsigned us) {assert(us==50);sleeps++;}
''' + source + r'''
static void reset(void) {
    memset(regs,0,sizeof(regs));writes=sleeps=0;scenario=0;expect_write=false;raphael_rpmh_faulted=false;
    regs[(RSC_TCS(1)+TCS_STATUS)/4]=1;
    regs[(RSC_TCS(1)+TCS_CONTROL)/4]=0x10000;
    regs[(RSC_TCS(1)+TCS_COMMAND(0))/4]=0x10108;
    regs[(RSC_TCS(1)+TCS_COMMAND(0)+4)/4]=0xdead;
    regs[(RSC_TCS(1)+TCS_COMMAND(0)+8)/4]=0xbeef;
    regs[(RSC_TCS(1)+0x10)/4]=0x100;
    regs[RSC_TCS(0)/4]=1;
}
static void restored(void) {
    assert(rd(RSC_TCS(1)+TCS_CONTROL)==0x10000);
    assert(rd(RSC_TCS(1)+TCS_ENABLED)==0);
    assert(rd(RSC_TCS(1)+TCS_COMMAND(0))==0x10108);
    assert(rd(RSC_TCS(1)+TCS_COMMAND(0)+4)==0xdead);
    assert(rd(RSC_TCS(1)+TCS_COMMAND(0)+8)==0xbeef);
    assert(rd(RSC_TCS(1)+0x10)==0x100);
    assert(rd(RSC_TCS(0))==1&&rd(RSC_TCS(0)+4)==0);
}
int main(void) {
    uint32_t v;
    assert(raphael_rpmh_decode_vrm(0x80000708,0,&v)&&v==1800);
    assert(raphael_rpmh_decode_vrm(0x80000001,1,&v)&&v==1);
    assert(raphael_rpmh_decode_vrm(0x80000004,2,&v)&&v==4);
    for(unsigned f=0;f<3;f++)assert(raphael_rpmh_decode_vrm(0,f,&v)&&v==0);
    v=99;assert(!raphael_rpmh_decode_vrm(0x2000,0,&v)&&v==99);
    assert(!raphael_rpmh_decode_vrm(2,1,&v)&&v==99);
    assert(!raphael_rpmh_decode_vrm(8,2,&v)&&v==99);
    assert(!raphael_rpmh_decode_vrm(0,3,&v)&&v==99);
    reset();v=99;assert(raphael_rpmh_read_locked(0x40100,&v)&&v==0x1234);restored();
    reset();expect_write=true;assert(raphael_rpmh_write_locked(0x40100,3100));restored();
    reset();expect_write=true;scenario=1;assert(!raphael_rpmh_write_locked(0x40100,3100));restored();
    reset();expect_write=true;scenario=2;assert(!raphael_rpmh_write_locked(0x40100,3100));
    assert(raphael_rpmh_faulted&&sleeps==2000);
    unsigned quarantine_writes=writes;
    assert(!raphael_rpmh_write_locked(0x40100,3100)&&writes==quarantine_writes);
    reset();scenario=1;v=99;assert(!raphael_rpmh_read_locked(0x40100,&v)&&v==99);restored();
    reset();scenario=2;v=99;assert(!raphael_rpmh_read_locked(0x40100,&v)&&v==99);
    assert(raphael_rpmh_faulted&&sleeps==2000);
    assert(rd(RSC_TCS(1)+TCS_COMMAND(0))==0x108);
    unsigned before=writes;assert(!raphael_rpmh_read_locked(0x40100,&v)&&writes==before);
    reset();regs[(RSC_TCS(1)+TCS_STATUS)/4]=0;assert(!raphael_rpmh_read_locked(0x40100,&v)&&writes==0);
    reset();regs[(RSC_TCS(1)+TCS_ENABLED)/4]=1;assert(!raphael_rpmh_read_locked(0x40100,&v)&&writes==0);
    reset();regs[(RSC_TCS(0)+4)/4]=2;assert(!raphael_rpmh_read_locked(0x40100,&v)&&writes==0);
    reset();regs[(RSC_TCS(1)+TCS_CONTROL)/4]=1u<<24;assert(!raphael_rpmh_read_locked(0x40100,&v)&&writes==0);
    reset();assert(!raphael_rpmh_read_locked(0x40101,&v)&&writes==0);
    assert(!raphael_rpmh_read_locked(0x40100,NULL)&&writes==0);
    puts("RPMh: VRM masks/RESP_REQ, READ/WRITE flags, validation, restore, busy rejection and timeout quarantine PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='rpmh-read-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
