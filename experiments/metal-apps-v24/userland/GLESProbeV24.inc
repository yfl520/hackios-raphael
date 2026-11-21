// Synthetic GLES2/CV texture round trip. Never reads application video data.
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <CoreVideo/CoreVideo.h>

static GLuint g24Shader(GLenum type,const char *source) {
    GLuint shader=glCreateShader(type);if(!shader)return 0;
    glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
    GLint passed=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&passed);
    if(!passed){char log[512]={};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
        fprintf(stderr,"GLES24 shader compile: %.500s\n",log);glDeleteShader(shader);return 0;}
    return shader;
}
static int g24Run(EAGLContext *context) {
    printf("{\"stage\":\"gles-context-v24\",\"created\":%s,\"api\":%lu}\n",
        context?"true":"false",(unsigned long)context.API);
    if(!context || ![EAGLContext setCurrentContext:context])return 41;
    fprintf(stderr,"GLES24 renderer: %.160s; version: %.100s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    CVOpenGLESTextureCacheRef cache=nullptr;
    CVReturn cv=CVOpenGLESTextureCacheCreate(kCFAllocatorDefault,nullptr,context,nullptr,&cache);
    printf("{\"stage\":\"gles-cache-v24\",\"code\":%d,\"created\":%s}\n",cv,cache?"true":"false");
    if(cv || !cache)return 42;
    // ARC does not own CF handles. Scope all early returns inside a lambda so cleanup is unconditional.
    CVPixelBufferRef input=nullptr,output=nullptr;
    CVOpenGLESTextureRef luma=nullptr,chroma=nullptr,target=nullptr;
    GLuint fb=0,program=0,vs=0,fs=0;
    int result=[&]()->int {
        NSDictionary *attrs=@{(__bridge id)kCVPixelBufferIOSurfacePropertiesKey:@{},
            (__bridge id)kCVPixelBufferOpenGLESCompatibilityKey:@YES};
        cv=CVPixelBufferCreate(kCFAllocatorDefault,16,16,kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            (__bridge CFDictionaryRef)attrs,&input);
        if(cv || !input)return 43;
        cv=CVPixelBufferCreate(kCFAllocatorDefault,16,16,kCVPixelFormatType_32BGRA,
            (__bridge CFDictionaryRef)attrs,&output);
        if(cv || !output)return 44;
        cv=CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault,cache,input,nullptr,
            GL_TEXTURE_2D,GL_LUMINANCE,16,16,GL_LUMINANCE,GL_UNSIGNED_BYTE,0,&luma);
        printf("{\"stage\":\"gles-luma-v24\",\"code\":%d}\n",cv);
        if(cv || !luma)return 45;
        cv=CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault,cache,input,nullptr,
            GL_TEXTURE_2D,GL_LUMINANCE_ALPHA,8,8,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,1,&chroma);
        printf("{\"stage\":\"gles-chroma-v24\",\"code\":%d}\n",cv);
        if(cv || !chroma)return 46;
        cv=CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault,cache,output,nullptr,
            GL_TEXTURE_2D,GL_RGBA,16,16,GL_BGRA,GL_UNSIGNED_BYTE,0,&target);
        printf("{\"stage\":\"gles-output-v24\",\"code\":%d}\n",cv);
        if(cv || !target)return 47;
        glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,CVOpenGLESTextureGetName(target),0);
        GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
        printf("{\"stage\":\"gles-framebuffer-v24\",\"status\":%u}\n",status);
        if(status!=GL_FRAMEBUFFER_COMPLETE)return 48;
        vs=g24Shader(GL_VERTEX_SHADER,"attribute vec2 p; varying vec2 uv; void main(){gl_Position=vec4(p,0.0,1.0);uv=p*0.5+0.5;}");
        fs=g24Shader(GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 uv; uniform sampler2D yTex; uniform sampler2D uvTex; void main(){float y=texture2D(yTex,uv).r; vec2 c=texture2D(uvTex,uv).ra; gl_FragColor=vec4(y,c,1.0);}");
        if(!vs || !fs)return 49;
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
        glBindAttribLocation(program,0,"p");glLinkProgram(program);GLint linked=0;
        glGetProgramiv(program,GL_LINK_STATUS,&linked);
        if(!linked){char log[512]={};glGetProgramInfoLog(program,sizeof(log),nullptr,log);
            fprintf(stderr,"GLES24 program link: %.500s\n",log);return 50;}
        glUseProgram(program);glViewport(0,0,16,16);
        glUniform1i(glGetUniformLocation(program,"yTex"),0);glUniform1i(glGetUniformLocation(program,"uvTex"),1);
        CVOpenGLESTextureRef textures[]={luma,chroma};
        for(unsigned i=0;i<2;i++){
            glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,CVOpenGLESTextureGetName(textures[i]));
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        const GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(0);
        for(unsigned frame=0;frame<4;frame++) {
            const unsigned y=40+frame*40,u=80+frame*20,v=200-frame*30;
            if(CVPixelBufferLockBaseAddress(input,0))return 51;
            for(size_t row=0;row<16;row++)memset((char*)CVPixelBufferGetBaseAddressOfPlane(input,0)+row*CVPixelBufferGetBytesPerRowOfPlane(input,0),y,16);
            for(size_t row=0;row<8;row++){
                auto p=(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(input,1)+row*CVPixelBufferGetBytesPerRowOfPlane(input,1);
                for(unsigned col=0;col<8;col++){p[col*2]=u;p[col*2+1]=v;}}
            CVPixelBufferUnlockBaseAddress(input,0);
            // Explicit cache maintenance matches the noncoherent test's existing contract.
            IOSurfaceRef surface=CVPixelBufferGetIOSurface(input);
            if(!surface)return 52;
            cacheFlush(IOSurfaceGetBaseAddress(surface),IOSurfaceGetAllocSize(surface));
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
            uint8_t pixels[16*16*4]={};glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
            GLenum error=glGetError();unsigned bad=0;
            for(unsigned i=0;i<256;i++)for(unsigned c=0;c<4;c++){
                int expected=c==0?y:c==1?u:c==2?v:255;
                if(abs((int)pixels[i*4+c]-expected)>2)bad++;}
            printf("{\"stage\":\"gles-nv12-draw-readback-v24\",\"frame\":%u,\"error\":%u,\"bad\":%u,\"passed\":%s}\n",frame,error,bad,(!error&&!bad)?"true":"false");
            if(error || bad)return 53;
            if(CVPixelBufferLockBaseAddress(output,kCVPixelBufferLock_ReadOnly))return 55;
            cacheFlush(CVPixelBufferGetBaseAddress(output),CVPixelBufferGetDataSize(output));
            unsigned outputBad=0;
            for(size_t row=0;row<16;row++){
                auto p=(uint8_t*)CVPixelBufferGetBaseAddress(output)+row*CVPixelBufferGetBytesPerRow(output);
                for(unsigned col=0;col<16;col++)for(unsigned c=0;c<4;c++){
                    int expected=c==0?v:c==1?u:c==2?y:255;
                    if(abs((int)p[col*4+c]-expected)>2)outputBad++;
                }
            }
            CVPixelBufferUnlockBaseAddress(output,kCVPixelBufferLock_ReadOnly);
            printf("{\"stage\":\"gles-output-cvbuffer-v24\",\"frame\":%u,\"bad\":%u,\"passed\":%s}\n",frame,outputBad,outputBad?"false":"true");
            if(outputBad)return 56;
        }
        return 0;
    }();
    if(program)glDeleteProgram(program);if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);
    if(fb)glDeleteFramebuffers(1,&fb);
    if(target)CFRelease(target);if(chroma)CFRelease(chroma);if(luma)CFRelease(luma);
    if(output)CFRelease(output);if(input)CFRelease(input);CFRelease(cache);
    [EAGLContext setCurrentContext:nil];
    return result;
}
static int testGLESV24() {
    @try {
        EAGLContext *context=[[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        int result=g24Run(context);
        printf("{\"stage\":\"gles-final-v24\",\"code\":%d,\"passed\":%s}\n",result,result?"false":"true");return result;
    } @catch(NSException *e) {
        fprintf(stderr,"GLES24 exception: %.100s %.300s\n",e.name.UTF8String,e.reason.UTF8String);
        return 54;
    }
}
