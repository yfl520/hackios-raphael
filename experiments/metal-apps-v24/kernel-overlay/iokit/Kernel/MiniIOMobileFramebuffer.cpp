/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"
#include "raphael-screenrec.inc"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
/* the panel's scan (bsd/dev/hakios.display.c; sys/hakios.display.h, which iokit/ does not see) */
struct hakios_display_scan {
	int ok;
	uint32_t lines, shown, millihz;
	uint64_t line_ns, frame_ns;
};
extern "C" int hakios_display_scan_start(void);
extern "C" const struct hakios_display_scan * hakios_display_scan(void);
extern "C" uint64_t hakios_display_next_frame_ns(void);
extern "C" int raphael_display_init(uint32_t width, uint32_t height, uint32_t stride);
extern "C" void *raphael_display_back(bool preserve);
extern "C" bool raphael_display_present(void);
static IOLock *gScanoutLock;

extern "C" void hakios_display_scanout_lock(void) { if (gScanoutLock) IOLockLock(gScanoutLock); }
extern "C" void hakios_display_scanout_unlock(void) { if (gScanoutLock) IOLockUnlock(gScanoutLock); }

/* CoreBrightness service shape follows ios-sailfish patch 0168. Raphael's
 * hardware calls run on the loader's CPU0 through the native firmware mailbox. */
extern "C" int hakios_display_brightness_get(uint32_t *percent);
extern "C" int hakios_display_brightness_set(uint32_t percent);

class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);
	IOWorkLoop *fWL;
	IOTimerEventSource *fFade;
	uint32_t fLevel, fFrom, fTo;
	uint64_t fStart, fPeriod;
	unsigned fLogged;
	static void fadeStep(OSObject *owner, IOTimerEventSource *sender);
	void publish(void);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publish(void)
{
	OSDictionary *b = OSDictionary::withCapacity(3);
	OSDictionary *p = OSDictionary::withCapacity(1);
	if (b && p) {
		OSNumber *n = OSNumber::withNumber(0ULL, 32);
		if (n) b->setObject("min", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(100ULL, 32);
		if (n) b->setObject("max", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(fLevel, 32);
		if (n) b->setObject("value", n); OSSafeReleaseNULL(n);
		p->setObject("brightness", b);
		setProperty("IODisplayParameters", p);
	}
	OSSafeReleaseNULL(b);
	OSSafeReleaseNULL(p);
}

bool
AppleARMBacklight::start(IOService *provider)
{
	if (!IOService::start(provider) || hakios_display_brightness_get(&fLevel)) return false;
	fWL = IOWorkLoop::workLoop();
	fFade = IOTimerEventSource::timerEventSource(this, &AppleARMBacklight::fadeStep);
	if (!fWL || !fFade || fWL->addEventSource(fFade) != kIOReturnSuccess) return false;
	setProperty("backlight-control", true);
	publish();
	IOLog("HAKIOS BL: AppleARMBacklight registered, native level %u%%\n", fLevel);
	registerService();
	return true;
}

void
AppleARMBacklight::stop(IOService *provider)
{
	if (fFade) fFade->cancelTimeout();
	IOService::stop(provider);
}

void
AppleARMBacklight::free(void)
{
	if (fFade) {
		fFade->cancelTimeout();
		if (fWL) fWL->removeEventSource(fFade);
	}
	OSSafeReleaseNULL(fFade);
	OSSafeReleaseNULL(fWL);
	IOService::free();
}

void
AppleARMBacklight::fadeStep(OSObject *owner, IOTimerEventSource *sender)
{
	AppleARMBacklight *bl = (AppleARMBacklight *)owner;
	uint64_t t = mach_absolute_time() - bl->fStart;
	uint32_t level = bl->fTo;
	if (t < bl->fPeriod)
		level = (uint32_t)((int64_t)bl->fFrom +
		    ((int64_t)bl->fTo - bl->fFrom) * (int64_t)t / (int64_t)bl->fPeriod);
	/* Minimum visible light; display sleep is a separate, unimplemented path. */
	if (!level) level = 1;
	if (level != bl->fLevel) {
		int error = hakios_display_brightness_set(level);
		if (!error && !hakios_display_brightness_get(&level)) {
			bl->fLevel = level;
			bl->publish();
		} else if (bl->fLogged++ < 40) IOLog("HAKIOS BL: native request failed %d\n", error);
	}
	/* Stop on elapsed time, including failed requests or a zero clamped to 1. */
	if (t < bl->fPeriod) sender->setTimeoutMS(20);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	if (!d) return kIOReturnBadArgument;
	OSNumber *b = OSDynamicCast(OSNumber, d->getObject("brightness"));
	OSNumber *f = OSDynamicCast(OSNumber, d->getObject("FadePeriod"));
	if (!b) return kIOReturnSuccess;
	uint32_t value = b->unsigned32BitValue();
	if (value > 100) return kIOReturnBadArgument;
	uint64_t ms = f ? ((uint64_t)f->unsigned32BitValue() * 1000) >> 16 : 0;
	return fWL->runActionBlock(^IOReturn (void) {
		fFade->cancelTimeout();
		fFrom = fLevel;
		fTo = value;
		fStart = mach_absolute_time();
		nanoseconds_to_absolutetime(MIN(ms, 5000ULL) * NSEC_PER_MSEC, &fPeriod);
		if (fLogged++ < 40) IOLog("HAKIOS BL: CoreBrightness level %u fade %llu ms\n", value, ms);
		fadeStep(this, fFade);
		return kIOReturnSuccess;
	});
}

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* unfinished frames are dropped */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
/* Nearest during motion; one bilinear redraw after 40 ms without a swap. */
static TUNABLE(uint32_t, gFilter, "fb_filter", 2);
#define SETTLE_MS 40
static TUNABLE(uint32_t, gThreadPriority, "fb_pri", 92);
extern "C" kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
static kern_return_t
panel_thread_start(thread_continue_t continuation, void *parameter, thread_t *thread)
{
	if (gThreadPriority >= 1 && gThreadPriority <= 95) {
		return kernel_thread_start_priority(continuation, parameter, (integer_t)gThreadPriority, thread);
	}
	return kernel_thread_start(continuation, parameter, thread);
}
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static uint32_t gScanHz;

static inline uint32_t
refreshHz(void)
{
	if (gScanHz) return gScanHz;
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "minifb-compose.h"

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* current composition target (boot framebuffer without page flipping) */
	bool            fPageFlip;
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	uint32_t        fSwapCompleted;
	uint8_t fLayerState[SWAP_SIZE];
	MiniIOSurface *fLayerSurfaces[SWAP_LAYERS]; /* independent accepted-state refs */
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
    enum { PROFILE_LOCK, PROFILE_PREP, PROFILE_CLEAR, PROFILE_LAYER0,
           PROFILE_LAYER1, PROFILE_LAYER2, PROFILE_LAYER3, PROFILE_COPY,
           PROFILE_PRESENT, PROFILE_POST, PROFILE_PARTS };
    uint64_t fComposeTicks[2][PROFILE_PARTS] = {};
    uint32_t fComposeFrames[2] = {};
    uint32_t fComposeReports = 0;
    uint32_t fComposeGroupReports[2] = {};
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	void publishRefresh(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	bool show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void settle(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool fSettling;
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool over);
    void drawShared(const RowJob *job,uint32_t *blend);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
    int32_t fNextRow;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishRefresh(void)
{
	OSDictionary *refresh = OSDictionary::withCapacity(4);
	if (refresh == NULL) return;
	uint32_t hz = refreshHz();
	uint64_t interval = (1ULL << 32) / hz, ticks;
	nanoseconds_to_absolutetime(NSEC_PER_SEC / hz, &ticks);
	setNumber(refresh, "displayMinRefreshInterval", interval);
	setNumber(refresh, "displayMaxRefreshInterval", interval);
	setNumber(refresh, "displayRefreshStep", interval);
	setNumber(refresh, "displayRefreshStepMachTime", ticks);
	setProperty("IOMFBDisplayRefresh", refresh);
	refresh->release();
	IOLog("MiniIOMobileFramebuffer: published actual panel refresh %u Hz\n", hz);
}

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	publishRefresh();
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	gScanoutLock = IOLockAlloc();
	if (panel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (panel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fPageFlip = raphael_display_init(fPanelWidth, fPanelHeight, fRowBytes) != 0;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer v14: alpha-classified tiles with exact bilinear BGRA; display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	if (hakios_display_scan_start()) {
		uint32_t hz;
		const struct hakios_display_scan *scan = hakios_display_scan();
		if (scan && !PE_parse_boot_argn("fb_hz", &hz, sizeof(hz))) gScanHz = (scan->millihz + 500) / 1000;
	}
	publishModes();
	setName("disp0");
	raphael_screenrec_init();
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/* Bit-exact unsigned-byte bilinear interpolation. On this ARM64 kernel,
 * exception entry/return saves/restores all Q registers; normal C calls and
 * context switches follow the AAPCS64 caller/callee convention. Use only v0-v5
 * (caller saved), no FP arithmetic, rounding, saturation or FPCR changes.
 * The caller takes the weight-zero / final-row path before arriving here. */
static inline void
blend_vertical_row(uint32_t *mixed,const uint32_t *upper,const uint32_t *lower,
    uint32_t first,uint32_t last,uint32_t weight)
{
    uint32_t c=first;
#if defined(__arm64__) || defined(__aarch64__)
    unsigned blocks=(last-first+1)/4;
    if(blocks){
        const uint32_t *a=upper+first,*b=lower+first;uint32_t *d=mixed+first;
        unsigned n=blocks,inverse=256-weight;
        __asm__ volatile(
            "dup v4.16b, %w[w]\n"
            "dup v5.16b, %w[i]\n"
            "1:\n"
            "ld1 {v0.16b}, [%[a]], #16\n"
            "ld1 {v1.16b}, [%[b]], #16\n"
            "umull v2.8h, v0.8b, v5.8b\n"
            "umlal v2.8h, v1.8b, v4.8b\n"
            "umull2 v3.8h, v0.16b, v5.16b\n"
            "umlal2 v3.8h, v1.16b, v4.16b\n"
            "shrn v2.8b, v2.8h, #8\n"
            "shrn2 v2.16b, v3.8h, #8\n"
            "st1 {v2.16b}, [%[d]], #16\n"
            "subs %w[n], %w[n], #1\n"
            "b.ne 1b\n"
            : [a] "+r"(a),[b] "+r"(b),[d] "+r"(d),[n] "+r"(n)
            : [w] "r"(weight),[i] "r"(inverse)
            : "v0","v1","v2","v3","v4","v5","cc","memory");
        c+=blocks*4;
    }
#endif
    for(;c<=last;c++){
        uint32_t a=upper[c],b=lower[c];
        mixed[c]=a==b?a:blend(a,b,weight);
    }
}

/* Four output pixels, including zero-weight samples. The final source
 * column stays on the scalar path, so every 64-bit pair load is in bounds. */
static inline uint32_t
blend_horizontal_rows(uint32_t *out,const uint32_t *from,const uint32_t *column,
    const uint16_t *weights,uint32_t n,uint32_t width)
{
    uint32_t i=0;
#if defined(__arm64__) || defined(__aarch64__)
    static const uint8_t replicate[16]={0,0,0,0,2,2,2,2,4,4,4,4,6,6,6,6};
    for(;i+4<=n && column[i+3]+1<width;i+=4){
        const uint32_t *p0=from+column[i],*p1=from+column[i+1],*p2=from+column[i+2],*p3=from+column[i+3];
        uint32_t equal;
        __asm__ volatile(
            "ldr d0, [%[p0]]\n"
            "ldr d1, [%[p1]]\n"
            "ldr d2, [%[p2]]\n"
            "ldr d3, [%[p3]]\n"
            "zip1 v16.4s, v0.4s, v1.4s\n"
            "zip1 v17.4s, v2.4s, v3.4s\n"
            "zip1 v0.2d, v16.2d, v17.2d\n"
            "zip2 v1.2d, v16.2d, v17.2d\n"
            "cmeq v2.4s, v0.4s, v1.4s\n"
            "uminv s2, v2.4s\n"
            "umov %w[eq], v2.s[0]\n"
            "cbz %w[eq], 1f\n"
            "st1 {v0.16b}, [%[out]]\n"
            "b 2f\n"
            "1:\n"
            "ldr d4, [%[weights]]\n"
            "ldr q5, [%[replicate]]\n"
            "tbl v4.16b, {v4.16b}, v5.16b\n"
            "ushll v2.8h, v0.8b, #0\n"
            "ushll v3.8h, v1.8b, #0\n"
            "ushll v6.8h, v4.8b, #0\n"
            "movi v7.8h, #1, lsl #8\n"
            "sub v7.8h, v7.8h, v6.8h\n"
            "mul v2.8h, v2.8h, v7.8h\n"
            "mla v2.8h, v3.8h, v6.8h\n"
            "shrn v2.8b, v2.8h, #8\n"
            "ushll2 v3.8h, v0.16b, #0\n"
            "ushll2 v6.8h, v1.16b, #0\n"
            "ushll2 v16.8h, v4.16b, #0\n"
            "movi v7.8h, #1, lsl #8\n"
            "sub v7.8h, v7.8h, v16.8h\n"
            "mul v3.8h, v3.8h, v7.8h\n"
            "mla v3.8h, v6.8h, v16.8h\n"
            "shrn2 v2.16b, v3.8h, #8\n"
            "st1 {v2.16b}, [%[out]]\n"
            "2:\n"
            : [eq] "=&r"(equal)
            : [p0] "r"(p0),[p1] "r"(p1),[p2] "r"(p2),[p3] "r"(p3),
              [out] "r"(out+i),[weights] "r"(weights+i),[replicate] "r"(replicate)
            : "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","memory");
    }
#endif
    return i;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled, over;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
        uint32_t *panel = (uint32_t *)(fBase + (size_t)y * fRowBytes);
        uint32_t *scratch = job->over && blendRow ? blendRow + fBlendRowSize / 2 : NULL;
        to = scratch ? scratch : panel;
		if (job->unscaled) {
			/* not scaled at all */
            const uint32_t *row = (const uint32_t *)(pixels + (size_t)(sy + y - py0) * stride) + sx;
            if (!job->over) memcpy(to + px0, row, (size_t)sw * 4);
            else minifb_over_row(panel + px0, row, (uint32_t)sw);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

            if (job->over && scratch && !fDiff) {
                const uint32_t *lower=(weight && row+1<(uint32_t)sh) ?
                    (const uint32_t *)(pixels+(size_t)(sy+row+1)*stride)+sx : NULL;
                // Each worker owns independent rows and scratch. Classify before
                // filtering so alpha-zero text margins need no scaling traffic.
                for (int32_t left=cx0; left<cx1; left+=64) {
                    int32_t right=left+64<cx1 ? left+64 : cx1;
                    uint32_t first=fColumn[left], last=fColumn[right-1];
                    if (fColumnWeight[right-1] && last+1<(uint32_t)sw) last++;
                    unsigned alpha=minifb_alpha_span(upper,first,last);
                    if (alpha!=256 && lower && minifb_alpha_span(lower,first,last)!=alpha) alpha=256;
                    if (!alpha) continue;
                    const uint32_t *input=upper;
                    if (lower) {
                        blend_vertical_row(blendRow,upper,lower,first,last,weight);
                        input=blendRow;
                    }
                    uint32_t *output=(alpha==255 ? panel : scratch)+left;
                    const uint32_t *columns=fColumn+left;
                    const uint16_t *weights=fColumnWeight+left;
                    uint32_t n=(uint32_t)(right-left);
                    uint32_t end=blend_horizontal_rows(output,input,columns,weights,n,(uint32_t)sw);
                    for (uint32_t i=end;i<n;i++) {
                        uint32_t c=columns[i], w=weights[i], a=input[c];
                        output[i]=w==0 || a==input[c+1] ? a : blend(a,input[c+1],w);
                    }
                    if (alpha!=255) minifb_over_row(panel+left,scratch+left,n);
                }
                continue;
            }

            if (job->over) {
                uint32_t first=fColumn[cx0], last=fColumn[cx1-1];
                if (fColumnWeight[cx1-1] && last+1<(uint32_t)sw) last++;
                bool transparent=minifb_transparent_span(upper,first,last);
                if (transparent && weight && row+1<(uint32_t)sh) {
                    const uint32_t *lower=(const uint32_t *)(pixels+(size_t)(sy+row+1)*stride)+sx;
                    transparent=minifb_transparent_span(lower,first,last);
                }
                if (transparent) continue;
            }
			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				blend_vertical_row(mixed,upper,lower,first,last,weight);
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

                if (job->over) value = minifb_over(value, panel[x]);
				if (panel[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
        if (job->over && !scratch) {
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) : from[fColumn[x]];
                to[x] = minifb_over(value, to[x]);
            }
            continue;
        }
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

            uint32_t n=(uint32_t)(cx1-cx0);
            uint32_t vectorEnd=blend_horizontal_rows(out,from,column,weights,n,(uint32_t)sw);
			for (uint32_t i = vectorEnd; i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}        if (scratch) minifb_over_row(panel + cx0, scratch + cx0, (uint32_t)(cx1-cx0));
	}
}

void
IOMobileFramebuffer::drawShared(const RowJob *job,uint32_t *blend)
{
    for(;;){
        IOLockLock(fHelperLock);
        int32_t y0=fNextRow,y1=y0+64;
        if(y1>job->py1)y1=job->py1;
        if(y0>=job->py1){IOLockUnlock(fHelperLock);break;}
        fNextRow=y1;
        IOLockUnlock(fHelperLock);
        drawRows(job,y0,y1,blend);
    }
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;

		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
        /* Enroll under the same lock that protects job closure. Helpers which
         * wake after all rows were claimed never retain a pointer to the job. */
        bool participate=job!=NULL && fb->fNextRow<job->py1;
        if(participate)fb->fHelpersBusy++;
		IOLockUnlock(fb->fHelperLock);
        if(!participate)continue;
		/* share index + 1 of fThreads (the presenter takes share 0) */
        blend = fb->fHelperBlend[index];
        fb->drawShared(job,blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool over)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter == 1 || (gFilter == 2 && fSettling);
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (int64_t) sx + sw > s->info.width || (int64_t) sy + sh > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) (((int64_t) dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) (((int64_t) dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	uint32_t rowCapacity = (uint32_t)sw > fPanelWidth ? (uint32_t)sw : fPanelWidth;
    uint32_t rowStorage = rowCapacity * 2; /* vertical source + horizontal output, per worker */
    if ((filter || over) && fBlendRowSize < rowStorage) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = rowStorage;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = px1 > (int32_t) fPanelWidth ? (int32_t) fPanelWidth : px1;
	if (cx0 >= cx1 || py1 <= 0 || py0 >= (int32_t)fPanelHeight) return;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.over = over;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
    fNextRow=py0;
	fHelperJob = &job;
	fHelpersBusy = 0;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
    drawShared(&job,fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
    /* No enrolled helper can still read the stack job or its column/scratch
     * inputs. A late wake observes NULL (or the next generation) under lock. */
    fHelperJob=NULL;
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

    /* SwapBegin zeroes its request. Only set/clear bits change a plane.
     * Queue a resolved snapshot so later requests cannot mutate this frame. */
    bool resolved = minifb_update_layers(fLayerState, fLayerSurfaces, swap,
        [](uint32_t sid) { return MiniIOSurfaceCopy(sid); },
        [](MiniIOSurface *surface) { MiniIOSurfaceDrop(surface); });
    if (!resolved && fStatLost++ < 8)
        IOLog("MiniIOMobileFramebuffer: missing layer surface; retaining accepted layout\n");
    memcpy(fSwap[slot], swap, SWAP_SIZE);
    for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
        minifb_layer_fields(fSwap[slot], fLayerState, layer);
        uint32_t sid = swapWord(fLayerState, SWAP_SURFACE(layer));
        fSwapSurfaces[slot][layer] = sid ? MiniIOSurfaceCopy(sid) : NULL;
    }
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];
	MiniIOSurface *surfaces[SWAP_LAYERS] = { NULL };
	bool unsettled = false;
	uint64_t deadline = 0;
	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			if (!unsettled) {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
				continue;
			}
			if (IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && fSwapsWaiting == 0) {
				IOLockUnlock(fSwapLock);
				settle(swap, surfaces);
				unsettled = false;
				for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
					if (surfaces[layer] != NULL) MiniIOSurfaceDrop(surfaces[layer]);
					surfaces[layer] = NULL;
				}
				IOLockLock(fSwapLock);
			}
		}
		IOLockUnlock(fSwapLock);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) MiniIOSurfaceDrop(surfaces[layer]);
			surfaces[layer] = NULL;
		}
		IOLockLock(fSwapLock);
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		unsettled = show(swap, surfaces) && gFilter == 2;
		clock_interval_to_deadline(SETTLE_MS, NSEC_PER_MSEC, &deadline);
		if (!unsettled) {
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				if (surfaces[layer] != NULL) MiniIOSurfaceDrop(surfaces[layer]);
				surfaces[layer] = NULL;
			}
		}
		MiniIOSurfaceClearKept();
	}
}

/* A settle is not a new swap: do not acknowledge it or advance the queue.
 * Preserve the Raphael scanout -> frame lock order and render to the back
 * buffer, copying the current front for partial/absent layers before flipping.
 * Skip a stale settle if another swap arrives or its GPU work is unfinished. */
void
IOMobileFramebuffer::settle(const uint8_t *swap, MiniIOSurface **surfaces)
{
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], 0)) return;
	}
	IOLockLock(gScanoutLock);
	IOLockLock(fLock);
	IOLockLock(fSwapLock);
	bool pending = fSwapsWaiting != 0;
	IOLockUnlock(fSwapLock);
	if (!fGraphics || pending) {
		IOLockUnlock(fLock);
		IOLockUnlock(gScanoutLock);
		return;
	}
	if (fPageFlip) {
		uint8_t *back = (uint8_t *)raphael_display_back(false);
		if (back == NULL) {
			IOLockUnlock(fLock);
			IOLockUnlock(gScanoutLock);
			return;
		}
		fBase = back;
	}
	uint8_t *scanoutBase=fBase;
	fBase=raphael_screenrec_render_begin(fBase,fPanelWidth,fPanelHeight,fRowBytes);
	for (uint32_t y = 0; y < fPanelHeight; y++) bzero(fBase + (size_t)y * fRowBytes, (size_t)fPanelWidth * 4);
	bool composed = false;
	fSettling = true;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		MiniIOSurface *s = surfaces[layer];
		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0) continue;
		int32_t src[4], dst[4];
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		drawLayer(s, src, dst, composed && s->info.pixelFormat == FOURCC('B','G','R','A'));
			composed = true;
	}
	fSettling = false;
	if(fBase!=scanoutBase)memcpy(scanoutBase,fBase,(size_t)fRowBytes*fPanelHeight);
	raphael_screenrec_present(fBase,fPanelWidth,fPanelHeight,fRowBytes);
	fBase=scanoutBase;
	if (fPageFlip && !raphael_display_present()) {
		fPageFlip = false;
		fBase = (uint8_t *)vinfo.v_baseaddr;
	}
	IOLockUnlock(fLock);
	IOLockUnlock(gScanoutLock);
}


/* Bounded local diagnostics. Called by the sole presenter after the GPU wait.
 * The surface references are retained by the swap. No pixels or request fields
 * are changed, and no application or surface identity selects behavior. */
extern "C" void loader_log_line(const char *line);
static void
trace_display_layout(const uint8_t *swap, MiniIOSurface **surfaces)
{
    static uint64_t lastSignature, lastTime;
    static uint32_t reports;
    if (reports >= 128) return;
    uint64_t signature = 1469598103934665603ULL;
    auto word = [&](uint32_t value) { signature ^= value; signature *= 1099511628211ULL; };
    for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
        MiniIOSurface *s = surfaces[layer];
        word(s != NULL);
        if (!s) continue;
        word(s->info.width); word(s->info.height); word(s->info.pixelFormat);
        word(s->info.bytesPerElement); word(s->info.planeCount);
        for (unsigned n = 0; n < 4; n++) {
            word(swapWord(swap, SWAP_SOURCE(layer) + 4 * n));
            word(swapWord(swap, SWAP_DEST(layer) + 4 * n));
        }
    }
    uint64_t now = mach_absolute_time(), nanos = 0;
    absolutetime_to_nanoseconds(now - lastTime, &nanos);
    if (reports && nanos < 500 * NSEC_PER_MSEC) return;
    if (reports && signature == lastSignature && nanos < 10 * NSEC_PER_SEC) return;
    lastSignature = signature; lastTime = now; reports++;
    char line[384];
    snprintf(line, sizeof(line), "metal-display-v14: sample=%u swap=%u set=%#x clear=%#x\n",
        reports, swapWord(swap, SWAP_ID), swapWord(swap, SWAP_LAYERS_SET), swapWord(swap, SWAP_LAYERS_CLEARED));
    loader_log_line(line);
    for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
        MiniIOSurface *s = surfaces[layer];
        if (!s) continue;
        int32_t src[4], dst[4];
        memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
        memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
        snprintf(line, sizeof(line),
            "metal-display-v14: sample=%u layer=%u sid=%u %ux%u fmt=%#x bpe=%u planes=%u row=%u seed=%u tiled=%u drawn=%u src=%d,%d,%d,%d dst=%d,%d,%d,%d enabled=%#x accepted32=%u\n",
            reports, layer, s->sid, s->info.width, s->info.height, s->info.pixelFormat,
            (unsigned)s->info.bytesPerElement, s->info.planeCount, s->info.bytesPerRow,
            s->shared()->seed, (unsigned)s->shared()->tiled, s->drawn,
            src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3],
            swapWord(swap, SWAP_ENABLED(layer)), (unsigned)(s->info.bytesPerElement == 4 && s->info.planeCount == 0));
        loader_log_line(line);
        for (unsigned plane = 0; plane < s->info.planeCount && plane < MINISURF_MAX_PLANES; plane++) {
            const minisurf_plane &p = s->planes[plane];
            snprintf(line, sizeof(line), "metal-display-v14: sample=%u layer=%u plane=%u %ux%u row=%u offset=%u bytes=%u bpe=%u compression=%u\n",
                reports, layer, plane, p.width, p.height, p.bytesPerRow, p.offset, p.size,
                (unsigned)p.bytesPerElement, (unsigned)p.compression);
            loader_log_line(line);
        }
    }
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
bool
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	bool complete = true;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; frame dropped\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
			complete = false;
		}
	}
	if (!complete) {
		IOLockLock(fSwapLock);
		fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
		fSwapsWaiting--;
		fSwapCompleted = swapWord(swap, SWAP_ID);
		IOLockWakeup(fSwapLock, &fSwapCompleted, true);
		IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
		IOLockUnlock(fSwapLock);
		return false;
	}
	trace_display_layout(swap, surfaces);
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat && !fPageFlip) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
    uint64_t stages[PROFILE_PARTS] = {};
	IOLockLock(gScanoutLock);
	if (fPageFlip) {
		fBase = (uint8_t *)raphael_display_back(false);
		if (fBase == NULL) {
			fPageFlip = false;
			fBase = (uint8_t *)vinfo.v_baseaddr;
		}
	}
	IOLockLock(fLock);
    uint64_t stage=mach_absolute_time();
    stages[PROFILE_LOCK]=stage-began;
	uint8_t *scanoutBase=fBase;
	fBase=raphael_screenrec_render_begin(fBase,fPanelWidth,fPanelHeight,fRowBytes);
    stages[PROFILE_PREP]=mach_absolute_time()-stage;
    stage=mach_absolute_time();
	for (uint32_t y = 0; y < fPanelHeight; y++) bzero(fBase + (size_t)y * fRowBytes, (size_t)fPanelWidth * 4);
    stages[PROFILE_CLEAR]=mach_absolute_time()-stage;
	bool composed = false;
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
            stage=mach_absolute_time();
			drawLayer(s, src, dst, composed && s->info.pixelFormat == FOURCC('B','G','R','A'));
            stages[PROFILE_LAYER0+layer]=mach_absolute_time()-stage;
			composed = true;
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
    stage=mach_absolute_time();
	if(fBase!=scanoutBase)memcpy(scanoutBase,fBase,(size_t)fRowBytes*fPanelHeight);
    stages[PROFILE_COPY]=mach_absolute_time()-stage;
    stage=mach_absolute_time();
	raphael_screenrec_present(fBase,fPanelWidth,fPanelHeight,fRowBytes);
	fBase=scanoutBase;
	if (fPageFlip && !raphael_display_present()) {
		fPageFlip = false;
		fBase = (uint8_t *)vinfo.v_baseaddr;
	}
    stages[PROFILE_PRESENT]=mach_absolute_time()-stage;
    stage=mach_absolute_time();
	IOLockUnlock(gScanoutLock);
	/* Release the surface only after the complete frame has reached scanout. */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	fSwapCompleted = swapWord(swap, SWAP_ID);
	IOLockWakeup(fSwapLock, &fSwapCompleted, true);
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);

	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
    stages[PROFILE_POST]=mach_absolute_time()-stage;
    unsigned group=(layers & (layers-1)) ? 1 : 0;
    fComposeFrames[group]++;
    for (unsigned part=0;part<PROFILE_PARTS;part++) fComposeTicks[group][part]+=stages[part];
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
        /* Bounded aggregate timings only while multiple display planes are active. */
        static uint32_t compositionReports;
        if ((layers & (layers - 1)) && compositionReports < 24) {
            char line[256];
            snprintf(line, sizeof(line), "metal-compose-v14: sample=%u frames=%u window_ms=%llu fps=%llu driver_us=%llu gpu_wait_us=%llu held=%u lost=%u layers=%#x\n",
                ++compositionReports, fStatFrames, nanos/NSEC_PER_MSEC,
                fStatFrames*NSEC_PER_SEC/nanos, drawing/fStatFrames/NSEC_PER_USEC,
                waiting/fStatFrames/NSEC_PER_USEC, fStatHeld, fStatLost, layers);
            loader_log_line(line);
        }
        for (unsigned group=0;group<2;group++) {
            uint32_t frames=fComposeFrames[group];
            if (frames && fComposeGroupReports[group]<32) {
                fComposeGroupReports[group]++;
                uint64_t us[PROFILE_PARTS];
                for (unsigned part=0;part<PROFILE_PARTS;part++) {
                    absolutetime_to_nanoseconds(fComposeTicks[group][part],&us[part]);
                    us[part]/=frames*NSEC_PER_USEC;
                }
                char line[512];
                snprintf(line,sizeof(line),"metal-stages-v14: sample=%u multi=%u frames=%u lock_us=%llu prep_us=%llu clear_us=%llu layer0_us=%llu layer1_us=%llu layer2_us=%llu layer3_us=%llu copy_us=%llu present_us=%llu post_us=%llu\n",
                    ++fComposeReports,group,frames,us[0],us[1],us[2],us[3],us[4],us[5],us[6],us[7],us[8],us[9]);
                loader_log_line(line);
            }
            fComposeFrames[group]=0;
            for (unsigned part=0;part<PROFILE_PARTS;part++) fComposeTicks[group][part]=0;
        }
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
	return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	if (hakios_display_scan() != NULL) {
		uint64_t ahead;
		nanoseconds_to_absolutetime(hakios_display_next_frame_ns(), &ahead);
		fVSyncTime = now + (ahead ? ahead : period);
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait: {                       /* do not let the producer overwrite a surface being copied */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		if (args->scalarInputCount < 1) return kIOReturnBadArgument;
		uint64_t deadline;
		clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
		IOLockLock(fOwner->fSwapLock);
		while ((int32_t)((uint32_t)in[0] - fOwner->fSwapCompleted) > 0) {
			if (IOLockSleepDeadline(fOwner->fSwapLock, &fOwner->fSwapCompleted,
			    deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
			    (int32_t)((uint32_t)in[0] - fOwner->fSwapCompleted) > 0) {
				IOLockUnlock(fOwner->fSwapLock);
				return kIOReturnTimeout;
			}
		}
		IOLockUnlock(fOwner->fSwapLock);
		return kIOReturnSuccess;
	}
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
static bool raphaelGLESClientV24(const char *name) {
    return name && (!strcmp(name,"bili-overseas") || !strcmp(name,"raphael-metal-video-probe"));
}
class IOAcceleratorES : public IOService
{
    OSDeclareDefaultStructors(IOAcceleratorES);
    bool glesPropertyVisible(const char *key) const {
        if (!key || strcmp(key,"IOGLESMetalBundleName")) return true;
        char name[64]={}; proc_selfname(name,sizeof(name));
        return raphaelGLESClientV24(name);
    }
public:
    using IOService::getProperty;
    OSObject *getProperty(const char *key) const APPLE_KEXT_OVERRIDE {
        return glesPropertyVisible(key) ? IOService::getProperty(key) : NULL;
    }
    OSObject *getProperty(const OSString *key) const APPLE_KEXT_OVERRIDE {
        return glesPropertyVisible(key ? key->getCStringNoCopy() : NULL) ? IOService::getProperty(key) : NULL;
    }
    OSObject *getProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE {
        return glesPropertyVisible(key ? key->getCStringNoCopy() : NULL) ? IOService::getProperty(key) : NULL;
    }
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
        /* This isolated probe build defaults on; raphael_metal=0 can disable it. */
        uint32_t metal = 2;
        (void)PE_parse_boot_argn("raphael_metal", &metal, sizeof(metal));
        if (metal >= 2) {
            accelerator->setProperty("MetalPluginName", "RaphaelMetal2");
            accelerator->setProperty("MetalPluginClassName", "SweetMTL2Device");
            accelerator->setProperty("IOGLESMetalBundleName", "AppleMetalGLRenderer");
            accelerator->setProperty(kIOUserClientClassKey, "RaphaelNativeGPUUserClient");
        }
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	if (gScanoutLock != NULL) {
		IOLockLock(gScanoutLock);
	}
	uint8_t *back = (uint8_t *)raphael_display_back(width < vinfo.v_width || height < vinfo.v_height);
	uint8_t *base = back != NULL ? back : (uint8_t *)vinfo.v_baseaddr;
	for (uint32_t y = 0; y < rows; y++) {
		memcpy(base + (size_t) y * vinfo.v_rowbytes, pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
	if (back != NULL) {
		raphael_display_present();
	}
	if (gScanoutLock != NULL) {
		IOLockUnlock(gScanoutLock);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
		uint32_t level;
		if (!hakios_display_brightness_get(&level)) {
			AppleARMBacklight *bl = OSTypeAlloc(AppleARMBacklight);
			if (bl && bl->init() && bl->attach(fb) && !bl->start(fb)) bl->detach(fb);
			OSSafeReleaseNULL(bl);
		}
	} else {
		fb->detach(provider);
	}
	fb->release();
}
