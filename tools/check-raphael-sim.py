#!/usr/bin/env python3
"""Exercise the actual SIM probe, including malformed replies and transport life."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-sim.h').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef void (*rx_t)(void *,uint32_t,uint32_t,const void *,size_t);
typedef void (*server_t)(void *,bool,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
struct hakios_qrtr_port { rx_t rx; server_t server;void *context;bool used; };
static struct hakios_qrtr_port ports[2];
static struct { bool open; } glink;
static bool raphael=true;
static unsigned enabled=1, opens, closes, sends, lookups;
static uint64_t now;
static uint8_t last[7];
static int send_error;
static uint64_t now_us(void) {return now;}
static bool hakios_is_raphael(void) {return raphael;}
static unsigned hakios_wifi_option(const char *s) {assert(!strcmp(s,"uim_probe"));return enabled;}
static struct hakios_qrtr_port *qrtr_open(rx_t rx,server_t server,void *ctx) {
    assert(opens<2);struct hakios_qrtr_port *p=&ports[opens++];
    *p=(struct hakios_qrtr_port){rx,server,ctx,true};return p;
}
static int qrtr_lookup(struct hakios_qrtr_port *p,uint32_t s,uint32_t v,uint32_t i) {
    assert(p->used&&(s==11||s==2)&&!v&&!i);lookups++;return 0;
}
static int qrtr_send(struct hakios_qrtr_port *p,uint32_t node,uint32_t to,const void *m,size_t n) {
    assert(p->used&&!node&&to==123&&n==7);memcpy(last,m,7);sends++;return send_error;
}
static void qrtr_close(struct hakios_qrtr_port *p) {assert(p->used);p->used=false;closes++;}
''' + source + r'''
static void setup(void) {
    memset(ports,0,sizeof(ports));memset(raphael_sim_queries,0,sizeof(raphael_sim_queries));
    raphael_sim_started=raphael_sim_finished=false;raphael_sim_deadline=0;
    glink.open=true;raphael=true;enabled=1;now=0;opens=closes=sends=lookups=0;send_error=0;
}
static void found(unsigned i) {
    struct hakios_qrtr_port *p=&ports[i];
    p->server(p->context,true,i?2:11,1,0,0,123); /* node zero is valid */
}
static const uint8_t card[]={255,255,255,255,255,255,255,255,1,
    1,2,3,10,0,1, /* one present card */
    2,7,0,0,0,0,3,0xaa,0xbb,0xcc,0,2,3,10,3,3,10}; /* ready USIM, 3-byte AID */
static size_t reply(uint8_t *m,unsigned i) {
    uint16_t txn=0x7301+i,msg=i?0x2d:0x2f;
    uint8_t h[]={2,txn&255,txn>>8,msg&255,msg>>8,0,0,2,4,0,0,0,0,0};memcpy(m,h,sizeof(h));
    size_t n=14;m[n++]=i?1:0x10;size_t z=i?1:sizeof(card);m[n++]=z;m[n++]=0;
    if(i)m[n++]=0;else{memcpy(m+n,card,z);n+=z;}
    m[5]=(n-7)&255;m[6]=(n-7)>>8;return n;
}
static void receive(unsigned i,const uint8_t *m,size_t n) {ports[i].rx(ports[i].context,0,123,m,n);}
int main(void) {
    struct raphael_sim_status s,unchanged;memset(&s,0x5a,sizeof(s));unchanged=s;
    for(size_t n=0;n<sizeof(card);n++) {
        assert(raphael_sim_decode(card,n,&s)==EPROTO);assert(!memcmp(&s,&unchanged,sizeof(s)));
    }
    assert(!raphael_sim_decode(card,sizeof(card),&s));
    assert(s.cards==1&&s.card[0].state==1&&s.card[0].app[0].state==7&&s.card[0].app[0].pin1==2);
    uint8_t absent[9]={0};assert(!raphael_sim_decode(absent,9,&s)&&!s.cards);
    uint8_t bad[sizeof(card)];memcpy(bad,card,sizeof(card));bad[8]=255;
    assert(raphael_sim_decode(bad,sizeof(bad),&s)==EPROTO);
    memcpy(bad,card,sizeof(card));bad[21]=255;assert(raphael_sim_decode(bad,sizeof(bad),&s)==EPROTO);
    setup();enabled=0;raphael_sim_tick();assert(!opens&&raphael_sim_finished);
    setup();glink.open=false;raphael_sim_tick();assert(!opens&&!raphael_sim_started);
    glink.open=true;raphael_sim_tick();assert(opens==2&&lookups==2);
    found(0);assert(sends==1&&!memcmp(last,(uint8_t[]){0,1,0x73,0x2f,0,0,0},7));
    found(0);assert(sends==1);found(1);assert(sends==2);
    uint8_t m[256];size_t n=reply(m,0);
    ports[0].rx(ports[0].context,1,123,m,n);assert(!raphael_sim_queries[0].done);
    m[1]++;receive(0,m,n);assert(!raphael_sim_queries[0].done);m[1]--;
    m[0]=4;receive(0,m,n);assert(!raphael_sim_queries[0].done);m[0]=2;
    receive(0,m,n);assert(raphael_sim_queries[0].done&&!raphael_sim_queries[0].error);
    n=reply(m,1);receive(1,m,n);raphael_sim_tick();assert(raphael_sim_finished&&closes==2);
    raphael_sim_tick();raphael_sim_close();assert(opens==2&&closes==2);
    setup();raphael_sim_tick();now=8000001;raphael_sim_tick();
    assert(raphael_sim_finished&&closes==2&&!sends&&raphael_sim_queries[0].error==ETIMEDOUT);
    setup();raphael_sim_tick();found(0);n=reply(m,0);m[n++]=99;m[5]++;
    receive(0,m,n);assert(raphael_sim_queries[0].done&&raphael_sim_queries[0].error==EPROTO);
    raphael_sim_close();assert(closes==2);
    setup();raphael_sim_tick();found(0);n=reply(m,0);m[10]=1;m[12]=3;
    receive(0,m,n);assert(raphael_sim_queries[0].error==EIO);raphael_sim_close();
    setup();raphael_sim_tick();found(0);ports[0].server(ports[0].context,false,11,1,0,0,123);
    assert(raphael_sim_queries[0].error==ENXIO);raphael_sim_close();
    setup();send_error=EIO;raphael_sim_tick();found(0);assert(raphael_sim_queries[0].done);raphael_sim_close();
    puts("raphael SIM: bounded parser, response correlation, timeout, teardown PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='raphael-sim-') as tmp:
    src, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
