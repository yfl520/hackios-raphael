#!/usr/bin/env python3
"""Exercise the actual rear supply sequence and restore on every failure."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-probe.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define say printf
static const uintptr_t addresses[]={0x0351c000,0x0395a000,0x03961000,0x0ad00000,0x00100000};
static uint32_t reg[5][0x10000/4];
static unsigned writes, failure, action, requested, restored, began, ended, inspections;
static unsigned sequence[12], ns;
static bool snapshot_fail;
static uintptr_t raphael_camera_map(uintptr_t p,unsigned size) {
    for(unsigned i=0;i<5;i++) {
        if(p==addresses[i]){assert(size==(i<3?0x1000u:0x10000u));return p;}
    }
    assert(0);return 0;
}
static uint32_t rd(uintptr_t p) {
    for(unsigned i=0;i<5;i++)if(p>=addresses[i]&&p<addresses[i]+(i<3?0x1000u:0x10000u))return reg[i][(p-addresses[i])/4];
    assert(0);return 0;
}
static void wr(uintptr_t p,uint32_t value) {
    for(unsigned i=0;i<3;i++)if(p==addresses[i]||p==addresses[i]+4) {
        /* Reset is never released in the supply-only stage. */
        if(i==0&&p==addresses[i]+4)assert(!(value&2));
        reg[i][(p-addresses[i])/4]=value;writes++;return;
    }
    assert(0); /* No clocks, PHY or unrelated GPIO writes. */
}
static void spin_us(unsigned us) {assert(us<=20000);}
bool hakios_raphael_camera_power_begin(void){began++;return !snapshot_fail;}
bool hakios_raphael_camera_power_step(unsigned r,bool restore) {
    assert(r<4&&ns<12);sequence[ns++]=r+(restore?10:0);
    if(restore){restored|=1u<<r;return true;}
    requested|=1u<<r;return ++action!=failure;
}
bool hakios_raphael_camera_power_end(void){ended++;return restored==15;}
void hakios_raphael_camera_clock_inspect(void){inspections++;}
''' + source + r'''
static void reset(void) {
    memset(reg,0,sizeof(reg));for(unsigned i=0;i<3;i++)reg[i][0]=1;
    reg[3][0x501c/4]=0x80000000;
    writes=failure=action=requested=restored=began=ended=inspections=ns=0;snapshot_fail=false;
}
int main(void) {
    for(unsigned f=0;f<=4;f++) {
        reset();failure=f;raphael_camera_supply_probe();
        assert(began==1&&ended==1&&restored==15&&inspections==1);
        for(unsigned i=0;i<3;i++)assert(reg[i][0]==1&&!(reg[i][1]&2));
        unsigned n=f?f:4;
        assert(ns==n+4);for(unsigned i=0;i<n;i++)assert(sequence[i]==i);
        for(unsigned i=0;i<4;i++)assert(sequence[n+i]==13-i);
    }
    reset();snapshot_fail=true;raphael_camera_supply_probe();assert(!writes&&!ended);
    reset();reg[0][0]|=0x200;raphael_camera_supply_probe();assert(!writes&&!began);
    reset();reg[1][1]=2;raphael_camera_supply_probe();assert(!writes&&!began);
    reset();reg[3][0x501c/4]|=1;raphael_camera_supply_probe();assert(!writes&&!began);
    puts("Probe: supply order, each rail failure, reset held, GPIO restore and busy rejection PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-probe-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
