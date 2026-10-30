#!/usr/bin/env python3
"""Fault-inject actual station_up/down functions; detect leaks and unpaired stop."""
from pathlib import Path
import re
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[1] / 'wifi/lx/src/lx_80211.c').read_text()
functions = '\n'.join(re.search(r'static (?:int|void)\s+%s\(.*?\n\}' % name, src, re.S)[0]
                      for name in ('station_up', 'station_down'))
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define ETH_ALEN 6
#define GFP_KERNEL 0
#define NL80211_IFTYPE_STATION 1
#define SUPPORTS_TX_ENCAP_OFFLOAD 1
#define SUPPORTS_RX_DECAP_OFFLOAD 2
#define IEEE80211_OFFLOAD_ENCAP_ENABLED 1
#define IEEE80211_OFFLOAD_DECAP_ENABLED 2
#define IEEE80211_RECONFIG_TYPE_RESTART 1
#define CONFIG_ATH10K_SNOC 1
#define roundup2(v,a) (((v)+(a)-1)&~((uintptr_t)(a)-1))
#define lx_tell(s) ((void)0)
#define ieee80211_hw_check(h,f) (true)
static unsigned allocations, starts, stops, adds, removes;
static int start_error, add_error;
static bool fail_alloc, locked;
static bool firmware_ready = true;
static bool lx_os_radio_ready(void) { return firmware_ready; }
static unsigned complete_recovery, forgets;
static int forget_error;
struct ieee80211_vif { int type; unsigned offload_flags; unsigned char addr[6];
  struct conf { unsigned char addr[6]; struct ieee80211_vif *vif; const void *bssid; } bss_conf;
  struct conf *link_conf[1]; };
struct wiphy { int mtx; unsigned char perm_addr[6]; };
struct ieee80211_hw { struct wiphy *wiphy; unsigned vif_data_size; };
static void lx_ath10k_restart_cancel(struct ieee80211_hw *h) { (void)h; }
struct ops { int (*start)(struct ieee80211_hw *); int (*add_interface)(struct ieee80211_hw *, struct ieee80211_vif *);
  void (*stop)(struct ieee80211_hw *, bool); void (*remove_interface)(struct ieee80211_hw *, struct ieee80211_vif *);
  void (*reconfig_complete)(struct ieee80211_hw *,int); };
struct lx_radio { struct ieee80211_hw hw; struct ieee80211_vif *vif; void *vif_memory;
  bool started,restarting,traffic_paused; unsigned restart_generation; struct ops *ops; };
static bool radio_restarting(struct lx_radio *r) { return r->restarting; }
static void radio_data_quiesce(struct lx_radio *r) { r->traffic_paused=true; }
static void lx_os_lock(void) {}
static void lx_os_unlock(void) {}
static int station_restart_forget(struct lx_radio *r) { (void)r;forgets++;return forget_error; }
static const unsigned char no_network[6];
static bool is_zero_ether_addr(const unsigned char *p) { return !memcmp(p,no_network,6); }
static void *kzalloc(size_t n,int f) { (void)f; if(fail_alloc)return NULL; void *p=calloc(1,n); assert(p); allocations++;return p; }
static void kfree(void *p) { if(p) { assert(allocations);allocations--; free(p); } }
static void mutex_lock(int *p) { (void)p;assert(!locked);locked=true; }
static void mutex_unlock(int *p) { (void)p;assert(locked);locked=false; }
static int start(struct ieee80211_hw *h) { (void)h;assert(locked);starts++;return start_error; }
static int add(struct ieee80211_hw *h,struct ieee80211_vif *v) { (void)h;assert(locked&&v);adds++;return add_error; }
static void stop(struct ieee80211_hw *h,bool suspend) { (void)h;assert(locked&&!suspend);stops++; }
static void remove_vif(struct ieee80211_hw *h,struct ieee80211_vif *v) { (void)h;assert(locked&&v);removes++; }
static void reconfigured(struct ieee80211_hw *h,int why) { (void)h;assert(why==1);complete_recovery++; }
''' + functions + r'''
int main(void) {
  struct wiphy w={.perm_addr={2,1,2,3,4,5}};
  struct ops ops={start,add,stop,remove_vif,reconfigured};
  struct lx_radio r={.hw={&w,128},.ops=&ops};
  firmware_ready=false;
  for(unsigned i=0;i<20;i++) assert(station_up(&r)==EAGAIN&&!starts&&!allocations&&!forgets);
  firmware_ready=true;
  fail_alloc=true;assert(station_up(&r)==ENOMEM&&!starts);fail_alloc=false;
  start_error=-ENODEV;
  for(unsigned i=0;i<20;i++) {assert(station_up(&r)==ENODEV);assert(!allocations&&!r.started&&!r.vif_memory);station_down(&r);}
  assert(!adds&&!stops&&!removes);
  start_error=0;add_error=-EIO;
  for(unsigned i=0;i<20;i++) {assert(station_up(&r)==EIO);assert(!allocations&&!r.started&&!r.vif_memory);station_down(&r);}
  assert(stops==20&&!removes);
  add_error=0;assert(!station_up(&r)&&r.started&&r.vif&&allocations==1);
  unsigned before=starts;assert(!station_up(&r)&&starts==before);
  firmware_ready=false;
  void *saved_vif=r.vif_memory;
  assert(station_up(&r)==EAGAIN&&starts==before&&r.vif_memory==saved_vif&&allocations==1);
  firmware_ready=true;
  station_down(&r);station_down(&r);
  assert(stops==21&&removes==1&&!allocations&&!r.vif&&!r.vif_memory&&!r.started);
  assert(!station_up(&r));station_down(&r);
  assert(stops==22&&removes==2&&!allocations);
  r.restarting=true;r.restart_generation=1;forget_error=EBUSY;
  firmware_ready=false;
  assert(station_up(&r)==EAGAIN&&!forgets&&r.restarting);
  firmware_ready=true;
  before=starts;assert(station_up(&r)==EBUSY&&starts==before&&r.restarting);
  forget_error=0;assert(!station_up(&r)&&!r.restarting&&!r.traffic_paused&&complete_recovery==1);
  station_down(&r);assert(!allocations&&r.traffic_paused);
  r.restarting=true;start_error=-ENODEV;
  assert(station_up(&r)==ENODEV&&r.restarting&&!allocations&&complete_recovery==1);
  start_error=0;assert(!station_up(&r)&&complete_recovery==2);station_down(&r);
  puts("PASS: station allocation/start/add failure, repeated retry, success/down and double down");
}
'''
with tempfile.TemporaryDirectory(prefix='station-lifecycle-') as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,stdout=subprocess.PIPE)
    print('PASS: actual station functions defer FW_DOWN without allocating or starting, resume after FW_READY, survive allocation/start/add failures and repeated retry/down (ASan/UBSan)')
