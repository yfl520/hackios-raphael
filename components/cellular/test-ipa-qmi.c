/* Executes the actual experiment header with fake transport/hardware only. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define __unused __attribute__((unused))
#define hakios_tl(...) ((void)0)
#define QRTR_DEL_SERVER 5
#define QRTR_NEW_SERVER 4
#define QRTR_CONTROL 0xfffffffe
#define QRTR_US 1
struct hakios_qrtr_port {uint32_t number;};
struct qrtr_control {uint32_t command,words[4];};
struct smp2p {struct {uint32_t value;} entry[1];};
static struct {bool hardware;uint32_t restricted;} ripa;
static uint32_t qrtr_peer=0;
static unsigned sends,setups,starts,stops;
static int send_error;
static uint8_t last_packet[1024];static size_t last_size;
static bool ready_signal;
static bool gsi_ready=true;
static bool ripa_data_gsi_ready(void) {return gsi_ready;}
static struct hakios_qrtr_port ports[2]={{100},{101}};
static unsigned opens;
static uint64_t now_us(void) {return 1;}
static bool PE_parse_boot_argn(const char *s,void *p,size_t n) {assert(!strcmp(s,"ipa_preboot"));assert(n==4);*(uint32_t*)p=6;return true;}
static int qrtr_send(struct hakios_qrtr_port *p,uint32_t n,uint32_t q,const void *v,size_t z) {(void)p;(void)n;(void)q;assert(z<=sizeof last_packet);memcpy(last_packet,v,z);last_size=z;++sends;return send_error;}
static bool dsp_words(struct smp2p *p) {p->entry[0].value=ready_signal?2:0;return true;}
static int smp2p_entry(const struct smp2p *p,const char *s) {(void)p;(void)s;return 0;}
static bool hakios_ipa_data_setup(void) {++setups;return true;}
static bool raphael_ipa_data_start(void) {++starts;return true;}
static void raphael_ipa_data_stop(void) {++stops;}
static void raphael_modem_data_poll(void) {}
static struct hakios_qrtr_port *qrtr_open(void (*r)(void*,uint32_t,uint32_t,const void*,size_t),void (*s)(void*,bool,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t),void *c) {(void)r;(void)s;(void)c;assert(opens<2);return &ports[opens++];}
static void qrtr_close(struct hakios_qrtr_port *p) {(void)p;}
static int qrtr_out_packet(uint32_t a,uint32_t b,uint32_t c,uint32_t d,const void *e,size_t f) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
static int qrtr_lookup(struct hakios_qrtr_port *p,uint32_t a,uint32_t b,uint32_t c) {(void)p;(void)a;(void)b;(void)c;return 0;}
#include "hakios.raphael-ipa-wire.h"
static const uint8_t *qmi_field(const uint8_t *p,size_t n,uint8_t type,size_t *size) {
  for(size_t i=7;i+3<=n;) {
    size_t z=raphael_ipa_u16(p+i+1);if(z>n-i-3)return NULL;
    if(p[i]==type){*size=z;return p+i+3;}i+=z+3;
  }return NULL;
}
static bool ripa_data_opted_in(void);
#include "hakios.raphael-ipa-qmi.h"
static void reset(void) {memset(&ripa_qmi,0,sizeof ripa_qmi);opens=sends=setups=starts=stops=0;send_error=0;ready_signal=false;ripa.hardware=true;assert(ripa_qmi_start());ripa_client_server(NULL,true,0x31,1,2,0,200);}
static void host(uint16_t id) {uint8_t p[]={0,3,0,id&255,id>>8,0,0};ripa_host_receive(NULL,0,300,p,sizeof p);}
static void answer(uint16_t txn,uint16_t error,uint32_t node) {uint8_t p[]={2,txn&255,txn>>8,0x21,0,7,0,2,4,0,error?1:0,0,error&255,error>>8};ripa_client_receive(NULL,node,200,p,sizeof p);}
static size_t filter_request(uint8_t *p,unsigned count) {
  memset(p,0,6400);p[0]=0;p[1]=0x56;p[2]=0x34;p[3]=0x23;
  size_t n=1+173*count;p[7]=0x15;p[8]=n&255;p[9]=n>>8;p[10]=count;
  for(unsigned i=0;i<count;i++) {
    uint8_t *r=p+11+173*i;raphael_ipa_put32(r,i%2?2:1);
    raphael_ipa_put32(r+168,0x12345600+i);r[4+6]=2;
  }
  n+=10;p[5]=(n-7)&255;p[6]=(n-7)>>8;return n;
}
static void test_native_requests(void) {
  reset();unsigned old=sends;host(0x27);
  assert(sends==old+1 && ripa_qmi.config_accepted && last_packet[10]==0);
  assert(!ripa_qmi.registered && !ripa_qmi.uc_ready);
  uint8_t config[]={0,1,0,0x27,0,4,0,0x11,1,0,1};
  ripa_host_receive(NULL,0,300,config,sizeof config);assert(last_packet[3]==0x27 && last_packet[10]==0);
  old=sends;config[7]=0x12;ripa_host_receive(NULL,0,300,config,sizeof config);assert(sends==old);
  uint8_t p[6400];size_t n=filter_request(p,2);
  ripa_host_receive(NULL,0,300,p,n);
  assert(ripa_qmi.ul_cached && ripa_qmi.ul_count==2 && !ripa_qmi.ul_xlat);
  const uint8_t expected[]={2,0x56,0x34,0x23,0,19,0,2,4,0,0,0,0,0,0x11,9,0,2,0,0x56,0x34,0x12,1,0x56,0x34,0x12};
  assert(last_size==sizeof expected && !memcmp(last_packet,expected,sizeof expected));
  assert(!memcmp(ripa_qmi.ul_rules,p+11,346));
  /* A malformed replacement must not overwrite an already accepted cache. */
  for(size_t z=7;z<n;z++) {
    old=sends;p[5]=(z-7)&255;p[6]=(z-7)>>8;
    ripa_host_receive(NULL,0,300,p,z);assert(sends==old && ripa_qmi.ul_count==2);
  }
  n=filter_request(p,35);p[n++]=0x14;p[n++]=5;p[n++]=0;p[n++]=1;
  raphael_ipa_put32(p+n,34);n+=4;p[5]=(n-7)&255;p[6]=(n-7)>>8;
  ripa_host_receive(NULL,0,300,p,n);assert(ripa_qmi.ul_count==35 && ripa_qmi.ul_xlat==(1ull<<34));
  old=sends;raphael_ipa_put32(p+n-4,35);ripa_host_receive(NULL,0,300,p,n);assert(sends==old && ripa_qmi.ul_count==35);
  n=filter_request(p,36);old=sends;ripa_host_receive(NULL,0,300,p,n);assert(sends==old && ripa_qmi.ul_count==35);
  n=filter_request(p,1);p[11+4+6]=3;old=sends;ripa_host_receive(NULL,0,300,p,n);assert(sends==old);
  n=filter_request(p,0);ripa_host_receive(NULL,0,300,p,n);assert(ripa_qmi.ul_cached && ripa_qmi.ul_count==0 && last_size==18);
  n=filter_request(p,2);old=sends;ripa_host_receive(NULL,99,300,p,n);assert(sends==old && !ripa_qmi.ul_count);
  host(0x24);assert(last_packet[3]==0x24 && !last_packet[10]);
  ripa_qmi_stop();assert(!ripa_qmi.ul_cached && !ripa_qmi.config_accepted);
}
int main(void) {
  test_native_requests();
  reset();assert(ripa_data_clock_available());ripa.hardware=false;assert(!ripa_data_clock_available());ripa.hardware=true;
  gsi_ready=false;ripa_data_tick();assert(!setups && !ripa_qmi.init_sent && !starts);
  gsi_ready=true;ripa_data_tick();assert(setups==1 && ripa_qmi.init_sent && !starts);
  send_error=1;host(0x20);host(0x35);assert(!ripa_qmi.registered && !ripa_qmi.uc_ready);
  send_error=0;host(0x20);host(0x35);assert(ripa_qmi.registered && ripa_qmi.uc_ready);
  answer(2,0,0);answer(1,0,99);assert(!ripa_qmi.init_accepted);
  answer(1,0,0);assert(ripa_qmi.init_accepted);
  send_error=1;ripa_data_tick();assert(!ripa_qmi.complete_sent && !starts);
  send_error=0;ripa_data_tick();assert(ripa_qmi.complete_sent && starts==1);
  ripa_data_tick();assert(starts==1 && setups==1);
  ripa_client_server(NULL,false,0x31,1,2,0,200);assert(ripa_qmi.data_failed && stops);ripa_data_tick();assert(starts==1);
  reset();ready_signal=true;ripa_data_tick();host(0x20);host(0x35);answer(1,94,0);ripa_data_tick();assert(ripa_qmi.data_failed && !starts);
  reset();uint8_t malformed[]={0,1,0,0x20,0,3,0,1,5,0};ripa_host_receive(NULL,0,300,malformed,sizeof malformed);assert(!ripa_qmi.registered);
  host(0x23);assert(!ripa_qmi.registered && !ripa_qmi.uc_ready);
  puts("PASS actual IPA handshake and native GSI config/UL rule acceptance: exact response IDs, cache ownership, truncations, limit35, XLAT34, malformed atomicity, no invented installed notification");return 0;
}
