#!/usr/bin/env python3
"""Run actual session orchestration/read ABI with deterministic CSI/DMA events.

Hardware helpers are mocked here; their actual implementations have separate
fault-injection tests. This does not certify real images or frame rate.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
dev=root/'xnu/files/bsd/dev'
source=(dev/'hakios.raphael-session.h').read_text().replace('#include "hakios.raphael-queue.h"',(dev/'hakios.raphael-queue.h').read_text())
source=source.replace('#include "hakios.raphael-content.h"',(dev/'hakios.raphael-content.h').read_text())
abi=(root/'userland/src/hakios.camera.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#define __unused __attribute__((unused))
#define bzero(p,n) memset(p,0,n)
#define PAGE_SIZE 16384
#define PAGE_MASK 16383
#define round_page(n) (((n)+16383ull)&~16383ull)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define KERN_SUCCESS 0
#define FRAME_ADDRESSES 0x40000000ull
#define RAPHAEL_MAX_FRAME_BYTES (64ull<<20)
#define NSEC_PER_MSEC 1000000
#define PZERO 0
#define PCATCH 0
#define FRAME_TIMEOUT_MS 1500
#define RDI0_SOF (1u<<27)
#define WM0_DONE 1u
#define IFE_IRQ_STATUS0 0x60
#define IFE_IRQ_CLEAR0 0x68
#define IFE_IRQ_GLOBAL_CLEAR 0x58
#define BUS_IRQ_STATUS1 0x2060
#define BUS_IRQ_CLEAR1 0x2054
#define BUS_IRQ_GLOBAL_CLEAR 0x2058
#define IFE_VIOLATION_STATUS 0x70
#define WM0_IMAGE_ADDR 0x2214
#define IFE_REG_UPDATE 0x4ac
#define CSID_CSI2_RX_CRC 0x168
#define CSID_RDI0_BYTES_PING 0x2e0
#define CSID_RDI0_BYTES_PONG 0x2e4
typedef uintptr_t vm_offset_t;typedef size_t vm_size_t;typedef void *thread_t;typedef int wait_result_t;
typedef char *caddr_t;
static int kernel_map,raphael_preview_lock=1;
static bool raphael_session_enabled,raphael_session_pending;
static bool raphael_preview_fault,broken,ios_open;
static uint64_t raphael_preview_sequence,raphael_preview_cutoff_ms;
static uint32_t raphael_preview_lines=3016,raphael_preview_gain=2048,raphael_preview_frame=3064;
static unsigned raphael_frame_warmup=4;
static uintptr_t ife=0x10000,csid=0x20000;
static uint32_t imem[0x4000/4],cmem[0x1000/4],sensor_regs[65536];
static unsigned fail,locks,stop_calls,end_calls,phy_ends,clock_ends,reads,moves;
static unsigned tick,events,reader_age;
static int held_reader=-1;
static bool sensor_live;
static uint64_t held_sequence,now;
static int say(const char *s,...){(void)s;return 0;}
static void lck_mtx_lock(int *p){assert(*p==1&&!locks);locks++;}
static void lck_mtx_unlock(int *p){assert(*p==1&&locks==1);locks--;}
static void wakeup(void *p){(void)p;}
static uint64_t ios_ms(void){return now;}
static uint64_t mach_absolute_time(void){return now*1000000;}
static uint32_t rd(uintptr_t p){
 if(p>=ife&&p<ife+0x4000)return imem[(p-ife)/4];
 if(p>=csid&&p<csid+0x1000)return cmem[(p-csid)/4];
 assert(p==0x30058);return fail==8?2:0;
}
static void wr(uintptr_t p,uint32_t v){
 if(p>=ife&&p<ife+0x4000){
  imem[(p-ife)/4]=v;
  if(p==ife+IFE_IRQ_CLEAR0)imem[IFE_IRQ_STATUS0/4]&=~v;
  if(p==ife+BUS_IRQ_CLEAR1)imem[BUS_IRQ_STATUS1/4]&=~v;
  return;
 }
 assert(p>=0x40000&&p<0x41000); /* emergency reset GPIO */
}
static unsigned raphael_actuator_mode,raphael_actuator_fixed;
struct raphael_sensor{bool prepared,stream_dirty;};struct raphael_i2c_bus{int unused;};
struct raphael_actuator_state{bool active;unsigned dac;};
static bool raphael_actuator_begin(struct raphael_actuator_state *s,struct raphael_i2c_bus *b){(void)b;s->active=raphael_actuator_mode==3;s->dac=180;return true;}
static bool raphael_actuator_end(struct raphael_actuator_state *s,struct raphael_i2c_bus *b){(void)s;(void)b;return true;}
static bool raphael_actuator_move(struct raphael_actuator_state *s,struct raphael_i2c_bus *b,unsigned target){(void)b;assert(!locks&&target>=180&&target<=579);s->dac=target;return true;}
struct raphael_frame_engine{struct {uintptr_t cb;} mmu;bool path_dirty,locked;};
struct raphael_capture_clocks{bool dirty;};struct raphael_phy{bool dirty;};
enum{R586_HOLDON,R586_HOLDOFF};
static bool raphael_sensor_prepare(struct raphael_sensor *s,struct raphael_i2c_bus *b){(void)b;s->prepared=true;return fail!=1;}
static bool raphael_sensor_apply(struct raphael_sensor *s,struct raphael_i2c_bus *b,unsigned n){(void)s;(void)b;(void)n;return fail!=2;}
static bool raphael_sensor_write(struct raphael_i2c_bus *b,uint16_t a,uint8_t v){(void)b;sensor_regs[a]=v;return true;}
static bool raphael_sensor_read(struct raphael_i2c_bus *b,uint16_t a,uint8_t *v,unsigned n){(void)b;assert(n==2);v[0]=sensor_regs[a];v[1]=sensor_regs[a+1];return fail!=3;}
static bool raphael_sensor_start(struct raphael_sensor *s,struct raphael_i2c_bus *b){(void)b;s->stream_dirty=true;sensor_live=fail!=7;return sensor_live;}
static bool raphael_sensor_stop(struct raphael_sensor *s,struct raphael_i2c_bus *b){(void)b;stop_calls++;if(fail==9)return false;s->stream_dirty=false;sensor_live=false;return true;}
static bool raphael_capture_clocks_begin(struct raphael_capture_clocks *c){c->dirty=true;return fail!=4;}
static bool raphael_phy_begin(struct raphael_phy *p){p->dirty=true;return fail!=5;}
static bool raphael_frame_engine_map(struct raphael_frame_engine *e,uintptr_t p,uint64_t z,uint32_t a,uint32_t bytes){
 assert(p&&z>45000000&&z<50000000&&a==FRAME_ADDRESSES&&bytes==15024000);e->path_dirty=true;e->locked=true;e->mmu.cb=0x30000;return fail!=6;
}
static bool raphael_frame_engine_share(struct raphael_frame_engine *e){assert(e->locked);e->locked=false;return true;}
static bool raphael_frame_engine_end(struct raphael_frame_engine *e,bool stopped){
 end_calls++;assert(!sensor_live||!stopped);if(!stopped||fail==10)return false;e->path_dirty=false;e->locked=false;return true;
}
static bool raphael_phy_end(struct raphael_phy *p){phy_ends++;if(fail==11)return false;p->dirty=false;return true;}
static bool raphael_capture_clocks_end(struct raphael_capture_clocks *c){clock_ends++;if(fail==12)return false;c->dirty=false;return true;}
static void raphael_frame_cache(uintptr_t p,uint64_t z,unsigned op){(void)p;(void)z;assert(op<=2);}
static void raphael_camera_map_status(uint64_t seq){assert(seq);}
static void spin_us(unsigned n){(void)n;}
static void paths_down(void){sensor_live=false;}
static void wait_ms(unsigned ms);
static int msleep(void *p,int *m,int pri,const char *s,const struct timespec *ts);
static bool raphael_camera_rear_probe(bool(*fn)(struct raphael_i2c_bus*,void*),void *o,bool *retain){struct raphael_i2c_bus b={0};(void)retain;return fn(&b,o);}
static bool raphael_sensor_load(struct raphael_sensor *s,const void *p,uint64_t z){(void)s;return p&&z;}
static const uint8_t *hakios_wifi_file(const char *n,uint64_t *z){static uint8_t v;(void)n;*z=1;return &v;}
static bool PE_parse_boot_argn(const char *n,void *p,size_t z){(void)n;(void)p;(void)z;return false;}
static int kmem_alloc_contig(int m,uintptr_t *p,size_t z,unsigned mask,int a,int b,int c,int d){(void)m;(void)mask;(void)a;(void)b;(void)c;(void)d;*p=(uintptr_t)aligned_alloc(16384,z);return *p?0:1;}
static uintptr_t raphael_camera_map(uintptr_t p,unsigned z){(void)p;(void)z;return 0x40000;}
static int kernel_thread_start(void(*fn)(void*,int),void *p,thread_t *t){(void)fn;(void)p;*t=(void *)1;return 0;}
static void thread_deallocate(thread_t t){(void)t;}
static void thread_terminate(thread_t t){(void)t;assert(0);}
static thread_t current_thread(void){return (void *)1;}
struct uio{uint8_t *data;size_t left,at;bool fault;};
static size_t uio_resid(struct uio *u){return u->left;}
static int uiomove(char *p,int n,struct uio *u){
 assert(n>=0&&(unsigned)n<=u->left);moves++;
 if(u->fault)return EFAULT;
 if(n>1024){assert(!locks);reads++;}
 memcpy(u->data+u->at,p,(unsigned)n);u->at+=(unsigned)n;u->left-=(unsigned)n;return 0;
}
''' + abi + source + r'''
static void wait_ms(unsigned ms){
 assert(ms==1&&!locks);now++;tick++;
 if(!sensor_live)return;
 /* Hardware never writes a slot reserved by a consumer, even when that
  * consumer spans several SOFs. Address used by DMA was set at prior SOF. */
 if(held_reader>=0){
  assert(raphael_session_queue.slot[held_reader].sequence==held_sequence);
  if(++reader_age>100){raphael_queue_release(&raphael_session_queue,held_reader);held_reader=-1;}
 }
 if(raphael_actuator_mode==3){
  for(unsigned i=0;i<RAPHAEL_SLOTS;i++){
   struct raphael_queue_slot *p=&raphael_session_queue.slot[i];
   if(p->state==RQ_READY||p->state==RQ_READING){
    assert(raphael_session_slot_focus[i]==(p->sequence<12?180u:360u));
   }
  }
 }
 if(tick%33==1){
  unsigned target=imem[WM0_IMAGE_ADDR/4]?(imem[WM0_IMAGE_ADDR/4]-FRAME_ADDRESSES)/RAPHAEL_SLOT_BYTES:0;
  assert(target<RAPHAEL_SLOTS&&(int)target!=held_reader);
  assert(raphael_session_queue.slot[target].state==RQ_QUEUED);
  imem[IFE_IRQ_STATUS0/4]|=RDI0_SOF;
 }
 if(tick%33==31&&fail!=13){
  imem[BUS_IRQ_STATUS1/4]|=WM0_DONE;
  cmem[CSID_RDI0_BYTES_PING/4]=cmem[CSID_RDI0_BYTES_PONG/4]=15000000;
  if(fail==14)cmem[CSID_RDI0_BYTES_PONG/4]=42;
  events++;
 }
 if(held_reader<0){
  held_reader=raphael_queue_take(&raphael_session_queue);
  if(held_reader>=0){held_sequence=raphael_session_queue.slot[held_reader].sequence;reader_age=0;}
 }
 if(tick==200){raphael_preview_gain=1024;raphael_preview_lines=1000;raphael_session_pending=true;}
 if(tick==300&&raphael_actuator_mode==3){
  lck_mtx_lock(&raphael_preview_lock);
  assert(!raphael_session_focus_command(360));
  assert(raphael_session_focus_command(580)==EINVAL);
  lck_mtx_unlock(&raphael_preview_lock);
 }
 if(fail!=13&&events>=60)ios_open=false;
}
static int msleep(void *p,int *m,int pri,const char *s,const struct timespec *ts){
 (void)p;(void)pri;(void)s;(void)ts;assert(*m==1&&locks==1);now+=100;
 if(raphael_session_exit)raphael_session_active=false;
 return EWOULDBLOCK;
}
static void reset(void){
 fail=locks=stop_calls=end_calls=phy_ends=clock_ends=reads=moves=0;
 tick=events=reader_age=0;now=0;held_reader=-1;held_sequence=0;
 memset(imem,0,sizeof(imem));memset(cmem,0,sizeof(cmem));memset(sensor_regs,0,sizeof(sensor_regs));
 bzero(&raphael_session_hw,sizeof(raphael_session_hw));
 raphael_preview_fault=broken=sensor_live=false;ios_open=true;
 raphael_session_exit=raphael_session_wanted=raphael_session_reading=raphael_session_copying=false;
 raphael_session_streaming=false;raphael_session_active=true;
 raphael_actuator_mode=0;
 for(unsigned i=0;i<RAPHAEL_SLOTS;i++)raphael_session_slot_focus[i]=HAKIOS_CAMERA_NO_FOCUS;
 raphael_session_last_read=0;raphael_preview_cutoff_ms=raphael_preview_sequence=0;
 raphael_preview_lines=3016;raphael_preview_gain=2048;raphael_preview_frame=3064;
}
int main(void){
 raphael_session_bytes=RAPHAEL_SLOTS*RAPHAEL_SLOT_BYTES;
 raphael_session_pool=(uintptr_t)aligned_alloc(16384,raphael_session_bytes);assert(raphael_session_pool);
 struct raphael_i2c_bus b={0};
 reset();assert(raphael_session_run(&b,NULL)&&!locks);
 assert(events==60&&raphael_session_queue.completed>=59&&stop_calls==1&&end_calls==1);
 assert(phy_ends==1&&clock_ends==1&&!raphael_session_hw.retain);
 assert(sensor_regs[0x204]==3&&sensor_regs[0x205]==0); /* requested 4x linear gain */
 reset();raphael_actuator_mode=3;raphael_session_enabled=true;
 assert(raphael_session_run(&b,NULL)&&raphael_session_hw.actuator.dac==360);
 assert(!raphael_session_focus_ready&&!raphael_session_focus_pending);
 assert(raphael_session_focus_command(180)==EAGAIN);
 for(unsigned f=1;f<=14;f++){
  reset();fail=f;assert(!raphael_session_run(&b,NULL)&&!locks&&stop_calls==1&&end_calls==1);
  assert(raphael_session_hw.retain==(f>=9&&f<=12));
 }
 reset();raphael_preview_cutoff_ms=500;
 assert(raphael_session_run(&b,NULL)&&now==500&&stop_calls==1);
 /* Actual frame read ABI, complete content guard, copy failure and closure. */
 uint8_t *output=malloc(sizeof(struct hakios_camera_frame)+15024000);assert(output);
 for(unsigned test=0;test<6;test++){
  reset();raphael_queue_init(&raphael_session_queue);raphael_session_streaming=true;
  raphael_session_queue.slot[0]=(struct raphael_queue_slot){.state=RQ_READY,.sequence=8,.time=123,.lines=3016,.gain=2048,.length=3064};
  raphael_session_queue.writing=raphael_session_queue.queued=1;
  raphael_session_queue.slot[1].state=RQ_QUEUED;
  memset((void *)raphael_session_pool,test==1?0:test==2?0xa5:255,RAPHAEL_RAW_BYTES);
  if(test==3)memset((void *)(raphael_session_pool+2999*5008),0xa5,5000);
  if(test==5)ios_open=false;
  struct uio u={.data=output,.left=sizeof(struct hakios_camera_frame)+15024000,.fault=test==4};
  int e=raphael_session_read(&u);
  assert(e==(test==0?0:test<=3?EAGAIN:test==4?EFAULT:EIO));
  assert(!locks&&!raphael_session_reading&&!raphael_session_copying);
  assert(raphael_session_queue.slot[0].state!=RQ_READING);
  if(test==0){
   struct hakios_camera_frame h;memcpy(&h,output,sizeof(h));
   assert(!memcmp(h.magic,"RWFRAME1",8)&&h.width==4000&&h.height==3000&&h.gain==2048&&h.sensor==1);
   assert(h.focus==HAKIOS_CAMERA_NO_FOCUS&&u.left==0&&reads==1);
  }else assert(!reads);
 }
 reset();raphael_session_enabled=true;hakios_camera_stop();
 assert(raphael_session_exit&&!raphael_session_active&&!locks);
 free(output);free((void *)raphael_session_pool);
 puts("Actual rear session: 60-frame SOF/DMA cycle, slow reader exclusion, controls, 14 start/stream/cleanup faults, cutoff, RAW read ABI/content checks and orderly drain PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'session.c',Path(td)/'session'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
