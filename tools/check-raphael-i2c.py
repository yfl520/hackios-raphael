#!/usr/bin/env python3
"""Check the actual rear I2C waveform, byte order and failure restoration."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-i2c.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
static uint32_t cfg[2],latch[2];
static unsigned snapshots[2],reads,writes,sleeps,scl_reads,sample_at,samples;
static unsigned expected[128],nack_at,stretch_at;
static bool fault,stuck_sda,stuck_scl,restoring;
static unsigned pin_of(uintptr_t p) {
    assert(p==0x3911000||p==0x3911004||p==0x3912000||p==0x3912004);
    return p>=0x3912000;
}
static uint32_t rd(uintptr_t p) {
    unsigned pin=pin_of(p);reads++;
    if(!(p&4)) return cfg[pin];
    if(!snapshots[pin]++) return 3;
    if(pin) {scl_reads++;return stuck_scl||scl_reads>=stretch_at?0:1;}
    if(stuck_sda) {fault=true;return 0;}
    if(scl_reads>=stretch_at) {fault=true;return 1;}
    if(fault) return 1;
    assert(sample_at<samples);
    unsigned want=expected[sample_at++];
    assert(((cfg[0]&0x200)==0)==((want&2)!=0));
    if(sample_at==nack_at) {fault=true;return 1;}
    return want&1;
}
static void wr(uintptr_t p,uint32_t value) {
    unsigned pin=pin_of(p);writes++;
    if(p&4) {
        assert(value==0||value==2);
        if(value==2)restoring=true;
        latch[pin]=value;
    } else {
        if(!restoring) {
            assert((value&~0x3ffu)==0x4000u);
            assert((value&0x1ff)==3); /* GPIO function, pullup, 2mA. */
            assert(latch[pin]==0); /* No push-pull high anywhere in a transaction. */
        }
        cfg[pin]=value;
    }
}
static void spin_us(unsigned us) {assert(us==5);sleeps++;assert(sleeps<1000);}
''' + source + r'''
static void add(bool release,bool value) {expected[samples++]=2*release+value;}
static void tx(unsigned byte) {for(int b=7;b>=0;b--)add((byte>>b)&1,(byte>>b)&1);add(true,false);}
static void rx(unsigned byte,bool more) {for(int b=7;b>=0;b--)add(true,(byte>>b)&1);add(!more,!more);}
static void reset(void) {
    for(unsigned p=0;p<2;p++){cfg[p]=0x4001;latch[p]=2;snapshots[p]=0;}
    reads=writes=sleeps=scl_reads=sample_at=samples=0;
    nack_at=stretch_at=~0u;fault=stuck_sda=stuck_scl=restoring=false;
    add(true,true);add(true,true);tx(0x34);tx(0);tx(0x16);
    add(true,true);tx(0x35);rx(0x05,true);rx(0x86,false);add(true,true);
}
static void restored(void) {
    assert(cfg[0]==0x4001&&cfg[1]==0x4001&&latch[0]==2&&latch[1]==2);
}
int main(void) {
    struct raphael_i2c_bus b={.sda=0x3911000,.scl=0x3912000};
    uint8_t out[]={0,0x16},in[2]={0};
    reset();assert(raphael_i2c_transfer(&b,0x1a,out,2,in,2)==RAPHAEL_I2C_OK);
    assert(in[0]==5&&in[1]==0x86&&sample_at==samples);restored();
    unsigned acks[]={11,20,29,39};
    for(unsigned i=0;i<4;i++) {
        reset();nack_at=acks[i];
        assert(raphael_i2c_transfer(&b,0x1a,out,2,in,2)==RAPHAEL_I2C_NACK);restored();
    }
    reset();stuck_sda=true;assert(raphael_i2c_transfer(&b,0x1a,out,2,in,2)==RAPHAEL_I2C_BUSY);restored();
    reset();stuck_scl=true;assert(raphael_i2c_transfer(&b,0x1a,out,2,in,2)==RAPHAEL_I2C_BUSY);restored();
    reset();stretch_at=12;assert(raphael_i2c_transfer(&b,0x1a,out,2,in,2)==RAPHAEL_I2C_STRETCH);restored();
    uint8_t start[]={1,0,1};
    reset();samples=0;
    add(true,true);add(true,true);tx(0x34);tx(1);tx(0);tx(1);add(true,true);
    assert(raphael_i2c_transfer(&b,0x1a,start,3,NULL,0)==RAPHAEL_I2C_OK);
    assert(sample_at==samples);restored(); /* No repeated START/read address on writes. */
    unsigned write_acks[]={11,20,29,38};
    for(unsigned i=0;i<4;i++) {
        reset();samples=0;
        add(true,true);add(true,true);tx(0x34);tx(1);tx(0);tx(1);add(true,true);
        nack_at=write_acks[i];
        assert(raphael_i2c_transfer(&b,0x1a,start,3,NULL,0)==RAPHAEL_I2C_NACK);restored();
    }
    reset();assert(raphael_i2c_transfer(&b,0xff,out,2,in,2)==RAPHAEL_I2C_ARGUMENT);
    assert(raphael_i2c_transfer(&b,0x1a,out,33,in,2)==RAPHAEL_I2C_ARGUMENT);
    assert(raphael_i2c_transfer(NULL,0x1a,out,2,in,2)==RAPHAEL_I2C_ARGUMENT);
    assert(raphael_i2c_transfer(&b,0x1a,out,2,NULL,1)==RAPHAEL_I2C_ARGUMENT);
    assert(reads==0&&writes==0);
    puts("I2C: 0x34/0x35, 0x0016, repeated START, 0x0586, ACK/NACK, open drain and failure rollback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='raphael-i2c-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
