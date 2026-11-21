// Transparent observation only. Never change renderer selection, pixels or method return values.
#include <mach/mach.h>
#include <cstddef>
#include <cstdint>
struct B24Overlay {
    int width,height;uint32_t format;int planes;const uint16_t *pitches;const char **pixels;
    int sarNum,sarDen;bool limited;CVPixelBufferRef pixel;int serial;float pts,duration;bool skip;void *hdr;
};
static_assert(sizeof(B24Overlay)==80 && offsetof(B24Overlay,pixel)==48,"Original arm64 overlay ABI");
static bool b22Sample(unsigned n){return n<=12 || n%240==0;}
static id b22Object(id object,const char *name) {
    Ivar ivar=class_getInstanceVariable(object_getClass(object),name);
    return ivar && ivar_getTypeEncoding(ivar)[0]=='@'?object_getIvar(object,ivar):nil;
}
static void b22Overlay(id object,const char *route,void *address,unsigned n) {
    if(!b22Sample(n))return;
    B24Overlay value={};vm_size_t got=0;
    if(!address || vm_read_overwrite(mach_task_self(),(vm_address_t)address,sizeof value,(vm_address_t)&value,&got)!=KERN_SUCCESS || got!=sizeof value){
        sm2_log("B24 route=%s sample=%u overlay unreadable",route,n);return;
    }
    id child=b22Object(object,"mRenderView");
    sm2_log("B24 route=%s sample=%u child=%s overlay=%dx%d fmt=%08x planes=%d pixel=%d skip=%d",route,n,
        child?object_getClassName(child):"none",value.width,value.height,value.format,value.planes,value.pixel!=nullptr,value.skip);
    if(value.pixel)biliTracePixel(value.pixel);
}
static bool b22Signature(Method m,const char *ret,unsigned count) {
    if(!m || method_getNumberOfArguments(m)!=count)return false;
    char *s=method_copyReturnType(m);bool ok=s&&!strcmp(s,ret);free(s);return ok;
}
static bool b22Pointer(Method m,unsigned index,const char *prefix) {
    char *s=method_copyArgumentType(m,index);bool ok=s&&!strncmp(s,prefix,strlen(prefix));free(s);return ok;
}
static void b22InstallRoutes() {
    const char *classes[]={"IJKBGROffscreenWrapView","IJKVideoRenderer","IJKMetalRenderer","IJKStratifyRenderView","IJKBGRWrapView","IJKSampleBufferWrapView"};
    for(const char *name:classes){
        Class cls=objc_getClass(name);Method method=class_getInstanceMethod(cls,sel_registerName("display_pixels:"));
        if(!method){sm2_log("B24 route class=%s absent",name);continue;}
        sm2_log("B24 route method=%s types=%.160s",name,method_getTypeEncoding(method));
        // Inspect overlay bytes only for the exact original arm64 layout seen on V21.
        if(!b22Signature(method,"v",3) || !biliArg(method,2,"^{IJKOverlay=iiIi^S^*iiB^{__CVBuffer}iffB^v}"))continue;
        // Do not change an inherited implementation on an unrelated class.
        unsigned count=0;Method *own=class_copyMethodList(cls,&count);bool owned=false;
        for(unsigned i=0;i<count;++i)if(own[i]==method)owned=true;free(own);if(!owned)continue;
        IMP original=method_getImplementation(method);SEL selector=method_getName(method);
        auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,void *overlay){
            unsigned n=++*counter;b22Overlay(object,name,overlay,n);
            ((void(*)(id,SEL,void*))original)(object,selector,overlay);
        });
        method_setImplementation(method,hook);sm2_log("B24 route installed=%s",name);
    }
    Class cls=objc_getClass("IJKMetalLayer");SEL selector=sel_registerName("drawFrame:texCache:pixelBuf:");
    Method method=class_getInstanceMethod(cls,selector);
    if(b22Signature(method,"v",5) && biliArg(method,2,"@") && b22Pointer(method,3,"^{__CVMetalTextureCache=") && b22Pointer(method,4,"^{__CVBuffer")){
        IMP original=method_getImplementation(method);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,id encoder,void *cache,CVPixelBufferRef pixel){
            unsigned n=++*counter;if(b22Sample(n)){sm2_log("B24 layer-upload[%u] cache=%d encoder=%d pixel=%d",n,cache!=nullptr,encoder!=nil,pixel!=nullptr);if(pixel)biliTracePixel(pixel);}
            ((void(*)(id,SEL,id,void*,CVPixelBufferRef))original)(object,selector,encoder,cache,pixel);
            if(b22Sample(n))sm2_log("B24 layer-upload[%u] textures y=%d uv=%d",n,b22Object(object,"_mtlTextureY")!=nil,b22Object(object,"_mtlTextureUV")!=nil);
        });method_setImplementation(method,hook);sm2_log("B24 layer upload observer installed");
    }
    cls=objc_getClass("IJKMTLYUVQuadRender");
    for(const char *name:{"uploadYUVPlan2Frame:texCache:pixelBuf:","uploadYUVPlan3Frame:texCache:pixelBuf:"}){
        SEL sel=sel_registerName(name);Method m=class_getInstanceMethod(cls,sel);
        if(!b22Signature(m,"v",5) || !biliArg(m,2,"@") || !b22Pointer(m,3,"^{__CVMetalTextureCache=") || !b22Pointer(m,4,"^{__CVBuffer"))continue;
        IMP original=method_getImplementation(m);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,id encoder,void *cache,CVPixelBufferRef pixel){
            unsigned n=++*counter;if(b22Sample(n)){sm2_log("B24 quad-upload=%s sample=%u cache=%d encoder=%d pixel=%d",name,n,cache!=nullptr,encoder!=nil,pixel!=nullptr);if(pixel)biliTracePixel(pixel);}
            ((void(*)(id,SEL,id,void*,CVPixelBufferRef))original)(object,sel,encoder,cache,pixel);
            if(b22Sample(n))sm2_log("B24 quad-upload=%s sample=%u textures y=%d uv=%d u=%d v=%d",name,n,
                b22Object(object,"_mtlTextureY")!=nil,b22Object(object,"_mtlTextureUV")!=nil,b22Object(object,"_mtlTextureU")!=nil,b22Object(object,"_mtlTextureV")!=nil);
        });method_setImplementation(m,hook);sm2_log("B24 quad upload observer installed=%s",name);
    }
    cls=objc_getClass("IJKMetalRenderer");selector=sel_registerName("drawInMTKView:");method=class_getInstanceMethod(cls,selector);
    if(b22Signature(method,"v",3) && biliArg(method,2,"@")){
        IMP original=method_getImplementation(method);auto counter=new std::atomic<unsigned>{0};
        IMP hook=imp_implementationWithBlock(^void(id object,id view){
            unsigned n=++*counter;if(b22Sample(n))sm2_log("B24 MTK callback[%u] view=%s",n,view?object_getClassName(view):"none");
            ((void(*)(id,SEL,id))original)(object,selector,view);
        });method_setImplementation(method,hook);sm2_log("B24 MTK callback observer installed");
    }
}
