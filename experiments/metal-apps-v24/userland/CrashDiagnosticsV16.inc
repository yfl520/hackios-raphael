// Observe failures without converting them into successful calls or preventing termination.
#include "MetalCrashV16.hpp"
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <sys/ucontext.h>
#include <execinfo.h>
#include <signal.h>
static int sm2FaultFD=-1;
static const int sm2FaultSignals[]={SIGABRT,SIGSEGV,SIGBUS,SIGILL,SIGTRAP};
static struct sigaction sm2PriorFaultActions[5];
static NSUncaughtExceptionHandler *sm2PriorException;
static void sm2FaultLine(raphael_metal_v16::CrashLine &line) {
    line.end();if(sm2FaultFD>=0)(void)!write(sm2FaultFD,line.bytes,line.used);
}
static void sm2ObserveFault(int number,siginfo_t *info,void *context) {
    using raphael_metal_v16::CrashLine;
    CrashLine line;line.text("MTL2-FAULT signal=");line.hex(number);
    line.text(" pid=");line.hex(getpid());line.text(" address=");line.hex(info?(uintptr_t)info->si_addr:0);
    sm2FaultLine(line);
    if(context && ((ucontext_t *)context)->uc_mcontext) {
        const auto &ss=((ucontext_t *)context)->uc_mcontext->__ss;
        uint64_t fp=__darwin_arm_thread_state64_get_fp(ss);
        CrashLine regs;regs.text("MTL2-FAULT pc=");regs.hex((uintptr_t)__darwin_arm_thread_state64_get_pc_fptr(ss));
        regs.text(" lr=");regs.hex((uintptr_t)__darwin_arm_thread_state64_get_lr_fptr(ss));
        regs.text(" sp=");regs.hex(__darwin_arm_thread_state64_get_sp(ss));regs.text(" fp=");regs.hex(fp);sm2FaultLine(regs);
        // Read frame records with the kernel copy primitive: never dereference a faulting pointer.
        for(unsigned i=0;i<20 && fp && !(fp&15) && fp<=UINT64_MAX-16;++i) {
            uint64_t record[2]{};vm_size_t got=0;
            if(vm_read_overwrite(mach_task_self(),(vm_address_t)fp,sizeof record,(vm_address_t)record,&got)!=KERN_SUCCESS || got!=sizeof record)break;
            CrashLine frame;frame.text("MTL2-FAULT frame=");frame.hex(i);frame.text(" fp=");frame.hex(fp);frame.text(" lr=");frame.hex(record[1]);sm2FaultLine(frame);
            if(!raphael_metal_v16::nextFrame(fp,record[0]))break;
            fp=record[0];
        }
    }
    // Restore the process's pre-existing action. A default action is re-raised;
    // a synchronous fault also faults normally if the previous handler returns.
    for(unsigned i=0;i<5;++i)if(number==sm2FaultSignals[i]) {
        const struct sigaction &prior=sm2PriorFaultActions[i];
        sigaction(number,&prior,nullptr);
        if(prior.sa_handler==SIG_DFL){kill(getpid(),number);return;}
        if(prior.sa_handler==SIG_IGN)return;
        if(prior.sa_flags&SA_SIGINFO)prior.sa_sigaction(number,info,context);
        else prior.sa_handler(number);
        return;
    }
}
static void sm2ObserveException(NSException *exception) {
    sm2_log("UNCAUGHT %s: %.420s",exception.name.UTF8String?:"?",exception.reason.UTF8String?:"?");
    void *frames[24];int count=backtrace(frames,24);
    for(int i=0;i<count;++i) {
        Dl_info image{};dladdr(frames[i],&image);
        sm2_log("EXCEPTION frame=%d address=%p image=%.160s base=%p offset=%llx",i,frames[i],image.dli_fname?:"?",image.dli_fbase,
            (unsigned long long)((uintptr_t)frames[i]-(uintptr_t)image.dli_fbase));
    }
    if(sm2PriorException && sm2PriorException!=sm2ObserveException)sm2PriorException(exception);
}
static void sm2InstallCrashDiagnostics() {
    static dispatch_once_t once;
    dispatch_once(&once,^{
        sm2FaultFD=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);
        if(sm2FaultFD<0){sm2_log("crash observer unavailable: console open failed");return;}
        sm2PriorException=NSGetUncaughtExceptionHandler();NSSetUncaughtExceptionHandler(sm2ObserveException);
        struct sigaction action{};sigemptyset(&action.sa_mask);
        action.sa_sigaction=sm2ObserveFault;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
        for(unsigned i=0;i<5;++i) {
            if(sigaction(sm2FaultSignals[i],&action,&sm2PriorFaultActions[i]))sm2_log("crash observer install failed signal=%d",sm2FaultSignals[i]);
        }
        for(uint32_t i=0;i<_dyld_image_count();++i) {
            const char *name=_dyld_get_image_name(i);
            if(name && (strstr(name,"/QuartzCore") || strstr(name,"/Metal.framework/") || strstr(name,"/libobjc.") ||
                strstr(name,"/libc++abi.") || strstr(name,"/libsystem_kernel.") || strstr(name,"/UIKitCore")))
                sm2_log("DIAG image=%.220s base=%p slide=%llx",name,_dyld_get_image_header(i),(unsigned long long)_dyld_get_image_vmaddr_slide(i));
        }
        sm2_log("V16 crash observer ready; existing fatal actions preserved");
    });
}
