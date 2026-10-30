#!/usr/bin/env python3
"""Compare actual render_piece output with 1/8/32 groups and all pixel layouts."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
device = (root / 'userland/src/camera_device.c').read_text()
piece = device[device.index('static void\nrender_piece('):device.index('static CVPixelBufferRef\noutput_buffer(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_isp.h"
#define PF_BGRA 1
#define PF_420V 2
#define PF_420F 3
struct buffer {uint8_t *data;size_t stride, ybytes;};
typedef struct buffer *CVPixelBufferRef;
static void *CVPixelBufferGetBaseAddress(CVPixelBufferRef b){return b->data;}
static size_t CVPixelBufferGetBytesPerRow(CVPixelBufferRef b){return b->stride;}
static void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef b,int i){return b->data+(i?b->ybytes:0);}
static size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef b,int i){(void)i;return b->stride;}
struct render_job {
 const struct isp *isp;
 const struct hakios_camera_frame *f;
 const uint8_t *raw;
 CVPixelBufferRef pb;
 uint32_t format,width,height,pieces;
 bool still;
};
''' + piece + r'''
int main(void){
 struct hakios_camera_frame f={.width=160,.height=120,.stride=208};
 size_t rawbytes=(size_t)f.stride*f.height;uint8_t *raw=malloc(rawbytes);assert(raw);
 for(size_t i=0;i<rawbytes;i++)raw[i]=(uint8_t)(i*71+i/43);
 const unsigned sizes[][2]={{1,1},{13,17},{64,48},{95,63},{128,90},{1504,1128}};
 unsigned tests=0;
 for(unsigned shape=0;shape<6;shape++)for(unsigned format=1;format<=3;format++)
 for(unsigned still=0;still<2;still++)for(unsigned variant=0;variant<4;variant++){
  if(shape==5&&(still||variant>0))continue;
  struct isp isp;isp_init(&isp);isp.zoom=variant&1?2.5f:1;isp.turned=(variant&2)!=0;
  isp.wb[0]=1.7f;isp.wb[2]=1.2f;isp.digital=1.35f;f.bayer=variant;
  unsigned width=sizes[shape][0],height=sizes[shape][1];
  size_t stride=format==PF_BGRA?(size_t)width*4+16:((size_t)width+1)/2*2+16;
  size_t bytes=format==PF_BGRA?stride*height:stride*(height+(height+1)/2);
  uint8_t *reference=malloc(bytes);assert(reference);
  for(unsigned groups=0;groups<3;groups++){
   uint8_t *data=malloc(bytes);assert(data);memset(data,0xa5,bytes);
   struct buffer pb={data,stride,stride*height};
   struct render_job job={&isp,&f,raw,&pb,format,width,height,groups==0?1:groups==1?8:32,still&&format!=PF_BGRA};
   for(unsigned i=0;i<job.pieces;i++)render_piece(&job,i);
   if(!groups)memcpy(reference,data,bytes);else assert(!memcmp(reference,data,bytes));
   free(data);
  }
  free(reference);tests++;
 }
 free(raw);printf("render_piece: %u exact 1/8/32-group comparisons, NV12 full/video/BGRA, still/preview, Bayer/zoom/rotation/odd edges passed\n",tests);
}
'''
with tempfile.TemporaryDirectory() as td:
    src, exe = Path(td) / 'pieces.c', Path(td) / 'pieces'
    src.write_text(code)
    subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(root/'userland/src'), str(src),
                    str(root/'userland/src/camera_isp.c'), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
