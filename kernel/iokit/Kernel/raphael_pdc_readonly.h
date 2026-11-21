/* SPDX-License-Identifier: BSD-2-Clause */
/* libqmi 392d8bf8: Get Selected Config 0x22 only, default PDC context.
 * No reset, set, load, delete or activate operation exists in this helper. */
#ifndef RAPHAEL_PDC_READONLY_H
#define RAPHAEL_PDC_READONLY_H
#include <stdint.h>
struct rpd_result { uint16_t result,error; uint8_t indication,active_present,pending_present,active_len,pending_len; };
static inline uint16_t rpd_u16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static inline uint32_t rpd_u32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline void rpd_w32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static inline uint32_t rpd_request(uint8_t *p,uint32_t room,uint32_t type,uint16_t txn,uint32_t token)
{
 if(!p || room<21 || type>1 || !txn)return 0;
 p[0]=0;p[1]=(uint8_t)txn;p[2]=(uint8_t)(txn>>8);p[3]=0x22;p[4]=0;p[5]=14;p[6]=0;
 p[7]=1;p[8]=4;p[9]=0;rpd_w32(p+10,type);
 p[14]=0x10;p[15]=4;p[16]=0;rpd_w32(p+17,token);return 21;
}
static inline int rpd_parse(const uint8_t *p,uint32_t n,uint16_t txn,uint32_t token,struct rpd_result *out)
{
 if(!p || !out || n<7 || n>1024 || (p[0]!=2 && p[0]!=4) || rpd_u16(p+3)!=0x22 || rpd_u16(p+5)!=n-7)return 0;
 if(p[0]==2 && rpd_u16(p+1)!=txn)return 0;
 struct rpd_result r={0,0,0,0,0,0,0};r.indication=p[0]==4;
 uint32_t seen=0;int matched_token=0,has_result=0;
 for(uint32_t at=7;at<n;) {
  if(n-at<3)return 0;
  uint32_t type=p[at],len=rpd_u16(p+at+1);if(len>n-at-3)return 0;
  const uint8_t *v=p+at+3;uint32_t bit=0;
  if(type==1)bit=1;else if(type==2)bit=2;else if(type==0x10)bit=4;else if(type==0x11)bit=8;else if(type==0x12)bit=16;
  if(bit && (seen&bit))return 0;seen|=bit;
  if(type==0x10) { if(len!=4 || rpd_u32(v)!=token)return 0;matched_token=1; }
  else if(type==2 && !r.indication) { if(len!=4)return 0;r.result=rpd_u16(v);r.error=rpd_u16(v+2);has_result=1; }
  else if(type==1 && r.indication) { if(len!=2)return 0;r.error=rpd_u16(v);has_result=1; }
  else if((type==0x11 || type==0x12) && r.indication) {
   if(!len || len!=1u+v[0])return 0;
   if(type==0x11){r.active_present=1;r.active_len=v[0];}else{r.pending_present=1;r.pending_len=v[0];}
  }
  at+=3+len;
 }
 if(!has_result || (r.indication && !matched_token))return 0;
 *out=r;return 1;
}
#endif
