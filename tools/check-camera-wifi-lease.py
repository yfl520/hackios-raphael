#!/usr/bin/env python3
"""Run actual Wi-Fi context scan/map against a live camera lease."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'xnu/files/bsd/dev/hakios.wifi.c').read_text()
find=s[s.index('static bool\nradio_space_find('):s.index('/* What the SMMU has recorded',s.index('static bool\nradio_space_find('))]
mapping=s[s.index('static bool\nradio_map('):s.index('bool\nhakios_wifi_radio_map(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define SMMU_SCR0 0
#define SMMU_IDR0 0x20
#define SMMU_IDR1 0x24
#define SMMU_SMR(n) (0x800+4*(n))
#define SMMU_S2CR(n) (0xc00+4*(n))
#define RADIO_STREAM 0x640u
#define RADIO_STREAM_MASK 1
#define SMMU_CBAR(n) (4*(n))
#define SMMU_CBA2R(n) (0x800+4*(n))
#define SMMU_CB_SCTLR 0
#define SMMU_CB_TTBR0 0x20
#define SMMU_CB_FSR 0x58
#define SMMU_CB_TLBIASID 0x610
#define SMMU_CB_TLBSYNC 0x7f0
#define RADIO_ADDRESSES 0xa0000000ull
#define RADIO_ADDRESSES_SIZE 0x10000000u
static struct {uint64_t base,phys;} apps_smmu,apps_smmu_second,apps_smmu_context;
static struct {unsigned page,pages,match,context,old_smr,old_s2cr,old_sctlr,next,wanted;bool found,on;} radio_space;
static uint32_t mem[0x100000/4];static unsigned step,locked,enters,ons;
static int hakios_smmu_lock;static bool whole=true;
static int quiet(const char *s,...){(void)s;return 0;}
#define printf quiet
static uint32_t rd(uint64_t p){assert(p>=0x15000000&&p<0x15100000);return mem[(p-0x15000000)/4];}
#define get(b,n,o) ((void)(n),rd((b)->phys+(o)))
#define put(b,n,o,v) ((void)(b),(void)(n),(void)(o),(void)(v))
#define tell(...) ((void)0)
static void lck_mtx_lock(int *p){(void)p;assert(!locked++);}
static void lck_mtx_unlock(int *p){(void)p;assert(locked--==1);}
static bool radio_enter(uint64_t a,uint64_t p,uint64_t z,uint64_t attr){(void)a;(void)p;(void)z;(void)attr;assert(locked);enters++;return true;}
static bool radio_space_on(void){assert(locked&&radio_space.context==2&&radio_space.match==2);radio_space.on=true;ons++;return true;}
''' + find + mapping + r'''
int main(void){
 apps_smmu.base=apps_smmu.phys=0x15000000;
 mem[SMMU_IDR0/4]=16;mem[SMMU_IDR1/4]=(6u<<28)|8;
 mem[SMMU_SMR(0)/4]=0x86200840;mem[SMMU_S2CR(0)/4]=1; /* camera owns the old radio place */
 mem[SMMU_SMR(1)/4]=0x80001000;mem[SMMU_S2CR(1)/4]=0; /* another consumer */
 mem[0x81000/4]=0x127;mem[(0x81000+0x20)/4]=0x88888000;
 radio_space.match=0;radio_space.context=1;radio_space.found=true;radio_space.on=false;
 unsigned long long address;
 assert(radio_map(0x90000000,4096,0,&address));
 assert(!locked&&enters==1&&ons==1&&radio_space.context==2&&radio_space.match==2);
 assert(mem[SMMU_SMR(0)/4]==0x86200840&&mem[SMMU_S2CR(0)/4]==1);
 assert(mem[0x81000/4]==0x127&&mem[(0x81000+0x20)/4]==0x88888000);
 /* Once on, normal DMA allocation reuses its own reserved context. */
 assert(radio_map(0x90001000,4096,0,&address)&&enters==2&&ons==1&&!locked);
 puts("Actual Wi-Fi find/map: stale cached context after shutdown re-scanned under lock, live camera SMR/CB excluded, active radio allocations preserved PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    p,e=Path(td)/'lease.c',Path(td)/'lease'
    p.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-parentheses','-fsanitize=address,undefined',str(p),'-o',str(e)],check=True)
    subprocess.run([str(e)],check=True)
