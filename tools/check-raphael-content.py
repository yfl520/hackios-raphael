#!/usr/bin/env python3
"""Compare scalar word counts with the original per-byte RAW validity counts."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "hakios.raphael-content.h"
static struct raphael_content_counts before(const uint8_t *raw,unsigned stride,unsigned rows,unsigned bytes,unsigned min){
 struct raphael_content_counts result={0};
 for(unsigned y=0;y<rows;y++){
  unsigned written=0;
  for(unsigned x=0;x<bytes;x++){uint8_t v=raw[(size_t)y*stride+x];result.zero+=v==0;written+=v!=0xa5;}
  result.written_rows+=written>min;
 }
 return result;
}
static unsigned tests;
static void compare(const uint8_t *raw,unsigned stride,unsigned rows,unsigned bytes,unsigned min){
 struct raphael_content_counts a=before(raw,stride,rows,bytes,min),b=raphael_content_count(raw,stride,rows,bytes,min);
 assert(a.zero==b.zero&&a.written_rows==b.written_rows);tests++;
}
int main(void){
 /* Exhaust every adjacent pair, including cross-byte borrow traps. */
 for(uint64_t pair=0;pair<65536;pair++){
  uint64_t word=pair*0x0001000100010001ull;unsigned n=0;
  for(unsigned k=0;k<8;k++)n+=((word>>(8*k))&255)==0;
  assert(raphael_zero_bytes(word)==n);
 }
 const unsigned widths[]={0,1,7,8,9,15,16,17,4999,5000};
 uint32_t state=0x5eed1234;
 for(unsigned shape=0;shape<10;shape++)for(unsigned offset=0;offset<8;offset++){
  unsigned bytes=widths[shape],stride=bytes+13,rows=5;size_t size=(size_t)stride*rows+8;
  uint8_t *raw=malloc(size);assert(raw);
  for(unsigned pattern=0;pattern<4;pattern++){
   for(size_t i=0;i<size;i++){state^=state<<13;state^=state>>17;state^=state<<5;raw[i]=pattern==0?0:pattern==1?0xa5:pattern==2?255:(uint8_t)state;}
   compare(raw+offset,stride,rows,bytes,bytes*4/5);
  }free(raw);
 }
 const unsigned stride=5008,rows=3000,bytes=5000;size_t size=(size_t)stride*rows;
 uint8_t *raw=malloc(size);assert(raw);
 for(unsigned pattern=0;pattern<7;pattern++){
  memset(raw,pattern==0?0:pattern==1?0xa5:pattern==2?255:0x33,size);
  if(pattern==3||pattern==4){memset(raw,0xa5,bytes);memset(raw,0x33,pattern==3?4000:4001);}
  if(pattern==5||pattern==6){unsigned remaining=pattern==5?13999999:14000000;for(unsigned y=0;y<rows&&remaining;y++){unsigned n=remaining>bytes?bytes:remaining;memset(raw+(size_t)y*stride,0,n);remaining-=n;}}
  compare(raw,stride,rows,bytes,4000);
  struct raphael_content_counts a=before(raw,stride,rows,bytes,4000),b=raphael_content_count(raw,stride,rows,bytes,4000);
  assert((a.zero>=14000000||a.written_rows!=3000)==(b.zero>=14000000||b.written_rows!=3000));
 }
 free(raw);printf("content: 65536 adjacent-byte patterns, %u exact counts, unaligned/tail/real-frame/threshold cases passed\n",tests);
}
'''
with tempfile.TemporaryDirectory() as td:
    src,exe=Path(td)/'content.c',Path(td)/'content'
    src.write_text(code)
    subprocess.run(['clang','-std=c11','-O2','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(root/'xnu/files/bsd/dev'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=45)
