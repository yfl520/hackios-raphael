#!/usr/bin/env python3
"""Actual bounded clock lifecycle: each write, timeout and dependency rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-clocks.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uintptr_t vm_offset_t;
#define say printf
static uint32_t cc[0x10000/4],gc[0x10000/4],pin;
static unsigned writes,fail_at,held,clock_polls;
static bool votes, vote_fail, restore_fail;
static uintptr_t raphael_camera_map(uintptr_t p,unsigned size) {
    assert(size==(p==0x0390d000?0x1000u:0x10000u));return p;
}
static uint32_t *ptr(uintptr_t p) {
    if(p==0x0390d000)return &pin;
    if(p>=0x0ad00000&&p<0x0ad10000)return &cc[(p-0x0ad00000)/4];
    if(p>=0x00100000&&p<0x00110000)return &gc[(p-0x00100000)/4];
    assert(p==0x0390d004);static uint32_t zero;return &zero;
}
static uint32_t rd(uintptr_t p){return *ptr(p);}
static void wr(uintptr_t p,uint32_t v) {
    assert(p==0x0390d000||p==0x0010b008||p==0x0010b044||
        p==0x0ad0c1bc||p==0x0ad05008||p==0x0ad05004||p==0x0ad0501c);
    writes++;if(writes==fail_at)return;
    *ptr(p)=v;
    if(p==0x0ad0c1bc) {
        assert(votes);assert((gc[0xb008/4]&1)&&(gc[0xb044/4]&1));
        if(held!=1)*ptr(p)=(v&~0x80000000u)|((v&1)?0:0x80000000u);
    }
    if(p==0x0ad05004&&held!=2)*ptr(p)=v&~1u;
    if(p==0x0ad0501c&&held!=3)*ptr(p)=(v&~0x80000000u)|((v&1)?0:0x80000000u);
}
static void spin_us(unsigned us){assert(us<=10);clock_polls++;assert(clock_polls<10000);}
bool hakios_raphael_camera_clock_votes(bool restore) {
    if(restore) {
        assert(!(cc[0xc1bc/4]&0x80000000u));
        if(restore_fail)return false;
        votes=false;return true;
    }
    votes=true;return !vote_fail;
}
''' + source + r'''
static void reset(void) {
    memset(cc,0,sizeof(cc));memset(gc,0,sizeof(gc));
    cc[0xc1bc/4]=0x22f001;cc[0x501c/4]=0x80000000u;
    cc[0x5008/4]=0x3205;gc[0xb008/4]=3;gc[0xb044/4]=0x80000000u;pin=1;
    writes=fail_at=held=clock_polls=0;votes=vote_fail=restore_fail=false;
}
static void restored(struct raphael_camera_clocks *c) {
    assert(!votes&&!c->votes&&!c->domain_dirty&&!c->cfg_dirty&&!c->branch_dirty&&!c->pin_dirty);
    assert(cc[0xc1bc/4]==0x22f001&&cc[0x501c/4]==0x80000000u);
    assert(cc[0x5008/4]==0x3205&&gc[0xb008/4]==3&&!(gc[0xb044/4]&1)&&pin==1);
}
int main(void) {
    reset();struct raphael_camera_clocks c={0};
    assert(raphael_camera_clocks_begin(&c));unsigned n=writes;
    assert((cc[0x5008/4]&0x371f)==1&&pin==4);
    assert(raphael_camera_clocks_end(&c));restored(&c);
    for(unsigned f=1;f<=n;f++) {
        reset();memset(&c,0,sizeof(c));fail_at=f;
        bool ok=raphael_camera_clocks_begin(&c);(void)ok;
        fail_at=0;assert(raphael_camera_clocks_end(&c));restored(&c);
    }
    for(unsigned h=1;h<=3;h++) {
        reset();memset(&c,0,sizeof(c));held=h;
        assert(!raphael_camera_clocks_begin(&c));
        held=0;/* emulate completion after the bounded timeout */
        cc[0x5004/4]&=~1u;
        assert(raphael_camera_clocks_end(&c));restored(&c);
    }
    reset();memset(&c,0,sizeof(c));assert(raphael_camera_clocks_begin(&c));
    held=1;assert(!raphael_camera_clocks_end(&c));
    assert(votes&&c.domain_dirty&&(gc[0xb044/4]&1)); /* preserve parents */
    held=0;assert(raphael_camera_clocks_end(&c));restored(&c);
    reset();memset(&c,0,sizeof(c));assert(raphael_camera_clocks_begin(&c));
    cc[0xc1bc/4]|=2;assert(!raphael_camera_clocks_end(&c));assert(votes&&c.domain_dirty);
    cc[0xc1bc/4]&=~2u;assert(raphael_camera_clocks_end(&c));restored(&c);
    reset();memset(&c,0,sizeof(c));assert(raphael_camera_clocks_begin(&c));
    restore_fail=true;assert(!raphael_camera_clocks_end(&c));assert(c.votes);
    restore_fail=false;assert(raphael_camera_clocks_end(&c));restored(&c);
    for(unsigned busy=0;busy<5;busy++) {
        reset();memset(&c,0,sizeof(c));
        if(busy==0)cc[0xc1bc/4]|=2;
        if(busy==1)cc[0xc1bc/4]|=0x80000000u;
        if(busy==2)cc[0x501c/4]|=1;
        if(busy==3)cc[0x5004/4]=1;
        if(busy==4)pin|=4;
        assert(!raphael_camera_clocks_begin(&c));assert(!writes&&!votes);
        assert(raphael_camera_clocks_end(&c));
    }
    reset();memset(&c,0,sizeof(c));vote_fail=true;
    assert(!raphael_camera_clocks_begin(&c));assert(!writes);
    assert(raphael_camera_clocks_end(&c));restored(&c);
    puts("Clocks: actual MMIO allowlist, every write failure, bounded timeouts, busy guards and parent retention PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-clocks-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
