// SPDX-License-Identifier: MIT
// A system process gets one GPU attempt per boot. Any restart uses CPU fallback,
// even if the first process previously made progress. Idle work is not a hang.
#include "MetalSafetyV15.hpp"
#include <mutex>
#include <sys/file.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

static bool sm2SystemProcess() {
    return !strcmp(getprogname(),"backboardd") || !strcmp(getprogname(),"SpringBoard");
}
static std::mutex sm2HealthLock;
static raphael_metal_v15::Progress sm2Progress;
static raphael_metal_v15::BootRecord sm2Record{};
static int sm2HealthFD=-1;
static uint64_t sm2ClockUs() {
    struct timespec t{};
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static bool sm2WriteHealth(uint64_t state) {
    sm2Record.state=state;
    return pwrite(sm2HealthFD,&sm2Record,sizeof(sm2Record),0)==sizeof(sm2Record) &&
        ftruncate(sm2HealthFD,sizeof(sm2Record))==0 && fsync(sm2HealthFD)==0;
}
static bool sm2BeginHealth(const char *cache) {
    if(!sm2SystemProcess())return true;
    struct timeval boot{};size_t count=sizeof(boot);
    if(sysctlbyname("kern.boottime",&boot,&count,NULL,0)||count!=sizeof(boot)||boot.tv_sec<=0)return false;
    char path[4096];
    if(snprintf(path,sizeof(path),"%s/system-boot-v15",cache)>=(int)sizeof(path))return false;
    int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return false;
    struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||flock(fd,LOCK_EX|LOCK_NB)) {close(fd);return false;}
    raphael_metal_v15::BootRecord prior{};
    ssize_t got=pread(fd,&prior,sizeof(prior),0);
    if(got<0 || (got>0 && (got!=sizeof(prior)||prior.magic!=raphael_metal_v15::HealthMagic)) ||
       (got==sizeof(prior) && raphael_metal_v15::sameBoot(prior,boot.tv_sec,boot.tv_usec))) {
        close(fd);return false;
    }
    sm2HealthFD=fd;
    sm2Record={raphael_metal_v15::HealthMagic,(uint64_t)boot.tv_sec,(uint64_t)boot.tv_usec,1};
    if(!sm2WriteHealth(1)){close(fd);sm2HealthFD=-1;return false;}
    sm2Progress.started=sm2ClockUs();
    return sm2Progress.started!=0;
}
static void sm2HealthFailLocked(const char *reason) {
    if(sm2Progress.failed)return;
    sm2Progress.failed=true;
    bool saved=sm2WriteHealth(3);
    sm2_log("system GPU failure: %s, boot record saved=%d; restart uses CPU",reason,saved);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),
        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^{kill(getpid(),SIGTERM);});
}
extern "C" void sm2_gpu_event(unsigned event,bool success) {
    if(!sm2SystemProcess() || sm2HealthFD<0)return;
    std::lock_guard<std::mutex> lock(sm2HealthLock);
    if(sm2Progress.failed)return;
    uint64_t now=sm2ClockUs();
    if(!now){sm2HealthFailLocked("monotonic clock unavailable");return;}
    if(event==0)sm2Progress.commit(now);
    else {
        if(!success){sm2HealthFailLocked("command buffer failed");return;}
        sm2Progress.finish(now,true);
    }
}
static void sm2WatchHealth() {
    if(!sm2SystemProcess()||sm2HealthFD<0)return;
    static dispatch_source_t timer;
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),NSEC_PER_SEC,NSEC_PER_MSEC*100);
    dispatch_source_set_event_handler(timer,^{
        std::lock_guard<std::mutex> lock(sm2HealthLock);
        uint64_t now=sm2ClockUs();
        if(sm2Progress.failed)return;
        if(!now||sm2Progress.stalled(now)){sm2HealthFailLocked("active commands stalled 20 seconds");return;}
        if(!sm2Progress.healthy && sm2Progress.ready(now)) {
            if(!sm2WriteHealth(2)){sm2HealthFailLocked("health record write failed");return;}
            sm2Progress.healthy=true;
            sm2_log("system GPU completed=%llu; boot health passed, restart guard remains active",
                (unsigned long long)sm2Progress.completed);
        }
    });
    dispatch_resume(timer);
}
