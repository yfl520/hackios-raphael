/* RAM-only resources for native GLES; original System is never written. */
#include "mini.h"
int main(int argc,char **argv,char **envp) {
 (void)argc;(void)argv;(void)envp;
 if(mount("bindfs","/System/Library/Extensions",MNT_RDONLY|MNT_UNION,(void *)"/redwood/metal/extensions")) {
  printf("raphael-metal: V24 plugin overlay failed\n");return 1;
 }
 const char *target="/System/Library/Extensions/AppleMetalGLRenderer.bundle";
 if(mount("bindfs",target,MNT_RDONLY|MNT_UNION,(void *)"/redwood/metal/resources/System/Library/Extensions/AppleMetalGLRenderer.bundle")) {
  printf("raphael-metal: V24 GLES resources overlay failed\n");return 2;
 }
 int fd=open("/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib",O_RDONLY,0);
 unsigned char magic[4]={};int count=fd>=0?read(fd,magic,4):-1;if(fd>=0)close(fd);
 if(count!=4 || memcmp(magic,"MTLB",4)) {printf("raphael-metal: V24 GLES shader resource unreadable\n");return 3;}
 printf("raphael-metal: V24 native GLES shader resource readable; CPU desktop retained\n");return 0;
}
