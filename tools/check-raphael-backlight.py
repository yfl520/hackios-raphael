#!/usr/bin/env python3
"""Fault-inject the actual DSI transport: packet, W1C, restore and timeout."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-dsi.h').read_text()
for name in ('dsi_read', 'dsi_write', 'dsi_barrier'):
    source = re.sub(r'static (?:uint32_t|void)\n' + name + r'\(.*?\n\}', '', source, flags=re.S)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
static uint32_t regs[0x400/4], fifo[4], fifo_count;
static unsigned writes, kicks, resets;
static uint64_t now;
static bool clocks=true, complete=true, bta=true, inject_error;
static bool paused;
static unsigned begins, ends;
static uint32_t response_type=0x22, response_level=0x07ff, response_count=4;
static uint32_t dsi_read(uint32_t off) { assert(off<sizeof(regs));return regs[off/4]; }
static void dsi_write(uint32_t off,uint32_t v) {
    assert(off<sizeof(regs)); writes++;
    if (off==0x110) {
        uint32_t pending=regs[off/4]&0x51115501;
        regs[off/4]=(v&~0x51115501)|(pending&~v);return;
    }
    regs[off/4]=v;
    if(off==0x17c) { assert(fifo_count<4);fifo[fifo_count++]=v; }
    if(off==0x1ec&&v==1) { resets++;fifo_count=0; }
    if(off==0x1d4&&v==1) regs[off/4]=0;
    if(off==0x90) {
	    assert(paused);
        kicks++;assert(fifo_count==2);assert(regs[0x4c/4]==4||regs[0x4c/4]==8);
        if(complete) regs[0x110/4]|=1;
        if(inject_error) regs[0x110/4]|=1u<<24;
        if((fifo[0]&0x00ff0000)==0x00060000) {
            if(bta) regs[0x110/4]|=1u<<20;
            regs[0x1d4/4]=response_count<<16;
            uint32_t data=response_type|((response_level>>8)<<8)|((response_level&255)<<16);
            regs[0x6c/4]=__builtin_bswap32(data);
        }
    }
}
static void dsi_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint64_t mach_absolute_time(void) { return now; }
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) { *ns=t; }
static void delay_for_interval(unsigned n,uint64_t scale) { now+=n*scale; }
static int raphael_display_scan_counter(uint32_t *v) { *v=0;return clocks; }
static vm_offset_t ml_io_map(vm_offset_t pa,unsigned n) { assert(pa==0x0ae94000&&n==0x400);return (vm_offset_t)regs; }
int raphael_display_command_begin(uint32_t *a,uint32_t *s) {assert(!paused);paused=true;begins++;*a=9;*s=10;return 0;}
void raphael_display_command_end(uint32_t a,uint32_t s) {assert(paused&&a==9&&s==10);paused=false;ends++;}
''' + source + r'''
static void setup(void) {
    memset(regs,0,sizeof(regs));fifo_count=writes=kicks=resets=0;now=0;
    raphael_dsi=0;dsi_failed=false;clocks=complete=bta=true;inject_error=false;
	paused=false;begins=ends=0;
    response_type=0x22;response_level=2047;response_count=4;
    regs[4/4]=5;regs[0x3c/4]=0x80000022;regs[0x4c/4]=96;regs[0x84/4]=0x80000001;
    regs[0x110/4]=(1u<<25)|(1u<<9)|(1u<<8)|1u;
}
static void restored(void) {
	assert(!paused&&begins==ends);
    assert(!regs[0x15c/4]&&!regs[0x1ec/4]);
    assert(regs[0x3c/4]==0x80000022&&regs[0x4c/4]==96&&regs[0x84/4]==0x80000001);
    assert(regs[0x110/4]&(1u<<8)); /* unrelated W1C status survives */
    assert((regs[0x110/4]&((1u<<25)|(1u<<9)))==((1u<<25)|(1u<<9)));
}
int main(void) {
    setup();clocks=false;assert(dsi_brightness_write(50)==ENXIO&&!writes);
    setup();regs[4/4]=3;assert(dsi_brightness_write(50)==EBUSY&&!writes);
    setup();regs[0x15c/4]=1;assert(dsi_brightness_write(50)==EBUSY&&!writes);
    setup();regs[0x008/4]=4;assert(dsi_brightness_write(50)==ETIMEDOUT&&!writes);
    setup();regs[0x110/4]|=1u<<24;assert(dsi_brightness_write(50)==EIO&&!writes);
    setup();assert(!dsi_brightness_write(50));assert(kicks==1&&resets==1);restored();
    /* Capture real FIFO words before cleanup instead of duplicating the builder. */
    assert(fifo[0]==0xc0390003&&fifo[1]==0xff000451); /* 1024: 51 04 00 ff */
    setup();assert(!dsi_brightness_write(1));assert(fifo[1]==0xff140051);restored();
    setup();assert(!dsi_brightness_write(100));assert(fifo[1]==0xffff0751);restored();
    setup();uint32_t level=99;assert(!dsi_brightness_read(&level)&&level==100&&kicks==2);restored();
    setup();response_level=1024;assert(!dsi_brightness_read(&level)&&level==50);
    setup();response_type=2;level=77;assert(dsi_brightness_read(&level)==EIO&&level==77);
    setup();response_level=2048;assert(dsi_brightness_read(&level)==ERANGE);
    setup();response_count=0;assert(dsi_brightness_read(&level)==EIO);
    setup();complete=false;assert(dsi_brightness_write(50)==ETIMEDOUT&&dsi_failed);restored();
    unsigned before=writes;assert(dsi_brightness_write(60)==ENXIO&&writes==before);
    setup();inject_error=true;assert(dsi_brightness_write(50)==EIO);restored();
    assert(regs[0x110/4]&(1u<<24)); /* error preserved for diagnosis */
    setup();bta=false;assert(dsi_brightness_read(&level)==ETIMEDOUT&&dsi_failed);restored();
    puts("PASS: real DSI packets, endian readback, clocks/ownership/idle guards, W1C isolation, timeout cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='raphael-backlight-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
