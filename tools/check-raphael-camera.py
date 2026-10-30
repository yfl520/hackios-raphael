#!/usr/bin/env python3
"""Check actual rear-platform reads against the primary-source register list."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-camera.h').read_text()
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
#define say printf
static unsigned maps, reads;
static const uintptr_t allowed[]={0x0390d000,0x03911000,0x03912000,0x0351c000,0x0395a000,0x03961000,0x0ad00000};
static uintptr_t raphael_camera_map(uintptr_t p,unsigned z) {
    assert(maps<7&&p==allowed[maps]);assert(z==(maps==6?0x10000u:0x1000u));maps++;return p;
}
static uint32_t rd(uintptr_t p) {
    /* Exact physical reads only; no GPIO outside rear-camera set. */
    int found=0;
    for(unsigned i=0;i<6;i++) if(p==allowed[i]||p==allowed[i]+4) found=1;
    if(p==0x0ad0c1bc||p==0x0ad05004||p==0x0ad05008||p==0x0ad0501c) found=1;
    assert(found);reads++;return 0;
}
static unsigned requests;
bool hakios_raphael_camera_rpmh_read(const char *name,unsigned field,uint32_t *value) {
    static const char *names[]={"ldoa17","bobc1","smpa5","smpc8"};
    assert(requests<12&&strcmp(name,names[requests/3])==0&&field==requests%3);
    requests++;*value=0;return true;
}
''' + source + r'''
int main(void) {raphael_camera_inspect();assert(maps==7&&reads==16);
    raphael_camera_power_inspect();assert(requests==12&&reads==16);
    puts("camera: rear GPIO/CAMCC and VRM read allowlist PASS");}
'''
with tempfile.TemporaryDirectory(prefix='camera-read-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
