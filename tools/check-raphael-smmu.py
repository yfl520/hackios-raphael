#!/usr/bin/env python3
"""Actual camera SMMU lifecycle with simulated registers and partial failures."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnu/files/bsd/dev/hakios.raphael-smmu.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
#define say(...) ((void)0)
static uint32_t mem[0x100000/4],original[0x100000/4];
static unsigned writes,fail_at,polls;
static bool held,map_fail,format_bits;
static bool fault_quirk;
static bool cbar_sticky;
static unsigned idx(uintptr_t p){
    assert(p>=0x15000000&&p<0x15100000&&!(p&3));return(p-0x15000000)/4;
}
static uintptr_t raphael_camera_map(uintptr_t p,unsigned size){assert(size==4096);idx(p);return map_fail?0:p;}
static uint32_t rd(uintptr_t p){
    unsigned o=idx(p)*4;
    if(format_bits&&o>=0x80000&&o%4096==0x10){
        unsigned cb=(o-0x80000)/4096;
        if((mem[(0x1000+4*cb)/4]&0x30000)==0x10000)return mem[o/4]|0x60;
    }
    return mem[o/4];
}
static void wr(uintptr_t p,uint32_t v);
static void spin_us(unsigned us){assert(us==10);assert(++polls<10000);}
''' + source + r'''
static void wr(uintptr_t p,uint32_t v){
    unsigned o=idx(p)*4;bool allowed=false;
    if((o>=0x800&&o<0x840)||(o>=0xc00&&o<0xc40))allowed=true;
    if((o>=0x1000&&o<0x1020)||(o>=0x1800&&o<0x1820))allowed=true;
    if(o>=0x80000){
        unsigned reg=o%4096;
        for(unsigned i=0;i<9;i++)if(reg==raphael_smmu_cb_regs[i])allowed=true;
        if(reg==0x610||reg==0x7f0)allowed=true;
    }
    assert(allowed&&o!=0); /* no global SCR0/fault/status writes */
    if(++writes==fail_at)return;
    if(fault_quirk&&o>=0xc00&&o<0xc40){
        if((v&0x30000)==0x20000)return;
        if((v&0x30000)==0x10000)v=(v&~0x30000)|0x20000;
    }
    if(cbar_sticky&&o>=0x1000&&o<0x1020&&!v)return;
    if(format_bits&&o>=0x80000&&o%4096==0x10)v&=~0x60u;
    mem[o/4]=v;
    if(o>=0x80000&&o%4096==0x7f0)mem[(o+4)/4]=held?1:0;
}
static void reset(void){
    memset(mem,0,sizeof(mem));
    mem[0]=0xdeadbeef;mem[0x20/4]=16;mem[0x24/4]=(6u<<28)|8;
    mem[0x800/4]=0x80001000;mem[0xc00/4]=0; /* unrelated live stream/context */
    mem[0x804/4]=0x1234;mem[0xc04/4]=0x200ff; /* free SMR, reset CB sentinel */
    for(unsigned cb=0;cb<8;cb++){
        unsigned at=0x80000+cb*4096;
        for(unsigned i=0;i<9;i++)mem[(at+raphael_smmu_cb_regs[i])/4]=0x2000+i*8;
        mem[(0x1000+4*cb)/4]=0x111000+cb;
        mem[(0x1800+4*cb)/4]=0x200+cb;
    }
    mem[0x80000/4]|=1;mem[0x81000/4]|=1; /* cb0 used, cb1 enabled without SMR */
    mem[(0x82000+0x58)/4]=2; /* cb2 has uncleared translation fault */
    mem[(0x83000+0x7f4)/4]=1; /* cb3 has pending TLB sync */
    memcpy(original,mem,sizeof(mem));
    writes=fail_at=polls=0;held=map_fail=format_bits=fault_quirk=cbar_sticky=false;
}
static void restored(struct raphael_smmu *s){
    assert(!s->ready&&!s->cb_dirty&&!s->smr_dirty&&!s->s2cr_dirty&&!s->cbar_dirty&&!s->cba2r_dirty);
    /* ASID invalidation and sync command state are not configuration snapshots. */
    for(unsigned cb=0;cb<8;cb++){
        unsigned at=0x80000+cb*4096;
        mem[(at+0x610)/4]=original[(at+0x610)/4];
        mem[(at+0x7f0)/4]=original[(at+0x7f0)/4];
    }
    assert(!memcmp(mem,original,sizeof(mem)));
}
int main(void){
    reset();struct raphael_smmu s={0};
    assert(raphael_smmu_begin(&s,0xabc000));unsigned n=writes;
    assert(s.context==4&&s.match==1&&s.ready&&raphael_smmu_owned(&s));
    /* A Wi-Fi-style allocator sees our valid route and excludes our CB. */
    bool used[256]={0};
    for(unsigned i=0;i<16;i++)if(mem[(0x800+4*i)/4]>>31)used[mem[(0xc00+4*i)/4]&255]=true;
    assert(used[s.context]);
    /* An unrelated live mapping neither changes our lease nor gets restored. */
    mem[0x808/4]=0x80000c00;mem[0xc08/4]=7;
    assert(raphael_smmu_owned(&s));
    mem[0x808/4]=original[0x808/4];mem[0xc08/4]=original[0xc08/4];
    const unsigned ownership_regs[]={0x804,0xc04,0x1010,0x1810,0x84000,0x84020,0x84024};
    for(unsigned i=0;i<7;i++){
        unsigned at=ownership_regs[i]/4;uint32_t saved=mem[at];unsigned before=writes;
        mem[at]^=1;assert(!raphael_smmu_owned(&s)&&writes==before);mem[at]=saved;
    }
    assert(mem[0x804/4]==0x86200840u&&mem[0xc04/4]==4);
    assert(mem[(0x84000+0x20)/4]==0xabc000&&mem[(0x84000+0x24)/4]==(4u<<16));
    assert(!raphael_smmu_begin(&s,0xdef000)&&writes==n);
    assert(raphael_smmu_end(&s));restored(&s);unsigned total=writes;
    reset();memset(&s,0,sizeof(s));format_bits=true;
    mem[(0x1000+4*4)/4]=original[(0x1000+4*4)/4]=0;
    assert(raphael_smmu_begin(&s,0xabc000));
    assert(raphael_smmu_end(&s));restored(&s);
    reset();memset(&s,0,sizeof(s));fault_quirk=true;
    assert(raphael_smmu_begin(&s,0xabc000));
    assert(raphael_smmu_end(&s));restored(&s);
    reset();memset(&s,0,sizeof(s));fault_quirk=cbar_sticky=true;
    mem[(0x1000+4*4)/4]=0;mem[(0x1800+4*4)/4]=0;
    assert(raphael_smmu_begin(&s,0xabc000));unsigned begun=writes;
    assert(raphael_smmu_end(&s)&&s.retired&&!s.cb_dirty);
    assert(!mem[0x84000/4]&&mem[0x804/4]==0x1234&&mem[0xc04/4]==0x200ff);
    assert(!mem[(0x84000+0x20)/4]&&!mem[(0x84000+0x24)/4]);
    assert(!mem[(0x84000+0x28)/4]&&!mem[(0x84000+0x2c)/4]);
    unsigned retirement_writes=writes-begun;
    for(unsigned f=1;f<=retirement_writes;f++){
        reset();memset(&s,0,sizeof(s));fault_quirk=cbar_sticky=true;
        mem[(0x1000+4*4)/4]=0;mem[(0x1800+4*4)/4]=0;
        assert(raphael_smmu_begin(&s,0xabc000));fail_at=writes+f;
        (void)raphael_smmu_end(&s);fail_at=0;
        assert(raphael_smmu_end(&s)&&!s.cb_dirty);
        assert(!mem[0x84000/4]&&mem[0xc04/4]==0x200ff);
        assert(!mem[(0x84000+0x20)/4]&&!mem[(0x84000+0x24)/4]);
    }
    for(unsigned f=1;f<=n;f++){
        reset();memset(&s,0,sizeof(s));fail_at=f;(void)raphael_smmu_begin(&s,0xabc000);
        fail_at=0;assert(raphael_smmu_end(&s));restored(&s);
    }
    for(unsigned f=1;f<=total-n;f++){
        reset();memset(&s,0,sizeof(s));assert(raphael_smmu_begin(&s,0xabc000));
        fail_at=writes+f;(void)raphael_smmu_end(&s);
        fail_at=0;assert(raphael_smmu_end(&s));restored(&s);
    }
    reset();memset(&s,0,sizeof(s));held=true;
    assert(!raphael_smmu_begin(&s,0xabc000)&&s.cb_dirty);
    held=false;polls=0;assert(raphael_smmu_end(&s));restored(&s);
    reset();memset(&s,0,sizeof(s));assert(raphael_smmu_begin(&s,0xabc000));
    held=true;assert(!raphael_smmu_end(&s)&&s.cb_dirty);
    held=false;polls=0;assert(raphael_smmu_end(&s));restored(&s);
    const unsigned aliases[]={0x840,0x860,0xa40,0xa60,0xc40,0xc60,0xe40,0xe60};
    for(unsigned i=0;i<8;i++){
        reset();memset(&s,0,sizeof(s));mem[0x800/4]=0x80000000u|aliases[i];
        assert(!raphael_smmu_begin(&s,0xabc000)&&!writes);
    }
    reset();memset(&s,0,sizeof(s));
    for(unsigned i=0;i<16;i++)mem[(0x800+4*i)/4]=0x80001000;
    assert(!raphael_smmu_begin(&s,0xabc000)&&!writes);
    reset();memset(&s,0,sizeof(s));
    for(unsigned i=0;i<8;i++)mem[(0x80000+4096*i)/4]|=1;
    assert(!raphael_smmu_begin(&s,0xabc000)&&!writes);
    reset();memset(&s,0,sizeof(s));map_fail=true;
    assert(!raphael_smmu_begin(&s,0xabc000)&&!writes);
    map_fail=false;
    assert(!raphael_smmu_begin(&s,0)&&!raphael_smmu_begin(&s,1));
    assert(!raphael_smmu_begin(&s,1ull<<36)&&!writes);
    puts("Camera SMMU actual C / simulated MMIO: aliases, ownership, partial faults, exact restore, mediated FAULT quirk, pointer-free retirement and TLB retry PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'smmu.c',Path(td)/'smmu'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
