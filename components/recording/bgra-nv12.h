#ifndef RAPHAEL_BGRA_NV12_H
#define RAPHAEL_BGRA_NV12_H
#include <stdint.h>
#include <stddef.h>
#if defined(__ARM_NEON) && !defined(REPLAY_FORCE_SCALAR)
#include <arm_neon.h>
#endif
/* BT.601 limited range. BGRA scanout to NV12 without an RGB24 intermediate.
 * Both implementations average 2x2 source pixels before chroma conversion. */
static inline uint8_t replay_bgra_clip(int v){return v<0?0:v>255?255:(uint8_t)v;}
#if defined(__ARM_NEON) && !defined(REPLAY_FORCE_SCALAR)
static inline uint8x16_t replay_bgra_y(uint8x16x4_t p){
 uint16x8_t lo=vmull_u8(vget_low_u8(p.val[2]),vdup_n_u8(66));
 uint16x8_t hi=vmull_u8(vget_high_u8(p.val[2]),vdup_n_u8(66));
 lo=vmlal_u8(lo,vget_low_u8(p.val[1]),vdup_n_u8(129));hi=vmlal_u8(hi,vget_high_u8(p.val[1]),vdup_n_u8(129));
 lo=vmlal_u8(lo,vget_low_u8(p.val[0]),vdup_n_u8(25));hi=vmlal_u8(hi,vget_high_u8(p.val[0]),vdup_n_u8(25));
 return vaddq_u8(vcombine_u8(vshrn_n_u16(vaddq_u16(lo,vdupq_n_u16(128)),8),
  vshrn_n_u16(vaddq_u16(hi,vdupq_n_u16(128)),8)),vdupq_n_u8(16));
}
#endif
static inline int replay_bgra_nv12(const uint8_t *bgra,unsigned w,unsigned h,size_t bs,
 uint8_t *y,size_t ys,uint8_t *uv,size_t uvs){
 if(!bgra || !y || !uv || !w || !h || (w&1) || (h&1) || bs<(size_t)w*4 || ys<w || uvs<w)return -1;
 for(unsigned row=0;row<h;row+=2){
  const uint8_t *a=bgra+(size_t)row*bs,*b=a+bs;
  uint8_t *ya=y+(size_t)row*ys,*yb=ya+ys,*c=uv+(size_t)(row/2)*uvs;
  unsigned x=0;
#if defined(__ARM_NEON) && !defined(REPLAY_FORCE_SCALAR)
  for(;x+16<=w;x+=16){
   uint8x16x4_t p=vld4q_u8(a+(size_t)x*4),q=vld4q_u8(b+(size_t)x*4);
   vst1q_u8(ya+x,replay_bgra_y(p));vst1q_u8(yb+x,replay_bgra_y(q));
   int16x8_t r=vreinterpretq_s16_u16(vshrq_n_u16(vaddq_u16(vaddq_u16(vpaddlq_u8(p.val[2]),vpaddlq_u8(q.val[2])),vdupq_n_u16(2)),2));
   int16x8_t g=vreinterpretq_s16_u16(vshrq_n_u16(vaddq_u16(vaddq_u16(vpaddlq_u8(p.val[1]),vpaddlq_u8(q.val[1])),vdupq_n_u16(2)),2));
   int16x8_t blue=vreinterpretq_s16_u16(vshrq_n_u16(vaddq_u16(vaddq_u16(vpaddlq_u8(p.val[0]),vpaddlq_u8(q.val[0])),vdupq_n_u16(2)),2));
   int16x8_t u=vmlaq_n_s16(vmlaq_n_s16(vmulq_n_s16(r,-38),g,-74),blue,112);
   int16x8_t v=vmlaq_n_s16(vmlaq_n_s16(vmulq_n_s16(r,112),g,-94),blue,-18);
   u=vaddq_s16(vshrq_n_s16(vaddq_s16(u,vdupq_n_s16(128)),8),vdupq_n_s16(128));
   v=vaddq_s16(vshrq_n_s16(vaddq_s16(v,vdupq_n_s16(128)),8),vdupq_n_s16(128));
   uint8x8x2_t chroma={{vqmovun_s16(u),vqmovun_s16(v)}};vst2_u8(c+x,chroma);
  }
#endif
  for(;x<w;x+=2){
   int r=0,g=0,blue=0;
   for(unsigned dy=0;dy<2;dy++)for(unsigned dx=0;dx<2;dx++){
    const uint8_t *p=(dy?b:a)+(size_t)(x+dx)*4;
    (dy?yb:ya)[x+dx]=replay_bgra_clip(((66*p[2]+129*p[1]+25*p[0]+128)>>8)+16);
    r+=p[2];g+=p[1];blue+=p[0];
   }
   r=(r+2)/4;g=(g+2)/4;blue=(blue+2)/4;
   c[x]=replay_bgra_clip(((-38*r-74*g+112*blue+128)>>8)+128);
   c[x+1]=replay_bgra_clip(((112*r-94*g-18*blue+128)>>8)+128);
  }
 }
 return 0;
}
#endif
