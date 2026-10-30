#!/usr/bin/env python3
"""Exercise the real request lifecycle, including partially applied writes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-power.h').read_text()
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
''' + source + r'''
struct mock { uint32_t vote[4][3], original[4][3]; unsigned writes, reads, fail_write, fail_read;
    unsigned rail[80], field[80]; uint32_t value[80]; bool quarantine, corrupt; };
static bool read_vote(void *ctx,unsigned r,unsigned f,uint32_t *v) {
    struct mock *m=ctx;assert(r<4&&f<3);
    if(m->quarantine||++m->reads==m->fail_read)return false;
    *v=m->vote[r][f];if(m->corrupt&&m->writes)*v^=1;return true;
}
static bool write_vote(void *ctx,unsigned r,unsigned f,uint32_t v) {
    struct mock *m=ctx;assert(r<4&&f<3&&m->writes<80);
    if(m->quarantine)return false;
    unsigned i=m->writes++;m->rail[i]=r;m->field[i]=f;m->value[i]=v;
    if(f==0)assert(v==m->original[r][f]||(v>=raphael_power_rails[r].minimum_mv&&v<=raphael_power_rails[r].maximum_mv));
    if(f==1)assert(v<=1);if(f==2)assert(v<=7);
    m->vote[r][f]=v; /* Failure can occur after the hardware applied the request. */
    return m->writes!=m->fail_write;
}
static const struct raphael_power_ops ops={read_vote,write_vote};
static void restore(struct raphael_power_state *s,struct mock *m) {
    m->fail_read=m->fail_write=0;m->corrupt=false;
    for(unsigned r=4;r--;)assert(raphael_power_restore(s,&ops,m,r));
    assert(raphael_power_end(s));assert(!memcmp(m->vote,m->original,sizeof(m->vote)));
}
int main(void) {
    struct raphael_power_state s={0};struct mock m={0};
    assert(raphael_power_begin(&s,&ops,&m));
    assert(!raphael_power_begin(&s,&ops,&m));
    for(unsigned r=0;r<4;r++)assert(raphael_power_enable(&s,&ops,&m,r));
    assert(m.vote[3][2]==6&&m.vote[0][2]==7&&m.vote[1][2]==2&&m.vote[2][2]==0);
    assert(!raphael_power_end(&s));restore(&s,&m);
    /* Every write failure, including an accepted enable without completion. */
    for(unsigned failed=1;failed<=11;failed++) {
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.fail_write=failed;
        assert(raphael_power_begin(&s,&ops,&m));bool ok=true;
        for(unsigned r=0;r<4&&ok;r++)ok=raphael_power_enable(&s,&ops,&m,r);
        assert(!ok&&s.failed);unsigned before=m.writes;
        assert(!raphael_power_enable(&s,&ops,&m,0)&&m.writes==before);restore(&s,&m);
    }
    /* A readback failure after each successful write must also restore. */
    for(unsigned failed=13;failed<=23;failed++) {
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.fail_read=failed;
        assert(raphael_power_begin(&s,&ops,&m));bool ok=true;
        for(unsigned r=0;r<4&&ok;r++)ok=raphael_power_enable(&s,&ops,&m,r);
        assert(!ok&&s.failed);restore(&s,&m);
    }
    /* Snapshot failures have no side effects. */
    for(unsigned failed=1;failed<=12;failed++) {
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.fail_read=failed;
        assert(!raphael_power_begin(&s,&ops,&m)&&!s.active&&!m.writes);
    }
    /* An existing shared BOB request remains enabled and is never lowered. */
    memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));
    m.vote[1][0]=3320;m.vote[1][1]=1;m.vote[1][2]=7;memcpy(m.original,m.vote,sizeof(m.vote));
    assert(raphael_power_begin(&s,&ops,&m));assert(raphael_power_enable(&s,&ops,&m,1));
    assert(m.writes==0);restore(&s,&m);
    /* No guessing if the original request exceeds the sensor's maximum. */
    memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.vote[3][0]=1500;
    assert(!raphael_power_begin(&s,&ops,&m)&&!m.writes);
    /* The dirty flag survives failed restore, preventing a false clean exit. */
    memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));assert(raphael_power_begin(&s,&ops,&m));
    assert(raphael_power_enable(&s,&ops,&m,0));m.quarantine=true;
    assert(!raphael_power_restore(&s,&ops,&m,0)&&s.dirty[0]&&!raphael_power_end(&s));
    m.quarantine=false;restore(&s,&m);
    memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));assert(raphael_power_begin(&s,&ops,&m));m.corrupt=true;
    assert(!raphael_power_enable(&s,&ops,&m,0));restore(&s,&m);
    puts("Power: board limits/modes, shared votes, every partial-write/read failure, rollback and dirty-state retention PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='camera-power-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
