#!/usr/bin/env python3
"""Walk the actual generated tables independently, including guard mappings."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnu/files/bsd/dev/hakios.raphael-pages.h').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#define bzero(p,n) memset(p,0,n)
''' + source + r'''
static uint64_t storage[34*512] __attribute__((aligned(4096)));
static uint64_t pa[34];
static uint64_t *page(uint64_t address){
    for(unsigned i=0;i<34;i++)if(pa[i]==address)return storage+i*512;
    assert(0);return NULL;
}
static uint64_t translate(uint64_t iova){
    uint64_t e=storage[(iova>>30)&511];
    if(!(e&1))return 0;assert((e&3)==3);
    uint64_t *l2=page(e&0xfffffffff000ull);e=l2[(iova>>21)&511];
    if(!(e&1))return 0;assert((e&3)==3);
    uint64_t *l3=page(e&0xfffffffff000ull);e=l3[(iova>>12)&511];
    if(!(e&1))return 0;assert((e&4095)==0xe43);
    return (e&0xfffffffff000ull)|(iova&4095);
}
int main(void){
    for(unsigned i=0;i<34;i++)pa[i]=0x88000000ull+((33-i)*0x4000ull);
    const uint64_t sizes[]={4096,2ull<<20,(2ull<<20)+4096,15024128,32ull<<20, 3ull*(15024128+16384), 64ull<<20};
    for(unsigned j=0;j<7;j++){
        uint64_t size=(sizes[j]+4095)&~4095ull;
        unsigned count=2+(size+0x1fffff)/0x200000;
        assert(raphael_camera_pages(storage,count*4096,pa,count,0x90000000,size));
        for(uint64_t off=0;off<size;off+=4096)
            assert(translate(RAPHAEL_FRAME_IOVA+off+123)==0x90000000+off+123);
        assert(!translate(RAPHAEL_FRAME_IOVA-4096)&&!translate(RAPHAEL_FRAME_IOVA+size));
        assert(!translate(0)&&!translate(RAPHAEL_FRAME_IOVA+(1ull<<30)));
    }
    uint64_t before[34*512];memset(storage,0xa5,sizeof(storage));memcpy(before,storage,sizeof(storage));
    assert(!raphael_camera_pages(storage,12288,pa,3,0x90000000,0));
    assert(!raphael_camera_pages(storage,12288,pa,3,0x90000001,4096));
    assert(!raphael_camera_pages(storage,12288,pa,3,1ull<<36,4096));
    assert(!raphael_camera_pages(storage,12288,pa,3,(1ull<<36)-4096,8192));
    assert(!raphael_camera_pages(storage,12288,pa,3,0x90000000,4097));
    assert(!raphael_camera_pages(storage,12288,pa,3,0x90000000,(64ull<<20)+4096));
    assert(!raphael_camera_pages(storage,12288,pa,4,0x90000000,4096));
    pa[2]=pa[1];assert(!raphael_camera_pages(storage,12288,pa,3,0x90000000,4096));
    pa[2]=0x90000000;assert(!raphael_camera_pages(storage,12288,pa,3,0x90000000,4096));
    assert(!memcmp(storage,before,sizeof(storage))); /* no partial write on bad input */
    puts("Camera actual 4K tables: independent full walk, all pages, guards, noncontiguous table PAs and bad-input atomicity PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'pages.c',Path(td)/'pages'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
