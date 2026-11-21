// Observe the original offscreen path. Preserve every implementation, argument and result.
#import <CoreMedia/CoreMedia.h>
#import <objc/message.h>
static bool b23Own(Class cls,Method method) {
    unsigned count=0;Method *methods=class_copyMethodList(cls,&count);bool found=false;
    for(unsigned i=0;i<count;++i)if(methods[i]==method)found=true;
    free(methods);return found;
}
static bool b23Field(id object,const char *name,const char *type,void *value,size_t bytes) {
    if(!object || !value || !bytes)return false;
    Class cls=object_getClass(object);Ivar ivar=class_getInstanceVariable(cls,name);
    if(!ivar || strcmp(ivar_getTypeEncoding(ivar),type))return false;
    ptrdiff_t offset=ivar_getOffset(ivar);size_t total=class_getInstanceSize(cls);
    if(offset<0 || (size_t)offset>total || bytes>total-(size_t)offset)return false;
    vm_size_t got=0;
    return vm_read_overwrite(mach_task_self(),(vm_address_t)(__bridge const void *)object+(size_t)offset,
        bytes,(vm_address_t)value,&got)==KERN_SUCCESS && got==bytes;
}
static void b23State(id object,const char *where,unsigned n) {
    if(!b22Sample(n))return;
    bool ready=false,prepared=false,visible=false,destroyed=false,first=false;
    uint32_t framebuffer=0,renderbuffer=0;uintptr_t pool=0,cache=0;
    bool ok=b23Field(object,"_isReady","B",&ready,sizeof ready);
    ok=b23Field(object,"renderPrepared","B",&prepared,sizeof prepared)&&ok;
    ok=b23Field(object,"_visible","AB",&visible,sizeof visible)&&ok;
    ok=b23Field(object,"_destoryed","AB",&destroyed,sizeof destroyed)&&ok;
    ok=b23Field(object,"_is_first_rendered","B",&first,sizeof first)&&ok;
    ok=b23Field(object,"_framebuffer","I",&framebuffer,sizeof framebuffer)&&ok;
    ok=b23Field(object,"_renderbuffer","I",&renderbuffer,sizeof renderbuffer)&&ok;
    bool poolRead=b23Field(object,"pixelBufferPool","^{__CVPixelBufferPool=}",&pool,sizeof pool);
    bool cacheRead=b23Field(object,"coreVideoTextureCache","^{__CVOpenGLESTextureCache=}",&cache,sizeof cache);
    sm2_log("B24 offscreen %s[%u] ctx=%d thread=%d ready=%d prepared=%d visible=%d destroyed=%d first=%d fb=%u rb=%u fields=%d pool=%d/%d cache=%d/%d callback=%s",
        where,n,b22Object(object,"_glContext")!=nil,b22Object(object,"_drawThread")!=nil,
        ready,prepared,visible,destroyed,first,framebuffer,renderbuffer,ok,poolRead,pool!=0,cacheRead,cache!=0,
        b22Object(object,"_renderCallback")?object_getClassName(b22Object(object,"_renderCallback")):"none");
}
static void b23InstallOffscreen() {
    Class cls=objc_getClass("BGROffscreenRenderer");
    unsigned count=0;Method *methods=class_copyMethodList(cls,&count);
    for(unsigned i=0;i<count;++i){
        SEL selector=method_getName(methods[i]);const char *name=sel_getName(selector);
        if(!strcmp(name,"initEGLContext") || !strcmp(name,"drawFrame") || !strcmp(name,"createTextureFromImage:") ||
           !strcmp(name,"displayAsync:") || !strcmp(name,"createFramebuffer"))
            sm2_log("B24 offscreen method=%s types=%.180s",name,method_getTypeEncoding(methods[i]));
    }free(methods);
    for(const char *name:{"drawFrame","createFramebuffer"}) {
        SEL sel=sel_registerName(name);Method m=class_getInstanceMethod(cls,sel);
        if(!b22Signature(m,"v",2)||!b23Own(cls,m))continue;
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object){
            unsigned n=++*counter;b23State(object,name,n);
            ((void(*)(id,SEL))original)(object,sel);
            b23State(object,!strcmp(name,"drawFrame")?"draw-end":"framebuffer-end",n);
        });method_setImplementation(m,hook);
    }
    SEL sel=sel_registerName("initEGLContext");Method m=class_getInstanceMethod(cls,sel);
    if(b22Signature(m,"B",2)&&b23Own(cls,m)){
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^BOOL(id object){
            BOOL result=((BOOL(*)(id,SEL))original)(object,sel);unsigned n=++*counter;
            b23State(object,"egl-init-end",n);if(b22Sample(n))sm2_log("B24 egl-init[%u] original-result=%d",n,result);
            return result;
        });method_setImplementation(m,hook);
    }
    sel=sel_registerName("displayAsync:");m=class_getInstanceMethod(cls,sel);
    if(b22Signature(m,"v",3)&&b22Pointer(m,2,"^{S_BGRVout=")&&b23Own(cls,m)){
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,void *frame){
            unsigned n=++*counter;b23State(object,"display-async",n);
            ((void(*)(id,SEL,void*))original)(object,sel,frame);
        });method_setImplementation(m,hook);
    }
    sel=sel_registerName("createTextureFromImage:");m=class_getInstanceMethod(cls,sel);
    if(b22Signature(m,"^{__CVBuffer=}",3)&&b22Pointer(m,2,"^{__CVBuffer=")&&b23Own(cls,m)){
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void*(id object,CVPixelBufferRef pixel){
            void *result=((void*(*)(id,SEL,CVPixelBufferRef))original)(object,sel,pixel);unsigned n=++*counter;
            if(b22Sample(n)){sm2_log("B24 offscreen-output-texture[%u] input=%d result=%d",n,pixel!=nullptr,result!=nullptr);biliTracePixel(pixel);}
            return result;
        });method_setImplementation(m,hook);
    }
    for(const char *name:{"IJKVideoRenderer","IJKBGROffscreenWrapView"}){
        cls=objc_getClass(name);sel=sel_registerName("onFrameEnd:");m=class_getInstanceMethod(cls,sel);
        if(!b22Signature(m,"v",3)||!b22Pointer(m,2,"^{__CVBuffer=")||!b23Own(cls,m))continue;
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,CVPixelBufferRef pixel){
            unsigned n=++*counter;if(b22Sample(n)){sm2_log("B24 frame-output=%s sample=%u pixel=%d",name,n,pixel!=nullptr);biliTracePixel(pixel);}
            ((void(*)(id,SEL,CVPixelBufferRef))original)(object,sel,pixel);
        });method_setImplementation(m,hook);
    }
    cls=objc_getClass("EAGLContext");
    for(const char *name:{"initWithAPI:","initWithAPI:properties:","getMacroContextPrivate","texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:"}){
        m=class_getInstanceMethod(cls,sel_registerName(name));if(!m)continue;
        Dl_info info={};dladdr((void*)method_getImplementation(m),&info);
        const char *image=info.dli_fname?strrchr(info.dli_fname,'/'):nullptr;
        sm2_log("B24 EAGL method=%s types=%.140s implementation=%s",name,method_getTypeEncoding(m),image?image+1:"unknown");
    }
    cls=object_getClass(cls);sel=sel_registerName("setCurrentContext:");m=class_getInstanceMethod(cls,sel);
    if(b22Signature(m,"B",3)&&biliArg(m,2,"@")&&b23Own(cls,m)){
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^BOOL(id object,id context){
            BOOL result=((BOOL(*)(id,SEL,id))original)(object,sel,context);unsigned n=++*counter;
            if(b22Sample(n))sm2_log("B24 EAGL-set-current[%u] context=%d result=%d",n,context!=nil,result);
            return result;
        });method_setImplementation(m,hook);
    }
    cls=objc_getClass("AVSampleBufferDisplayLayer");sel=sel_registerName("enqueueSampleBuffer:");m=class_getInstanceMethod(cls,sel);
    if(b22Signature(m,"v",3)&&b22Pointer(m,2,"^{opaqueCMSampleBuffer=")&&b23Own(cls,m)){
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,CMSampleBufferRef sample){
            unsigned n=++*counter;
            ((void(*)(id,SEL,CMSampleBufferRef))original)(object,sel,sample);
            if(b22Sample(n)){
                sm2_log("B24 sample-layer-enqueue[%u] sample=%d",n,sample!=nullptr);
                if(sample && CFGetTypeID(sample)==CMSampleBufferGetTypeID())biliTracePixel(CMSampleBufferGetImageBuffer(sample));
                Method status=class_getInstanceMethod(object_getClass(object),sel_registerName("status"));
                if(b22Signature(status,"q",2))sm2_log("B24 sample-layer-status[%u]=%ld",n,
                    (long)((NSInteger(*)(id,SEL))objc_msgSend)(object,sel_registerName("status")));
            }
        });method_setImplementation(m,hook);
    }
    sm2_log("B24 original GLES/offscreen observation ready; selection and results unchanged");
}
