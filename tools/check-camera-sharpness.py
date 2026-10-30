#!/usr/bin/env python3
"""Compare optimized green-only AF statistics exactly against the frozen v13 code."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
old=(root/'tools/fixtures/camera-sharpness-v13.c').read_text()
new=(root/'userland/src/camera_isp.c').read_text()
new=new[new.index('/* where red and blue'):new.index('static inline void lsc_at')]
def names(text,prefix):
    return text.replace('cell_places',prefix+'places').replace('cell(',prefix+'cell(').replace('isp_sharpness',prefix+'sharpness')
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
'''+(root/'userland/src/hakios.camera.h').read_text()+names(old,'old_')+names(new,'new_')+r'''
static uint32_t state=0x5eeda1;
static uint32_t random_word(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int main(void){
 const unsigned sizes[][2]={{16,16},{128,96},{1296,972},{4000,3000}};
 const float windows[][4]={{0,0,1,1},{.013f,.027f,.943f,.869f},{.25f,.25f,.75f,.75f},{0,0,.01f,.01f},{.9f,.9f,1.1f,1.1f}};
 unsigned tests=0;
 for(unsigned shape=0;shape<4;shape++){
  struct hakios_camera_frame f={.width=sizes[shape][0],.height=sizes[shape][1]};
  f.stride=(f.width/4*5+63u)&~63u;size_t bytes=(size_t)f.stride*f.height;
  uint8_t *raw=malloc(bytes);assert(raw);
  for(unsigned pattern=0;pattern<4;pattern++){
   for(size_t i=0;i<bytes;i++)raw[i]=pattern==0?0:pattern==1?255:pattern==2?(uint8_t)random_word():(i/5&1?0x99:0x22);
   for(f.bayer=0;f.bayer<4;f.bayer++)for(unsigned w=0;w<5;w++){
    float before_level=0,after_level=0;const float *win=windows[w];
    float before=old_sharpness(&f,raw,win[0],win[1],win[2],win[3],&before_level);
    float after=new_sharpness(&f,raw,win[0],win[1],win[2],win[3],&after_level);
    assert(!memcmp(&before,&after,sizeof(float))&&!memcmp(&before_level,&after_level,sizeof(float)));tests++;
   }
  }
  free(raw);
 }
 printf("AF sharpness: %u exact float/level comparisons across four Bayer orders, geometries, patterns and clipped windows passed\n",tests);
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'sharp.c',Path(td)/'sharp'
    src.write_text(code)
    subprocess.run(['clang','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
