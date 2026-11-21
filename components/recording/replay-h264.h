#ifndef RAPHAEL_REPLAY_H264_H
#define RAPHAEL_REPLAY_H264_H
#include <stdint.h>
#include <stddef.h>
#include <x264.h>
/* Fixed low-latency encoder configuration shared by native adapter and smoke
 * test. Sliced threads return each frame immediately: no lookahead/B frames. */
static inline x264_t *replay_h264_open(void){
 x264_param_t p;
 if(x264_param_default_preset(&p,"ultrafast","zerolatency"))return NULL;
 p.i_width=1080;p.i_height=2340;p.i_csp=X264_CSP_NV12;p.i_bitdepth=8;
 /* x264's VFR mode buffers a frame even with zerolatency. Use sequential
  * encoder ticks; the adapter timestamps the compressed sample separately
  * with its original host-clock capture time, including dropped-frame gaps. */
 p.i_fps_num=60;p.i_fps_den=1;p.i_timebase_num=1;p.i_timebase_den=60;
 p.b_vfr_input=0;p.b_annexb=0;p.b_repeat_headers=0;
 p.i_threads=4;p.b_sliced_threads=1;p.i_sync_lookahead=0;p.rc.i_lookahead=0;
 p.i_keyint_max=60;p.i_keyint_min=1;p.i_bframe=0;
 p.rc.i_rc_method=X264_RC_CRF;p.rc.f_rf_constant=23;
 p.vui.b_fullrange=0;p.vui.i_colmatrix=6;p.vui.i_colorprim=6;p.vui.i_transfer=6;
 p.i_log_level=X264_LOG_WARNING;
 if(x264_param_apply_profile(&p,"baseline"))return NULL;
 return x264_encoder_open(&p);
}
static inline int replay_h264_parameter_sets(x264_t *encoder,const uint8_t **sets,size_t *sizes){
 x264_nal_t *nals=NULL;int count=0,found=0;
 if(x264_encoder_headers(encoder,&nals,&count)<0)return -1;
 for(int i=0;i<count;i++)if(nals[i].i_type==NAL_SPS || nals[i].i_type==NAL_PPS){
  if(nals[i].i_payload<5)return -1;
  unsigned index=nals[i].i_type==NAL_SPS?0:1;
  const uint8_t *p=nals[i].p_payload;
  uint32_t bytes=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
  if(bytes!=(uint32_t)nals[i].i_payload-4)return -1;
  sets[index]=p+4;sizes[index]=bytes;found|=1<<index;
 }
 return found==3?0:-1;
}
static inline int replay_h264_encode(x264_t *encoder,uint8_t *y,size_t ys,uint8_t *uv,size_t uvs,
 int64_t sequence,x264_nal_t **nals,int *count,x264_picture_t *encoded){
 x264_picture_t picture;x264_picture_init(&picture);
 picture.img.i_csp=X264_CSP_NV12;picture.img.i_plane=2;
 picture.img.plane[0]=y;picture.img.plane[1]=uv;
 picture.img.i_stride[0]=(int)ys;picture.img.i_stride[1]=(int)uvs;picture.i_pts=sequence;
 return x264_encoder_encode(encoder,nals,count,&picture,encoded);
}
#endif
