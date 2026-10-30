#!/usr/bin/env python3
"""Compile the actual PHY helper against simulated MMIO with fault injection."""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-phy.h').read_text()
parser = argparse.ArgumentParser()
parser.add_argument('--reference', type=Path, help='Pinned downstream v1.1 hardware table')
args = parser.parse_args()
expected = []
if args.reference:
    raw = args.reference.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == 'dabf3a5b9fe7f507f0bb4dbfb2ddb04a0463f4d31c58db19ae27dfe63427e234'
    ref = raw.decode()
    # Derive the expected write sequence independently of our C constants.
    common = ref.split('csiphy_common_reg_1_1[] = {', 1)[1].split('};', 1)[0]
    lanes = ref.split('csiphy_2ph_v1_1_reg[MAX_LANES][MAX_SETTINGS_PER_LANE] = {', 1)[1].split('};', 1)[0]
    pattern = r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(CSIPHY_\w+)\}'
    expected = [(0x82c + 4*i, 0) for i in range(11)]  # polling: no IRQ delivery
    for table in (common, lanes):
        for offset, value, delay, kind in re.findall(pattern, table):
            if kind in ('CSIPHY_DNP_PARAMS', 'CSIPHY_3PH_REGS'):
                continue
            assert kind in ('CSIPHY_LANE_ENABLE', 'CSIPHY_DEFAULT_PARAMS', 'CSIPHY_SETTLE_CNT_LOWER_BYTE')
            expected.append((int(offset, 16), int(value, 16)))
    assert len(expected) == 78
expected_c = '\n'.join('{%s,%s},' % (hex(o), hex(v)) for o, v in expected) or '{0,0},'
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define say(...) ((void)0)
typedef uintptr_t vm_offset_t;
static uint32_t mem[1024], original[1024];
static unsigned writes, fail_at, delays;
static uint32_t trace[512][2];
static bool map_fail;
static bool irq_stuck;
static uintptr_t raphael_camera_map(uintptr_t p, unsigned size) {
    assert(p==0xac65000 && size==0x1000);return map_fail?0:p;
}
static unsigned idx(uintptr_t p) {
    assert(p>=0xac65000 && p<0xac66000 && !(p&3));
    return (p-0xac65000)/4;
}
static uint32_t rd(uintptr_t p) {
    unsigned o=idx(p)*4;
    /* A powered-down PHY may expose all-ones status, as on the handset. */
    if(o>=0x8b0&&o<=0x8d8&&!mem[0x818/4])return 0xff;
    return mem[o/4];
}
static void wr(uintptr_t p, uint32_t v) {
    unsigned o=idx(p)*4;
    /* Status is read-only; only documented mask/clear/command registers. */
    bool allowed=o==0x800||o==0x814||o==0x818||o==0x81c||o==0x828||
        (o>=0x82c&&o<=0x880);
    static const unsigned lanes[]={0,0x700,0x200,0x400,0x600};
    static const unsigned regs[]={4,0x2c,0x34,0x1c,0x14,0x3c,0,8,0x10,0x38,0x60,0x64};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<12;j++)
        if(o==lanes[i]+regs[j])allowed=true;
    if(o==0x728||o==0x70c)allowed=true;
    assert(allowed);
    if(o==0x828)assert(mem[0x818/4]==1); /* no drain while common is off */
    assert(writes<512);trace[writes][0]=o;trace[writes][1]=v;
    if(++writes==fail_at)return;
    if(o==0x828&&v==1&&!irq_stuck)
        for(unsigned i=0;i<11;i++)mem[(0x8b0+4*i)/4]&=~mem[(0x858+4*i)/4];
    mem[idx(p)]=v;
}
static void spin_us(unsigned us) {assert(us==10||us==1000||us==5000);delays+=us;assert(delays<100000);}
''' + source + '\nstatic const unsigned expected[][2] = {' + expected_c + '};\n' + (
    '#define HAVE_REFERENCE 1\n' if expected else '#define HAVE_REFERENCE 0\n') + r'''
static void reset(void) {
    for(unsigned i=0;i<1024;i++)mem[i]=0x40+(i%16);
    mem[0x800/4]=mem[0x814/4]=mem[0x818/4]=mem[0x81c/4]=0;
    for(unsigned i=0;i<11;i++) {
        mem[(0x8b0+4*i)/4]=0;
        mem[(0x82c+4*i)/4]=0;
    }
    memcpy(original,mem,sizeof(mem));
    writes=fail_at=delays=0;map_fail=irq_stuck=false;
}
static void restored(struct raphael_phy *p) {
    assert(!p->dirty&&!p->ready);
    for(unsigned i=0;i<11;i++)mem[(0x858+4*i)/4]=original[(0x858+4*i)/4];
    mem[0x828/4]=original[0x828/4]; /* strobe/W1C state is not restored */
    assert(!memcmp(mem,original,sizeof(mem)));
}
int main(void) {
    reset();struct raphael_phy p={0};
    assert(raphael_phy_begin(&p));unsigned n=writes;
    if(HAVE_REFERENCE) {
        assert(n==sizeof(expected)/sizeof(expected[0]));
        assert(!memcmp(trace,expected,sizeof(expected)));
    }
    assert(p.ready&&p.dirty&&p.count==77&&delays==1000);
    assert(mem[0x814/4]==0xd5&&mem[0x81c/4]==2);
    assert(mem[0x700/4]==0x80&&mem[0x738/4]==0x1f&&mem[0x70c/4]==0xa5);
    assert(mem[8/4]==14&&mem[0x708/4]==14&&mem[0x608/4]==14);
    unsigned before=writes;assert(!raphael_phy_begin(&p));assert(writes==before);
    assert(raphael_phy_end(&p));restored(&p);unsigned total=writes;
    assert(raphael_phy_end(&p)&&writes==total);
    for(unsigned f=1;f<=n;f++) {
        reset();memset(&p,0,sizeof(p));fail_at=f;
        (void)raphael_phy_begin(&p);
        fail_at=0;assert(raphael_phy_end(&p));restored(&p);
    }
    /* A failed stop or restore retains dirty; retry restores everything. */
    for(unsigned f=1;f<=total-n;f++) {
        reset();memset(&p,0,sizeof(p));assert(raphael_phy_begin(&p));
        fail_at=writes+f;bool ok=raphael_phy_end(&p);
        if(!ok)assert(p.dirty&&!p.ready);
        fail_at=0;assert(raphael_phy_end(&p));restored(&p);
    }
    for(unsigned busy=0;busy<4;busy++) {
        reset();memset(&p,0,sizeof(p));
        unsigned o=busy<4?(unsigned[]){0x800,0x814,0x818,0x81c}[busy]:0x8b0+4*(busy-4);
        mem[o/4]=1;assert(!raphael_phy_begin(&p)&&!writes&&!p.dirty);
    }
    reset();memset(&p,0,sizeof(p));map_fail=true;
    assert(!raphael_phy_begin(&p)&&!writes);
    assert(!raphael_phy_begin(NULL)&&!raphael_phy_end(NULL));
    for(unsigned i=0;i<11;i++) {
        reset();memset(&p,0,sizeof(p));mem[(0x82c+4*i)/4]=1;
        assert(!raphael_phy_begin(&p)&&!writes&&!p.dirty);
    }
    reset();memset(&p,0,sizeof(p));
    for(unsigned i=0;i<11;i++)mem[(0x8b0+4*i)/4]=0xff;
    assert(raphael_phy_begin(&p));
    for(unsigned i=0;i<11;i++)assert(!mem[(0x8b0+4*i)/4]);
    assert(raphael_phy_end(&p));restored(&p);
    reset();memset(&p,0,sizeof(p));mem[0x8b0/4]=0xff;irq_stuck=true;
    assert(!raphael_phy_begin(&p)&&p.dirty);
    /* Stopped lanes/common establish cleanup even if powered-down IRQ status
     * remains all ones. Do not retain rails solely for inactive status. */
    assert(raphael_phy_end(&p)&&!p.dirty);
    mem[0x8b0/4]=0;restored(&p);
    /* Each IRQ-ack write may fail, including command assertion/deassertion.
     * A retry must clear remaining sampled status and restore configuration. */
    for(unsigned f=79;f<=102;f++){
        reset();memset(&p,0,sizeof(p));mem[0x8b0/4]=0xff;fail_at=f;
        (void)raphael_phy_begin(&p);
        fail_at=0;assert(raphael_phy_end(&p));
        mem[0x8b0/4]=0;restored(&p);
    }
    puts("Raphael PHY v1.1 actual helper / simulated MMIO: lifecycle and failure recovery passed");
}
'''
with tempfile.TemporaryDirectory() as td:
    src = Path(td) / 'phy.c'
    exe = Path(td) / 'phy'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
