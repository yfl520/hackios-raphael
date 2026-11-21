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
static uint8_t *hsrPixels, *hsrReadPixels;
static hsr_frame hsrFrame;
static bool hsrOwned;

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
  if (c->version!=HSR_VERSION || c->reserved || c->step<2 || c->step>4 || c->fps<1 || c->fps>30) error=EINVAL;
  else {
   bzero(&hsrFrame,sizeof(hsrFrame));
   hsrFrame.magic=HSR_MAGIC;hsrFrame.version=HSR_VERSION;
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
 IOLockLock(hsrLock);
 header=hsrFrame;
 int error=0;
 if (!header.sequence) error=EAGAIN;
 else if (header.bytes>HSR_MAX_PIXELS || uio_resid(uio)<(user_ssize_t)(sizeof(header)+header.bytes)) error=EMSGSIZE;
 else memcpy(hsrReadPixels,hsrPixels,header.bytes);
 IOLockUnlock(hsrLock);
 if (!error) error=uiomove((caddr_t)&header,sizeof(header),uio);
 if (!error) error=uiomove((caddr_t)hsrReadPixels,(int)header.bytes,uio);
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
 hsrReadPixels=(uint8_t *)IOMalloc(HSR_MAX_PIXELS);
 if (!hsrLock || !hsrReadLock || !hsrPixels || !hsrReadPixels) return;
 int major=cdevsw_add(-1,&hsrCdev);
 if (major>=0 && devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,"raphael-screen")!=NULL)
  IOLog("Raphael screen capture: root-only device ready; disabled until requested\n");
}
static void raphael_screenrec_present(const uint8_t *base,uint32_t width,uint32_t height,uint32_t stride)
{
 if (!hsrLock || !hsrPixels || !IOLockTryLock(hsrLock)) return;
 uint64_t now;
 absolutetime_to_nanoseconds(mach_absolute_time(),&now);
 if (!hsrFrame.active || (hsrFrame.monotonic_ns && now-hsrFrame.monotonic_ns<NSEC_PER_SEC/hsrFrame.fps)) {
  IOLockUnlock(hsrLock);return;
 }
 uint32_t step=hsrFrame.step,w=width/step,h=height/step;
 if (!base || w<1 || h<1 || w>HSR_MAX_W || h>HSR_MAX_H || stride<(uint64_t)width*4) {
  hsrFrame.active=0;IOLockUnlock(hsrLock);return;
 }
 uint8_t *out=hsrPixels;
 for (uint32_t y=0;y<h;y++) {
  const uint8_t *row=base+(size_t)(y*step+step/2)*stride;
  for (uint32_t x=0;x<w;x++) {
   const uint8_t *pixel=row+(x*step+step/2)*4;
   *out++=pixel[2];*out++=pixel[1];*out++=pixel[0];
  }
 }
 hsrFrame.width=w;hsrFrame.height=h;hsrFrame.stride=w*3;hsrFrame.bytes=w*h*3;
 hsrFrame.monotonic_ns=now;hsrFrame.sequence++;
 IOLockUnlock(hsrLock);
}
