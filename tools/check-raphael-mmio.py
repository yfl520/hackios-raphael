#!/usr/bin/env python3
"""Exercise the actual MMIO cache with repeats, concurrent calls and bounds."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/files/bsd/dev/hakios.raphael-mmio.h').read_text()
prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define PAGE_MASK (((size_t)1 << TEST_PAGE_SHIFT)-1)
#define trunc_page(x) ((x)&~PAGE_MASK)
#define round_page(x) (((x)+PAGE_MASK)&~PAGE_MASK)
#define LCK_GRP_DECLARE(n,s) int n __attribute__((unused))
#define LCK_MTX_DECLARE(n,p) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
static void lck_mtx_lock(pthread_mutex_t *p){assert(!pthread_mutex_lock(p));}
static void lck_mtx_unlock(pthread_mutex_t *p){assert(!pthread_mutex_unlock(p));}
static int say(const char *format,...){(void)format;return 0;}
static unsigned allocations;
static bool map_fail;
static vm_offset_t ml_io_map(vm_offset_t p, vm_size_t n){
    assert(!(p&PAGE_MASK) && n && !(n&PAGE_MASK));
    allocations++;
    return map_fail?0:p+0x100000000ull;
}
'''
suffix = r'''
static const struct {uintptr_t pa;size_t size;} blocks[]={
 {0x0351c000,0x1000},{0x0395a000,0x1000},{0x03961000,0x1000},
 {0x03911000,0x1000},{0x03912000,0x1000},{0x0390d000,0x1000},
 {0x0ad00000,0x10000},{0x00100000,0x10000},{0x0ac65000,0x1000},
 {0x0acc8000,0x1000},{0x0acc4000,0x4000},{0x15000000,0x1000},
 {0x15001000,0x1000},{0x15080000,0x1000}
};
static void *repeat(void *p){
 (void)p;
 for(unsigned frame=0;frame<10000;frame++)
  for(unsigned i=0;i<sizeof(blocks)/sizeof(blocks[0]);i++)
   assert(raphael_camera_map(blocks[i].pa,blocks[i].size)==blocks[i].pa+0x100000000ull);
 return NULL;
}
int main(void){
 assert(!raphael_camera_map(0,0));
 assert(!raphael_camera_map(UINTPTR_MAX,2));
 assert(!raphael_camera_map(1,SIZE_MAX-1));
 assert(!allocations);
 map_fail=true;assert(!raphael_camera_map(0x20000000,0x1000));
 assert(!raphael_mmio_count);
 map_fail=false;
 assert(raphael_camera_map(0x20000003,0x100)==0x120000003ull);
 assert(raphael_camera_map(0x20000004,0x10)==0x120000004ull);
 assert(allocations==2 && raphael_mmio_count==1);
 repeat(NULL);
 unsigned stable=allocations;
 uint64_t bytes=raphael_mmio_bytes;
 pthread_t threads[8];
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,repeat,NULL));
 for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 assert(allocations==stable && bytes==raphael_mmio_bytes);
 raphael_camera_map_status(90000);
 unsigned capacity=sizeof(raphael_mmio)/sizeof(raphael_mmio[0]);
 for(unsigned i=0;raphael_mmio_count<capacity;i++)
  assert(raphael_camera_map(0x40000000ull+i*0x10000ull,0x1000));
 stable=allocations;
 assert(!raphael_camera_map(0x60000000,0x1000));
 assert(allocations==stable);
 puts("MMIO cache: 90,000 simulated frames, 8 concurrent readers, no new maps after warmup; bounds/failure cases pass");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)
    c = path / 'mmio.c'
    c.write_text(prefix + source + suffix)
    for shift in (12, 14):
        exe = path / f'mmio-{shift}'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', f'-DTEST_PAGE_SHIFT={shift}', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
