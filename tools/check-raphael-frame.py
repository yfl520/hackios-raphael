#!/usr/bin/env python3
"""Actual bounded frame orchestration plus reused HackiOS path helpers."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
camera=(root/'xnu/files/bsd/dev/hakios.camera.c').read_text()
defines=camera[camera.index('#define CSID_PHYS'):camera.index('static vm_offset_t csid, ife, csiphy;')]
head=camera[camera.index('struct cam_header {'):camera.index('extern uint64_t loader_recording')]
config=camera[camera.index('static bool\ncsid_config('):camera.index('static bool\ntpg_start(')]
ife=camera[camera.index('static bool\nife_config('):camera.index('/* The camera\'s blocks up',camera.index('static bool\nife_config('))]
pages=(root/'xnu/files/bsd/dev/hakios.raphael-pages.h').read_text()
frame=(root/'xnu/files/bsd/dev/hakios.raphael-frame.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;typedef size_t vm_size_t;
static int say(const char *f,...){(void)f;return 0;}
#define bzero(p,n) memset(p,0,n)
#define PAGE_SIZE 16384
#define PAGE_MASK 16383
#define round_page(n) (((n)+16383)&~(size_t)16383)
#define CAM_HEADER_BYTES 4096
#define FRAME_ADDRESSES 0x40000000ull
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define KERN_SUCCESS 0
static int kernel_map,hakios_smmu_lock;
static uintptr_t frames,allocation;
static uint64_t frames_bytes;
static uint8_t buffer[32*1024*1024] __attribute__((aligned(16384)));
static uint32_t cmem[1024],imem[4096],gmem[16384],mmu_fault;
static uint32_t original_c[1024],original_i[4096],original_g[16384];
static vm_offset_t csid,ife;
static bool csid_running,ife_running,tpg_running,broken,streaming,locked;
static unsigned writes,fail_at,fault,sensor_on,freed,paints;
static bool hakios_is_raphael(void){return true;}
static uintptr_t raphael_camera_map(uintptr_t p,unsigned z){
    assert((p==0x100000&&z==0x10000)||(p==0xacc8000&&z==0x1000)||(p==0xacc4000&&z==0x4000));return p;
}
static uint32_t *ptr(uintptr_t p){
    if(p==0x20000058)return &mmu_fault;
    if(p>=0x100000&&p<0x110000)return &gmem[(p-0x100000)/4];
    if(p>=0xacc8000&&p<0xacc9000)return &cmem[(p-0xacc8000)/4];
    if(p>=0xacc4000&&p<0xacc8000)return &imem[(p-0xacc4000)/4];
    assert(0);return NULL;
}
static uint32_t rd(uintptr_t p){assert(!(p&3));return *ptr(p);}
static void wr(uintptr_t p,uint32_t v){
    assert(p!=0xacc4000+0x211c); /* unsupported legacy BUS_TEST_CTRL excluded */
    assert(p!=0xacc4000+0x18); /* no global reset of another path */
    if(++writes==fail_at)return;
    *ptr(p)=v;
    if(p==0x10b030||p==0x10b034)*ptr(p)=(v&~0x80000000u)|((v&1)?0:0x80000000u);
    if(p==0xacc4000+0x2054)imem[0x2060/4]&=~v;
    if(p==0xacc8000+0x208&&(v&3)==2&&fault!=9)cmem[0x250/4]=1;
    if(p==0xacc8000+0x208&&(v&3)==1)cmem[0x250/4]=0;
}
static void spin_us(unsigned us){
    assert(us<=100000);
    if(streaming&&(imem[0x2208/4]&1)&&fault!=3&&!(imem[0x2060/4]&1)){
        assert(locked&&(gmem[0xb030/4]&1)&&(gmem[0xb034/4]&1));
        bool first=paints++==0;
        uint8_t *raw=buffer+4096;
        if(first&&fault!=4&&fault!=5)for(unsigned y=0;y<3000;y++)for(unsigned x=0;x<5000;x++)
            raw[y*5008+x]=(uint8_t)(x+y);
        if(fault==5)memset(raw,0,15024000);
        if(first&&fault==13)for(unsigned y=0;y<3000;y++)memset(raw+y*5008,255,5000);
        if(first&&fault==14)for(unsigned y=0;y<3000;y++)memset(raw+y*5008,40,5000);
        if(first&&fault==15)for(unsigned y=1500;y<3000;y++)memset(raw+y*5008,0xa5,5000);
        if(first&&fault==16)memset(raw+2999*5008,0xa5,5000);
        imem[0x2060/4]|=1;
        cmem[0x2e0/4]=cmem[0x2e4/4]=15000000;
        if(fault==17)cmem[0x2e4/4]=14995000;
        if(fault==6)mmu_fault=2;
        if(fault==7)cmem[0x168/4]++;
    }
}
static size_t strlcpy(char *a,const char *b,size_t n){size_t z=strlen(b);if(n){size_t m=z<n-1?z:n-1;memcpy(a,b,m);a[m]=0;}return z;}
static void give_up(const char *s){(void)s;broken=true;}
static bool blk_put(uintptr_t base,const char *name,uint32_t off,uint32_t v,bool check){
    (void)name;wr(base+off,v);return !check||rd(base+off)==v;
}
static bool wait_reg(uintptr_t base,uint32_t off,uint32_t mask,uint32_t want,uint32_t us){
    for(unsigned i=0;i<=us;i+=10){if((rd(base+off)&mask)==want)return true;spin_us(10);}return false;
}
static bool raphael_clock_wait(uintptr_t at,uint32_t mask,uint32_t expected){
    return wait_reg(at,0,mask,expected,10000);
}
static void lck_mtx_lock(int *p){(void)p;assert(!locked);locked=true;}
static void lck_mtx_unlock(int *p){(void)p;assert(locked);locked=false;}
static int kmem_alloc(int map,uintptr_t *p,size_t bytes,int a,int b){
    (void)map;(void)a;(void)b;if(fault==1)return 1;
    assert(!allocation);allocation=(uintptr_t)aligned_alloc(16384,bytes);assert(allocation);
    memset((void *)allocation,0,bytes);*p=allocation;return 0;
}
static void kmem_free(int map,uintptr_t p,size_t n){
    (void)map;(void)n;assert(!locked&&p==allocation);free((void *)p);allocation=0;freed++;
}
static uint64_t kvtophys(uintptr_t p){
    if(p>=frames&&p<frames+frames_bytes)return 0x90000000+(p-frames);
    assert(p>=allocation&&p<allocation+0x20000);return 0xa0000000+(p-allocation);
}
struct raphael_sensor {bool stream_dirty;};
struct raphael_i2c_bus {int unused;};
struct raphael_smmu {uintptr_t cb;bool ready;};
static bool raphael_smmu_begin(struct raphael_smmu *s,uint64_t pa){
    assert(locked&&pa==0xa0000000);s->cb=0x20000000;s->ready=true;return fault!=2;
}
static bool raphael_smmu_owned(const struct raphael_smmu *s){return s->ready&&fault!=18;}
static bool raphael_smmu_end(struct raphael_smmu *s){
    assert(locked&&!(imem[0x2208/4]&1)&&!(cmem[0x200/4]&0x80000000u));
    if(fault==10)return false;s->ready=false;return true;
}
static bool raphael_sensor_start(struct raphael_sensor *s,struct raphael_i2c_bus *b){
    (void)b;sensor_on++;s->stream_dirty=true;if(fault==11)return false;streaming=true;return true;
}
static bool raphael_sensor_stop(struct raphael_sensor *s,struct raphael_i2c_bus *b){
    (void)b;if(fault==8&&s->stream_dirty)return false;streaming=false;s->stream_dirty=false;return true;
}
static bool raphael_sensor_read(struct raphael_i2c_bus *b,uint16_t a,uint8_t *p,unsigned n){
    (void)b;assert(n==2&&(a==0x202||a==0x204||a==0x340));p[0]=0;p[1]=1;return true;
}
bool hakios_raphael_camera_data_ready(void){return fault!=12;}
''' + defines + head + r'''
static bool frames_alloc(uint32_t w,uint32_t h,uint32_t stride,uint32_t source){
    assert(w==4000&&h==3000&&stride==5008&&source==1);
    frames=(uintptr_t)buffer;frames_bytes=round_page(4096+2ull*stride*h);
    memset(buffer,0,frames_bytes);struct cam_header *p=(void *)buffer;
    p->width=w;p->height=h;p->stride=stride;p->frame_bytes=stride*h;
    return true;
}
''' + config + ife + pages + frame + r'''
static void reset(void){
    if(allocation)free((void *)allocation);allocation=0;
    memset(cmem,0,sizeof(cmem));memset(imem,0,sizeof(imem));memset(gmem,0,sizeof(gmem));
    cmem[0x100/4]=0x32103;cmem[0x104/4]=1;cmem[0x600/4]=0xa06406;cmem[0x250/4]=1;
    imem[0x5c/4]=0x80000000;imem[0x2044/4]=1; /* handset reset-done mask defaults */
    gmem[0xb030/4]=gmem[0xb034/4]=0x80000000u;
    memcpy(original_c,cmem,sizeof(cmem));memcpy(original_i,imem,sizeof(imem));memcpy(original_g,gmem,sizeof(gmem));
    csid_running=ife_running=tpg_running=broken=streaming=locked=false;
    writes=fail_at=fault=sensor_on=freed=paints=mmu_fault=0;
    raphael_frame_warmup=8;raphael_frame_content_rejected=false;
}
static void check_clean(void){
    assert(!locked&&!streaming&&!allocation);
    for(unsigned i=0;i<sizeof(raphael_frame_registers)/sizeof(raphael_frame_registers[0]);i++){
        unsigned o=raphael_frame_registers[i].offset/4;
        assert((raphael_frame_registers[i].vfe?imem[o]:cmem[o])==
               (raphael_frame_registers[i].vfe?original_i[o]:original_c[o]));
    }
    assert(gmem[0xb030/4]==original_g[0xb030/4]&&gmem[0xb034/4]==original_g[0xb034/4]);
}
int main(void){
    struct raphael_sensor s={0};struct raphael_i2c_bus b={0};bool retain=false;
    reset();assert(raphael_frame_capture(&s,&b,&retain)&&!retain);
    check_clean();assert(((struct cam_header *)buffer)->frames==1);
    unsigned n=writes;
    for(unsigned warmup=4;warmup<=8;warmup++){
        reset();s=(struct raphael_sensor){0};retain=false;raphael_frame_warmup=warmup;
        assert(raphael_frame_capture(&s,&b,&retain)&&!retain&&paints==warmup);
        check_clean();
    }
    for(unsigned f=13;f<=17;f++){
        reset();s=(struct raphael_sensor){0};retain=false;fault=f;
        bool ok=raphael_frame_capture(&s,&b,&retain);
        assert(ok==(f==13||f==14)); /* saturated/flat scenes are not DMA faults */
        assert(raphael_frame_content_rejected==(f==15||f==16));
        assert(((struct cam_header *)buffer)->frames==(unsigned)ok&&!retain);
        check_clean();
    }
    for(unsigned f=1;f<=12;f++){
        reset();s=(struct raphael_sensor){0};retain=false;fault=f;
        bool result=raphael_frame_capture(&s,&b,&retain);
        if(result)fprintf(stderr,"unexpected fault acceptance %u\n",f);
        assert(!result);
        assert(((struct cam_header *)buffer)->frames==0);
        if(f==8||f==9||f==10)assert(retain);
        else {assert(!retain);check_clean();}
    }
    /* Partial setup/restore failures must never publish a frame when retained. */
    for(unsigned f=1;f<=n;f++){
        reset();s=(struct raphael_sensor){0};retain=false;fail_at=f;
        bool ok=raphael_frame_capture(&s,&b,&retain);
        if(retain)assert(!ok&&((struct cam_header *)buffer)->frames==0);
        else check_clean();
    }
    /* The persistent path yields the allocator mutex with its route live,
     * reacquires before teardown, and retains memory on ownership loss. */
    reset();assert(frames_alloc(4000,3000,5008,1));
    struct raphael_frame_engine e={0};
    assert(raphael_frame_engine_begin(&e,FRAME_ADDRESSES+4096,15024000)&&locked);
    assert(raphael_frame_engine_share(&e)&&!locked&&allocation);
    assert(raphael_frame_engine_end(&e,true));check_clean();
    reset();e=(struct raphael_frame_engine){0};
    assert(raphael_frame_engine_begin(&e,FRAME_ADDRESSES+4096,15024000));
    assert(raphael_frame_engine_share(&e));fault=18;unsigned prior=writes;
    assert(!raphael_frame_engine_end(&e,true)&&!locked&&allocation&&writes==prior);
    reset();
    puts("Actual frame orchestration + reused HackiOS RDI/IFE helpers: partial writes, stop/rollback retention, content rejection and export gating with simulated DMA PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'frame.c',Path(td)/'frame'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
