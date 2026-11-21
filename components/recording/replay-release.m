/* Experimental native ReplayKit source adapter for the Raphael final framebuffer.
 * Only system recording sessions are adapted. The OS retains start/stop, error,
 * Photos, and recording indicator handling. No capture is started at boot.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Photos/Photos.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>
#include "capture-fd.h"
#include "raphael-screenrec.h"
#include "bgra-nv12.h"
#include "replay-h264.h"
#define REPLAY_WIDTH 1080
#define REPLAY_HEIGHT 2340
#define REPLAY_FPS 60

static IMP original_start,original_stop,original_setup,original_input,original_input_hint;
static _Thread_local unsigned writer_setup_depth;
static BOOL software_encoder;
static CMVideoFormatDescriptionRef passthrough_hint;
static OSStatus h264_description(x264_t *encoder,CMVideoFormatDescriptionRef *format){
 const uint8_t *sets[2]={0};size_t sizes[2]={0};
 if(replay_h264_parameter_sets(encoder,sets,sizes))return -1;
 return CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,2,sets,sizes,4,format);
}
static const char context_key,queue_key,native_movie_key;
static __weak AVAssetWriter *active_writer;
static IMP original_finish,original_photos,original_photos_changes,original_photos_async,original_initial;
static IMP original_mix;
static unsigned photos_events;
static NSMutableSet<NSURL *> *adapted_movie_urls;
static CFStringRef const native_sample_key=CFSTR("org.hakios.raphael.system-recording-source");
static IMP original_append;
static uint64_t video_accepted,video_rejected,append_ns,append_max_ns;
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static BOOL append_video(id self,SEL sel,CMSampleBufferRef sample){
 BOOL adapted=sample && CMGetAttachment(sample,native_sample_key,NULL)==kCFBooleanTrue;
 uint64_t began=adapted?clock_ns():0;
 BOOL ok=((BOOL(*)(id,SEL,CMSampleBufferRef))original_append)(self,sel,sample);
 if(adapted){
  uint64_t elapsed=clock_ns()-began;
  __atomic_add_fetch(ok?&video_accepted:&video_rejected,1,__ATOMIC_RELAXED);
  __atomic_add_fetch(&append_ns,elapsed,__ATOMIC_RELAXED);
  uint64_t old=__atomic_load_n(&append_max_ns,__ATOMIC_RELAXED);
  while(elapsed>old && !__atomic_compare_exchange_n(&append_max_ns,&old,elapsed,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
 }
 return ok;
}

static NSURL *movie_key(NSURL *url) {return url.isFileURL?url.URLByResolvingSymlinksInPath:nil;}
static BOOL adapted_movie(NSURL *url) {
 NSURL *key=movie_key(url);if(!key)return NO;
 @synchronized(AVAssetWriter.class){return [adapted_movie_urls containsObject:key];}
}

/* Do not print URLs, filenames, userInfo descriptions, or photo identifiers. */
static void export_error(const char *where,NSError *error) {
 for(unsigned depth=0;error && depth<6;depth++) {
  ((void)0);
  id next=error.userInfo[NSUnderlyingErrorKey];error=[next isKindOfClass:NSError.class]?next:nil;
 }
}
static void movie_info(const char *where,NSURL *url) {
 if(!url.isFileURL){((void)0);return;}
 NSError *error=nil;
 NSDictionary *attributes=[[NSFileManager defaultManager] attributesOfItemAtPath:url.path error:&error];
 ((void)0);
 export_error("movie-stat",error);
 if(!attributes)return;
 AVURLAsset *asset=[AVURLAsset URLAssetWithURL:url options:nil];
 CMTime duration=asset.duration;
 NSArray *video=[asset tracksWithMediaType:AVMediaTypeVideo],*audio=[asset tracksWithMediaType:AVMediaTypeAudio];
 ((void)0);
 for(AVAssetTrack *track in video) {
  CMFormatDescriptionRef format=(__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
  ((void)0);
 }
}
static void finish_movie(id self,SEL sel,void (^handler)(void)) {
 if(![objc_getAssociatedObject(self,&native_movie_key) boolValue]){((void(*)(id,SEL,id))original_finish)(self,sel,handler);return;}
 AVAssetWriter *writer=self;
 ((void(*)(id,SEL,id))original_finish)(self,sel,^{
  ((void)0);
  uint64_t accepted=__atomic_load_n(&video_accepted,__ATOMIC_RELAXED),rejected=__atomic_load_n(&video_rejected,__ATOMIC_RELAXED);
  ((void)0);
  export_error("writer-completed",writer.error);
  if(writer.status==AVAssetWriterStatusCompleted){
   movie_info("writer-completed",writer.outputURL);
   NSURL *key=movie_key(writer.outputURL);
   if(key)@synchronized(AVAssetWriter.class){
    if(adapted_movie_urls.count>=16)[adapted_movie_urls removeAllObjects];
    [adapted_movie_urls addObject:key];
   }
  }
  if(handler)handler();
 });
}
/* setup may return before its asynchronous writer creation. Bind the actual
 * writer only after the tagged source's first sample has been accepted. The
 * CoreMedia attachment survives the queue handoff and any sample copies. */
static BOOL initial_movie(id self,SEL sel,CMSampleBufferRef sample,CGAffineTransform transform) {
 BOOL adapted=sample && CMGetAttachment(sample,native_sample_key,NULL)==kCFBooleanTrue;
 BOOL ok=((BOOL(*)(id,SEL,CMSampleBufferRef,CGAffineTransform))original_initial)(self,sel,sample,transform);
 if(ok && adapted && [self respondsToSelector:sel_registerName("assetWriter")]) {
  AVAssetWriter *writer=((id(*)(id,SEL))objc_msgSend)(self,sel_registerName("assetWriter"));
  if(writer){
   objc_setAssociatedObject(writer,&native_movie_key,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   active_writer=writer;
   ((void)0);
  }
 }
 return ok;
}
/* The completion ABI is verified against this phone's replayd: x1=NSError,
 * x2=localIdentifier, d0=duration. Only a successfully completed file from our
 * system-recording source uses the public import path. Other exports retain
 * the original implementation, including any native audio mixing. */
static void export_photos(id self,SEL sel,NSURL *url,void (^handler)(NSError *,NSString *,double)) {
 BOOL observe=__atomic_fetch_add(&photos_events,1,__ATOMIC_RELAXED)<8;
 if(observe)movie_info("photos-input",url);
 BOOL adapted=adapted_movie(url);
 if(observe)((void)0);
 if(adapted) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
   AVURLAsset *asset=[AVURLAsset URLAssetWithURL:url options:nil];
   double duration=CMTimeGetSeconds(asset.duration);
   __block NSString *identifier=nil;
   ((void)0);
   [[PHPhotoLibrary sharedPhotoLibrary] performChanges:^{
    PHAssetChangeRequest *request=[PHAssetChangeRequest creationRequestForAssetFromVideoAtFileURL:url];
    identifier=request.placeholderForCreatedAsset.localIdentifier;
   } completionHandler:^(BOOL success,NSError *error){
    /* A transaction with no asset request is not a successful recording. */
    BOOL saved=success && identifier.length>0;
    ((void)0);
    export_error("photos-public-import",error);
    NSError *result=nil;
    if(!saved){
     NSDictionary *info=error?@{NSUnderlyingErrorKey:error}:@{};
     result=[NSError errorWithDomain:@"com.apple.ReplayKit.RPRecordingErrorDomain" code:-5831 userInfo:info];
     SEL alert=sel_registerName("showAlertFailedSaveToPhotosWithError:");
     if([self respondsToSelector:alert])((BOOL(*)(id,SEL,id))objc_msgSend)(self,alert,result);
    }else{
     NSError *cleanup=nil;[[NSFileManager defaultManager] removeItemAtURL:url error:&cleanup];
     export_error("photos-source-cleanup",cleanup);
     NSURL *key=movie_key(url);
     @synchronized(AVAssetWriter.class){[adapted_movie_urls removeObject:key];}
    }
    if(handler)handler(result,saved?identifier:nil,duration);
   }];
  }});
  return;
 }
 ((void(*)(id,SEL,id,id))original_photos)(self,sel,url,^(NSError *error,NSString *identifier,double duration){
  if(observe){
   ((void)0);
   export_error("photos-completed",error);
  }
  if(handler)handler(error,identifier,duration);
 });
}
static BOOL photos_changes(id self,SEL sel,void (^changes)(void),NSError **error) {
 NSError *actual=nil;
 BOOL ok=((BOOL(*)(id,SEL,id,NSError **))original_photos_changes)(self,sel,changes,&actual);
 ((void)0);
 export_error("photos-transaction",actual);
 if(error)*error=actual;
 return ok;
}
static void export_async(id self,SEL sel,id url,BOOL mix,id handler) {
 ((void)0);
 ((void(*)(id,SEL,id,BOOL,id))original_photos_async)(self,sel,url,mix,handler);
}
/* Exact firmware: RPPhotosUtility's mixer completion receives NSURL in x1
 * and NSError in x2. Native mixing can replace the completed writer's URL.
 * Propagate provenance only through this successful, tagged input operation;
 * all audio mixing, fallback and original completion behavior is preserved. */
static void mix_audio(id self,SEL sel,NSURL *url,void (^handler)(NSURL *,NSError *)) {
 if(!adapted_movie(url)){
  ((void(*)(id,SEL,id,id))original_mix)(self,sel,url,handler);return;
 }
 ((void(*)(id,SEL,id,id))original_mix)(self,sel,url,^(NSURL *mixed,NSError *error){
  BOOL tagged=NO;NSURL *key=movie_key(mixed);
  if(!error && key){
   @synchronized(AVAssetWriter.class){
    [adapted_movie_urls addObject:key];
    NSURL *old=movie_key(url);if(old && ![old isEqual:key])[adapted_movie_urls removeObject:old];
   }
   tagged=YES;
  }
  ((void)0);
  export_error("audio-mix",error);
  if(handler)handler(mixed,error);
 });
}

static int source_fd(void) {
 int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
 struct sockaddr_un address={0};address.sun_family=AF_UNIX;
 snprintf(address.sun_path,sizeof(address.sun_path),"/var/run/raphael-recorder.sock");
 struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
 const char request[]="CAPTURE_FD\n";
 if(connect(fd,(struct sockaddr *)&address,sizeof(address)) ||
    send(fd,request,sizeof(request)-1,0)!=sizeof(request)-1) {
  int error=errno;close(fd);errno=error;return -1;
 }
 int result=capture_receive_fd(fd),error=errno;close(fd);errno=error;
 if(result>=0)fcntl(result,F_SETFD,FD_CLOEXEC);
 return result;
}

@interface RaphaelReplaySource : NSObject
@property(nonatomic) int fd;
@property(nonatomic) BOOL stopped,started;
@property(nonatomic) uint64_t frames,attempts;
@property(nonatomic) uint64_t lastPTS;
@property(nonatomic) uint64_t beganNS,readNS,convertNS,deliverNS,poolDrops;
@property(nonatomic) uint64_t encodeNS,encodedFrames;
@property(nonatomic) uint64_t lastTickNS,tickNS,tickIntervals,tickMaxNS;
@property(nonatomic) x264_t *encoder;
@property(nonatomic) CMVideoFormatDescriptionRef encodedFormat;
@property(nonatomic,strong) dispatch_queue_t encoderQueue;
@property(nonatomic) CVPixelBufferPoolRef pool;
@property(nonatomic,strong) dispatch_queue_t queue;
@property(nonatomic,strong) dispatch_source_t timer;
@property(nonatomic,weak) id manager;
@property(nonatomic,copy) void (^output)(CMSampleBufferRef,unsigned long long);
@property(nonatomic,copy) void (^didStart)(NSError *);
@property(nonatomic) uint8_t *packet;
- (void)stop;
- (void)tick;
- (void)encodeImage:(CVPixelBufferRef)image pts:(uint64_t)pts;
@end

@implementation RaphaelReplaySource
- (instancetype)init {self=[super init];if(self)_fd=-1;return self;}
- (void)dealloc {if(_fd>=0){ioctl(_fd,HSR_STOP);close(_fd);}if(_pool)CVPixelBufferPoolRelease(_pool);if(_encoder)x264_encoder_close(_encoder);if(_encodedFormat)CFRelease(_encodedFormat);free(_packet);}
- (void)stop {
 if(_stopped)return;_stopped=YES;
 if(_timer){dispatch_source_cancel(_timer);_timer=nil;}
 if(_fd>=0){ioctl(_fd,HSR_STOP);close(_fd);_fd=-1;}
 ((void)0);
 double seconds=_beganNS?(clock_ns()-_beganNS)/1e9:0;
 ((void)0);
 ((void)0);
 ((void)0);
 _output=nil;_didStart=nil;
}
- (void)fail:(int)code {
 if(_stopped)return;
 NSError *error=[NSError errorWithDomain:@"com.apple.ReplayKit.RPRecordingErrorDomain" code:-5817
   userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil]}];
 ((void)0);
 id manager=_manager;
 if(!_started && _didStart)_didStart(error);
 [self stop];
 if(manager && [manager respondsToSelector:sel_registerName("handleScreenCaptureFailureWithError:")])
  ((void(*)(id,SEL,id))objc_msgSend)(manager,sel_registerName("handleScreenCaptureFailureWithError:"),error);
}
- (void)tick {
 if(_stopped || _fd<0)return;
 uint64_t tick=clock_ns();
 if(_lastTickNS){uint64_t interval=tick-_lastTickNS;_tickNS+=interval;_tickIntervals++;if(interval>_tickMaxNS)_tickMaxNS=interval;}
 _lastTickNS=tick;
 if(active_writer.status==AVAssetWriterStatusFailed) {
  NSError *error=active_writer.error;
  ((void)0);
  [self fail:EIO];return;
 }
 _attempts++;
 uint64_t beforeRead=clock_ns();
 ssize_t count=read(_fd,_packet,sizeof(struct hsr_frame)+HSR_MAX_PIXELS);
 uint64_t afterRead=clock_ns();_readNS+=afterRead-beforeRead;
 if(count<0){
  if(_attempts==1 || _attempts==10 || _attempts==50 || _attempts==100) {
   int saved=errno;struct hsr_frame state={0};int info=ioctl(_fd,HSR_INFO,&state);
   ((void)0);
   errno=saved;
  }
  if((errno==EAGAIN || errno==EINTR) && (_started || _attempts<300))return;
  [self fail:errno];return;
 }
 struct hsr_frame *frame=(struct hsr_frame *)_packet;
 if(count<(ssize_t)sizeof(*frame) || frame->magic!=HSR_MAGIC || frame->version!=HSR_VERSION_NATIVE || frame->reserved!=HSR_FORMAT_BGRA ||
    frame->width<2 || frame->width>HSR_MAX_W || frame->height<2 || frame->height>HSR_MAX_H ||
    frame->stride!=frame->width*4 || frame->bytes!=frame->stride*frame->height ||
    count!=(ssize_t)(sizeof(*frame)+frame->bytes) || !frame->active) {[self fail:EPROTO];return;}
 if(frame->width!=REPLAY_WIDTH || frame->height!=REPLAY_HEIGHT){[self fail:EPROTO];return;}
 CVPixelBufferRef image=NULL;
 CVReturn status=0;
 if(!_pool){
  NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},
   (id)kCVPixelBufferWidthKey:@(REPLAY_WIDTH),(id)kCVPixelBufferHeightKey:@(REPLAY_HEIGHT),
   (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
  NSDictionary *poolAttributes=@{(id)kCVPixelBufferPoolMinimumBufferCountKey:@3};
  status=CVPixelBufferPoolCreate(kCFAllocatorDefault,(__bridge CFDictionaryRef)poolAttributes,(__bridge CFDictionaryRef)attributes,&_pool);
  if(status || !_pool){[self fail:ENOMEM];return;}
 }
 NSDictionary *limit=@{(id)kCVPixelBufferPoolAllocationThresholdKey:@8};
 status=CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,_pool,(__bridge CFDictionaryRef)limit,&image);
 if(status==kCVReturnWouldExceedAllocationThreshold){_poolDrops++;return;}
 if(status || !image){[self fail:ENOMEM];return;}
 status=CVPixelBufferLockBaseAddress(image,0);
 if(status){CVPixelBufferRelease(image);[self fail:EIO];return;}
 int converted=CVPixelBufferGetPlaneCount(image)==2?replay_bgra_nv12(_packet+sizeof(*frame),frame->width,frame->height,frame->stride,
  CVPixelBufferGetBaseAddressOfPlane(image,0),CVPixelBufferGetBytesPerRowOfPlane(image,0),
  CVPixelBufferGetBaseAddressOfPlane(image,1),CVPixelBufferGetBytesPerRowOfPlane(image,1)):-1;
 if(converted){CVPixelBufferUnlockBaseAddress(image,0);CVPixelBufferRelease(image);[self fail:EPROTO];return;}
 _convertNS+=clock_ns()-afterRead;
 CMTime host=CMTimeConvertScale(CMClockGetTime(CMClockGetHostTimeClock()),1000000000,kCMTimeRoundingMethod_Default);
 uint64_t pts=(uint64_t)host.value;if(pts<=_lastPTS)pts=_lastPTS+1;_lastPTS=pts;
 CVPixelBufferUnlockBaseAddress(image,0);
 // The bounded pixel pool caps queued input. Capture and sliced-thread encode
 // overlap, but native writer callbacks remain serialized on the source queue.
 dispatch_async(_encoderQueue,^{@autoreleasepool{
  [self encodeImage:image pts:pts];CVPixelBufferRelease(image);
 }});
}
- (void)encodeImage:(CVPixelBufferRef)image pts:(uint64_t)pts {
 if(!_encoder){
  _encoder=replay_h264_open();
  if(!_encoder || h264_description(_encoder,&_encodedFormat)){
   dispatch_async(_queue,^{[self fail:EIO];});return;
  }
 }
 if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly)){
  dispatch_async(_queue,^{[self fail:EIO];});return;
 }
 x264_nal_t *nals=NULL;int count=0;x264_picture_t encoded;
 int64_t sequence=(int64_t)__atomic_load_n(&_encodedFrames,__ATOMIC_RELAXED);
 uint64_t before=clock_ns();
 int bytes=replay_h264_encode(_encoder,CVPixelBufferGetBaseAddressOfPlane(image,0),CVPixelBufferGetBytesPerRowOfPlane(image,0),
  CVPixelBufferGetBaseAddressOfPlane(image,1),CVPixelBufferGetBytesPerRowOfPlane(image,1),sequence,&nals,&count,&encoded);
 __atomic_add_fetch(&_encodeNS,clock_ns()-before,__ATOMIC_RELAXED);
 CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
 if(bytes<=0 || count<=0 || bytes>16*1024*1024 || encoded.i_pts!=sequence || encoded.i_dts!=sequence){
  dispatch_async(_queue,^{[self fail:EIO];});return;
 }
 __atomic_add_fetch(&_encodedFrames,1,__ATOMIC_RELAXED);
 CMBlockBufferRef block=NULL;CMSampleBufferRef sample=NULL;
 OSStatus error=CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,NULL,(size_t)bytes,kCFAllocatorDefault,NULL,0,(size_t)bytes,0,&block);
 size_t offset=0;
 for(int i=0;!error && i<count;i++){
  if(nals[i].i_payload<4 || (size_t)nals[i].i_payload>(size_t)bytes-offset){error=-1;break;}
  error=CMBlockBufferReplaceDataBytes(nals[i].p_payload,block,offset,(size_t)nals[i].i_payload);
  offset+=(size_t)nals[i].i_payload;
 }
 if(offset!=(size_t)bytes)error=-1;
 /* No B frames or encoder delay: DTS equals the actual capture PTS. Keep
  * scheduling/drop gaps rather than fabricating a constant-rate timeline. */
 CMSampleTimingInfo timing={CMTimeMake(1,REPLAY_FPS),CMTimeMake((int64_t)pts,1000000000),CMTimeMake((int64_t)pts,1000000000)};
 size_t size=(size_t)bytes;
 if(!error)error=CMSampleBufferCreateReady(kCFAllocatorDefault,block,_encodedFormat,1,1,&timing,1,&size,&sample);
 if(block)CFRelease(block);
 if(error || !sample){dispatch_async(_queue,^{[self fail:EIO];});return;}
 CFArrayRef attachments=CMSampleBufferGetSampleAttachmentsArray(sample,true);
 if(attachments && CFArrayGetCount(attachments)){
  CFMutableDictionaryRef flags=(CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments,0);
  CFDictionarySetValue(flags,kCMSampleAttachmentKey_NotSync,encoded.b_keyframe?kCFBooleanFalse:kCFBooleanTrue);
 }
 CMSetAttachment(sample,native_sample_key,kCFBooleanTrue,kCMAttachmentMode_ShouldPropagate);
 dispatch_async(_queue,^{
  if(!self.stopped){
   if(!self.started){
    self.started=YES;self.beganNS=clock_ns();
    ((void)0);
    if(self.didStart)self.didStart(nil);self.didStart=nil;
   }
   if(self.output){uint64_t began=clock_ns();self.frames++;self.output(sample,0);self.deliverNS+=clock_ns()-began;}
  }
  CFRelease(sample);
 });
}

@end

static void stop_source(RaphaelReplaySource *source) {
 if(dispatch_get_specific(&queue_key)==(__bridge void *)source)[source stop];
 else dispatch_sync(source.queue,^{[source stop];});
}
static void capture_start(id self,SEL sel,int pid,CGSize size,BOOL system,id contexts,id output,id started) {
 if(!system || !software_encoder) {
  ((void(*)(id,SEL,int,CGSize,BOOL,id,id,id))original_start)(self,sel,pid,size,system,contexts,output,started);return;
 }
 RaphaelReplaySource *old=objc_getAssociatedObject(self,&context_key);if(old)stop_source(old);
 RaphaelReplaySource *source=[RaphaelReplaySource new];source.manager=self;
 __atomic_store_n(&video_accepted,0,__ATOMIC_RELAXED);__atomic_store_n(&video_rejected,0,__ATOMIC_RELAXED);
 __atomic_store_n(&append_ns,0,__ATOMIC_RELAXED);__atomic_store_n(&append_max_ns,0,__ATOMIC_RELAXED);
 active_writer=nil;source.output=output;source.didStart=started;source.fd=source_fd();
 int error=source.fd<0?errno:0;
 struct hsr_config config={HSR_VERSION_NATIVE,1,REPLAY_FPS,HSR_FORMAT_BGRA};
 if(!error && ioctl(source.fd,HSR_START,&config))error=errno;
 if(!error && !(source.packet=malloc(sizeof(struct hsr_frame)+HSR_MAX_PIXELS)))error=ENOMEM;
 source.queue=dispatch_queue_create("org.hakios.raphael.native-screen",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_USER_INTERACTIVE,0));
 source.encoderQueue=dispatch_queue_create("org.hakios.raphael.h264-encode",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_USER_INITIATED,0));
 dispatch_queue_set_specific(source.queue,&queue_key,(__bridge void *)source,NULL);
 objc_setAssociatedObject(self,&context_key,source,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 if(error){dispatch_async(source.queue,^{[source fail:error];});return;}
 __weak RaphaelReplaySource *weak=source;
 source.timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,DISPATCH_TIMER_STRICT,source.queue);
 dispatch_source_set_event_handler(source.timer,^{@autoreleasepool{[weak tick];}});
 dispatch_source_set_timer(source.timer,DISPATCH_TIME_NOW,NSEC_PER_SEC/REPLAY_FPS,500000);
 dispatch_resume(source.timer);
 ((void)0);
}
static void capture_stop(id self,SEL sel) {
 RaphaelReplaySource *source=objc_getAssociatedObject(self,&context_key);
 if(source){stop_source(source);objc_setAssociatedObject(self,&context_key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return;}
 ((void(*)(id,SEL))original_stop)(self,sel);
}
static BOOL adapted_video(id media){return writer_setup_depth && software_encoder && [media isEqual:AVMediaTypeVideo];}
typedef id (*InputInit)(id,SEL,id,id) __attribute__((ns_returns_retained));
typedef id (*InputHintInit)(id,SEL,id,id,CMFormatDescriptionRef) __attribute__((ns_returns_retained));
static id make_input(id self,SEL sel,id media,id settings) __attribute__((ns_returns_retained));
static id make_input(id self,SEL sel,id media,id settings) {
 if(!adapted_video(media))return ((InputInit)original_input)(self,sel,media,settings);
 AVAssetWriterInput *input=((InputHintInit)original_input_hint)(self,sel_registerName("initWithMediaType:outputSettings:sourceFormatHint:"),media,nil,passthrough_hint);
 input.expectsMediaDataInRealTime=YES;
 ((void)0);
 return input;
}
static id make_input_hint(id self,SEL sel,id media,id settings,CMFormatDescriptionRef hint) __attribute__((ns_returns_retained));
static id make_input_hint(id self,SEL sel,id media,id settings,CMFormatDescriptionRef hint) {
 BOOL adapted=adapted_video(media);
 AVAssetWriterInput *input=((InputHintInit)original_input_hint)(self,sel,media,adapted?nil:settings,adapted?passthrough_hint:hint);
 if(adapted)input.expectsMediaDataInRealTime=YES;
 return input;
}

static void writer_setup(id self,SEL sel,id handler) {
 writer_setup_depth++;((void(*)(id,SEL,id))original_setup)(self,sel,handler);writer_setup_depth--;
 if([self respondsToSelector:sel_registerName("assetWriter")])
  active_writer=((id(*)(id,SEL))objc_msgSend)(self,sel_registerName("assetWriter"));
}
static BOOL install(Class cls,const char *selector,IMP replacement,IMP *original,const char *result,unsigned argc,const char **arguments) {
 Method method=class_getInstanceMethod(cls,sel_registerName(selector));
 if(!method || method_getNumberOfArguments(method)!=argc)return NO;
 char *ret=method_copyReturnType(method);BOOL valid=ret && !strcmp(ret,result);free(ret);
 for(unsigned i=2;valid && i<argc;i++) {
  char *arg=method_copyArgumentType(method,i);
  valid=arg && !strncmp(arg,arguments[i-2],strlen(arguments[i-2]));free(arg);
 }
 if(!valid){((void)0);return NO;}
 *original=method_getImplementation(method);
 if(!class_addMethod(cls,sel_registerName(selector),replacement,method_getTypeEncoding(method)))method_setImplementation(method,replacement);
 ((void)0);return YES;
}
static BOOL register_software_encoder(void) {
 x264_t *encoder=replay_h264_open();if(!encoder)return NO;
 OSStatus status=h264_description(encoder,&passthrough_hint);x264_encoder_close(encoder);
 ((void)0);
 return status==0;
}

__attribute__((constructor))static void prepare(void) {
 @autoreleasepool {
  if(strcmp(getprogname(),"replayd"))return;
  adapted_movie_urls=[NSMutableSet new];
  software_encoder=register_software_encoder();if(!software_encoder)return;
  const char *inputArgs[]={"@","@"},*hintArgs[]={"@","@","^"};
  const char *setupArgs[]={"@?"},*captureArgs[]={"i","{CGSize=","B","@","@?","@?"};
  Class input=objc_getClass("AVAssetWriterInput");
  const char *sampleArgs[]={"^"};
  install(input,"appendSampleBuffer:",(IMP)append_video,&original_append,"B",3,sampleArgs);
  BOOL a=install(input,"initWithMediaType:outputSettings:",(IMP)make_input,&original_input,"@",4,inputArgs);
  BOOL b=install(input,"initWithMediaType:outputSettings:sourceFormatHint:",(IMP)make_input_hint,&original_input_hint,"@",5,hintArgs);
  BOOL c=install(objc_getClass("RPMovieWriter"),"setUpAssetWriterWithHandler:",(IMP)writer_setup,&original_setup,"v",3,setupArgs);
  BOOL d=install(objc_getClass("RPScreenCaptureManagerIOS"),"stop",(IMP)capture_stop,&original_stop,"v",2,NULL);
  const char *initialArgs[]={"^","{CGAffineTransform="};
  BOOL e=install(objc_getClass("RPMovieWriter"),"appendInitialSampleBuffer:withTransform:",(IMP)initial_movie,&original_initial,"B",4,initialArgs);
  const char *photoArgs[]={"@","@?"},*changeArgs[]={"@?","^@"},*asyncArgs[]={"@","B","@?"};
  install(objc_getClass("AVAssetWriter"),"finishWritingWithCompletionHandler:",(IMP)finish_movie,&original_finish,"v",3,setupArgs);
  Class photos=object_getClass(objc_getClass("RPPhotosUtility"));
  BOOL f=install(object_getClass(objc_getClass("RPAudioMixUtility")),"mixAudioForMovie:withCompletionHandler:",(IMP)mix_audio,&original_mix,"v",4,photoArgs);
  install(photos,"exportVideoToPhotosLibrary:completionHandler:",(IMP)export_photos,&original_photos,"v",4,photoArgs);
  install(photos,"exportVideoToPhotosAsynchronously:mixAudioTracks:completionHandler:",(IMP)export_async,&original_photos_async,"v",5,asyncArgs);
  void *photosFramework=dlopen("/System/Library/Frameworks/Photos.framework/Photos",RTLD_NOW|RTLD_LOCAL);
  if(photosFramework)install(objc_getClass("PHPhotoLibrary"),"performChangesAndWait:error:",(IMP)photos_changes,&original_photos_changes,"B",4,changeArgs);
  if(a && b && c && d && e && f)install(objc_getClass("RPScreenCaptureManagerIOS"),
   "startSessionWithPID:windowSize:systemRecording:contextIDs:outputHandler:didStartHandler:",(IMP)capture_start,&original_start,"v",8,captureArgs);
 }
}
