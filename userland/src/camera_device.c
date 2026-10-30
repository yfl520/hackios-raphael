/*
 * The phone's cameras as iOS's capture stack wants them: a FigCaptureDevice with two FigCaptureStreams,
 * the wide camera as the back camera and the front one as the front camera, made for mediaserverd's
 * BWFigCaptureDeviceVendor in place of the device Apple's H10ISP.mediacapture makes of the iPhone's ISP
 * (part of libhakios.media.dylib; inject_media.c gives the vendor our device's create function when
 * the kernel has a camera: cam=5).
 *
 * Both are CoreMedia "derived objects" (CMDerivedObjectCreate): a table of functions for the base
 * class (properties) and one for the device or stream class, as H10ISP's are laid out in iOS 16.5:
 *   device  RequestStreams, RelinquishStreams (and a processing session and Pearl calibration, not here)
 *   stream  Start, Stop, AddAttachmentsToSampleBuffer
 * A stream's frames go to the capture graph through the blocks it is given in its property
 * "VideoOutputHandlers" ({ output: { PixelBufferHandler: ^(CVPixelBufferRef, CMTime) } }), for the
 * outputs "VideoOutputConfigurations" describes (the graph's own buffer pools) and "VideoOutputsEnabled"
 * switches on. A frame: read from the kernel's /dev/hakios-camera (RAW10), made into a picture by
 * camera_isp.c (exposure, white balance kept to the sensor's reference lights, the colour matrix and
 * lens shading of the camera's own tuning, gamma, zoom), one pixel buffer for each output, with the
 * metadata the graph keeps its frame statistics and its zoom by. The kernel gives one sensor's frames
 * at a time: a stream that starts stops the other's frames and has the kernel change sensors.
 *
 * The formats the streams offer are named as the iPhone X's cameras' are in CMCapture's
 * D22/AVCaptureSession.plist (FigCaptureStreamFormats, by the sensor sizes and frame rates the plist
 * matches them by), so that their AVCaptureDevice formats and session presets resolve: the pictures
 * are scaled from the sensor's frames to whatever size is asked for.
 *
 * The shutter: StillImageCaptureNow asks for one frame of the whole sensor on the StillImage output
 * (the time machine's TimeMachineSuspendNow/ResumeNow are taken and ignored). Focus, where there is
 * a lens motor (the wide camera's): by contrast, continuous, and on the window a tap gives (af_frame()).
 *
 * Everything asked of the device and the streams that is not answered here is said in mediaserverd's
 * log ("hakios: camera: ..."), once per property.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"
#include "camera_prefetch.h"

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

void
hakios_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "hakios: camera: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}
#define say hakios_camera_say

/* Diagnostic intervals only: do not change the sensor/graph clock or pacing. */
static double
camera_interval_ms(uint64_t begin, uint64_t end)
{
	mach_timebase_info_data_t tb;

	if (end < begin || mach_timebase_info(&tb) != 0 || tb.denom == 0) {
		return -1;
	}
	return (double)(end - begin) * tb.numer / tb.denom / 1e6;
}

/* ---- CoreMedia's derived objects, as iOS 16.5 lays them out ---------------------------------------- */

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;

struct base_class {
	uint64_t        version;
	uint64_t        storage_size;
	Boolean         (*equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus        (*invalidate)(CMBaseObjectRef);
	void            (*finalize)(CMBaseObjectRef);
	CFStringRef     (*copy_debug_description)(CMBaseObjectRef);
	OSStatus        (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus        (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus        (*notification_barrier)(CMBaseObjectRef);
	const void *    protocols;
	const void *    reserved;
};
struct device_class {
	uint64_t        version;                /* 3 */
	void *          processing_session_create;
	OSStatus        (*request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	OSStatus        (*relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	void *          pearl_calibration;
};
struct stream_class {
	uint64_t        version;                /* 2 */
	OSStatus        (*start)(CMBaseObjectRef);
	OSStatus        (*stop)(CMBaseObjectRef);
	OSStatus        (*add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct vtable {
	const void *    reserved;
	const struct base_class *base;
	const void *    derived;
};

#define ERR_PARAM               (-12780)
#define ERR_NOT_AVAILABLE       (-12783)
#define ERR_UNSUPPORTED         (-12782)

static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CFStringRef port_back;           /* kFigCapturePortType_BackFacingCamera */
static CFStringRef port_front;          /* kFigCapturePortType_FrontFacingCamera */

/*
 * The frames' metadata: what H10ISP's stream attaches to every frame (a dictionary under
 * kFigCaptureSampleBufferAttachmentKey_MetadataDictionary, through the stream's
 * AddAttachmentsToSampleBuffer when the graph defers it), and what the capture device keeps its frame
 * statistics of. -[BWFrameStatisticsByPortType updateWithFrameMetadata:updateFocusDistance:] counts
 * a frame only for a buffer that has it, and -[BWFigVideoCaptureDevice
 * _stillImageCaptureInProgressSubstateForFrameStatisticsByPortType:...] puts a standard photograph
 * off until more than four frames have been counted (then only while AFStatus is 2, a coarse focus
 * scan): without it the shutter's capture was put off on every frame (the retry in
 * _updateStateUsingVideoSampleBuffer:fromCaptureStream:), for ever, and StillImageCaptureNow never
 * came. The dictionary goes on the pixel buffer too, and mutable, as the ISP's does: FigImageControl_Focus
 * (the device's camera controls, on every frame once the sample buffer has metadata) reads it from
 * the pixel buffer with no check that it is there -- CFDictionaryGetValue(NULL, ...): mediaserverd
 * died of SIGSEGV -- and adds its own keys to it. The keys are CoreMedia's; the values come from the
 * frame's header and our ISP's levels, in Apple's units where they are known (the exposure in seconds,
 * gains with 256 for 1x); lux and SNR are rough.
 */
enum {
	MD_PORT, MD_PTS, MD_EXPOSURE, MD_AGC, MD_ISP_DGAIN, MD_SENSOR_DGAIN, MD_ISO_BASE, MD_FRAME_RATE, MD_LUX, MD_SNR,
	MD_AF_STATUS, MD_FOCUSING, MD_AE_STABLE, MD_AE_LOCKED, MD_AWB_STABLE, MD_AWB_LOCKED, MD_AF_LOCKED, MD_ZOOM, MD_KEYS
};
static const char *const md_names[MD_KEYS] = {
	"kFigCaptureStreamMetadata_PortType", "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp",
	"kFigCaptureStreamMetadata_ExposureTime", "kFigCaptureStreamMetadata_AGC", "kFigCaptureStreamMetadata_ispDGain",
	"kFigCaptureStreamMetadata_sensorDGain", "kFigCaptureStreamMetadata_ISOBase", "kFigCaptureStreamMetadata_CurrentFrameRate",
	"kFigCaptureStreamMetadata_LuxLevel", "kFigCaptureStreamMetadata_NormalizedSNR", "kFigCaptureStreamMetadata_AFStatus",
	"kFigCaptureStreamMetadata_FocusingMethod", "kFigCaptureStreamMetadata_AEStable", "kFigCaptureStreamMetadata_AELocked",
	"kFigCaptureStreamMetadata_AWBStable", "kFigCaptureStreamMetadata_AWBLocked", "kFigCaptureStreamMetadata_AFLocked",
	"kFigCaptureStreamMetadata_ZoomFactor",
};
static CFStringRef md_key[MD_KEYS];
static CFStringRef md_attachment;       /* kFigCaptureSampleBufferAttachmentKey_MetadataDictionary */

static bool
find_functions(void)
{
	CFStringRef *port;

	derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
	stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
	port_back = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
	port_front = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary");
	md_attachment = port != NULL ? *port : NULL;
	for (int i = 0; i < MD_KEYS; i++) {
		port = dlsym(RTLD_DEFAULT, md_names[i]);
		md_key[i] = port != NULL ? *port : NULL;
		if (md_key[i] == NULL) {
			say("%s is not there: the frames' metadata go without it", md_names[i]);
		}
	}
	if (md_attachment == NULL || md_key[MD_PORT] == NULL) {
		say("kFigCaptureSampleBufferAttachmentKey_MetadataDictionary or the port's key is not there: frames without "
		    "metadata, and the shutter will wait for ever");
		md_attachment = NULL;
	}
	if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL ||
	    port_front == NULL) {
		say("CoreMedia's or CMCapture's functions are not there: no camera");
		return false;
	}
	return true;
}

/* ---- what is asked and not answered: said once per name ------------------------------------------- */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say_value(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[300] = "";

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	say("%s %s%s%s", what, name, value ? " = " : "", text);
}

static void
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (first) {
		say_value(what, key, value);
	}
}

/* ---- numbers and dictionaries ---------------------------------------------------------------------- */

static CFNumberRef
number_i(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
number_f(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	CFDictionarySetValue(d, k, value);
	CFRelease(k);
	CFRelease(value);
}

static CFDictionaryRef
new_dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static bool
key_is(CFStringRef key, const char *name)
{
	char buffer[128];

	return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}

/* ---- what the device and the stream say they have ---------------------------------------------------- */

/*
 * The capture stack asks only for the properties an object lists in its "SupportedPropertiesDictionary"
 * ({ name: { PropertyType, ReadWriteStatus } }). Listed here: what is answered, and what is taken
 * (and, for now, mostly not acted on) without failing the graph that sets it.
 */
struct prop {
	const char *name, *type, *access;
};

static const struct prop stream_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "SupportedFormatsArray", "Collection", "ReadOnly" }, { "FocalLength", "Number", "ReadOnly" },
	{ "LensFNumber", "Number", "ReadOnly" }, { "PixelSize", "Number", "ReadOnly" },
	{ "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
	{ "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
	{ "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
	{ "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" }, { "TorchLevel", "Number", "ReadWrite" },
	{ "HighCurrentTorchEnabled", "Boolean", "ReadWrite" }, { "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
	{ "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "Number", "ReadWrite" },
	{ "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
	{ "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
	{ "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
	{ "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadWrite" },
	{ "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
	{ "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
	{ "TimeMachineSuspendNow", "Boolean", "WriteOnly" }, { "TimeMachineResumeNow", "Boolean", "WriteOnly" },
	/* focus (the wide camera's lens motor; see af_frame()) */
	{ "AutoFocusNow", "Collection", "WriteOnly" }, { "ContinuousAutoFocusNow", "Collection", "WriteOnly" },
	{ "FocusNow", "Collection", "WriteOnly" }, { "PeakTrackingFocusNow", "Collection", "WriteOnly" },
	{ "LockAFNow", "Boolean", "WriteOnly" }, { "ManualFocusNow", "Collection", "WriteOnly" },
	{ "AFWindowParams", "Collection", "ReadWrite" }, { "AutoFocusMode", "Number", "ReadWrite" },
}, device_props[] = {
	{ "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
	{ "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
};

static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
	CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();

	for (size_t i = 0; i < n; i++) {
		CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();

		put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
		put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
		put(all, p[i].name, one);
	}
	return all;
}

/* ---- the formats ----------------------------------------------------------------------------------- */

#define PF_420F         0x34323066      /* '420f' */
#define PF_420V         0x34323076      /* '420v' */
#define PF_BGRA         0x42475241      /* 'BGRA' */

static CFDictionaryRef
output_info(int width, int height)
{
	CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
	CFNumberRef pf[3] = { number_i(PF_420F), number_i(PF_420V), number_i(PF_BGRA) };
	CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 3, &kCFTypeArrayCallBacks);

	for (int i = 0; i < 3; i++) {
		CFRelease(pf[i]);
	}
	put(o, "SupportedPixelFormats", pfs);
	put(o, "NativeWidth", number_i(width));
	put(o, "NativeHeight", number_i(height));
	put(o, "MaxDownscaleFactor", number_f(32));
	put(o, "MaxScaleFactor", number_f(1));
	put(o, "MinScaleFactor", number_f(1.0 / 32));
	put(o, "OutputWidthAlignmentRequirement", number_i(16));
	return o;
}

static CFDictionaryRef
format(int sensor_width, int sensor_height, int max_fps, bool binned, double fov)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict(), outputs = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, sensor_width, sensor_height, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(2));
	put(f, "VideoMaxFrameRate", number_f(max_fps));
	put(f, "VideoIsBinned", CFRetain(binned ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "VideoBinningFactorHorizontal", number_i(binned ? 2 : 1));
	put(f, "VideoBinningFactorVertical", number_i(binned ? 2 : 1));
	put(f, "VideoFieldOfView", number_f(fov));
	put(f, "VideoMinIntegrationTime", number_i(100));
	put(f, "VideoMaxIntegrationTime", number_i(1000000 / 15));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(sensor_width));
	put(f, "VideoMaxHeight", number_i(sensor_height));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(sensor_width));
	put(f, "SensorHeight", number_i(sensor_height));
	put(f, "ISOBase", number_f(32));
	put(f, "DefaultAEMaxGain", number_f(64));

	/*
	 * The sensor crop geometry the capture-graph builder reads off this format to resolve non-zero
	 * output dimensions (confirmed by a hook: -[FigCaptureSourceVideoFormat sensorCropDimensions] /
	 * postGDCCropDimensions] return these). Everything spans the full sensor: no GDC crop.
	 */
	put(f, "SensorCropWidth", number_i(sensor_width));
	put(f, "SensorCropHeight", number_i(sensor_height));
	put(f, "PreGDCSensorCropWidth", number_i(sensor_width));
	put(f, "PreGDCSensorCropHeight", number_i(sensor_height));
	put(f, "PostGDCCropWidth", number_i(sensor_width));
	put(f, "PostGDCCropHeight", number_i(sensor_height));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(sensor_width));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(sensor_height));

	put(outputs, "PrimaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "SecondaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "StillImage", output_info(sensor_width, sensor_height));
	put(f, "SupportedOutputs", outputs);
	return f;
}

/* ---- the stream ------------------------------------------------------------------------------------- */

#define HELD    3               /* the moving pictures kept from the pool after they are given (see deliver) */

struct output {
	const char *    name;
	CFDictionaryRef config;                 /* from VideoOutputConfigurations */
	bool            enabled;
	void *          handler;                /* a block: ^(CVPixelBufferRef, CMTime) */
	bool            said_shape;
	uint64_t        delivered, refused;
	uint64_t        rate_frames, rate_first, rate_last; /* actual successful output callbacks */
	CVPixelBufferRef held[HELD];            /* the pictures given last, kept from the pool (see deliver) */
	unsigned        held_next;
	bool            said_pool;
};

static void let_go(struct output *out);

#define OUTPUTS 3
static const char *const output_names[OUTPUTS] = { "PrimaryScaler", "SecondaryScaler", "StillImage" };

#define MAX_ZOOM        16              /* (the most the iPhone X's wide camera zooms digitally) */

/*
 * The device's cameras, a stream each, as the iPhone X's are in CMCapture's D22/AVCaptureSession.plist
 * (its devices' uniqueIDs, and the stream formats its formats and presets are matched to by sensor size
 * and frame rate): the wide camera as the back camera, the front one as the front camera. The kernel
 * gives one sensor's frames at a time; a stream's frames start with its own sensor's (see worker()).
 */
static const struct camera {
	uint32_t        sensor;         /* the kernel's (the frames' sensor, HAKIOS_CAMERA_SENSOR) */
	const char      *name, *unique_id;
	bool            turned;         /* its sensor the other way up from the iPhone's: the pictures turned half round */
	double          focal_length, f_number, pixel_size, fov;
	int             formats[2][3];  /* sensor width, height, frames a second of "Photo" and of a video format */
} cameras[2] = {
	{ 1, "Back Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:0", false, 4.0, 1.8, 1.22, 65.0,
	  { { 4032, 3024, 30 }, { 4224, 2376, 60 } } },        /* "Photo", "Video-60" */
	{ 2, "Front Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:1", true, 2.87, 2.2, 1.0, 70.0,
	  { { 3088, 2320, 30 }, { 3840, 2160, 30 } } },        /* "Photo", "Video-30" */
};

struct stream {
	CFArrayRef      formats;
	CFDictionaryRef supported;
	CFDictionaryRef colour_info;    /* from VideoOutputsColorInfo */
	CFDictionaryRef metadata;       /* the last frame's (frame_metadata()), for a buffer that has none of its own */
	pthread_mutex_t metadata_lock;  /* (not `lock': AddAttachmentsToSampleBuffer may come while a frame is given out) */
	unsigned        stills_wanted;  /* photographs asked for by StillImageCaptureNow, not yet given */
	int             format_index;
	double          min_fps, max_fps;
	float           zoom;           /* ZoomFactor: the picture is the middle 1/zoom of the sensor each way */
	const struct camera *camera;
	CFStringRef     port;
	bool            af_now;         /* a focus asked for (AutoFocusNow and the like): a search at the next frame */
	bool            af_locked;      /* LockAFNow: no search but one asked for */
	float           af_window[4];   /* AFWindowParams: where to focus, as fractions of the frame (x0, y0, x1, y1) */
	struct output   out[OUTPUTS];
	pthread_mutex_t lock;
	pthread_t       worker;
	volatile bool   running;
	volatile bool   graph_running;  /* the capture graph has started this stream: frames may be given out */
	bool            worker_started;
	int             torch;
	uint64_t        frames, stills_given;
};

static struct stream *
stream_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static int
output_index(CFStringRef name)
{
	for (int i = 0; i < OUTPUTS; i++) {
		if (key_is(name, output_names[i])) {
			return i;
		}
	}
	return -1;
}

static bool
raphael_rear_only(void)
{
	return access("/redwood/fw/raphael-camera-rear-only", F_OK) == 0;
}

static bool
raphael_front_enabled(void)
{
    return access("/redwood/fw/raphael-camera-front-ready", F_OK) == 0;
}

/* Keep the graph's three pieces of output state separate in the log: a configured
 * pool, its handler, and the enabled bit must all arrive before a frame can be
 * handed to Camera.app. CFCopyDescription() of the graph's configuration is too
 * long for say_value()'s diagnostic buffer and used to appear empty. */
static void
output_state(const char *when, const struct output *out)
{
	CFTypeRef pool = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;
	CFIndex fields = out->config != NULL ? CFDictionaryGetCount(out->config) : 0;
	CFTypeID pool_type = pool != NULL ? CFGetTypeID(pool) : 0;
	bool valid_pool = pool != NULL && pool_type == CVPixelBufferPoolGetTypeID();

	say("%s %s: handler=%u config=%u fields=%ld pool=%s enabled=%u", when, out->name, out->handler != NULL,
	    out->config != NULL, (long)fields, valid_pool ? "CVPixelBufferPool" : pool != NULL ? "wrong-type" : "none",
	    out->enabled);
}

static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct stream *s = stream_of(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "PortType")) {
		value = CFRetain(s->port);
	} else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
		value = CFStringCreateWithCString(NULL, s->camera->unique_id, kCFStringEncodingUTF8);
	} else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
		value = CFStringCreateWithCString(NULL, s->camera->name, kCFStringEncodingUTF8);
	} else if (key_is(key, "Manufacturer")) {
		value = CFRetain(CFSTR("Apple Inc."));
	} else if (key_is(key, "SupportedFormatsArray")) {
		value = CFRetain(s->formats);
	} else if (key_is(key, "FormatIndex")) {
		value = number_i(s->format_index);
	} else if (key_is(key, "MinimumFrameRate")) {
		value = number_f(s->min_fps);
	} else if (key_is(key, "MaximumFrameRate")) {
		value = number_f(s->max_fps);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "TorchLevel") || key_is(key, "ExposureBias")) {
		value = number_f(0);
	} else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
		/*
		 * ReadWrite, but the capture-graph builder READS these back while it builds the graph
		 * (BWMultiStreamCameraSourceNode's _updateOutputsStorage does a getProperty: for
		 * VideoOutputConfigurations, not IfSupported:), and a failed read tears the graph down. Return
		 * what each output holds now.
		 */
		bool configs = key_is(key, "VideoOutputConfigurations");
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < OUTPUTS; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);

			if (configs) {
				if (s->out[i].config != NULL) {
					CFDictionarySetValue(d, name, s->out[i].config);
				}
			} else {
				CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
			}
			CFRelease(name);
		}
		pthread_mutex_unlock(&s->lock);
		if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
			say("stream: read back %s with %ld outputs", key_is(key, "VideoOutputConfigurations") ?
			    "VideoOutputConfigurations" : "VideoOutputsEnabled", (long)CFDictionaryGetCount(d));
		}
		value = d;
	} else if (key_is(key, "VideoOutputsColorInfo")) {
		/*
		 * ReadWrite, and the capture graph READS it before it starts the session:
		 * -[BWMultiStreamCameraSourceNode _verifyColorSpacePropertiesForAllOutputs] does a
		 * getProperty:error: for it, and an error there fails the session's transition to running
		 * and tears the whole graph down again. What it holds matters only for an output whose
		 * colour space is not sRGB (_verifyColorSpacePropertiesForOutputID: returns at once when
		 * the output's colour space is 0), and sRGB is the only one this stream offers, so what
		 * the graph set is given back as it is.
		 */
		pthread_mutex_lock(&s->lock);
		value = s->colour_info != NULL ? CFRetain(s->colour_info) : new_dict();
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(s->supported);
	} else if (key_is(key, "FocalLength")) {
		value = number_f(s->camera->focal_length);     /* (the iPhone X's cameras', in millimetres) */
	} else if (key_is(key, "LensFNumber")) {
		value = number_f(s->camera->f_number);
	} else if (key_is(key, "PixelSize")) {
		/*
		 * The capture-graph builder REQUIRES this of the stream: -[BWFigVideoCaptureStream
		 * initWithCaptureStream:...] does a getProperty: (not IfSupported:) for it, and without it the
		 * init fails, so FigVideoCaptureSourcesActivateAndCreateDevices errors and the graph is never
		 * built. The sensor's pixel pitch in micrometres (the iPhone X cameras'); any value in
		 * (0, 100) is accepted.
		 */
		value = number_f(s->camera->pixel_size);
	} else if (key_is(key, "MaxZoomFactor")) {
		value = number_f(MAX_ZOOM);
	} else if (key_is(key, "ZoomFactor")) {
		value = number_f(s->zoom);
	} else if (key_is(key, "SupportedColorSpaces")) {
		CFNumberRef srgb = number_i(0);

		value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
		CFRelease(srgb);
	} else if (key_is(key, "ColorSpace") || key_is(key, "DeviceOrientation") || key_is(key, "FlashMode")) {
		value = number_i(0);
	} else if (key_is(key, "HasAutoFocusPositionSensor") || key_is(key, "HighCurrentTorchEnabled") ||
	    key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else {
		say_once("stream: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static void torch_set(struct stream *s, CFTypeRef value);

static double
dict_number(CFDictionaryRef d, const char *name, double otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	double n = otherwise;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberDoubleType, &n);
	}
	return n;
}

/* AFWindowParams: the first window, as fractions of the frame; none (or not one) is the middle */
static void
af_window(struct stream *s, CFTypeRef value)
{
	CFDictionaryRef w = NULL;
	double x0, y0, width, height;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
		w = CFArrayGetValueAtIndex(value, 0);
	}
	if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
	    (height = dict_number(w, "height", 0)) <= 0) {
		s->af_window[0] = s->af_window[1] = 0.3f;
		s->af_window[2] = s->af_window[3] = 0.7f;
		return;
	}
	x0 = dict_number(w, "hStart", 0) / 1000;
	y0 = dict_number(w, "vStart", 0) / 1000;
	width /= 1000;
	height /= 1000;
	/* (at least a tenth of the frame each way, about the middle of what was asked) */
	if (width < 0.1) {
		x0 -= (0.1 - width) / 2;
		width = 0.1;
	}
	if (height < 0.1) {
		y0 -= (0.1 - height) / 2;
		height = 0.1;
	}
	x0 = x0 < 0 ? 0 : x0 + width > 1 ? 1 - width : x0;
	y0 = y0 < 0 ? 0 : y0 + height > 1 ? 1 - height : y0;
	s->af_window[0] = (float)x0;
	s->af_window[1] = (float)y0;
	s->af_window[2] = (float)(x0 + width);
	s->af_window[3] = (float)(y0 + height);
}

static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct stream *s = stream_of(o);

	if (key_is(key, "FormatIndex")) {
		int64_t i = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberSInt64Type, &i);
		}
		if (i < 0 || i >= CFArrayGetCount(s->formats)) {
			return ERR_PARAM;
		}
		s->format_index = (int)i;
		say("stream: format %lld", (long long)i);
		return 0;
	}
	if (key_is(key, "ZoomFactor")) {
		/*
		 * The zoom: the camera's pictures -- moving and still alike -- made of the middle of the sensor
		 * (src/camera_isp.c, crop()), as the iPhone's ISP crops for it. Digital: no more detail than
		 * that part of the sensor has.
		 */
		double zoom = 1;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &zoom);
		}
		zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
		if (fabs(zoom - s->zoom) >= 0.25 || (zoom == 1) != (s->zoom == 1)) {
			say("stream: zoom %.2fx", zoom);
		}
		s->zoom = (float)zoom;
		return 0;
	}
	if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
		double fps = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &fps);
		}
		*(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
		bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");

		say_once("stream: set", key, value);
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
			say("stream: ignored %s with value type %lu", configs ? "VideoOutputConfigurations" :
			    enabled ? "VideoOutputsEnabled" : "VideoOutputHandlers", value != NULL ? (unsigned long)CFGetTypeID(value) : 0ul);
			return 0;
		}
		pthread_mutex_lock(&s->lock);
		CFIndex n = CFDictionaryGetCount(value);
		const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		for (CFIndex i = 0; i < n; i++) {
			int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
			struct output *out;

			if (k < 0) {
				say_once("stream: an output not here:", keys[i], NULL);
				continue;
			}
			out = &s->out[k];
			if (configs) {
				if (out->config != NULL) {
					CFRelease(out->config);
				}
				let_go(out);
				out->config = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ? CFRetain(values[i]) : NULL;
			} else if (enabled) {
				int64_t on = 1;

				if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
					on = CFBooleanGetValue(values[i]);
				} else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
					CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
				}
				out->enabled = on != 0;
			} else {
				CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
				    CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;

				if (out->handler != NULL) {
					CFRelease(out->handler);
				}
				out->handler = h != NULL ? (void *)CFRetain(h) : NULL;
			}
		}
		free(keys);
		free(values);
		for (int i = 0; i < OUTPUTS; i++) {
			output_state(configs ? "stream: configured" : enabled ? "stream: enabled state" : "stream: handler state", &s->out[i]);
		}
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "StillImageCaptureNow")) {
		/*
		 * The shutter: the capture graph's still image coordinator asks for a photograph by writing
		 * this, with what it wants of it (how many pictures, the flash, the settings' number by which
		 * it will know the frames again). One frame of the sensor goes to the StillImage output for
		 * each ask.
		 */
		pthread_mutex_lock(&s->lock);
		s->stills_wanted++;
		pthread_mutex_unlock(&s->lock);
		say_once("stream: a photograph asked for", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputsColorInfo")) {
		pthread_mutex_lock(&s->lock);
		if (s->colour_info != NULL) {
			CFRelease(s->colour_info);
		}
		s->colour_info = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ? CFRetain(value) : NULL;
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "TorchLevel")) {
		torch_set(s, value);
		return 0;
	}
	if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
	    key_is(key, "PeakTrackingFocusNow") || key_is(key, "LockAFNow") || key_is(key, "ManualFocusNow") ||
	    key_is(key, "AFWindowParams") || key_is(key, "AutoFocusMode")) {
		/*
		 * Camera.app's tap: AutoFocusMode "AutoFocusModeWeightedSum", AFWindowParams an array of
		 * { hStart, vStart, width, height, weight } in thousandths of the frame (the first window is
		 * the one taken here; an empty value is the middle), then AutoFocusNow (true). LockAFNow
		 * keeps the lens where it is until focus is asked for again.
		 */
		if (!key_is(key, "AFWindowParams") || (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() &&
		    CFArrayGetCount(value) > 0)) {
			say_value("stream: focus:", key, value);        /* (an empty window, the middle: often, unsaid) */
		}
		if (key_is(key, "AFWindowParams")) {
			af_window(s, value);
		} else if (key_is(key, "LockAFNow")) {
			s->af_locked = true;
		} else if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
		    key_is(key, "PeakTrackingFocusNow")) {
			s->af_locked = false;
			s->af_now = true;
		}
		return 0;
	}
	say_once("stream: set (not used)", key, value);
	return 0;
}

/* the flashlight: TorchLevel (0 to 1) as thousandths to the kernel's /dev/hakios-torch (torch=3) */
static void
torch_set(struct stream *s, CFTypeRef value)
{
	float level = 0;
	uint32_t thousandths;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberFloatType, &level);
	}
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	thousandths = (uint32_t)(level * 1000 + 0.5f);
	if (s->torch < 0) {
		s->torch = open("/dev/hakios-torch", O_WRONLY);
	}
	if (s->torch < 0 || write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
		say("the flashlight at %u of 1000 NOT TAKEN (no /dev/hakios-torch)", thousandths);
	} else {
		say("the flashlight at %u of 1000", thousandths);
	}
}

/* ---- the frames ------------------------------------------------------------------------------------- */

struct render_job {
	const struct isp *isp;
	const struct hakios_camera_frame *f;
	const uint8_t *raw;
	CVPixelBufferRef pb;
	uint32_t format, width, height, pieces;
	bool still;                     /* the photograph: every pixel of the sensor (isp_render_still_nv12) */
};

static void
render_piece(void *context, size_t i)
{
	struct render_job *j = context;
	uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;

	first &= ~1u;
	end = i + 1 == j->pieces ? j->height : end & ~1u;
	if (j->format == PF_BGRA) {
		isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
		    j->height, first, end);
	} else if (j->still) {
		isp_render_still_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0), CVPixelBufferGetBaseAddressOfPlane(j->pb, 1),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end, j->format == PF_420V);
	} else {
		isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
		    CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V);
	}
}

static CVPixelBufferRef
output_buffer(struct output *out)
{
	CVPixelBufferPoolRef pool = NULL;
	CVPixelBufferRef pb = NULL;
	CFTypeRef value = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;

	/*
	 * The graph makes the pool itself and hands it over in the output's configuration
	 * (kFigCaptureStreamVideoOutputConfigurationKey_BufferPool), sized and laid out as the node
	 * behind the output wants it (an IOSurface each, extra rows at the bottom where a scaler wants
	 * them, as few buffers as the node holds at once) -- there is nothing for this side to choose,
	 * and a buffer of our own making is not one the graph would take. The configuration carries no
	 * width or height: the pool's buffers have them.
	 */
	if (value != NULL && CFGetTypeID(value) == CVPixelBufferPoolGetTypeID()) {
		pool = (CVPixelBufferPoolRef)value;
	}
	if (pool == NULL) {
		if (!out->said_shape) {
			out->said_shape = true;
			say("%s: no pool of the graph's in the configuration: fields=%ld pool-type=%lu; no pictures", out->name,
			    (long)(out->config != NULL ? CFDictionaryGetCount(out->config) : 0),
			    value != NULL ? (unsigned long)CFGetTypeID(value) : 0ul);
		}
		return NULL;
	}
	for (unsigned i = 0; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess && i < HELD; i++) {
		/* a pool that will not grow that far: the oldest picture kept is the one to have */
		unsigned oldest = (out->held_next + i) % HELD;

		if (out->held[oldest] != NULL) {
			CVPixelBufferRelease(out->held[oldest]);
			out->held[oldest] = NULL;
		}
		if (!out->said_pool) {
			out->said_pool = true;
			say("%s: the graph's pool will not grow by %u: fewer pictures kept", out->name, HELD);
		}
	}
	if (pb == NULL) {
		/*
		 * The preview pool holds one buffer (MinimumBufferCount 1): while the graph still has it,
		 * there is none to be had, and this frame is not given out. Said at most every 300th frame.
		 */
		if (out->refused++ % 300 == 0) {
			say("%s: the graph's pool has no buffer to spare (%llu frames so)", out->name,
			    (unsigned long long)out->refused);
		}
		return NULL;
	}
	if (!out->said_shape) {
		uint32_t pf = CVPixelBufferGetPixelFormatType(pb);
		out->said_shape = true;
		say("%s: graph pool buffer %ux%u %c%c%c%c planes=%zu row-bytes=%zu", out->name,
		    (uint32_t)CVPixelBufferGetWidth(pb), (uint32_t)CVPixelBufferGetHeight(pb), pf >> 24, pf >> 16 & 255,
		    pf >> 8 & 255, pf & 255, (size_t)CVPixelBufferGetPlaneCount(pb),
		    (size_t)CVPixelBufferGetBytesPerRow(pb));
	}
	return pb;
}

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

static void
output_rate(struct output *out)
{
	if (out->rate_frames > 1) {
		double ms = camera_interval_ms(out->rate_first, out->rate_last);
		if (ms > 0) say("Raphael output rate: output=%s callbacks=%llu elapsed=%.2f ms fps=%.2f",
		    out->name, (unsigned long long)out->rate_frames, ms, (out->rate_frames - 1) * 1000.0 / ms);
	}
}

static void
let_go(struct output *out)
{
	if (out->rate_frames) output_rate(out);
	out->rate_frames = out->rate_first = out->rate_last = 0;
	for (unsigned i = 0; i < HELD; i++) {
		if (out->held[i] != NULL) {
			CVPixelBufferRelease(out->held[i]);
			out->held[i] = NULL;
		}
	}
}

static void
md_put(CFMutableDictionaryRef m, int key, CFTypeRef value)
{
	if (md_key[key] != NULL && value != NULL) {
		CFDictionarySetValue(m, md_key[key], value);
	}
	if (value != NULL) {
		CFRelease(value);
	}
}

/* the frame's metadata, as H10ISP's stream gives them (see md_names) */
static CFDictionaryRef
frame_metadata(const struct isp *isp, const struct hakios_camera_frame *f, const struct isp_stats *st, CFStringRef port)
{
	CFMutableDictionaryRef m;
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9, analog = f->gain_unity ? (double)f->gain / f->gain_unity : 1;
	double total = analog * isp->digital, fps = f->frame_lines && f->line_ns ? 1e9 / ((double)f->frame_lines * f->line_ns) : 30;
	/* (green's level for a second at 1x, a quarter of it: a room's 70 or so, daylight's tens of thousands) */
	double lux = exposure > 0 && total > 0 ? st->mean[1] / (exposure * total) / 4 : 0;

	if (md_attachment == NULL) {
		return NULL;
	}
	m = (CFMutableDictionaryRef)new_dict();
	md_put(m, MD_PORT, CFRetain(port));
	md_put(m, MD_PTS, CMTimeCopyAsDictionary(CMClockMakeHostTimeFromSystemUnits(f->time), NULL));
	md_put(m, MD_EXPOSURE, number_f(exposure));
	md_put(m, MD_AGC, number_i((int64_t)(analog * 256 + 0.5)));
	md_put(m, MD_ISP_DGAIN, number_i((int64_t)(isp->digital * 256 + 0.5)));
	md_put(m, MD_SENSOR_DGAIN, number_i(256));
	md_put(m, MD_ISO_BASE, number_f(32));
	md_put(m, MD_FRAME_RATE, number_f(fps));
	md_put(m, MD_LUX, number_i((int64_t)(lux + 0.5)));
	md_put(m, MD_SNR, number_f(40 - 10 * log10(total < 1 ? 1 : total)));
	md_put(m, MD_AF_STATUS, number_i(0));
	md_put(m, MD_FOCUSING, number_i(0));
	md_put(m, MD_AE_STABLE, CFRetain(kCFBooleanTrue));
	md_put(m, MD_AE_LOCKED, CFRetain(kCFBooleanFalse));
	md_put(m, MD_AWB_STABLE, CFRetain(kCFBooleanTrue));
	md_put(m, MD_AWB_LOCKED, CFRetain(kCFBooleanFalse));
	md_put(m, MD_AF_LOCKED, CFRetain(kCFBooleanFalse));
	/*
	 * The zoom the picture was made with: the source node takes it for what the ISP applied
	 * (-[BWMultiStreamCameraSourceNode _ispAppliedZoomFactorFromSampleBufferMetadataDictionary:outputIndex:])
	 * and would make up the rest of what was asked for itself; without it, a zoomed preview stood still.
	 */
	md_put(m, MD_ZOOM, number_f(isp->zoom));
	return m;
}

static void
deliver(struct stream *s, struct output *out, const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw,
    CFDictionaryRef metadata)
{
	bool trace = raphael_rear_only() && (out->delivered < 8 || out->delivered % 30 == 0);
	uint64_t pool_begin = mach_absolute_time();
	CVPixelBufferRef pb = output_buffer(out);
	uint64_t pool_end = mach_absolute_time();
	uint32_t pf, width, height;

	(void)s;
	if (pb == NULL) {
		return;
	}
	pf = CVPixelBufferGetPixelFormatType(pb);
	width = (uint32_t)CVPixelBufferGetWidth(pb);
	height = (uint32_t)CVPixelBufferGetHeight(pb);
	if (width == 0 || height == 0 || (pf != PF_420F && pf != PF_420V && pf != PF_BGRA)) {
		if (out->delivered == 0 && out->said_shape == false) {
			out->said_shape = true;
			say("%s: %ux%u %c%c%c%c buffers are not for this ISP", out->name, width, height, pf >> 24, pf >> 16 & 255,
			    pf >> 8 & 255, pf & 255);
		}
		CVPixelBufferRelease(pb);
		return;
	}
	CVPixelBufferLockBaseAddress(pb, 0);

	/* Retain upstream's eight groups: the 32-group experiment did not show
	 * a repeatable device gain. */
	uint32_t pieces = 8;
	/* AE/AWB updates the live ISP state while the camera worker is acquiring
	 * the next frame.  Rendering pieces directly from that state lets
	 * different horizontal bands of one output use different gains, CCM or
	 * lens-shading values.  Take one frame-local snapshot so every piece is
	 * rendered with exactly the same colour and exposure state. */
	struct isp frame_isp = *isp;
	frame_isp.preview = strcmp(out->name, "StillImage") != 0;
	struct render_job job = { &frame_isp, f, raw, pb, pf, width, height, pieces, strcmp(out->name, "StillImage") == 0 && pf != PF_BGRA };
	uint64_t began = mach_absolute_time();

	bool lsc_cached = isp_preview_lsc_prepare(&frame_isp, f, width, height);
	uint64_t prepared = mach_absolute_time();
	dispatch_apply_f(job.pieces, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &job, render_piece);
	uint64_t rendered = mach_absolute_time();
	isp_preview_lsc_release(&frame_isp);
	if (trace && lsc_cached) say("Raphael preview LSC cache: width=%u height=%u prepare=%.2f ms",
	    width, height, camera_interval_ms(began, prepared));
	if (job.still) {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		say("%s: %ux%u made of every pixel in %.0f ms", out->name, width, height,
		    (double)(mach_absolute_time() - began) * tb.numer / tb.denom / 1e6);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
	    kCVAttachmentMode_ShouldPropagate);

	if (metadata != NULL) {
		CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, metadata);

		CVBufferSetAttachment(pb, md_attachment, m, kCVAttachmentMode_ShouldPropagate);
		CFRelease(m);
	}

	CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);

	uint64_t callback_begin = mach_absolute_time();
	if (trace) {
		say("Raphael delivery enter: frame=%llu output=%s graph=%u pool=%.2f ms render=%.2f ms",
		    (unsigned long long)f->sequence, out->name, s->graph_running,
		    camera_interval_ms(pool_begin, pool_end), camera_interval_ms(began, rendered));
	}
	((pixel_buffer_handler)out->handler)(pb, pts);
	if (raphael_rear_only()) {
		out->rate_last = mach_absolute_time();
		if (out->rate_frames++ == 0) out->rate_first = out->rate_last;
		if (out->rate_frames % 300 == 0) output_rate(out);
	}
	if (trace) {
		say("Raphael delivery exit: frame=%llu output=%s callback=%.2f ms",
		    (unsigned long long)f->sequence, out->name,
		    camera_interval_ms(callback_begin, mach_absolute_time()));
	}
	/*
	 * The moving picture's buffers are kept for HELD frames after they are given. The preview's pool
	 * would otherwise hand a buffer straight back -- the graph lets go of it as soon as it has passed
	 * it on -- while it is still on its way to the screen (the preview sink, Camera.app's image queue,
	 * the window server copying it), and the next frame drawn into it showed as bands of two frames,
	 * torn across. Kept, they make the pool take others. (One kept was not enough.)
	 */
	if (strcmp(out->name, "StillImage") != 0) {
		if (out->held[out->held_next] != NULL) {
			CVPixelBufferRelease(out->held[out->held_next]);
		}
		out->held[out->held_next] = pb;
		out->held_next = (out->held_next + 1) % HELD;
	} else {
		CVPixelBufferRelease(pb);
	}
	if (out->delivered++ == 0) {
		say("%s: the first picture given (%ux%u %c%c%c%c)", out->name, width, height, pf >> 24, pf >> 16 & 255,
		    pf >> 8 & 255, pf & 255);
	}
}

/*
 * The scene's light as the cameras' tuning measures it (Qualcomm's lux index, higher is darker): about
 * 46 a doubling of exposure times gain, 197 at a 33 ms exposure and 1x -- the black level's ranges in
 * the tuning of the wide camera pair lux indexes with gains, 195-200 with 1-1.1x and 380-385 with
 * 15.9-16x, at the longest exposure a frame at 30 a second allows.
 */
static float
lux_index(const struct isp *isp, const struct hakios_camera_frame *f)
{
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9 * f->gain / f->gain_unity * isp->digital;

	return (float)(197 + 46 * log2(exposure > 1e-6 ? exposure / 0.0333 : 1e-6 / 0.0333));
}

/*
 * The light's colour temperature: where the white balance is on the line of the tuning's reference
 * lights; without them, guessed from how much more the blue is raised than the red, by what is typical of a phone's sensor --
 * blue's gain over red's about 0.8 in daylight (6500 K), 1.3 under a fluorescent tube (4000 K), 2 under
 * a filament lamp (2850 K) -- between those on the logarithm of the ratio, and no further.
 */
static float
colour_temperature(const struct isp *isp)
{
	static const float ratio[3] = { 0.8f, 1.3f, 2.0f }, kelvin[3] = { 6500, 4000, 2850 };
	float r = isp->wb[0] > 0 ? isp->wb[2] / isp->wb[0] : 1, cct = isp_cct(isp);

	if (cct > 0) {
		return cct;     /* (by the tuning's reference lights) */
	}

	if (r <= ratio[0]) {
		return kelvin[0];
	}
	for (int i = 1; i < 3; i++) {
		if (r <= ratio[i]) {
			float t = logf(r / ratio[i - 1]) / logf(ratio[i] / ratio[i - 1]);

			return kelvin[i - 1] + t * (kelvin[i] - kelvin[i - 1]);
		}
	}
	return kelvin[2];
}

/*
 * Focus, where the camera has a lens motor (the frames' focus is a position, not HAKIOS_CAMERA_NO_FOCUS:
 * the wide camera's): by contrast, as there is nothing else to go by -- the sensor's phase-detection
 * pixels are not read. A search climbs from where the lens is: a step of AF_STEP one way, on that way
 * while the middle of the picture gets sharper, back the other way if the first step was not, until
 * two steps in a row are less sharp than the sharpest (or the travel, AF_NEAR to AF_FAR, ends); then
 * around the sharpest in steps of AF_FINE, and there it stays. Each place is measured two frames after
 * the lens was told to go there (it settles in less than one). The lens is only ever moved a step at a
 * time: thrown across its travel it knocked, audibly, against its stop at 0. In focus, it stays until
 * the middle has been a good deal less sharp or quite differently lit for a second, and then until
 * the picture has kept still for a third of one: no searching while the phone is being moved.
 * A focus asked for (a tap: AutoFocusNow, on the window AFWindowParams gave) sweeps instead: the
 * lens is walked to the nearer end of its travel, then measured towards the other in steps of
 * AF_STEP until it is past the sharpest (two steps in a row well under it), then around the sharpest
 * -- it finds what was tapped however far out of focus it was, where the climb gave up on the flat
 * of the curve.
 */
enum { AF_WATCH, AF_STEADY, AF_CLIMB, AF_SWEEP, AF_FINE_SEARCH, AF_SETTLE };
#define AF_NEAR         16              /* (off the stop at 0) */
#define AF_FAR          768             /* (its module file's actuator region goes to 700; near 1023 the PM8008i's LDO5 said over-current) */
#define AF_STEP         48
#define AF_FINE         12

struct af {
	int             state;
	uint32_t        minimum, maximum; /* inclusive command bounds from the frame, or defaults */
	uint32_t        at, target;     /* where the lens was told to go; where it is going a step at a time */
	unsigned        wait;           /* frames before the next measurement */
	int             way;            /* +1 nearer, -1 further */
	bool            turned;
	unsigned        worse;          /* steps in a row less sharp than the sharpest */
	uint32_t        start, best, fine_to;
	float           best_sharpness;
	float           sharpness, level;       /* in focus: the middle's, to watch */
	float           last_sharpness, last_level;
	unsigned        unlike, steady, steps;
	bool            now;            /* asked for: no waiting for the picture to keep still */
	bool            sweep;          /* ... and the whole travel measured */
	float           sum_sharpness, sum_level;
	unsigned        measured;
	uint64_t        unlike_since, steady_since;
	bool            warm_check;
	bool            bounded_driver_ramp;
};
/* Rear workers are joined before another is started. A completed search's
 * position and contrast are a hint, validated against fresh RAW after parking. */
static uint32_t raphael_saved_focus;
static float raphael_saved_sharpness, raphael_saved_level;

static void
af_go(int device, struct af *af, uint32_t target)
{
	struct hakios_camera_command c = { .command = HAKIOS_CAMERA_FOCUS };

	af->target = target < af->minimum ? af->minimum : target > af->maximum ? af->maximum : target;
	c.a = af->bounded_driver_ramp ? af->target :
	    af->at < af->target ? (af->target - af->at > AF_STEP ? af->at + AF_STEP : af->target) :
	    (af->at - af->target > AF_STEP ? af->at - AF_STEP : af->target);
	/*
	 * The lens settles in a few milliseconds; what is spoilt is the frame being exposed while it moved
	 * (written at a frame's end, the next one's exposure has begun): one frame let go by before a
	 * measurement, none on the way.
	 */
	if (write(device, &c, sizeof(c)) != (ssize_t)sizeof(c)) return;
	af->at = c.a;
	af->wait = af->at == af->target ? 1 : 0;
	af->steps++;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
}

static void
af_frame(int device, struct af *af, const struct hakios_camera_frame *f, const uint8_t *raw, const float window[4], bool locked)
{
	float level, sharpness;
	unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;      /* (in the dark, two frames to a measurement) */
	bool position_feedback = f->sensor == 1 &&
	    f->reserved[0] == HAKIOS_CAMERA_FOCUS_RANGE_MAGIC &&
	    f->reserved[1] == 180 && f->reserved[2] == 579;
	/* This rear driver already ramps in <=32-DAC hardware steps, checking
	 * idle and exact register readback for each step. Do not add a second
	 * frame-paced ramp on unmeasured return/positioning moves. */
	af->bounded_driver_ramp = position_feedback;

	if (f->focus == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	/* The kernel may publish the module's inclusive DAC limits. Other boards
	 * retain the original search range; the RAW frame ABI size is unchanged. */
	af->minimum = AF_NEAR; af->maximum = AF_FAR;
	if (f->reserved[0] == HAKIOS_CAMERA_FOCUS_RANGE_MAGIC &&
	    f->reserved[1] < f->reserved[2] && f->reserved[2] <= 1023) {
		af->minimum = f->reserved[1]; af->maximum = f->reserved[2];
	}
	if (af->at == 0 && af->target == 0) {
		af->at = af->target = f->focus;         /* (where the kernel put it) */
	}
	/* Raphael reports the lens position effective for this completed RAW.
	 * Prefetched older frames must not be scored at the newly requested DAC.
	 * Its two-frame hardware epoch replaces a further arbitrary frame wait. */
	if (position_feedback) {
		if (f->focus != af->at) return;
		af->wait = 0;
	}
	if (af->wait > 0) {
		af->wait--;
		return;
	}
	if (af->at != af->target) {
		af_go(device, af, af->target);          /* (the next step of a longer way) */
		return;
	}
	sharpness = isp_sharpness(f, raw, window[0], window[1], window[2], window[3], &level);
	af->sum_sharpness += sharpness;
	af->sum_level += level;
	if (++af->measured < needed) {
		return;
	}
	sharpness = af->sum_sharpness / af->measured;
	level = af->sum_level / af->measured;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	if (position_feedback && af->warm_check) {
		af->warm_check = false;
		/* Validate the remembered position against this scene before reuse. */
		if (af->sharpness > 0 && sharpness >= af->sharpness * 0.85f &&
		    sharpness <= af->sharpness * 1.5f &&
		    level > af->level * 0.7f && level < af->level * 1.4f) {
			af->state = AF_WATCH; af->now = af->sweep = false;
			af->sharpness = sharpness; af->level = level;
			say("Raphael focus warm position validated: DAC=%u sharpness=%.4f", af->at, sharpness);
			return;
		}
		say("Raphael focus warm position needs fresh local search: DAC=%u sharpness=%.4f", af->at, sharpness);
	}
	switch (af->state) {
	case AF_WATCH:
		if (locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
		    level < af->level * 1.4f)) {
			af->unlike = 0;
			af->unlike_since = 0;
			return;
		}
		if (af->sharpness > 0) {
			if (!af->unlike_since) af->unlike_since = f->time;
			if (position_feedback ? camera_interval_ms(af->unlike_since, f->time) < 250 :
			    ++af->unlike < 30) return;
		}
		af->state = AF_STEADY;
		af->steady = 0;
		af->steady_since = 0;
		af->last_sharpness = sharpness;
		af->last_level = level;
		return;
	case AF_STEADY:
		if (fabsf(sharpness - af->last_sharpness) < 0.15f * af->last_sharpness && fabsf(level - af->last_level) < 0.1f * af->last_level) {
			af->steady++;
			if (!af->steady_since) af->steady_since = f->time;
		} else {
			af->steady = 0;
			af->steady_since = 0;
		}
		af->last_sharpness = sharpness;
		af->last_level = level;
		if ((position_feedback ? !af->steady_since || camera_interval_ms(af->steady_since, f->time) < 150 :
		    af->steady < 10) && !af->now) {
			return;
		}
		af->now = false;
		if (af->sweep) {
			af->sweep = false;
			af->state = AF_SWEEP;
			af->best_sharpness = -1;
			af->worse = 0;
			af->steps = 0;
			af->way = af->at - af->minimum < af->maximum - af->at ? 1 : -1;
			af_go(device, af, af->way > 0 ? af->minimum : af->maximum);     /* (walked there a step at a time) */
			return;
		}
		af->state = AF_CLIMB;
		af->best = af->start = af->at;
		af->best_sharpness = sharpness;
		af->way = af->at + AF_STEP > af->maximum ? -1 : 1;
		af->turned = false;
		af->worse = 0;
		af->steps = 0;
		af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
		return;
	case AF_CLIMB: {
		int next = (int)af->at + af->way * AF_STEP;
		bool end = next < (int)af->minimum || next > (int)af->maximum;

		if (sharpness > af->best_sharpness * 1.02f) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else {
			af->worse++;
		}
		if (af->worse < 2 && !end) {
			af_go(device, af, (uint32_t)next);
			return;
		}
		next = (int)af->best - af->way * AF_STEP;
		if (!af->turned && af->best == af->start && next >= (int)af->minimum && next <= (int)af->maximum) {
			/* not sharper that way at all: the other way, from where it started */
			af->turned = true;
			af->way = -af->way;
			af->worse = 0;
			af_go(device, af, (uint32_t)next);
			return;
		}
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > af->maximum ? af->maximum : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + af->minimum ? af->best - 2 * AF_FINE : af->minimum);
		return;
	}
	case AF_SWEEP:
		say("focus: sweep: %u %.4f", af->at, sharpness);
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else if (sharpness < af->best_sharpness * 0.85f) {
			af->worse++;
		}
		if (af->worse < 2 && (int)af->at + af->way * AF_STEP >= (int)af->minimum && (int)af->at + af->way * AF_STEP <= (int)af->maximum) {
			af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
			return;
		}
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > af->maximum ? af->maximum : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + af->minimum ? af->best - 2 * AF_FINE : af->minimum);
		return;
	case AF_FINE_SEARCH:
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
		}
		if (af->at + AF_FINE <= af->fine_to) {
			af_go(device, af, af->at + AF_FINE);
			return;
		}
		af->state = AF_SETTLE;
		af_go(device, af, af->best);
		return;
	case AF_SETTLE:
		af->state = AF_WATCH;
		af->sharpness = sharpness;
		af->level = level;
		af->unlike = 0;
		af->unlike_since = af->steady_since = 0;
		if (position_feedback) {
			raphael_saved_focus = af->at;
			raphael_saved_sharpness = sharpness; raphael_saved_level = level;
		}
		say("focus: the lens at %u after %u moves (sharpness %.3f)", af->at, af->steps, sharpness);
		return;
	}
}

/* the sensor's colour tuning, when the image has one (fw/camera-<sensor>.isp, tools/camtuning.py) */
static void
load_tuning(struct isp *isp, uint32_t sensor)
{
	static const char *const names[] = { "ov02b10", "s5khm2sd", "ov16a1q", "s5k4h7" };
	const char *name;
	char path[96];

	/* The experimental Raphael graph exposes only its rear IMX586 (kernel sensor 1).
	 * Keep the other devices' existing IDs intact, and do not guess a tuning for
	 * Raphael's pop-up front camera. */
	if (raphael_rear_only()) {
		if (sensor == 1) name = "imx586";
		else if (sensor == 2 && raphael_front_enabled()) name = "s5k3t2";
		else return;
	} else {
		if (sensor >= sizeof(names) / sizeof(names[0])) {
			return;
		}
		name = names[sensor];
	}
	snprintf(path, sizeof(path), "/redwood/fw/camera-%s.isp", name);
	if (isp_load_tuning(isp, path)) {
		say("the %s's colour tuning from %s: %u colour matrices' lights, %u lens shadings'", name, path,
		    isp->tuning->ccm.lights, isp->tuning->lsc.lights);
	} else {
		say("no colour tuning for the %s (%s): no colour matrix, no lens shading", name, path);
		isp_set_ccm(isp, (const float[9]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
		isp->lsc.on = false;
	}
}

static ssize_t
read_complete_frame(void *context, void *buffer, size_t bytes)
{
	return read(*(const int *)context, buffer, bytes);
}

static void *
worker(void *context)
{
	CMBaseObjectRef o = context;
	struct stream *s = stream_of(o);
	struct hakios_camera_frame head;
	struct isp isp;
	struct isp_stats st;
	uint8_t *buffer = NULL;
	size_t bytes = 0;
	uint32_t tuned_for = ~0u;
	bool switched = false;
	bool rear_only = raphael_rear_only();
	bool complete_frame = rear_only && (s->camera->sensor == 1 ||
	    (s->camera->sensor == 2 && raphael_front_enabled()));
	bool front = s->camera->sensor == 2;
	bool rear_warm_pending = complete_frame && !front &&
	    raphael_saved_focus >= 180 && raphael_saved_focus <= 579;
	bool rear_warm_starting = false;
	uint32_t raw_width = front ? 2592 : 4000, raw_height = front ? 1940 : 3000;
	uint32_t raw_stride = front ? 3248 : 5008;
	float lux = -1, cct = -1;
	struct af af = { 0 };
	int device = open(HAKIOS_CAMERA_DEVICE, O_RDWR), failures = 0;

	pthread_setname_np("hakios-camera");
	if (device < 0) {
		say("no %s (%s): no frames", HAKIOS_CAMERA_DEVICE, strerror(errno));
		return NULL;
	}
	if (complete_frame) {
		struct hakios_camera_command select = {.command=HAKIOS_CAMERA_SENSOR,.a=s->camera->sensor,.b=~0u};
		if (write(device,&select,sizeof(select)) != (ssize_t)sizeof(select)) {
			say("Raphael sensor %u selection failed: %s",s->camera->sensor,strerror(errno));
			close(device); return NULL;
		}
	}
	struct camera_prefetch prefetch;
	bool prefetched = false;
	if (complete_frame) {
		bytes = sizeof(head) + (size_t)raw_stride * raw_height;
		int error = camera_prefetch_start(&prefetch, bytes, read_complete_frame, &device, mach_absolute_time);
		prefetched = error == 0;
		say("Raphael complete-frame prefetch: enabled=%u bytes=%zu error=%d", prefetched, bytes, error);
	}
	isp_init(&isp);
	while (s->running && failures < 5) {
		ssize_t n;
		uint64_t loop_begin = mach_absolute_time();
		bool trace = rear_only && (s->frames < 8 || s->frames % 30 == 0);

		/* Raphael's preview adapter captures once per read and returns the
		 * complete frame in that read. The upstream worker's header-then-body
		 * protocol is for a streaming cdev; using it here discarded one real
		 * sensor capture and paired the next capture with the previous header. */
		if (!prefetched && buffer == NULL && complete_frame) {
			bytes = sizeof(head) + (size_t)raw_stride * raw_height;
			buffer = malloc(bytes);
			if (buffer == NULL) {
				say("cannot allocate %zu bytes for a complete Raphael frame", bytes);
				break;
			}
			say("reading a complete Raphael frame in one cdev read (%zu bytes)", bytes);
		} else if (!prefetched && buffer == NULL) {
			n = read(device, &head, sizeof(head));
			if (n != (ssize_t)sizeof(head)) {
				failures += n < 0 && errno == EAGAIN ? 0 : 1;
				if (n < 0 && errno == EIO) {
					say("the kernel's camera has had a fault: header read returned %zd errno=%d (%s), frames=%llu graph=%u",
					    n, errno, strerror(errno), (unsigned long long)s->frames, s->graph_running);
					break;
				}
				if (s->frames < 2 && (n < 0 || n != (ssize_t)sizeof(head))) {
					say("camera header read returned %zd errno=%d (%s)", n, errno, n < 0 ? strerror(errno) : "short read");
				}
				continue;
			}
			if (head.sensor != s->camera->sensor) {
				/* another camera's frames (the kernel's last sensor): changed, and the next ones are this one's */
				if (!switched) {
					struct hakios_camera_command c = { .command = HAKIOS_CAMERA_SENSOR, .a = s->camera->sensor, .b = ~0u };

					switched = true;
					say("the kernel's frames are camera %u's: to %u, the %s", head.sensor, s->camera->sensor, s->camera->name);
					(void)write(device, &c, sizeof(c));
				}
				continue;
			}
			bytes = sizeof(head) + (size_t)head.stride * head.height;
			buffer = malloc(bytes);
			if (buffer == NULL) {
				say("cannot allocate %zu bytes for a camera frame", bytes);
				break;
			}
			say("frames of %ux%u from the kernel (the %s)", head.width, head.height, s->camera->name);
			continue;
		}
		uint64_t read_begin = mach_absolute_time();
		uint64_t capture_begin = read_begin, capture_end = read_begin;
		if (prefetched) {
			const struct camera_prefetch_frame *next = camera_prefetch_take(&prefetch);
			if (next == NULL) break;
			buffer = next->data;
			n = next->length;
			errno = next->error;
			capture_begin = next->began; capture_end = next->ended;
		} else {
			n = read(device, buffer, bytes);
		}
		uint64_t read_end = mach_absolute_time();
		if (n != (ssize_t)bytes) {
			if (n < 0 && errno == EIO) {
				say("the kernel's camera has had a fault: frame read returned %zd errno=%d (%s), frames=%llu graph=%u",
				    n, errno, strerror(errno), (unsigned long long)s->frames, s->graph_running);
				break;
			}
			failures += n < 0 && errno == EAGAIN ? 0 : 1;
			if (s->frames < 2 && (n < 0 || n != (ssize_t)bytes)) {
				say("camera frame read returned %zd/%zu errno=%d (%s)", n, bytes, errno,
				    n < 0 ? strerror(errno) : "short read");
			}
			continue;
		}
		failures = 0;
		/* Stop can arrive during Raphael's bounded, complete-frame read.
		 * Its hardware cleanup has finished when read returns. Do not spend
		 * another ISP pass or issue AE/AF writes after Stop has asked to join. */
		if (!s->running) {
			if (rear_only) {
				say("Raphael worker: stop observed after read (%.2f ms); discard before ISP/control writes",
				    camera_interval_ms(read_begin, read_end));
			}
			break;
		}

		const struct hakios_camera_frame *f = (const void *)buffer;
		if (complete_frame &&
		    (f->header_bytes != sizeof(*f) || f->width != raw_width || f->height != raw_height || f->stride != raw_stride ||
		    f->sensor != s->camera->sensor || memcmp(f->magic, HAKIOS_CAMERA_MAGIC, sizeof(f->magic)) != 0)) {
			say("Raphael frame header rejected: header=%u size=%ux%u stride=%u sensor=%u", f->header_bytes,
			    f->width, f->height, f->stride, f->sensor);
			break;
		}
		const uint8_t *raw = buffer + f->header_bytes;
		struct hakios_camera_command c;
		if (complete_frame && !front) {
			/* Match the existing rear driver, rather than waiting forever for
			 * impossible 1x gain or a frame longer than its 6112-line cap. */
			isp.gain_min = 288;
			isp.max_frame_lines = f->frame_lines_max;
		}

		isp_measure(&isp, f, raw, &st);
		isp_awb(&isp, &st);
		uint64_t measured = mach_absolute_time();
		if (f->sensor != tuned_for) {
			tuned_for = f->sensor;
			load_tuning(&isp, f->sensor);
		}
		/*
		 * Slowly, a second or so: the colour temperature is guessed from the white balance, the tuning it
		 * picks changes what the white balance sees, and the colour matrix makes every swing of it plain.
		 */
		lux = lux < 0 ? lux_index(&isp, f) : lux * 0.9f + lux_index(&isp, f) * 0.1f;
		cct = cct < 0 ? colour_temperature(&isp) : cct * 0.97f + colour_temperature(&isp) * 0.03f;
		isp_tune(&isp, lux, cct);
		isp.zoom = s->zoom;
		isp.turned = s->camera->turned;
		if (isp_ae(&isp, f, &st, &c)) {
			ssize_t accepted = write(device, &c, sizeof(c));
			if (complete_frame && !front && trace)
				say("Raphael AE: seq=%llu actual=%u/%u/%u request=%u/%u/%u accepted=%u mean=%.2f bright=%.2f",
				    f->sequence, f->exposure_lines, f->gain, f->frame_lines, c.a, c.b, c.c,
				    accepted == (ssize_t)sizeof(c), st.mean[1], st.bright);
		} else if (complete_frame && !front && trace) {
			say("Raphael AE hold: seq=%llu actual=%u/%u asked=%u/%u mean=%.2f bright=%.2f",
			    f->sequence, f->exposure_lines, f->gain, isp.asked_lines, isp.asked_gain, st.mean[1], st.bright);
		}
		if (rear_warm_pending && f->focus != HAKIOS_CAMERA_NO_FOCUS &&
		    f->reserved[0] == HAKIOS_CAMERA_FOCUS_RANGE_MAGIC &&
		    f->reserved[1] == 180 && f->reserved[2] == 579) {
			rear_warm_pending = false; rear_warm_starting = true;
			af.minimum = 180; af.maximum = 579;
			af.at = af.target = f->focus; af.state = AF_STEADY; af.now = true;
			af.warm_check = true;
			af.bounded_driver_ramp = true;
			af.sharpness = raphael_saved_sharpness; af.level = raphael_saved_level;
			af_go(device, &af, raphael_saved_focus);
			say("Raphael focus warm start: parked=%u last-focused=%u; bounded steps then fresh local search",
			    f->focus, raphael_saved_focus);
		}
		if (s->af_now) {
			s->af_now = false;
			/* (whatever it was doing: at once, from the next frame) */
			af.state = AF_STEADY;
			af.now = true;
			af.sweep = !rear_warm_starting;
		}
		af_frame(device, &af, f, raw, s->af_window, s->af_locked);
		if (rear_warm_starting && af.state == AF_WATCH && af.sharpness > 0)
			rear_warm_starting = false;
		uint64_t controlled = mach_absolute_time();
		s->frames++;
		if (s->frames <= 3) {
			pthread_mutex_lock(&s->lock);
			say("frame %llu output gate: graph=%u", (unsigned long long)s->frames, s->graph_running);
			for (int i = 0; i < OUTPUTS; i++) {
				output_state("frame output", &s->out[i]);
			}
			pthread_mutex_unlock(&s->lock);
		}

		CFDictionaryRef metadata = frame_metadata(&isp, f, &st, s->port);
		uint64_t metadata_done = mach_absolute_time();

		/*
		 * Only once the graph has started the stream. Its PixelBufferHandler blocks hand the buffer
		 * to multiStreamCameraSourceNode_sendBufferToBufferServicingThread, which reaches into the
		 * source node's innards: called before the graph is started (or after it has been torn down)
		 * it walks a pointer that is not there yet and mediaserverd dies of SIGSEGV. The frames
		 * before that are read and thrown away, which is also what settles the exposure.
		 */
		if (metadata != NULL) {
			pthread_mutex_lock(&s->metadata_lock);
			if (s->metadata != NULL) {
				CFRelease(s->metadata);
			}
			s->metadata = CFRetain(metadata);
			pthread_mutex_unlock(&s->metadata_lock);
		}
		uint64_t lock_begin = mach_absolute_time();
		pthread_mutex_lock(&s->lock);
		uint64_t lock_acquired = mach_absolute_time();
		for (int i = 0; s->graph_running && i < OUTPUTS; i++) {
			struct output *out = &s->out[i];

			/*
			 * The scalers' outputs are the moving picture and are given every frame. The still image
			 * output is the whole sensor (4032x3024) and is the photograph: a frame goes to it only
			 * when the graph has asked for one, which is not something to make thirty times a second.
			 */
			bool still = strcmp(out->name, "StillImage") == 0;

			if (out->handler == NULL || out->config == NULL || !(out->enabled || i == 0)) {
				continue;
			}
			if (still && s->stills_wanted == 0) {
				continue;
			}
			deliver(s, out, &isp, f, raw, metadata);
			if (still) {
				s->stills_wanted--;
				say("the photograph is given (%llu so far)", (unsigned long long)++s->stills_given);
			}
		}
		pthread_mutex_unlock(&s->lock);
		uint64_t outputs_done = mach_absolute_time();
		if (trace) {
			if (prefetched) say("Raphael prefetch timing: sequence=%llu capture=%.2f wait=%.2f queued=%.2f ms",
			    (unsigned long long)f->sequence, camera_interval_ms(capture_begin, capture_end),
			    camera_interval_ms(read_begin, read_end), camera_interval_ms(capture_end, read_end));
			say("Raphael frame timing: sequence=%llu read=%.2f stats=%.2f control=%.2f metadata=%.2f lock-wait=%.2f outputs=%.2f total=%.2f ms graph=%u delivered=%llu",
			    (unsigned long long)f->sequence, camera_interval_ms(read_begin, read_end),
			    camera_interval_ms(read_end, measured), camera_interval_ms(measured, controlled),
			    camera_interval_ms(controlled, metadata_done), camera_interval_ms(lock_begin, lock_acquired),
			    camera_interval_ms(lock_acquired, outputs_done), camera_interval_ms(loop_begin, outputs_done),
			    s->graph_running, (unsigned long long)s->out[0].delivered);
		}
		if (metadata != NULL) {
			CFRelease(metadata);
		}
		if (s->frames == 1 || s->frames % 300 == 0) {
			say("frame %llu: exposure %.1f ms, gain %.1fx, digital %.2fx, green %.0f above black; white balance R %.2f "
			    "B %.2f, lux index %.0f, about %.0f K", f->sequence, f->exposure_lines * (double)f->line_ns / 1e6,
			    (double)f->gain / f->gain_unity, isp.digital, st.mean[1], isp.wb[0], isp.wb[2], lux, cct);
		}
	}
	if (prefetched) camera_prefetch_finish(&prefetch);
	else free(buffer);
	close(device);
	say("the frames stopped (%llu)", s->frames);
	return NULL;
}

/*
 * Reading frames from the kernel and giving them to the graph are two things: the first begins when
 * the capture graph reserves the stream (`graph` false, from the device's RequestStreams: the sensor
 * takes a moment to settle and the exposure longer), the second only when the graph starts the
 * stream itself (`graph` true, the FigCaptureStream start method).
 */
static CMBaseObjectRef all_streams[2];  /* the device's, back and front */
static OSStatus stream_stop(CMBaseObjectRef o);

static OSStatus
stream_begin(CMBaseObjectRef o, bool graph)
{
	struct stream *s = stream_of(o);

	if (graph) {
		pthread_mutex_lock(&s->lock);
		s->graph_running = true;
		for (int i = 0; i < OUTPUTS; i++) {
			output_state("stream: graph start state", &s->out[i]);
		}
		pthread_mutex_unlock(&s->lock);
		say("stream: the graph starts the stream (format %d)", s->format_index);
	}
	if (s->running) {
		return 0;
	}
	/* one camera at a time: the kernel gives one sensor's frames (the other stream's worker stopped first) */
	for (int i = 0; i < 2; i++) {
		if (all_streams[i] != NULL && all_streams[i] != o && stream_of(all_streams[i])->running) {
			say("stream: the %s's frames stop for the %s's", stream_of(all_streams[i])->camera->name, s->camera->name);
			(void)stream_stop(all_streams[i]);
		}
	}
	s->running = true;
	s->frames = 0;
	say("stream: frames begin (format %d)", s->format_index);
	if (pthread_create(&s->worker, NULL, worker, o) != 0) {
		s->running = false;
		return ERR_UNSUPPORTED;
	}
	s->worker_started = true;
	return 0;
}

static OSStatus
stream_start(CMBaseObjectRef o)
{
	return stream_begin(o, true);
}

static OSStatus
stream_stop(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);
	bool trace = raphael_rear_only();
	uint64_t began = mach_absolute_time();

	say("stream: stop (%llu frames, %llu pictures given)", (unsigned long long)s->frames,
	    (unsigned long long)s->out[0].delivered);
	pthread_mutex_lock(&s->lock);
	s->graph_running = false;
	for (int i = 0; i < OUTPUTS; i++) {
		let_go(&s->out[i]);
	}
	pthread_mutex_unlock(&s->lock);
	s->running = false;
	if (s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
	}
	if (trace) {
		say("Raphael stream stop complete: %.2f ms, worker-joined=1", camera_interval_ms(began, mach_absolute_time()));
	}
	return 0;
}

static void
add_absent(const void *key, const void *value, void *context)
{
	if (!CFDictionaryContainsKey(context, key)) {
		CFDictionarySetValue(context, key, value);
	}
}

/*
 * The frame's metadata into the sample buffer the graph made of our pixel buffer: the pixel buffer's
 * own dictionary (deliver()), the same object, else the last frame's; whatever the graph has put in a
 * dictionary of the sample buffer's already stays as it is.
 */
static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
	struct stream *s = stream_of(o);
	static bool said;
	CVImageBufferRef image = sbuf != NULL ? CMSampleBufferGetImageBuffer(sbuf) : NULL;
	CFTypeRef ours = image != NULL && md_attachment != NULL ? CVBufferGetAttachment(image, md_attachment, NULL) : NULL, theirs;
	CFMutableDictionaryRef m;

	if (!said) {
		said = true;
		say("stream: AddAttachmentsToSampleBuffer (options %s): the frames' metadata %s", options ? "given" : "none",
		    md_attachment != NULL ? "attached" : "NOT THERE");
	}
	if (md_attachment == NULL || sbuf == NULL) {
		return 0;
	}
	if (ours != NULL && CFGetTypeID(ours) == CFDictionaryGetTypeID()) {
		CFRetain(ours);
	} else {
		pthread_mutex_lock(&s->metadata_lock);
		ours = s->metadata != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, s->metadata) : NULL;
		pthread_mutex_unlock(&s->metadata_lock);
	}
	if (ours == NULL) {
		return 0;
	}
	theirs = CMGetAttachment(sbuf, md_attachment, NULL);
	if (theirs == ours) {
		CFRelease(ours);
		return 0;
	}
	if (theirs != NULL && CFGetTypeID(theirs) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(NULL, 0, theirs);
		CFDictionaryApplyFunction(ours, add_absent, m);
	} else {
		m = (CFMutableDictionaryRef)CFRetain(ours);
	}
	CMSetAttachment(sbuf, md_attachment, m, kCMAttachmentMode_ShouldPropagate);
	CFRelease(m);
	CFRelease(ours);
	return 0;
}

static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
	(void)stream_stop(o);
	return 0;
}

static void
stream_finalize(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	(void)stream_stop(o);
	for (int i = 0; i < OUTPUTS; i++) {
		if (s->out[i].config) {
			CFRelease(s->out[i].config);
		}
		if (s->out[i].handler) {
			CFRelease(s->out[i].handler);
		}
	}
	if (s->formats) {
		CFRelease(s->formats);
	}
	if (s->supported) {
		CFRelease(s->supported);
	}
	if (s->colour_info) {
		CFRelease(s->colour_info);
	}
	if (s->metadata) {
		CFRelease(s->metadata);
	}
	if (s->torch >= 0) {
		close(s->torch);
	}
}

static CFStringRef
stream_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<HakiosCaptureStream: the wide camera>"));
}

static const struct base_class stream_base = {
	1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
	NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct vtable stream_vtable = { NULL, &stream_base, &stream_derived };

static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, const struct camera *camera, CFStringRef port)
{
	CMBaseObjectRef o = NULL;
	struct stream *s;

	if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
		say("the stream cannot be made");
		return NULL;
	}
	s = stream_of(o);
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->metadata_lock, NULL);
	s->torch = -1;
	s->min_fps = 2;
	s->zoom = 1;
	s->af_window[0] = s->af_window[1] = 0.3f;
	s->af_window[2] = s->af_window[3] = 0.7f;
	s->max_fps = 30;
	s->camera = camera;
	s->port = port;
	for (int i = 0; i < OUTPUTS; i++) {
		s->out[i].name = output_names[i];
	}

	CFDictionaryRef f[2] = { format(camera->formats[0][0], camera->formats[0][1], camera->formats[0][2], false, camera->fov),
		format(camera->formats[1][0], camera->formats[1][1], camera->formats[1][2], false, camera->fov) };

	s->formats = CFArrayCreate(NULL, (const void **)f, 2, &kCFTypeArrayCallBacks);
	s->supported = supported(stream_props, sizeof(stream_props) / sizeof(stream_props[0]));
	CFRelease(f[0]);
	CFRelease(f[1]);
	return o;
}

/* ---- the device ------------------------------------------------------------------------------------- */

struct device {
	CFArrayRef      streams;
	CFDictionaryRef supported;
};

static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct device *d = derived_storage(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "StreamArray")) {
		value = CFRetain(d->streams);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(d->supported);
	} else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
		value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	} else {
		say_once("device: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	say_once("device: set (not used)", key, value);
	return 0;
}

static unsigned requests, relinquishes;

static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	if (requests++ < 3) {
		say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
		if (options != NULL) {
			say_once("device: the request's options", CFSTR("RequestStreams"), options);
		}
	}
	/*
	 * Requesting a stream is this device's cue to begin producing: mediaserverd's capture graph
	 * configures the stream (it installs the PrimaryScaler PixelBufferHandler) and then reserves the
	 * streams through here, but it never calls the FigCaptureStream start method, so start the stream's
	 * frame worker from the reservation. stream_start is idempotent (it no-ops when already running),
	 * and relinquish does not stop the worker, so the reservation thrash does not tear it down.
	 */
	if (streams != NULL) {
		CFIndex n = CFArrayGetCount(streams);

		for (CFIndex i = 0; i < n; i++) {
			CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

			if (st != NULL) {
				(void)stream_begin(st, false);
			}
		}
	}
	return 0;
}

static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	(void)options;
	if (relinquishes++ < 3) {
		say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
	} else if (relinquishes % 1000 == 0) {
		say("device: streams given back %u times", relinquishes);
	}
	return 0;
}

static OSStatus
device_invalidate(CMBaseObjectRef o)
{
	(void)o;
	return 0;
}

static void
device_finalize(CMBaseObjectRef o)
{
	struct device *d = derived_storage(o);

	if (d->streams) {
		CFRelease(d->streams);
	}
	if (d->supported) {
		CFRelease(d->supported);
	}
}

static CFStringRef
device_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<HakiosCaptureDevice: the POCO X5 Pro's wide camera>"));
}

static const struct base_class device_base = {
	1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
	NULL, NULL, NULL
};
static const struct device_class device_derived = { 3, NULL, device_request_streams, device_relinquish_streams, NULL };
static const struct vtable device_vtable = { NULL, &device_base, &device_derived };

/* The vendor's device create function (as H10ISPCaptureDeviceCreate is called). */
OSStatus hakios_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);

OSStatus
hakios_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
	static bool found, usable;
	CMBaseObjectRef device = NULL, stream[2] = { NULL, NULL };
	/* Board opt-in: an unsupported pop-up front camera must not be
	 * advertised, selected and repeatedly requested by the capture graph. */
	bool rear_only = raphael_rear_only() && !raphael_front_enabled();
	CFIndex count = rear_only ? 1 : 2;
	struct device *d;

	(void)options;
	if (!found) {
		found = true;
		usable = find_functions();
	}
	if (!usable || out == NULL) {
		return ERR_UNSUPPORTED;
	}
	if ((stream[0] = stream_create(allocator, &cameras[0], port_back)) == NULL) {
		return ERR_UNSUPPORTED;
	}
	if (!rear_only && (stream[1] = stream_create(allocator, &cameras[1], port_front)) == NULL) {
		CFRelease(stream[0]);
		return ERR_UNSUPPORTED;
	}
	if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
		CFRelease(stream[0]);
		if (stream[1]) CFRelease(stream[1]);
		say("the device cannot be made");
		return ERR_UNSUPPORTED;
	}
	d = derived_storage(device);
	memset(d, 0, sizeof(*d));
	d->streams = CFArrayCreate(NULL, (const void **)stream, count, &kCFTypeArrayCallBacks);
	all_streams[0] = stream[0];             /* (kept by the device's array) */
	all_streams[1] = stream[1];
	d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
	CFRelease(stream[0]);
	if (stream[1]) CFRelease(stream[1]);
	*out = device;
	say("the device made: %s", rear_only ? "rear-only Raphael stream" : "two streams, the back camera and the front one");
	return 0;
}
