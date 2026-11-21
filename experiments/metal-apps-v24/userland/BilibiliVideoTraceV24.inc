// Observe the original player's frame handoff without changing its decoder or return values.
#import <CoreVideo/CoreVideo.h>
#include <cstdlib>
static std::atomic<unsigned> biliTraceLines{0};
static void biliTracePixel(CVPixelBufferRef pixel) {
    unsigned n=++biliTraceLines;
    if(n>16 && n%120)return;
    if(!pixel){sm2_log("B24 frame=%u pixel=null",n);return;}
    if(CFGetTypeID(pixel)!=CVPixelBufferGetTypeID()){sm2_log("B24 frame=%u unexpected CF type",n);return;}
    sm2_log("B24 frame=%u pixel=%zux%zu format=%08x planes=%zu surface=%d",n,
        CVPixelBufferGetWidth(pixel),CVPixelBufferGetHeight(pixel),(unsigned)CVPixelBufferGetPixelFormatType(pixel),
        CVPixelBufferGetPlaneCount(pixel),CVPixelBufferGetIOSurface(pixel)!=nullptr);
}
static bool biliArg(Method method,unsigned index,const char *type) {
    char *actual=method_copyArgumentType(method,index);bool same=actual&&!strcmp(actual,type);free(actual);return same;
}
#include "BilibiliRouteTraceV24.inc"
#include "BilibiliOffscreenTraceV24.inc"
static void biliTraceInstall() {
    if(strcmp(getprogname(),"bili-overseas"))return;
    static dispatch_once_t once;
    dispatch_once(&once,^{
        // Notification names only; do not record URLs, userInfo or frame content.
        [[NSNotificationCenter defaultCenter] addObserverForName:nil object:nil queue:nil usingBlock:^(NSNotification *note){
            NSString *name=note.name;
            if(![name hasPrefix:@"IJK"] && ![name hasPrefix:@"BFC"])return;
            if([name rangeOfString:@"Video"].location==NSNotFound && [name rangeOfString:@"Playback"].location==NSNotFound &&
               [name rangeOfString:@"AudioFrame"].location==NSNotFound)return;
            static std::atomic<unsigned> count{0};unsigned n=++count;
            if(n<=96)sm2_log("B24 notification[%u]=%.180s",n,name.UTF8String?:"?");
        }];
        const char *classes[]={"IJKMTLYUVQuadRender","IJKMTLQuadRender","IJKMetalLayerRenderer","IJKVideoRenderer",
            "IJKSDLGLView","IJKSDLGLViewMetal","IJKSDLGLViewGLES2","IJKMTLEnhanceRender","WMMetalTerminalRender"};
        for(const char *name:classes){
            Class cls=objc_getClass(name);
            if(!cls){sm2_log("B24 class %s absent",name);continue;}
            unsigned count=0;Method *methods=class_copyMethodList(cls,&count);
            unsigned printed=0;
            for(unsigned i=0;i<count;++i){
                const char *selector=sel_getName(method_getName(methods[i]));
                if((strstr(selector,"pixel")||strstr(selector,"Pixel")||strstr(selector,"Texture")||strstr(selector,"render")||
                    strstr(selector,"Render")||strstr(selector,"Frame")) && printed++<20)
                    sm2_log("B24 method %s %s types=%.120s",name,selector,method_getTypeEncoding(methods[i]));
                if(strcmp(name,"IJKMTLYUVQuadRender")||strcmp(selector,"updateTextureByDevice:matrixType:pixelBuf:"))continue;
                Method method=methods[i];char *returns=method_copyReturnType(method);
                char *pixelType=method_copyArgumentType(method,4);
                bool pointer=pixelType && (!strcmp(pixelType,"^v") || !strncmp(pixelType,"^{__CVBuffer",12));
                bool layout=method_getNumberOfArguments(method)==5 && biliArg(method,2,"@") && pointer;
                IMP original=method_getImplementation(method),replacement=nullptr;SEL sel=method_getName(method);
                if(layout && returns && !strcmp(returns,"B") && biliArg(method,3,"^v")){
                    replacement=imp_implementationWithBlock(^BOOL(id object,id device,void *matrix,CVPixelBufferRef pixel){
                        static std::atomic<unsigned> count{0};unsigned n=++count;
                        if(n<=12 || n%240==0)biliTracePixel(pixel);
                        BOOL result=((BOOL(*)(id,SEL,id,void*,CVPixelBufferRef))original)(object,sel,device,matrix,pixel);
                        if(n<=12 || n%240==0)sm2_log("B24 quad-update[%u] device=%d result=%d",n,device!=nil,result);
                        return result;
                    });
                } else if(layout && returns && !strcmp(returns,"B") && biliArg(method,3,"i")){
                    replacement=imp_implementationWithBlock(^BOOL(id object,id device,int matrix,CVPixelBufferRef pixel){
                        biliTracePixel(pixel);return ((BOOL(*)(id,SEL,id,int,CVPixelBufferRef))original)(object,sel,device,matrix,pixel);
                    });
                } else if(layout && returns && !strcmp(returns,"B") && biliArg(method,3,"q")){
                    replacement=imp_implementationWithBlock(^BOOL(id object,id device,NSInteger matrix,CVPixelBufferRef pixel){
                        biliTracePixel(pixel);return ((BOOL(*)(id,SEL,id,NSInteger,CVPixelBufferRef))original)(object,sel,device,matrix,pixel);
                    });
                } else if(layout && returns && !strcmp(returns,"v") && biliArg(method,3,"i")){
                    replacement=imp_implementationWithBlock(^void(id object,id device,int matrix,CVPixelBufferRef pixel){
                        biliTracePixel(pixel);((void(*)(id,SEL,id,int,CVPixelBufferRef))original)(object,sel,device,matrix,pixel);
                    });
                } else if(layout && returns && !strcmp(returns,"v") && biliArg(method,3,"q")){
                    replacement=imp_implementationWithBlock(^void(id object,id device,NSInteger matrix,CVPixelBufferRef pixel){
                        biliTracePixel(pixel);((void(*)(id,SEL,id,NSInteger,CVPixelBufferRef))original)(object,sel,device,matrix,pixel);
                    });
                }
                if(replacement){method_setImplementation(method,replacement);sm2_log("B24 pixel handoff observation installed; original invoked");}
                else sm2_log("B24 pixel handoff signature unsupported; original unchanged");
                free(returns);free(pixelType);
            }
            free(methods);
        }
        b22InstallRoutes();
        b23InstallOffscreen();
        sm2_log("B24 original player observation ready");
    });
}
