#!/usr/bin/env python3
"""Fault-inject the actual rear actuator ID/pin ownership code, no hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root/'xnu/files/bsd/dev/hakios.raphael-actuator.h').read_text()
harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef uintptr_t vm_offset_t;
struct raphael_probe_pin { vm_offset_t base; uint32_t cfg,io; };
struct raphael_i2c_bus { int unused; };
enum raphael_i2c_result {RAPHAEL_I2C_OK,RAPHAEL_I2C_ARGUMENT,RAPHAEL_I2C_BUSY,RAPHAEL_I2C_STRETCH,RAPHAEL_I2C_NACK};
static unsigned failure,maps,transfers,pin_calls,high_calls,writes;
static uint32_t cfg,io,original_cfg,original_io;
static uint8_t chip[8];
static unsigned raphael_actuator_mode;
static unsigned inject_transfer,at_transfer,dac_writes,max_dac;
static int say(const char *fmt,...){(void)fmt;return 0;}
static void spin_us(unsigned n){assert(n==1000);}
static uintptr_t raphael_camera_map(uint32_t p,unsigned n){assert(p==0x03953000&&n==0x1000);maps++;return failure==1?0:0x1000;}
static uint32_t rd(uintptr_t p){assert(p==0x1000||p==0x1004);return p==0x1000?cfg:io;}
static void wr(uintptr_t p,uint32_t v){assert(p==0x1000||p==0x1004);writes++;if(p==0x1000)cfg=v;else io=v;}
static bool raphael_probe_gpio(struct raphael_probe_pin *p,bool high){
 assert(p->base==0x1000);pin_calls++;high_calls+=high;
 if((high&&failure==2)||(!high&&failure==6))return false;
 cfg=(p->cfg&~0x3ffu)|0x200u;io=(p->io&~2u)|(high?2u:0);return true;
}
static enum raphael_i2c_result raphael_i2c_transfer(struct raphael_i2c_bus *b,uint8_t a,
 const uint8_t *out,unsigned no,uint8_t *in,unsigned ni){
 assert(b&&a==0x0c&&out);
 assert(cfg==(original_cfg&~0x3ffu|0x200u)&&(io&2));
 transfers++;
 if(failure==3||(inject_transfer&&++at_transfer==inject_transfer))return RAPHAEL_I2C_NACK;
 if(failure==4)return RAPHAEL_I2C_STRETCH;
 if(no==1){assert(in&&ni&&out[0]+ni<=8);memcpy(in,chip+out[0],ni);if(failure==5&&out[0]==0)in[0]=0;}
 else{
  assert(raphael_actuator_mode==2&&!in&&!ni&&(no==2||no==3));
  unsigned r=out[0];assert(r==2||r==3||r==6||r==7);
  if(r==3){assert(no==3);unsigned dac=(unsigned)out[1]<<8|out[2];assert(dac<=579);dac_writes++;if(dac>max_dac)max_dac=dac;chip[3]=out[1];chip[4]=out[2];}
  else{assert(no==2);assert((r==2&&out[1]<=2)||(r==6&&out[1]==0x60)||(r==7&&out[1]==2));chip[r]=out[1];}
 }
 return RAPHAEL_I2C_OK;
}
'''
main = r'''
int main(void){
 struct raphael_i2c_bus bus={0};
 for(unsigned mode=0;mode<=1;mode++)for(failure=0;failure<=6;failure++){
  maps=transfers=pin_calls=high_calls=writes=0;
  cfg=original_cfg=0x4003;io=original_io=1;raphael_actuator_mode=mode;memset(chip,0,sizeof(chip));chip[0]=0xf9;
  assert(raphael_actuator_identify(&bus)==!(mode&&failure==6));
  assert(cfg==original_cfg&&(io&2)==(original_io&2));
  assert(!mode?maps==0&&pin_calls==0&&transfers==0:
   failure==1?pin_calls==0&&transfers==0:high_calls==1&&pin_calls==2&&writes==2);
 }
 for(unsigned busy=0;busy<3;busy++){
  failure=0;maps=transfers=pin_calls=high_calls=writes=0;raphael_actuator_mode=1;
  cfg=original_cfg=busy==0?4:busy==1?0x200:0;io=original_io=busy==2?2:0;
  assert(raphael_actuator_identify(&bus));assert(!transfers&&!pin_calls&&!writes);
  assert(cfg==original_cfg&&io==original_io);
 }
 failure=0;raphael_actuator_mode=2;
 for(unsigned position=0;position<11;position++)for(unsigned injection=0;injection<=70;injection++){
  unsigned target=position==10?579:180+40*position;
  struct raphael_actuator_state state={0};
  cfg=original_cfg=0x4003;io=original_io=1;memset(chip,0,sizeof(chip));chip[0]=0xf9;
  inject_transfer=injection;at_transfer=dac_writes=max_dac=0;raphael_actuator_fixed=target;
  assert(raphael_actuator_begin(&state,&bus));
  if(!injection){assert(state.active&&state.dac==target&&max_dac==target);}
  inject_transfer=0;
  assert(raphael_actuator_end(&state,&bus));assert(!state.active&&!state.owned);
  assert(cfg==original_cfg&&(io&2)==(original_io&2)&&max_dac<=579);
 }
 struct raphael_actuator_state state={.initialized=true};
 assert(!raphael_actuator_move(&state,&bus,580));
 assert(!raphael_actuator_write(&bus,4,0));assert(!raphael_actuator_write(&bus,6,0x41));
 assert(!raphael_actuator_write(&bus,3,1023));
 puts("actuator ID: disabled/map/enable/NACK/stretch/identity/restore failures and busy ownership passed; ID requests read-only");
 puts("fixed rear lens: 11 positions x 71 transfer failure points, bounded DAC, readback and GPIO restoration passed");
}
'''
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp)/'actuator.c'
    exe = Path(temp)/'actuator'
    src.write_text(harness+'\n'+header+'\n'+main)
    subprocess.run(['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
