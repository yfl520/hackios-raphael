#!/usr/bin/env python3
"""Exercise the actual upstream contrast AF with module limits and synthetic optics.

No hardware or real image claim: this checks range commands/convergence only.
"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
camera = (root/'userland/src/camera_device.c').read_text()
begin = camera.index('enum { AF_WATCH,')
end = camera.index('/* the sensor\'s colour tuning', begin)
af = camera[begin:end]
abi = (root/'userland/src/hakios.camera.h').read_text()
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <sys/types.h>
''' + abi + r'''
static unsigned position,minimum,maximum,moves,peak;
static int say(const char *s,...){(void)s;return 0;}
static ssize_t my_write(int fd,const void *p,size_t size){
 (void)fd;const struct hakios_camera_command *c=p;
 assert(size==sizeof(*c)&&c->command==HAKIOS_CAMERA_FOCUS);
 assert(c->a>=minimum&&c->a<=maximum);position=c->a;moves++;return size;
}
#define write my_write
static float isp_sharpness(const struct hakios_camera_frame *f,const uint8_t *raw,
 float x,float y,float w,float h,float *level){
 (void)f;(void)raw;(void)x;(void)y;(void)w;(void)h;*level=1;
 float distance=(float)position-(float)peak;
 return 100.0f/(1+distance*distance/2000.0f);
}
'''
main = r'''
int main(void){
 const unsigned peaks[]={180,360,579};
 for(unsigned kind=0;kind<2;kind++)for(unsigned k=0;k<3;k++){
  struct af af={.now=true,.sweep=true};
  minimum=kind?16:180;maximum=kind?768:579;position=minimum;peak=peaks[k];moves=0;
  struct hakios_camera_frame f={.gain=256,.gain_unity=256};
  if(!kind){f.reserved[0]=HAKIOS_CAMERA_FOCUS_RANGE_MAGIC;f.reserved[1]=180;f.reserved[2]=579;}
  float window[4]={0,0,1,1};
  for(unsigned frame=0;frame<1000;frame++){f.focus=position;af_frame(1,&af,&f,NULL,window,false);}
  assert(moves>0&&abs((int)position-(int)peak)<=24&&af.state==AF_WATCH);
 }
 struct af af={0};minimum=16;maximum=768;position=300;peak=360;moves=0;
 struct hakios_camera_frame f={.focus=300,.gain=256,.gain_unity=256};float window[4]={0,0,1,1};
 f.reserved[0]=HAKIOS_CAMERA_FOCUS_RANGE_MAGIC;f.reserved[1]=579;f.reserved[2]=180;
 af_frame(1,&af,&f,NULL,window,false);assert(af.minimum==16&&af.maximum==768);
 f.reserved[1]=0;f.reserved[2]=1024;af_frame(1,&af,&f,NULL,window,false);
 assert(af.minimum==16&&af.maximum==768);
 f.focus=HAKIOS_CAMERA_NO_FOCUS;unsigned before=moves;af_frame(1,&af,&f,NULL,window,false);assert(moves==before);
 puts("Actual contrast AF: OEM/legacy bounds, 3 synthetic focus peaks, invalid metadata fallback and no-motor path passed");
}
'''
with tempfile.TemporaryDirectory() as td:
    source, exe = Path(td)/'af.c', Path(td)/'af'
    source.write_text(prefix+af+main)
    subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(source),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
