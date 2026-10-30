#!/usr/bin/env python3
"""Actual capture-clock helper, bounded simulated MMIO and lifecycle faults."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
common = (root/'xnu/files/bsd/dev/hakios.raphael-clocks.h').read_text()
common = common[common.index('static bool\nraphael_clock_wait'):common.index('static bool\nraphael_camera_clocks_begin')]
source = (root/'xnu/files/bsd/dev/hakios.raphael-capture-clocks.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define say(...) ((void)0)
static uint32_t mem[0x4000], original[0x4000];
static unsigned writes,fail_at,held,polls;
static bool votes, map_fail;
static bool delayed;
static unsigned delayed_count;
static uint32_t delayed_value;
static uintptr_t raphael_camera_map(uintptr_t p,unsigned z){assert(p==0xad00000&&z==0x10000);return map_fail?0:p;}
static unsigned idx(uintptr_t p){assert(p>=0xad00000&&p<0xad10000&&!(p&3));return (p-0xad00000)/4;}
static uint32_t rd(uintptr_t p){return mem[idx(p)];}
static void wr(uintptr_t p,uint32_t v);
static void spin_us(unsigned us){
    assert(us==10);assert(++polls<100000);
    if(delayed_count && !--delayed_count)mem[0xc168/4]=delayed_value;
}
bool hakios_raphael_camera_capture_ready(void){return votes;}
''' + common + source + r'''
static void wr(uintptr_t p,uint32_t v) {
    unsigned o=idx(p)*4;bool allowed=o==0||o==0x38;
    for(unsigned i=0;i<12;i++)if(o==raphael_capture_pll_regs[i])allowed=true;
    for(unsigned i=0;i<6;i++)if(o==raphael_capture_roots[i].cmd||o==raphael_capture_roots[i].cmd+4u)allowed=true;
    for(unsigned i=0;i<RAPHAEL_CAPTURE_BRANCHES;i++)if(o==raphael_capture_branches[i])allowed=true;
    assert(o!=0xc1c8);
    assert(allowed);if(++writes==fail_at)return;
    if(o==0xc168&&delayed){delayed_value=(v&~0x80000000u)|((v&1)?0:0x80000000u);delayed_count=2;return;}
    /* MODE contains hardware-owned lock state. */
    if(!o)v=(v&~0xe0000000u)|(mem[0]&0xe0000000u);
    mem[o/4]=v;
    if(o==0x38) {
        if(held!=1) {
            if(v){mem[0]|=0x80000000u;if(held!=4)mem[0x30/4]|=0x04000000u;}
            else mem[0]&=~0x80000000u;
        }
    }
    for(unsigned i=0;i<6;i++)if(o==raphael_capture_roots[i].cmd&&held!=2)mem[o/4]&=~1u;
    for(unsigned i=0;i<RAPHAEL_CAPTURE_BRANCHES;i++)if(o==raphael_capture_branches[i]&&held!=3)
        mem[o/4]=(v&~0x80000000u)|((v&1)?0:0x80000000u);
}
static void reset(void) {
    memset(mem,0,sizeof(mem));mem[0xc1bc/4]=0x80000000u;mem[0]=0x800004;
    for(unsigned i=0;i<12;i++)mem[raphael_capture_pll_regs[i]/4]=0x200+i*8;
    mem[0x0c/4]|=7u; /* inactive output selection is not a running PLL */
    for(unsigned i=0;i<6;i++)mem[(raphael_capture_roots[i].cmd+4)/4]=0x40000|i;
    for(unsigned i=0;i<RAPHAEL_CAPTURE_BRANCHES;i++)mem[raphael_capture_branches[i]/4]=0x80000000u;
    memcpy(original,mem,sizeof(mem));writes=fail_at=held=polls=0;votes=true;map_fail=delayed=false;delayed_count=0;
}
static void restored(struct raphael_capture_clocks *c) {
    assert(!c->ready&&!c->pll_dirty);
    for(unsigned i=0;i<6;i++)assert(!c->cfg_dirty[i]);
    for(unsigned i=0;i<RAPHAEL_CAPTURE_BRANCHES;i++)assert(!c->branch_dirty[i]);
    /* Calibration done is hardware-owned, not a writable snapshot. */
    mem[0x30/4]=original[0x30/4];
    assert(!memcmp(mem,original,sizeof(mem)));
}
int main(void) {
    reset();struct raphael_capture_clocks c={0};
    assert(raphael_capture_clocks_begin(&c));unsigned n=writes;
    assert(c.ready&&c.pll_dirty&&mem[4/4]==0x3e&&mem[0x28/4]==0);
    unsigned before=writes;assert(!raphael_capture_clocks_begin(&c)&&writes==before);
    assert(raphael_capture_clocks_end(&c));restored(&c);unsigned total=writes;
    for(unsigned f=1;f<=n;f++) {
        reset();memset(&c,0,sizeof(c));fail_at=f;
        (void)raphael_capture_clocks_begin(&c);
        fail_at=0;assert(raphael_capture_clocks_end(&c));restored(&c);
    }
    for(unsigned f=1;f<=total-n;f++) {
        reset();memset(&c,0,sizeof(c));assert(raphael_capture_clocks_begin(&c));
        fail_at=writes+f;bool ok=raphael_capture_clocks_end(&c);
        if(!ok)assert(c.pll_dirty);
        fail_at=0;assert(raphael_capture_clocks_end(&c));restored(&c);
    }
    for(unsigned h=1;h<=4;h++){
        reset();memset(&c,0,sizeof(c));held=h;
        assert(!raphael_capture_clocks_begin(&c));held=0;
        for(unsigned i=0;i<6;i++)mem[raphael_capture_roots[i].cmd/4]&=~1u;
        assert(raphael_capture_clocks_end(&c));restored(&c);
    }
    reset();memset(&c,0,sizeof(c));assert(raphael_capture_clocks_begin(&c));
    held=3;assert(!raphael_capture_clocks_end(&c)&&c.pll_dirty&&(mem[0]&1));
    held=0;assert(raphael_capture_clocks_end(&c));restored(&c);
    reset();memset(&c,0,sizeof(c));mem[0]=original[0]=0;
    assert(raphael_capture_clocks_begin(&c));
    assert(raphael_capture_clocks_end(&c));restored(&c);
    reset();memset(&c,0,sizeof(c));delayed=true;
    assert(raphael_capture_clocks_begin(&c));
    assert(raphael_capture_clocks_end(&c));restored(&c);
    for(unsigned busy=0;busy<6;busy++){
        reset();memset(&c,0,sizeof(c));
        if(busy==0)votes=false;
        if(busy==1)mem[0]|=0x100000;
        if(busy==2)mem[0x38/4]=1;
        if(busy==3)mem[0]|=1;
        if(busy==4)mem[raphael_capture_roots[5].cmd/4]=1;
        if(busy==5)mem[raphael_capture_branches[RAPHAEL_CAPTURE_BRANCHES-1]/4]|=1;
        assert(!raphael_capture_clocks_begin(&c)&&!writes&&!c.pll_dirty);
    }
    reset();memset(&c,0,sizeof(c));map_fail=true;
    assert(!raphael_capture_clocks_begin(&c)&&!writes);
    puts("Capture clocks actual C / simulated MMIO: failures, timeouts, ownership, restore and PLL retention PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src, exe = Path(td)/'capture.c', Path(td)/'capture'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
