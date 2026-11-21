/* Included by MiniIOMobileFramebuffer.cpp. Capture the composed display, never
 * an application's private surface; allocation/encoding are off the presenter.
 * Device is root-only and capture remains disabled until an explicit ioctl.
 */
extern "C" {
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
}
#include "raphael-screenrec.h"
static IOLock *hsrLock, *hsrReadLock;
static uint8_t *hsrPixels;
static uint8_t *hsrCompose;
static bool hsrComposeValid;
static bool hsrUseForNextRender;
static hsr_frame hsrFrame;
static bool hsrOwned;
/* These describe the last completely composed scanout. Access only while
 * holding the existing display scanout lock; buffers live for the boot. */
static const uint8_t *hsrScanout;
static uint32_t hsrScanoutWidth,hsrScanoutHeight,hsrScanoutStride;
extern "C" void hakios_display_scanout_lock(void);
extern "C" void hakios_display_scanout_unlock(void);
static void raphael_screenrec_present(const uint8_t *,uint32_t,uint32_t,uint32_t);
static void hsr_sample_locked(void);
/* Caller holds scanout lock. The cached composition is also the recording
 * source: never repeatedly read write-combined panel memory at 60 Hz. */
static bool raphael_screenrec_cached(void)
{
 if(!hsrLock || !hsrCompose)return false;
 IOLockLock(hsrLock);
 bool use=hsrFrame.active && hsrFrame.version==HSR_VERSION_NATIVE && hsrComposeValid;
 hsrUseForNextRender=use;
 IOLockUnlock(hsrLock);return use;
}
static uint8_t *raphael_screenrec_render_begin(uint8_t *target,uint32_t width,uint32_t height,uint32_t stride)
{
 if(!hsrLock || !hsrCompose || stride!=(uint64_t)width*4 || width>HSR_MAX_W || height>HSR_MAX_H)return target;
 IOLockLock(hsrLock);
 bool use=hsrUseForNextRender || (hsrFrame.active && hsrFrame.version==HSR_VERSION_NATIVE);
 hsrUseForNextRender=false;
 IOLockUnlock(hsrLock);
 if(!use)return target;
 if(!hsrComposeValid){
  if(!hsrScanout || hsrScanoutWidth!=width || hsrScanoutHeight!=height || hsrScanoutStride!=stride)return target;
  memcpy(hsrCompose,hsrScanout,(size_t)stride*height);hsrComposeValid=true;
 }
 return hsrCompose;
}

static int hsr_open(dev_t, int, int, struct proc *)
{
 if (!hsrLock || !hsrPixels) return ENXIO;
 IOLockLock(hsrLock);
 int error=hsrOwned ? EBUSY : 0;
 if (!error) hsrOwned=true;
 IOLockUnlock(hsrLock);
 return error;
}
static int hsr_close(dev_t, int, int, struct proc *)
{
 IOLockLock(hsrLock);
 hsrFrame.active=0; hsrOwned=false;
 IOLockUnlock(hsrLock);
 return 0;
}
static int hsr_ioctl(dev_t, u_long command, caddr_t arg, int, struct proc *)
{
 int error=0;
 IOLockLock(hsrLock);
 if (command==HSR_START) {
  const hsr_config *c=(const hsr_config *)arg;
  const bool legacy=c->version==HSR_VERSION && !c->reserved && c->step>=2 && c->step<=4 && c->fps>=1 && c->fps<=30;
  const bool native=c->version==HSR_VERSION_NATIVE && c->reserved==HSR_FORMAT_BGRA && c->step==1 && c->fps>=1 && c->fps<=60;
  if (!legacy && !native) error=EINVAL;
  else {
   bzero(&hsrFrame,sizeof(hsrFrame));
   hsrFrame.magic=HSR_MAGIC;hsrFrame.version=c->version;hsrFrame.reserved=c->reserved;
   hsrFrame.active=1;hsrFrame.fps=c->fps;hsrFrame.step=c->step;
  }
 } else if (command==HSR_INFO) memcpy(arg,&hsrFrame,sizeof(hsrFrame));
 else if (command==HSR_STOP) hsrFrame.active=0;
 else error=ENOTTY;
 IOLockUnlock(hsrLock);
 return error;
}
static int hsr_read(dev_t, struct uio *uio, int)
{
 hsr_frame header;
 IOLockLock(hsrReadLock);
 /* A static control center need not submit another swap after HSR_START.
  * Sample its already composed frame on demand, with the same scanout lock
  * used by the presenter. This never asks an app to redraw or captures idle. */
 hakios_display_scanout_lock();
 if(hsrScanout)hsr_sample_locked();
 IOLockLock(hsrLock);
 header=hsrFrame;
 int error=0;
 if (!header.sequence) error=EAGAIN;
 else if (header.bytes>HSR_MAX_PIXELS || uio_resid(uio)<(user_ssize_t)(sizeof(header)+header.bytes)) error=EMSGSIZE;
 /* hsrReadLock serializes all samplers, and the presenter never writes this
  * staging buffer. It remains stable while copyout runs without scanout lock. */
 IOLockUnlock(hsrLock);
 hakios_display_scanout_unlock();
 if (!error) error=uiomove((caddr_t)&header,sizeof(header),uio);
 if (!error) error=uiomove((caddr_t)hsrPixels,(int)header.bytes,uio);
 IOLockUnlock(hsrReadLock);
 return error;
}
static const struct cdevsw hsrCdev={
 .d_open=hsr_open,.d_close=hsr_close,.d_read=hsr_read,.d_write=eno_rdwrt,
 .d_ioctl=hsr_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_select=eno_select,
 .d_mmap=eno_mmap,.d_strategy=eno_strat,.d_type=0
};
static void raphael_screenrec_init(void)
{
 hsrLock=IOLockAlloc();hsrReadLock=IOLockAlloc();
 hsrPixels=(uint8_t *)IOMalloc(HSR_MAX_PIXELS);
 hsrCompose=(uint8_t *)IOMalloc(HSR_MAX_PIXELS);
 if (!hsrLock || !hsrReadLock || !hsrPixels || !hsrCompose) return;
 int major=cdevsw_add(-1,&hsrCdev);
 if (major>=0 && devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,"raphael-screen")!=NULL)
  IOLog("Raphael screen capture: root-only device ready; disabled until requested\n");
}
static void raphael_screenrec_present(const uint8_t *base,uint32_t width,uint32_t height,uint32_t stride)
{
 /* Presenter only remembers the completed frame. Capture and conversion never
  * run on the UI presenter's path, even during full-resolution recording. */
 hsrScanout=base;hsrScanoutWidth=width;hsrScanoutHeight=height;hsrScanoutStride=stride;
 if(base!=hsrCompose)hsrComposeValid=false;
}
static void hsr_sample_locked(void)
{
 const uint8_t *base=hsrScanout;
 const uint32_t width=hsrScanoutWidth,height=hsrScanoutHeight,stride=hsrScanoutStride;
 if (!hsrLock || !hsrPixels || !IOLockTryLock(hsrLock)) return;
 uint64_t now;
 absolutetime_to_nanoseconds(mach_absolute_time(),&now);
 /* The explicit reader supplies pacing. Re-reading a static display is valid;
  * do not alternate duplicate frames because a 60 Hz timer fires slightly early. */
 if (!hsrFrame.active) {
  IOLockUnlock(hsrLock);return;
 }
 uint32_t step=hsrFrame.step,w=width/step,h=height/step;
 if (!base || w<1 || h<1 || w>HSR_MAX_W || h>HSR_MAX_H || stride<(uint64_t)width*4) {
  hsrFrame.active=0;IOLockUnlock(hsrLock);return;
 }
 const bool bgra=hsrFrame.version==HSR_VERSION_NATIVE && hsrFrame.reserved==HSR_FORMAT_BGRA;
 if(bgra && hsrCompose && stride==width*4 && !hsrComposeValid){
  memcpy(hsrCompose,base,(size_t)stride*height);hsrComposeValid=true;
  base=hsrCompose;hsrScanout=hsrCompose;
 }
 uint8_t *out=hsrPixels;
 if(bgra){
  for(uint32_t y=0;y<h;y++)memcpy(out+(size_t)y*w*4,base+(size_t)y*stride,(size_t)w*4);
 }else for (uint32_t y=0;y<h;y++) {
  const uint8_t *row=base+(size_t)(y*step+step/2)*stride;
  for (uint32_t x=0;x<w;x++) {
   const uint8_t *pixel=row+(x*step+step/2)*4;
   *out++=pixel[2];*out++=pixel[1];*out++=pixel[0];
  }
 }
 hsrFrame.width=w;hsrFrame.height=h;hsrFrame.stride=w*(bgra?4:3);hsrFrame.bytes=hsrFrame.stride*h;
 hsrFrame.monotonic_ns=now;hsrFrame.sequence++;
 IOLockUnlock(hsrLock);
}
