#!/usr/bin/env python3
"""Exercise real table loader/executor with malformed input and bus failures.

An optional R586TB01 file also exercises the privately generated vendor table.
The default fixture contains no vendor firmware and only models register IO.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-sensor.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define say(...) ((void)0)
struct raphael_i2c_bus { unsigned unused; };
enum raphael_i2c_result { RAPHAEL_I2C_OK, RAPHAEL_I2C_NACK };
static uint8_t reg[65536];
static unsigned calls, fail_at, on_writes, off_writes;
static bool bad_id, bad_readback;
static void spin_us(unsigned us){assert(us<=100000);}
static enum raphael_i2c_result
raphael_i2c_transfer(struct raphael_i2c_bus *bus,uint8_t address,const uint8_t *out,unsigned n,uint8_t *in,unsigned m) {
    assert(bus&&address==0x1a&&n>=2);calls++;
    if(calls==fail_at)return RAPHAEL_I2C_NACK;
    unsigned a=(out[0]<<8)|out[1];
    if(m) {
        assert(n==2&&in&&m<=2);
        for(unsigned i=0;i<m;i++)in[i]=reg[a+i];
        if(a==0x16&&bad_id)in[0]=0;
        if(a==0x34c&&bad_readback)in[0]=0;
    } else {
        assert(n==3&&!in);reg[a]=out[2];
        if(a==0x100){if(out[2])on_writes++;else off_writes++;}
    }
    return RAPHAEL_I2C_OK;
}
''' + source + r'''
static union { uint32_t align; uint8_t data[1704]; } buffer;
static struct raphael_i2c_bus bus;
static void reset(void){memset(reg,0,sizeof(reg));reg[0x16]=5;reg[0x17]=0x86;calls=fail_at=on_writes=off_writes=0;bad_id=bad_readback=false;}
static void fixture(void) {
    struct raphael_sensor_header h={"R586TB01",1704,4000,3000,0x3210,4,459520000,8976,3064,14,30010,3,199,6,0};
    memcpy(buffer.data,&h,sizeof(h));
    struct raphael_sensor_stage *p=(void*)(buffer.data+64);
    struct raphael_sensor_entry *e=(void*)(p+6);
    unsigned counts[]={103,92,1,1,1,1},at=0;
    for(unsigned i=0;i<6;i++){p[i]=(struct raphael_sensor_stage){at,counts[i]};at+=counts[i];}
    const struct {uint16_t a;uint8_t v;} checks[]={
        {0x136,0x13},{0x137,0x33},{0x112,10},{0x113,10},{0x114,3},
        {0x340,0x0b},{0x341,0xf8},{0x342,0x23},{0x343,0x10},
        {0x34c,0x0f},{0x34d,0xa0},{0x34e,0x0b},{0x34f,0xb8},
    };
    for(unsigned i=0;i<199;i++)e[i]=(struct raphael_sensor_entry){0x7000,0,0,0};
    for(unsigned i=0;i<sizeof(checks)/sizeof(checks[0]);i++)e[103+i]=(struct raphael_sensor_entry){checks[i].a,checks[i].v,0,0};
    for(unsigned i=2;i<6;i++)e[p[i].first]=(struct raphael_sensor_entry){i<4?0x100:0x104,i==2||i==4,0,0};
}
static bool load(struct raphael_sensor *s){return raphael_sensor_load(s,buffer.data,sizeof(buffer.data));}
int main(int argc,char **argv) {
    fixture();
    if(argc==2){FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(buffer.data,1,1704,f)==1704);assert(fgetc(f)==EOF);fclose(f);}
    struct raphael_sensor s={0};
    for(unsigned n=0;n<sizeof(buffer.data);n++){assert(!raphael_sensor_load(&s,buffer.data,n));assert(!s.head);}
    assert(!raphael_sensor_load(&s,buffer.data+1,1703));
    struct raphael_sensor_header *h=(void*)buffer.data;
    struct raphael_sensor_stage *p=(void*)(buffer.data+64);
    struct raphael_sensor_entry *e=(void*)(p+6);
    h->entries=0xffffffff;assert(!load(&s));h->entries=199;
    p[5].first++;assert(!load(&s));p[5].first--;
    e[198].value=1;assert(!load(&s));e[198].value=0;
    uint16_t saved=e[0].address;e[0].address=0x100;assert(!load(&s));e[0].address=saved;
    reset();assert(load(&s));assert(!raphael_sensor_start(&s,&bus)&&!calls);
    assert(raphael_sensor_prepare(&s,&bus));unsigned configure_calls=calls;
    assert(s.prepared&&!s.stream_dirty&&!reg[0x100]&&!on_writes);
    assert(!load(&s)); /* Active table ownership preserved. */
    assert(raphael_sensor_start(&s,&bus));assert(s.stream_dirty&&reg[0x100]==1&&on_writes==1);
    unsigned before=calls;assert(!raphael_sensor_start(&s,&bus)&&calls==before);
    fail_at=calls+1;assert(!raphael_sensor_stop(&s,&bus)&&s.stream_dirty);
    fail_at=0;assert(raphael_sensor_stop(&s,&bus));assert(!reg[0x100]&&!s.stream_dirty&&!s.prepared);
    for(unsigned f=1;f<=configure_calls;f++) {
        reset();s=(struct raphael_sensor){0};assert(load(&s));fail_at=f;
        assert(!raphael_sensor_prepare(&s,&bus));assert(!s.prepared&&!on_writes);
        fail_at=0;assert(raphael_sensor_stop(&s,&bus));
    }
    reset();s=(struct raphael_sensor){0};assert(load(&s));bad_id=true;
    assert(!raphael_sensor_prepare(&s,&bus)&&!off_writes&&!on_writes);
    reset();s=(struct raphael_sensor){0};assert(load(&s));bad_readback=true;
    assert(!raphael_sensor_prepare(&s,&bus)&&!s.prepared&&!on_writes);
    for(unsigned extra=1;extra<=2;extra++) {
        reset();s=(struct raphael_sensor){0};assert(load(&s));assert(raphael_sensor_prepare(&s,&bus));
        fail_at=calls+extra;assert(!raphael_sensor_start(&s,&bus)&&s.stream_dirty);
        fail_at=0;assert(raphael_sensor_stop(&s,&bus)&&!reg[0x100]);
    }
    puts("IMX586: preflight bounds, stages, bus failures, stream ownership, stop retry and mode readback with simulated I2C PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='raphael-sensor-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe), *sys.argv[1:]], check=True)
