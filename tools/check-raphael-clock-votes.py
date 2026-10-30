#!/usr/bin/env python3
"""Actual ARC prerequisite/vote code: malformed aux, failures, rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
wifi = (root / 'xnu/files/bsd/dev/hakios.wifi.c').read_text()
types = wifi[wifi.index('struct cmd_db_slave {'):wifi.index('static const struct cmd_db_header *cmd_db;')]
lookup = wifi[wifi.index('static const struct cmd_db_entry *\nrpmh_resource('):wifi.index('\nstatic void\nshow_rpmh_resource(')]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-clock-votes.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define CMD_DB_SIZE 0x20000
#define __offsetof offsetof
#define tell printf
''' + types + r'''
static union { uint64_t align; uint8_t bytes[CMD_DB_SIZE]; } storage;
static struct cmd_db_header *cmd_db=(void *)storage.bytes;
static bool cmd_db_there(void){return true;}
static bool hakios_is_raphael(void){return true;}
static int rpmh_lock;
static unsigned locks;
static void lck_mtx_lock(int *p){assert(p==&rpmh_lock);assert(!locks++);}
static void lck_mtx_unlock(int *p){assert(p==&rpmh_lock);assert(locks--==1);}
static struct { bool active; } raphael_camera_power;
static bool raphael_rpmh_faulted, fail_write, fail_read, mismatch;
static uint32_t values[9];
static unsigned written;
static unsigned slot(uint32_t a) {
    if(a==0x500a8)return 0;
    const uint32_t bcm[]={0x50088,0x50084,0x5003c,0x50000,0x5000c};
    for(unsigned i=0;i<5;i++)if(a==bcm[i])return 4+i;
    assert(a==0x30010||a==0x30020||a==0x30040);
    return a==0x30010?1:a==0x30020?2:3;
}
static bool raphael_rpmh_read_locked(uint32_t a,uint32_t *v) {
    unsigned i=slot(a);
    if(fail_read&&i==3&&written)return false;
    *v=values[i]|(i?0x80000000u:0);return true;
}
static bool raphael_rpmh_write_locked(uint32_t a,uint32_t v) {
    assert(a==0x30040);written++;
    /* A failed write may already have reached the controller. */
    values[3]=mismatch?0:v;return !fail_write;
}
''' + lookup + source + r'''
static struct cmd_db_entry *entries;
static void reset(void) {
    memset(&storage,0,sizeof(storage));
    entries=(void *)cmd_db->data;
    cmd_db->slave[0].id=3;cmd_db->slave[0].count=4;cmd_db->slave[0].data_offset=512;
    const char *names[]={"CN0","mx.lvl","cx.lvl","mmcx.lvl"};
    const uint32_t addresses[]={0x500a8,0x30010,0x30020,0x30040};
    const uint16_t levels[]={0,16,64,128,192,256,384,0};
    for(unsigned i=0;i<4;i++) {
        memcpy(entries[i].id,names[i],strlen(names[i]));entries[i].address=addresses[i];
        entries[i].length=16;entries[i].offset=i*16;
        memcpy(cmd_db->data+512+i*16,levels,16);
    }
    values[0]=0x20003fff;values[1]=5;values[2]=4;values[3]=0;
    locks=written=0;raphael_camera_power.active=true;
    raphael_camera_mmcx_dirty=raphael_rpmh_faulted=fail_write=fail_read=mismatch=false;
}
int main(void) {
    reset();
    cmd_db->slave[0].count=5;
    const char *bn[]={"MM1","MM0","SH0","MC0","ACV"};
    const uint32_t ba[]={0x50088,0x50084,0x5003c,0x50000,0x5000c};
    const uint32_t units[]={1670000,1670000,3910000,2050000,1};
    const uint16_t widths[]={32,64,64,16,0};
    const uint32_t requests[]={0x22570bb3,0x24add765,0x213d912a,0x225da0bd,0x20000008};
    for(unsigned i=0;i<5;i++){
        memset(&entries[i],0,sizeof(entries[i]));memcpy(entries[i].id,bn[i],strlen(bn[i]));
        entries[i].address=ba[i];entries[i].length=8;entries[i].offset=16*i;
        uint8_t *aux=cmd_db->data+512+16*i;memset(aux,0,8);
        memcpy(aux,&units[i],4);memcpy(aux+4,&widths[i],2);values[4+i]=requests[i];
    }
    assert(hakios_raphael_camera_data_ready()&&!written&&!locks);
    for(unsigned i=0;i<5;i++){
        values[4+i]=0;assert(!hakios_raphael_camera_data_ready()&&!written);
        values[4+i]=requests[i];
    }
    reset();memset(entries[0].id,0,8);memcpy(entries[0].id,"MM1",3);
    entries[0].length=8;hakios_raphael_camera_data_inspect();assert(!written&&!locks);
    entries[0].offset=0xffff;cmd_db->slave[0].data_offset=0xffff;
    hakios_raphael_camera_data_inspect();assert(!written&&!locks);
    reset();values[3]=3;values[1]=4;
    assert(hakios_raphael_camera_capture_ready()&&!written); /* MX192/MMCX128 */
    values[1]=3;assert(!hakios_raphael_camera_capture_ready()&&!written);
    values[1]=4;values[3]=2;assert(!hakios_raphael_camera_capture_ready()&&!written);
    values[3]=3;raphael_rpmh_faulted=true;
    assert(!hakios_raphael_camera_capture_ready()&&!written);
    reset();assert(hakios_raphael_camera_clock_votes(false));
    assert(values[3]==2&&written==1&&raphael_camera_mmcx_dirty);
    assert(hakios_raphael_camera_clock_votes(true));assert(!values[3]&&!raphael_camera_mmcx_dirty);
    reset();values[3]=5;assert(hakios_raphael_camera_clock_votes(false));
    assert(!written&&values[3]==5);assert(hakios_raphael_camera_clock_votes(true));
    reset();values[0]=0x20000001;assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    reset();values[1]=2;assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    reset();values[2]=1;assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    reset();raphael_camera_power.active=false;assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    reset();raphael_rpmh_faulted=true;assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    for(unsigned f=0;f<3;f++) {
        reset();fail_write=f==0;fail_read=f==1;mismatch=f==2;
        assert(!hakios_raphael_camera_clock_votes(false));assert(raphael_camera_mmcx_dirty);
        fail_write=fail_read=mismatch=false;assert(hakios_raphael_camera_clock_votes(true));
        assert(!values[3]&&!raphael_camera_mmcx_dirty);
    }
    reset();assert(hakios_raphael_camera_clock_votes(false));fail_write=true;
    assert(!hakios_raphael_camera_clock_votes(true));assert(raphael_camera_mmcx_dirty);
    fail_write=false;assert(hakios_raphael_camera_clock_votes(true));
    for(unsigned invalid=0;invalid<9;invalid++) {
        reset();uint16_t *aux=(void *)(cmd_db->data+512+3*16);
        switch(invalid) {
        case 0:entries[3].length=0;break;
        case 1:entries[3].length=15;break;
        case 2:entries[3].length=34;break;
        case 3:aux[0]=1;break;
        case 4:aux[2]=aux[1];break;
        case 5:aux[1]=0;break; /* no sufficient level */
        case 6:entries[3].address=0x40040;break;
        case 7:entries[3].address|=1;break;
        case 8:values[3]=15;break;
        }
        assert(!hakios_raphael_camera_clock_votes(false));assert(!written);
    }
    reset();cmd_db->slave[0].count=513;
    unsigned sel,count;uint16_t levels[16];
    assert(!raphael_camera_arc_level(entries+3,64,&sel,&count,levels));
    assert(!locks);
    puts("Clock votes: ARC parsing, full 8-byte MMCX, CN0/MX/CX guards, partial-write and restore failures PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-clock-votes-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
