#!/usr/bin/env python3
"""Run real RX-only helper against bounded MMIO and sensor stream mocks."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'xnu/files/bsd/dev/hakios.raphael-csid.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define say(...) ((void)0)
static uint32_t mem[1024],saved[1024];
static unsigned writes, fail_at, fault;
static bool streaming,map_fail;
struct raphael_sensor {bool stream_dirty;};
struct raphael_i2c_bus {int unused;};
static uintptr_t raphael_camera_map(uintptr_t p,unsigned z){assert(p==0xacc8000&&z==0x1000);return map_fail?0:p;}
static unsigned idx(uintptr_t p){assert(p>=0xacc8000&&p<0xacc9000&&!(p&3));return(p-0xacc8000)/4;}
static uint32_t rd(uintptr_t p){return mem[idx(p)];}
static void wr(uintptr_t p,uint32_t v){
    unsigned o=idx(p)*4;
    assert(o==0x100||o==0x104||o==0x108); /* no IRQ/RDI/VFE/DMA writes */
    if(++writes!=fail_at)mem[o/4]=v;
}
static void spin_us(unsigned us){
    assert(us==20000||us==40000||us==100000);
    if((streaming&&fault!=1)||fault==2){
        mem[0x160/4]+=123;
        mem[0x130/4]=(0x2bu<<16)|5000;
        mem[0x138/4]=0x12341234;
        if(fault==3)mem[0x130/4]|=1u<<22;
        if(fault==4)mem[0x130/4]^=1u<<16;
        if(fault==5)mem[0x130/4]++;
        if(fault==6)mem[0x164/4]++;
        if(fault==7)mem[0x168/4]++;
        if(fault==8)mem[0x138/4]++;
    }
}
static bool raphael_sensor_start(struct raphael_sensor *s,struct raphael_i2c_bus *b){
    (void)b;assert(!streaming);s->stream_dirty=true;
    if(fault==9)return false;streaming=true;return true;
}
static bool raphael_sensor_stop(struct raphael_sensor *s,struct raphael_i2c_bus *b){
    (void)b;if(fault==10)return false;streaming=false;s->stream_dirty=false;return true;
}
''' + source + r'''
static void reset(void){
    memset(mem,0,sizeof(mem));mem[0]=0x10070000;mem[0x104/4]=0x80;
    memcpy(saved,mem,sizeof(mem));writes=fail_at=fault=0;streaming=map_fail=false;
}
static void restored(struct raphael_csid_rx *c){
    assert(!c->dirty&&!c->ready);
    for(unsigned o=0x100;o<=0x108;o+=4)assert(mem[o/4]==saved[o/4]);
}
int main(void){
    reset();struct raphael_csid_rx c={0};
    assert(raphael_csid_begin(&c));unsigned n=writes;
    assert(mem[0x100/4]==0x32103&&mem[0x104/4]==0x41&&mem[0x108/4]==0x2b1);
    assert(!raphael_csid_begin(&c)&&writes==n);
    assert(raphael_csid_end(&c));restored(&c);unsigned total=writes;
    reset();memset(&c,0,sizeof(c));
    mem[0x100/4]=0x32103;mem[0x104/4]=1;mem[0x600/4]=0xa06406;
    memcpy(saved,mem,sizeof(mem));
    assert(raphael_csid_begin(&c));assert(raphael_csid_end(&c));restored(&c);
    for(unsigned f=1;f<=n;f++){
        reset();memset(&c,0,sizeof(c));fail_at=f;(void)raphael_csid_begin(&c);
        fail_at=0;assert(raphael_csid_end(&c));restored(&c);
    }
    for(unsigned f=1;f<=total-n;f++){
        reset();memset(&c,0,sizeof(c));assert(raphael_csid_begin(&c));
        fail_at=writes+f;bool ok=raphael_csid_end(&c);if(!ok)assert(c.dirty);
        fail_at=0;assert(raphael_csid_end(&c));restored(&c);
    }
    for(unsigned f=0;f<=10;f++){
        reset();memset(&c,0,sizeof(c));struct raphael_sensor s={0};struct raphael_i2c_bus b={0};
        assert(raphael_csid_begin(&c));fault=f;
        assert(raphael_csid_stream_probe(&c,&s,&b)==(f==0));
        if(f==10)assert(s.stream_dirty&&streaming);
        fault=0;assert(raphael_sensor_stop(&s,&b));
        assert(raphael_csid_end(&c));restored(&c);
    }
    for(unsigned i=0;i<14;i++){
        reset();memset(&c,0,sizeof(c));
        if(i==0)mem[0]=0;
        else if(i==1)mem[0]=0xffffffff;
        else if(i==2)mem[0x104/4]|=0x40;
        else if(i==3)mem[0x600/4]|=1;
        else if(i==4)mem[0x108/4]=1;
        else if(i==5)mem[0x24/4]=1;
        else if(i==6)mem[0x74/4]=1;
        else if(i==7)mem[0x600/4]=1;
        else if(i<12)mem[(0x200+0x100*(i-8))/4]=0x80000000u;
        else if(i==12)mem[0x34/4]=1;
        else map_fail=true;
        assert(!raphael_csid_begin(&c)&&!writes&&!c.dirty);
    }
    puts("CSID actual RX-only C / simulated packets: lifecycle, failures, ownership and evidence rejection PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'rx.c',Path(td)/'rx'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
