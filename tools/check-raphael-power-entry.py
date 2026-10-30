#!/usr/bin/env python3
"""Exercise the real power adapter's boot-mode gate without hardware access."""
from pathlib import Path
import subprocess
import tempfile
text = (Path(__file__).resolve().parents[1] / 'xnu/files/bsd/dev/hakios.wifi.c').read_text()
start = text.index('bool\nhakios_raphael_camera_power_begin(void)')
end = text.index('\nbool\nhakios_raphael_camera_power_step', start)
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
static unsigned boot_cam, locks, reads;
static bool raphael=true, has_arg=true, raphael_rpmh_faulted;
static int rpmh_lock, raphael_camera_power_ops;
static struct {bool active;uint32_t saved[4][3];} raphael_camera_power;
static uint32_t raphael_camera_power_address[4];
static struct {const char *name;} raphael_power_rails[4];
static struct {uintptr_t base;} rsc;
struct cmd_db_entry {uint32_t address;};
static bool hakios_is_raphael(void){return raphael;}
static bool PE_parse_boot_argn(const char *n,void *out,size_t z) {
    (void)n;assert(z==4);*(uint32_t *)out=boot_cam;return has_arg;
}
static void lck_mtx_lock(int *m){(void)m;locks++;}
static void lck_mtx_unlock(int *m){(void)m;}
static const struct cmd_db_entry *rpmh_resource(const char *n,int i) {
    (void)n;(void)i;reads++;assert(0);return NULL;
}
static void need(void *r){(void)r;assert(0);}
static bool raphael_power_begin(void *p,void *ops,void *ctx){
    (void)p;(void)ops;(void)ctx;assert(0);return false;
}
#define tell(...) ((void)0)
''' + text[start:end] + r'''
int main(void){
    /* An already active owner forces safe refusal after entering the gate,
     * letting us distinguish allowed diagnostics from rejected boot modes. */
    raphael_camera_power.active=true;
    for(boot_cam=0;boot_cam<12;boot_cam++){
        locks=0;assert(!hakios_raphael_camera_power_begin());
        assert(locks==(boot_cam>=3&&boot_cam<=10));
        assert(!reads);
    }
    boot_cam=6;locks=0;raphael=false;
    assert(!hakios_raphael_camera_power_begin()&&!locks);
    raphael=true;has_arg=false;
    assert(!hakios_raphael_camera_power_begin()&&!locks);
}
'''
with tempfile.TemporaryDirectory() as td:
    src, exe = Path(td)/'gate.c', Path(td)/'gate'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Actual power adapter: cam3..10 diagnostics/live-preview admitted; others, wrong board and missing argument refused')
