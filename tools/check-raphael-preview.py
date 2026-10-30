#!/usr/bin/env python3
"""Run real low-rate preview ABI/control adapter against simulated captures."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnu/files/bsd/dev/hakios.raphael-preview.h').read_text()
abi=(root/'userland/src/hakios.camera.h').read_text()
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#define LCK_GRP_DECLARE(n,s) int n
#define LCK_MTX_DECLARE(n,p) int n
#define PAGE_MASK 16383
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define KERN_SUCCESS 0
#define CAM_HEADER_BYTES 4096
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef uintptr_t vm_offset_t;
typedef char *caddr_t;
typedef long user_ssize_t;
static vm_offset_t keep,frames;static uint64_t keep_bytes;
static bool camdev_made,broken,ios_open;
static unsigned locks,captures,fail,txns,fail_txn;
static unsigned raphael_frame_warmup=8,mock_warmup;
static bool raphael_frame_content_rejected;
static uint8_t regs[65536],fw;
static uint8_t pool[32*1024*1024];
static int kernel_map;
struct cam_header {
 char magic[8];uint32_t width,height,stride,format,bayer,frames,frame_bytes,source;
 char sensor[16];uint32_t last;
};
struct raphael_sensor {bool prepared;};
struct raphael_i2c_bus {int unused;};
struct raphael_capture_clocks {bool dirty;};
struct raphael_phy {bool dirty;};
enum{R586_HOLDON,R586_HOLDOFF};
struct uio {uint8_t *bytes;size_t remaining,offset;bool write,error;};
static int say(const char *f,...){(void)f;return 0;}
static void lck_mtx_lock(int *p){(void)p;assert(!locks++);}
static void lck_mtx_unlock(int *p){(void)p;assert(locks--==1);}
static int kmem_alloc_contig(int map,uintptr_t *p,uint64_t n,unsigned mask,int a,int b,int c,int d){
 (void)map;(void)mask;(void)a;(void)b;(void)c;(void)d;assert(n==sizeof(pool));*p=(uintptr_t)pool;return fail==1?1:0;
}
static const uint8_t *hakios_wifi_file(const char *name,uint64_t *size){
 assert(!strcmp(name,"raphael-imx586.table"));*size=1704;return fail==2?NULL:&fw;
}
static bool raphael_sensor_load(struct raphael_sensor *s,const void *p,uint64_t z){
 (void)s;return p&&z==1704;
}
static void camdev_make(void){camdev_made=fail!=3;}
static bool raphael_sensor_prepare(struct raphael_sensor *s,struct raphael_i2c_bus *b){
 (void)b;s->prepared=true;return fail!=4;
}
static bool raphael_sensor_apply(struct raphael_sensor *s,struct raphael_i2c_bus *b,unsigned stage){
 (void)s;(void)b;assert(stage<2);txns++;return txns!=fail_txn;
}
static bool raphael_sensor_write(struct raphael_i2c_bus *b,uint16_t a,uint8_t v){
 (void)b;assert(a==0x340||a==0x341||a==0x202||a==0x203||a==0x204||a==0x205);
 regs[a]=v;return ++txns!=fail_txn;
}
static bool raphael_sensor_read(struct raphael_i2c_bus *b,uint16_t a,uint8_t *p,unsigned n){
 (void)b;assert(n==2);memcpy(p,regs+a,2);return ++txns!=fail_txn;
}
static bool raphael_capture_clocks_begin(struct raphael_capture_clocks *c){c->dirty=true;return fail!=5;}
static bool raphael_capture_clocks_end(struct raphael_capture_clocks *c){if(fail==10)return false;c->dirty=false;return true;}
static bool raphael_phy_begin(struct raphael_phy *p){p->dirty=true;return fail!=6;}
static bool raphael_phy_end(struct raphael_phy *p){if(fail==9||fail==12)return false;p->dirty=false;return true;}
static bool raphael_sensor_stop(struct raphael_sensor *s,struct raphael_i2c_bus *b){(void)s;(void)b;return fail!=8;}
static bool raphael_frame_capture(struct raphael_sensor *s,struct raphael_i2c_bus *b,bool *retain){
 (void)s;(void)b;(void)retain;assert(locks==1);captures++;
 frames=(uintptr_t)pool;struct cam_header *h=(void *)pool;memset(h,0,4096);
 h->frames=1;h->width=4000;h->height=3000;h->stride=5008;h->frame_bytes=15024000;
 raphael_frame_content_rejected=fail==11||fail==12||fail==13;
 if(raphael_frame_content_rejected){h->frames=0;return false;}
 pool[4096]=(uint8_t)captures;return fail!=7;
}
static bool raphael_camera_rear_probe(bool(*fn)(struct raphael_i2c_bus*,void*),void *p,bool *retain){
 struct raphael_i2c_bus b={0};bool ok=fn(&b,p);if(*retain)assert(!ok);return ok&&fail!=13;
}
static long uio_resid(struct uio *u){return (long)u->remaining;}
static int uiomove(char *p,int n,struct uio *u){
 assert(n>=0&&(size_t)n<=u->remaining);if(u->error)return EFAULT;
 if(u->write)memcpy(p,u->bytes+u->offset,n);else memcpy(u->bytes+u->offset,p,n);
 u->offset+=n;u->remaining-=n;return 0;
}
static uint64_t mach_absolute_time(void){return captures*123456ull;}
static void raphael_camera_map_status(uint64_t sequence){assert(sequence<=captures);}
static uint64_t mock_ms;
static uint64_t ios_ms(void){return mock_ms;}
static bool PE_parse_boot_argn(const char *n,void *p,size_t z){
 assert(z==4);if(!strcmp(n,"cam_warmup")){*(uint32_t *)p=mock_warmup;return mock_warmup!=0;}
 assert(!strcmp(n,"loader_return_after"));*(uint32_t *)p=300;return true;
}
''' + abi + r'''
static int keep_picture(const struct hakios_camera_command *c,struct uio *u){
 (void)u;assert(c->command==HAKIOS_CAMERA_KEEP);return 0;
}
''' + source + r'''
static bool raphael_session_init(void){return true;}
static int raphael_session_read(struct uio *u){(void)u;assert(0);return EIO;}
static int raphael_session_focus_command(uint32_t target){(void)target;return ENOTSUP;}

static void reset(void){
 keep=frames=keep_bytes=0;camdev_made=broken=ios_open=false;
 captures=fail=txns=fail_txn=locks=0;memset(regs,0,sizeof(regs));
 raphael_preview_ready=raphael_preview_fault=false;raphael_preview_sequence=0;
 raphael_preview_cutoff_ms=mock_ms=0;raphael_preview_rejections=0;
 raphael_preview_lines=3016;raphael_preview_gain=2048;raphael_preview_frame=3064;
 raphael_preview_content_drops=mock_warmup=0;raphael_frame_warmup=8;raphael_frame_content_rejected=false;
}
static int command(unsigned cmd,unsigned a,unsigned b,unsigned c){
 struct hakios_camera_command p={.command=cmd,.a=a,.b=b,.c=c};
 struct uio u={.bytes=(void *)&p,.remaining=sizeof(p),.write=true};
 return raphael_camdev_write(&u);
}
int main(void){
 reset();assert(raphael_preview_init());ios_open=true;
 uint8_t output[sizeof(struct hakios_camera_frame)+1];
 for(unsigned i=1;i<=3;i++){
  struct uio u={.bytes=output,.remaining=sizeof(output)};
  assert(!raphael_camdev_read(&u)&&captures==i&&!locks);
  struct hakios_camera_frame f;memcpy(&f,output,sizeof(f));
  assert(!memcmp(f.magic,"RWFRAME1",8)&&f.width==4000&&f.height==3000&&f.stride==5008);
  assert(f.sensor==1&&f.sequence==i&&f.gain_unity==256&&f.gain==2048);
  assert(f.focus==HAKIOS_CAMERA_NO_FOCUS&&output[sizeof(f)]==i);
  assert(regs[0x204]==3&&regs[0x205]==0x80); /* linear8x -> code896 */
 }
 assert(!command(HAKIOS_CAMERA_EXPOSURE,0,0,0));
 assert(raphael_preview_lines==4&&raphael_preview_gain==288&&raphael_preview_frame==3064);
 assert(!command(HAKIOS_CAMERA_EXPOSURE,~0u,~0u,~0u));
 assert(raphael_preview_lines==6064&&raphael_preview_gain==4096&&raphael_preview_frame==6112);
 assert(!command(HAKIOS_CAMERA_SENSOR,1,~0u,0));
 assert(command(HAKIOS_CAMERA_SENSOR,2,~0u,0)==ENODEV);
 assert(command(HAKIOS_CAMERA_FOCUS,1,0,0)==ENOTSUP);
 assert(command(100,0,0,0)==EINVAL);
 for(unsigned warmup=3;warmup<=9;warmup++){
  reset();mock_warmup=warmup;assert(raphael_preview_init());
  assert(raphael_frame_warmup==(warmup>=4&&warmup<=8?warmup:8));
 }
 reset();assert(raphael_preview_init());ios_open=true;fail=11;
 struct uio drop={.bytes=output,.remaining=sizeof(output)};
 for(unsigned i=0;i<3;i++){
  assert(raphael_camdev_read(&drop)==EAGAIN&&!raphael_preview_fault&&!locks);
  assert(drop.remaining==sizeof(output)&&drop.offset==0); /* rejected RAW never copied */
 }
 assert(!command(HAKIOS_CAMERA_EXPOSURE,100,288,3064));
 fail=0;assert(!raphael_camdev_read(&drop)&&!raphael_preview_fault&&raphael_preview_sequence==1);
 for(unsigned f=12;f<=13;f++){
  reset();assert(raphael_preview_init());ios_open=true;fail=f;
  struct uio bad={.bytes=output,.remaining=sizeof(output)};
  assert(raphael_camdev_read(&bad)==EIO&&raphael_preview_fault&&!locks);
 } /* content rejection + failed inner/outer cleanup still latches */
 for(unsigned f=1;f<=3;f++){reset();fail=f;assert(!raphael_preview_init()&&!locks);}
 for(unsigned f=4;f<=10;f++){
  reset();assert(raphael_preview_init());ios_open=true;fail=f;
  struct uio u={.bytes=output,.remaining=sizeof(output)};
  assert(raphael_camdev_read(&u)==EIO&&raphael_preview_fault&&!locks);
  assert(raphael_camdev_read(&u)==EIO); /* fault latches for this boot */
 }
 for(unsigned f=1;f<=11;f++){
  reset();assert(raphael_preview_init());ios_open=true;fail_txn=f;
  struct uio u={.bytes=output,.remaining=sizeof(output)};
  assert(raphael_camdev_read(&u)==EIO&&!locks);
 }
 reset();assert(raphael_preview_init());ios_open=true;
 struct uio u={.bytes=output,.remaining=sizeof(output),.error=true};
 assert(raphael_camdev_read(&u)==EFAULT&&!locks);
 mock_ms=281000;u.error=false;
 assert(raphael_camdev_read(&u)==EIO&&!locks); /* stop accepting before RAM return */
 reset();mock_ms=8000000;assert(raphael_preview_init());ios_open=true;
 assert(raphael_preview_cutoff_ms==8280000);u=(struct uio){.bytes=output,.remaining=sizeof(output)};
 assert(!raphael_camdev_read(&u)); /* hardware counter need not start at zero */
 puts("Actual low-rate preview adapter / simulated fresh captures: ABI, linear-gain conversion, bounds, rear-only, failure latch and lock release PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
 src,exe=Path(td)/'preview.c',Path(td)/'preview'
 src.write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                 '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
