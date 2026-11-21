/* SPDX-License-Identifier: MIT
 * Hardware acceptance: Metal discovery, padded RGB and NV12 IOSurface imports,
 * GPU texture-to-buffer copies and byte-for-byte readback. No app/UI automation.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "bridge/NativeMetalBridge.h"

static void cacheFlush(void *address, size_t bytes) {
    uintptr_t end=(uintptr_t)address+bytes;
    for(uintptr_t at=(uintptr_t)address&~(uintptr_t)63;at<end;at+=64)
        __asm__ volatile("dc civac, %0"::"r"(at):"memory");
    __asm__ volatile("dsb sy":::"memory");
}

static uint8_t pattern(unsigned plane,size_t x,size_t y) {
    return (uint8_t)(19+71*plane+5*x+7*y);
}

static int readSurface(id<MTLDevice> device, bool planar) {
    const size_t width=planar?126:73, height=planar?62:37;
    NSDictionary *properties;
    if(planar) {
        properties=@{@"IOSurfaceWidth":@(width),@"IOSurfaceHeight":@(height),@"IOSurfacePixelFormat":@(0x34323076),
            @"IOSurfaceAllocSize":@24576,@"IOSurfacePlaneInfo":@[
                @{@"IOSurfacePlaneWidth":@(width),@"IOSurfacePlaneHeight":@(height),@"IOSurfacePlaneBytesPerElement":@1,
                  @"IOSurfacePlaneBytesPerRow":@192,@"IOSurfacePlaneOffset":@0,@"IOSurfacePlaneSize":@(192*height)},
                @{@"IOSurfacePlaneWidth":@(width/2),@"IOSurfacePlaneHeight":@(height/2),@"IOSurfacePlaneBytesPerElement":@2,
                  @"IOSurfacePlaneBytesPerRow":@192,@"IOSurfacePlaneOffset":@16384,@"IOSurfacePlaneSize":@(192*height/2)}]};
    } else {
        properties=@{@"IOSurfaceWidth":@(width),@"IOSurfaceHeight":@(height),@"IOSurfacePixelFormat":@(0x42475241),
            @"IOSurfaceBytesPerElement":@4,@"IOSurfaceBytesPerRow":@320,@"IOSurfaceAllocSize":@12288};
    }
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if(!surface){printf("{\"stage\":\"surface-create\",\"planar\":%d,\"passed\":false}\n",planar);return 10;}
    size_t planes=IOSurfaceGetPlaneCount(surface);
    if(planes!=(planar?2:0)) {
        printf("{\"stage\":\"surface-layout\",\"planes\":%zu,\"passed\":false}\n",planes);
        CFRelease(surface);return 11;
    }
    if(IOSurfaceLock(surface,0,NULL)!=0){CFRelease(surface);return 12;}
    uint8_t *base=(uint8_t *)IOSurfaceGetBaseAddress(surface);
    memset(base,0xa5,IOSurfaceGetAllocSize(surface));
    const size_t count=planar?2:1;
    for(size_t plane=0;plane<count;plane++) {
        uint8_t *pixels=planar?(uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,plane):base;
        size_t row=planar?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
        size_t rows=plane?height/2:height,bytes=planar?width:width*4;
        for(size_t y=0;y<rows;y++)for(size_t x=0;x<bytes;x++)pixels[row*y+x]=pattern((unsigned)plane,x,y);
    }
    cacheFlush(base,IOSurfaceGetAllocSize(surface));
    IOSurfaceUnlock(surface,0,NULL);
    int result=0;
    @try {
        id<MTLCommandQueue> queue=[device newCommandQueue];
        if(!queue){result=13;}
        for(size_t plane=0;queue && !result && plane<count;plane++) {
            size_t w=plane?width/2:width,h=plane?height/2:height,bytes=planar?width:width*4;
            MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                planar?(plane?MTLPixelFormatRG8Unorm:MTLPixelFormatR8Unorm):MTLPixelFormatBGRA8Unorm
                width:w height:h mipmapped:NO];
            desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> texture=[device newTextureWithDescriptor:desc iosurface:surface plane:plane];
            if(!texture){printf("{\"stage\":\"texture-import\",\"planar\":%d,\"plane\":%zu,\"passed\":false}\n",planar,plane);result=14;break;}
            size_t outRow=(bytes+255)&~(size_t)255;
            id<MTLBuffer> output=[device newBufferWithLength:outRow*h options:MTLResourceStorageModeShared];
            if(!output || !output.contents){result=15;break;}
            memset(output.contents,0xcc,outRow*h);cacheFlush(output.contents,outRow*h);
            id<MTLCommandBuffer> commands=[queue commandBuffer];
            id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
            if(!commands || !blit){result=16;break;}
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(w,h,1) toBuffer:output destinationOffset:0
                destinationBytesPerRow:outRow destinationBytesPerImage:outRow*h];
            [blit endEncoding];[commands commit];[commands waitUntilCompleted];
            if(commands.status!=MTLCommandBufferStatusCompleted) {
                printf("{\"stage\":\"gpu-completion\",\"status\":%lu,\"passed\":false}\n",(unsigned long)commands.status);
                fprintf(stderr,"GPU error: %s\n",commands.error.description.UTF8String);result=17;break;
            }
            cacheFlush(output.contents,outRow*h);
            const uint8_t *got=(const uint8_t *)output.contents;size_t bad=0;
            for(size_t y=0;y<h;y++)for(size_t x=0;x<bytes;x++)bad+=got[outRow*y+x]!=pattern((unsigned)plane,x,y);
            printf("{\"stage\":\"gpu-readback\",\"planar\":%d,\"plane\":%zu,\"checked_bytes\":%zu,\"bad_bytes\":%zu,\"passed\":%s}\n",
                   planar,plane,bytes*h,bad,bad?"false":"true");fflush(stdout);
            if(bad)result=18;
        }
    } @catch(NSException *error) {
        fprintf(stderr,"Metal exception: %s: %s\n",error.name.UTF8String,error.reason.UTF8String);result=19;
    }
    CFRelease(surface);return result;
}

#include "GLESProbeV24.inc"

int main(int argc,char **argv) {
    @autoreleasepool {
        setbuf(stdout,NULL);alarm(90);
        if(argc>2 && !strcmp(argv[2],"--suite")) {
            alarm(240);sleep(30);
            const char *modes[]={"--runtime","--surface","--gles"};
            for(const char *mode:modes) {
                pid_t pid=fork();
                if(pid<0)return 20;
                if(pid==0){execl(argv[0],argv[0],argv[1],mode,(char *)NULL);_exit(21);}
                int status=0;
                while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return 22;}
                printf("{\"stage\":\"suite-child\",\"mode\":\"%s\",\"wait_status\":%d}\n",mode,status);
                if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)return 23;
            }
            puts("{\"stage\":\"suite-final\",\"passed\":true}");return 0;
        }
        setenv("TU_DARWIN_SERVICE","IOAcceleratorES",1);
        setenv("TU_DARWIN_OPEN_TYPE","0x4147",1);
        setenv("RAPHAEL_METAL_TRACE","1",1);
        setenv("NMB_LOG_STDERR","1",1);
        fprintf(stderr,"Metal probe process: %s; mode: %s\n",getprogname(),argc>2?argv[2]:"--surface");
        const char *bundle=argc>1?argv[1]:"/System/Library/Extensions/RaphaelMetal2.bundle/SweetMetal2";
        if(argc>2 && !strcmp(argv[2],"--runtime")) {
            // Framework discovery must load the plugin itself in the other child.
            // Preloading its RAM alias creates two Objective-C class registries.
            void *image=dlopen(bundle,RTLD_NOW|RTLD_GLOBAL);
            if(!image){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
            auto selftest=(decltype(&nmb_selftest))dlsym(image,"nmb_selftest");
            if(!selftest){fprintf(stderr,"nmb_selftest: %s\n",dlerror());return 4;}
            NSString *folder=[[@(bundle) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
            NmbSelftest result={};
            uint32_t code=selftest(folder.UTF8String,"/var/tmp/raphael-metal-cache",NULL,0,
                [](uint32_t stage){printf("{\"stage\":\"runtime-marker\",\"marker\":%u}\n",stage);},&result);
            printf("{\"stage\":\"runtime-final\",\"code\":%u,\"compute_bad\":%u,\"render_bad\":%u,\"import_bad\":%u,\"passed\":%s}\n",
                code,result.compute_bad,result.render_bad,result.import_bad,code?"false":"true");
            fprintf(stderr,"Runtime message: %.96s\n",result.message);return code?5:0;
        }
        if(argc>2 && !strcmp(argv[2],"--gles"))return testGLESV24();
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device){puts("{\"stage\":\"metal-discovery\",\"passed\":false}");return 3;}
        printf("{\"stage\":\"metal-discovery\",\"passed\":true}\n");
        fprintf(stderr,"Metal device: %s\n",device.name.UTF8String);
        int result=readSurface(device,false);
        if(!result)result=readSurface(device,true);
        printf("{\"stage\":\"final\",\"exit_code\":%d,\"passed\":%s}\n",result,result?"false":"true");
        return result;
    }
}
