/* Explicitly started local screen recorder. Idle after boot. No network server. */
#define _DARWIN_C_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include "raphael-screenrec.h"
#include "mjpeg-mov.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#define SOCKET_PATH "/var/run/raphael-recorder.sock"
#define RECORD_DIR "/var/mobile/Media/HakiOSRecordings"
static volatile sig_atomic_t quitting;
static struct {
 int fd,active,step,fps;unsigned frames;
 uint64_t began,next,sequence,video_began;
 uint8_t *packet,*jpeg;size_t jpeg_bytes,jpeg_capacity;
 hsr_movie movie;FILE *file;
 char partial[512],last[512],error[180];
} r={.fd=-1};
static uint64_t clock_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void signal_quit(int sig) {(void)sig;quitting=1;}
static void set_error(const char *fmt,...) {va_list a;va_start(a,fmt);vsnprintf(r.error,sizeof(r.error),fmt,a);va_end(a);}
static void jpeg_collect(void *context,void *bytes,int size) {
 (void)context;
 if (size<0 || r.jpeg_bytes>HSR_MAX_PIXELS*2u || (unsigned)size>HSR_MAX_PIXELS*2u-r.jpeg_bytes) {r.jpeg_bytes=HSR_MAX_PIXELS*2u+1;return;}
 size_t need=r.jpeg_bytes+(unsigned)size;
 if (need>r.jpeg_capacity) {
  size_t cap=need+65536;uint8_t *p=realloc(r.jpeg,cap);
  if (!p) {r.jpeg_bytes=HSR_MAX_PIXELS*2u+1;return;}
  r.jpeg=p;r.jpeg_capacity=cap;
 }
 memcpy(r.jpeg+r.jpeg_bytes,bytes,(size_t)size);r.jpeg_bytes=need;
}
static void stop_recording(void) {
 if (!r.active) return;
 r.active=0;
 if (r.fd>=0) {ioctl(r.fd,HSR_STOP);close(r.fd);r.fd=-1;}
 if (r.file) {
  int ok=mov_finish(&r.movie);
  if (!ok && fsync(fileno(r.file))) ok=-1;
  if (!ok) (void)fchown(fileno(r.file),501,501);
  if (fclose(r.file)) ok=-1;r.file=NULL;
  if (!ok) {
   size_t n=strlen(r.partial);
   if (n>5 && !strcmp(r.partial+n-5,".part")) {
    char completed[512];snprintf(completed,sizeof(completed),"%.*s.mov",(int)(n-5),r.partial);
    if (!rename(r.partial,completed)) snprintf(r.last,sizeof(r.last),"%s",completed);
    else set_error("rename: %s",strerror(errno));
   }
  } else {
   if (!r.error[0]) set_error("Recording could not be finalized");
   if (!r.frames) unlink(r.partial);
  }
 }
 free(r.packet);r.packet=NULL;free(r.jpeg);r.jpeg=NULL;r.jpeg_bytes=r.jpeg_capacity=0;
}
static int start_recording(int step,int fps) {
 if (r.active) return 0;
 r.error[0]=0;r.frames=0;r.sequence=0;r.video_began=0;r.partial[0]=0;
 if (step<2 || step>4 || fps<1 || fps>30) {set_error("Invalid capture options");return -1;}
 struct stat st;
 if (mkdir(RECORD_DIR,0755) && errno!=EEXIST) {set_error("Create recording directory: %s",strerror(errno));return -1;}
 if (lstat(RECORD_DIR,&st) || !S_ISDIR(st.st_mode)) {set_error("Recording directory is not a directory");return -1;}
 (void)chown(RECORD_DIR,501,501);
 struct statvfs fs;
 if (statvfs(RECORD_DIR,&fs) || (uint64_t)fs.f_bavail*fs.f_frsize<64u*1024*1024) {set_error("Less than 64 MiB free");return -1;}
 r.fd=open("/dev/raphael-screen",O_RDONLY|O_CLOEXEC);
 if (r.fd<0) {set_error("Capture driver: %s",strerror(errno));return -1;}
 struct hsr_config cfg={HSR_VERSION,(uint32_t)step,(uint32_t)fps,0};
 if (ioctl(r.fd,HSR_START,&cfg)) {set_error("Capture start: %s",strerror(errno));close(r.fd);r.fd=-1;return -1;}
 r.packet=malloc(sizeof(struct hsr_frame)+HSR_MAX_PIXELS);
 if (!r.packet) {set_error("Out of memory");ioctl(r.fd,HSR_STOP);close(r.fd);r.fd=-1;return -1;}
 r.step=step;r.fps=fps;r.began=clock_ns();r.next=r.began+3000000000ull;r.active=1;
 return 0;
}
static void record_tick(void) {
 uint64_t now=clock_ns();
 if (!r.active || now<r.next) return;
 if (now-r.began>603000000000ull) {stop_recording();return;}
 r.next+=1000000000ull/r.fps;
 if (r.next<=now) r.next=now+1000000000ull/r.fps; /* no busy catch-up after a long encode */
 ssize_t n=read(r.fd,r.packet,sizeof(struct hsr_frame)+HSR_MAX_PIXELS);
 if (n<0) {
  if ((errno==EAGAIN || errno==EINTR) && now-r.began<13000000000ull) return;
  set_error("Capture read: %s",strerror(errno));stop_recording();return;
 }
 struct hsr_frame *f=(struct hsr_frame *)r.packet;
 if (n<(ssize_t)sizeof(*f) || f->magic!=HSR_MAGIC || f->version!=HSR_VERSION ||
     !f->width || f->width>HSR_MAX_W || !f->height || f->height>HSR_MAX_H ||
     f->stride!=f->width*3 || f->bytes!=f->stride*f->height || n!=(ssize_t)(sizeof(*f)+f->bytes)) {
  set_error("Invalid capture packet");stop_recording();return;
 }
 if (f->sequence!=r.sequence) {
  r.jpeg_bytes=0;
  if (!stbi_write_jpg_to_func(jpeg_collect,NULL,(int)f->width,(int)f->height,3,r.packet+sizeof(*f),75) ||
      r.jpeg_bytes>HSR_MAX_PIXELS*2u || !r.jpeg_bytes) {
   set_error("JPEG encoding failed");stop_recording();return;
  }
  r.sequence=f->sequence;
 }
 if (!r.file) {
  time_t t=time(NULL);struct tm date;localtime_r(&t,&date);char name[96];
  strftime(name,sizeof(name),"Screen-%Y%m%d-%H%M%S",&date);
  snprintf(r.partial,sizeof(r.partial),RECORD_DIR "/%s-%d.part",name,getpid());
  int fd=open(r.partial,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
  if (fd<0 || !(r.file=fdopen(fd,"wb"))) {
   if (fd>=0) close(fd);set_error("Create recording: %s",strerror(errno));stop_recording();return;
  }
  if (mov_begin(&r.movie,r.file,f->width,f->height,(uint32_t)r.fps)) {
   set_error("Movie initialization failed");stop_recording();return;
  }
 }
 if (f->width!=r.movie.width || f->height!=r.movie.height) {
  set_error("Display dimensions changed");stop_recording();return;
 }
 if (!r.video_began) r.video_began=now;
 unsigned desired=(unsigned)((now-r.video_began)*r.fps/1000000000ull)+1;
 while (r.frames<desired) {
 if (mov_frame(&r.movie,r.jpeg,r.jpeg_bytes)) {
  set_error("Recording reached size/frame limit, or disk write failed");stop_recording();return;
 }
 r.frames++;
 }
 if (r.frames%30==0) {
  struct statvfs fs;
  if (statvfs(RECORD_DIR,&fs) || (uint64_t)fs.f_bavail*fs.f_frsize<32u*1024*1024) {set_error("Low disk space");stop_recording();}
 }
}
static void reply(int client) {
 char json[1400];
 /* All values below are fixed strings, numeric fields, or generated safe paths. */
 snprintf(json,sizeof(json),"{\"active\":%s,\"frames\":%u,\"fps\":%d,\"elapsed\":%.1f,\"file\":\"%s\",\"error\":\"%s\"}\n",
  r.active?"true":"false",r.frames,r.fps,r.active?(clock_ns()-r.began)/1e9:0,r.last,r.error);
 (void)send(client,json,strlen(json),0);
}
int main(void) {
 signal(SIGTERM,signal_quit);signal(SIGINT,signal_quit);signal(SIGPIPE,SIG_IGN);
 struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;snprintf(addr.sun_path,sizeof(addr.sun_path),SOCKET_PATH);
 int server=socket(AF_UNIX,SOCK_STREAM,0);if (server<0) return 1;
 /* A second daemon must not unlink the first daemon's active socket. */
 if (!connect(server,(struct sockaddr *)&addr,sizeof(addr))) {close(server);return 0;}
 close(server);server=socket(AF_UNIX,SOCK_STREAM,0);if(server<0) return 1;
 unlink(SOCKET_PATH);
 if (bind(server,(struct sockaddr *)&addr,sizeof(addr)) || chmod(SOCKET_PATH,0600) || chown(SOCKET_PATH,501,501) || listen(server,4)) return 2;
 while (!quitting) {
  record_tick();struct pollfd p={server,POLLIN,0};int ready=poll(&p,1,r.active?20:1000);
  if (ready<=0 || !(p.revents&POLLIN)) continue;
  int client=accept(server,NULL,NULL);if (client<0) continue;
  uid_t uid;gid_t gid;
  if (getpeereid(client,&uid,&gid) || (uid!=0 && uid!=501)) {close(client);continue;}
  struct timeval timeout={1,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  char command[80]={0};ssize_t n=recv(client,command,sizeof(command)-1,0);
  if (n>0) {
   if (!strncmp(command,"START ",6)) {int step=0,fps=0;if (sscanf(command+6,"%d %d",&step,&fps)==2) start_recording(step,fps);}
   else if (!strncmp(command,"STOP",4)) stop_recording();
   reply(client);
  }
  close(client);
 }
 stop_recording();close(server);unlink(SOCKET_PATH);return 0;
}
