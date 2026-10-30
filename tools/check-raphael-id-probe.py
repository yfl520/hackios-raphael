#!/usr/bin/env python3
"""Run real rear-ID orchestration with failed supplies, clocks and I2C."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
probe = (root / 'xnu/files/bsd/dev/hakios.raphael-probe.h').read_text()
pins = probe[:probe.index('static void\nraphael_camera_supply_probe')]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-id-probe.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define say printf
static const uintptr_t addresses[]={0x0351c000,0x0395a000,0x03961000,0x03911000,0x03912000};
static uint32_t regs[5][2];
static unsigned rails,restores,step,fail_step,reads,read_fail,writes,clock_fail;
static bool powered,clocked,clock_attempted,bad_id,end_fail;
static uintptr_t raphael_camera_map(uintptr_t a,unsigned n){assert(n==0x1000);return a;}
static uint32_t *ptr(uintptr_t p) {
    for(unsigned i=0;i<5;i++)if(p==addresses[i]||p==addresses[i]+4)return &regs[i][(p-addresses[i])/4];
    assert(0);return 0;
}
static uint32_t rd(uintptr_t p){return *ptr(p);}
static void wr(uintptr_t p,uint32_t v) {
    if(p==addresses[0]+4&&(v&2))assert(clocked&&powered&&rails==15);
    *ptr(p)=v;writes++;
}
static void spin_us(unsigned us){assert(us<=20000);}
bool hakios_raphael_camera_power_begin(void){assert(!powered&&!rails);step=restores=0;powered=true;return true;}
bool hakios_raphael_camera_power_step(unsigned r,bool restore) {
    assert(powered&&r<4);
    if(restore) {
        assert(!(regs[0][1]&2));assert(!(regs[1][1]&2));assert(!(regs[2][1]&2));
        assert(r==3-restores);restores++;rails&=~(1u<<r);return true;
    }
    assert(r==step);step++;rails|=1u<<r;return step!=fail_step;
}
bool hakios_raphael_camera_power_end(void){assert(restores==4);powered=false;return !end_fail;}
void hakios_raphael_camera_clock_inspect(void){}
struct raphael_camera_clocks {bool started;};
static bool raphael_camera_clocks_begin(struct raphael_camera_clocks *c) {
    assert(powered&&rails==15&&!(regs[0][1]&2)&&(regs[1][1]&2)&&(regs[2][1]&2));
    clock_attempted=true;c->started=true;clocked=true;return clock_fail!=1;
}
static bool raphael_camera_clocks_end(struct raphael_camera_clocks *c) {
    assert(!(regs[0][1]&2));clocked=false;c->started=false;return clock_fail!=2;
}
struct raphael_i2c_bus {vm_offset_t sda,scl;};
enum raphael_i2c_result {RAPHAEL_I2C_OK,RAPHAEL_I2C_ARGUMENT,RAPHAEL_I2C_BUSY,RAPHAEL_I2C_STRETCH,RAPHAEL_I2C_NACK};
static enum raphael_i2c_result
raphael_i2c_transfer(struct raphael_i2c_bus *b,uint8_t a,const uint8_t *out,unsigned n,uint8_t *in,unsigned m) {
    assert(b->sda==addresses[3]&&b->scl==addresses[4]);assert(a==0x1a&&n==2&&m==2);
    assert(out[0]==0&&out[1]==0x16&&clocked&&(regs[0][1]&2)&&rails==15);
    reads++;if(read_fail==1)return RAPHAEL_I2C_NACK;
    if(read_fail==2)return RAPHAEL_I2C_STRETCH;
    if(read_fail==3&&reads==1)return RAPHAEL_I2C_NACK;
    in[0]=bad_id?0:5;in[1]=0x86;return RAPHAEL_I2C_OK;
}
''' + pins + source + r'''
static void reset(void) {
    memset(regs,0,sizeof(regs));for(unsigned i=0;i<5;i++)regs[i][0]=1;
    rails=restores=step=fail_step=reads=read_fail=writes=clock_fail=0;
    powered=clocked=clock_attempted=bad_id=end_fail=false;
}
static void restored(void) {
    assert(!powered&&!clocked&&!rails&&restores==4);
    for(unsigned i=0;i<5;i++)assert(regs[i][0]==1&&!(regs[i][1]&2));
}
static bool config_callback(struct raphael_i2c_bus *bus,void *context) {
    assert(bus->sda==addresses[3]&&clocked&&powered&&(regs[0][1]&2));
    assert(reads==1);return *(bool *)context;
}
int main(void) {
    reset();bool configured=true;
    assert(raphael_camera_rear_probe(config_callback,&configured,NULL));restored();
    reset();configured=false;
    assert(!raphael_camera_rear_probe(config_callback,&configured,NULL));restored();
    reset();configured=true;bool retain=true;
    assert(!raphael_camera_rear_probe(config_callback,&configured,&retain));
    assert(powered&&clocked&&rails==15&&!restores); /* no parent teardown */
    reset();assert(raphael_camera_id_probe());assert(reads==1);restored();
    assert(raphael_camera_id_probe());assert(reads==2);restored(); /* same boot, no fixture reset */
    for(unsigned f=1;f<=4;f++) {
        reset();fail_step=f;assert(!raphael_camera_id_probe());
        assert(!reads&&!clock_attempted);restored();
    }
    for(unsigned f=1;f<=2;f++) {
        reset();clock_fail=f;assert(!raphael_camera_id_probe());assert(reads==(f==2));restored();
    }
    reset();read_fail=1;assert(!raphael_camera_id_probe());assert(reads==20);restored();
    reset();read_fail=2;assert(!raphael_camera_id_probe());assert(reads==1);restored();
    reset();read_fail=3;assert(raphael_camera_id_probe());assert(reads==2);restored();
    reset();bad_id=true;assert(!raphael_camera_id_probe());restored();
    reset();end_fail=true;assert(!raphael_camera_id_probe());restored();
    for(unsigned p=0;p<5;p++) {
        reset();regs[p][0]|=4;assert(!raphael_camera_id_probe());assert(!writes&&!powered&&!reads);
    }
    puts("Rear ID: actual orchestration, exact address/ID, supply failures, retry bound, clock failure and shutdown order PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-id-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
