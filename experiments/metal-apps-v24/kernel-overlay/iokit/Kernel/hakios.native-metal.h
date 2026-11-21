/* SPDX-License-Identifier: MIT
 * Experimental NativeAdreno ABI adapter over the existing Raphael GPU core.
 * Included by hakios.gpu.cpp so both clients share gGPULock and fence ordering.
 * raphael_metal=1 enables this ABI; mode 2 additionally enables the opt-in plugin.
 * Wire ABI: UniXNU / Pixel 4 NativeAdrenoABI.h, version 1.
 */
#include "NativeAdrenoABI.h"

extern "C" uint64_t hakios_gpu_timestamp_ticks(void);

enum { RM_BO_COUNT=4096, RM_WINDOW_BYTES=512u*1024u*1024u };
struct RaphaelMetalBO {
    IOMemoryDescriptor *memory;
    IOMemoryMap *kernelMap;
    uint64_t bytes, iova;
    uint32_t fence, metadataBytes;
    bool imported, pending;
    char metadata[NativeAdrenoAbiMetadataBytes];
};

class RaphaelNativeGPUUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RaphaelNativeGPUUserClient);
    task_t fTask;
    IOLock *fLock;
    RaphaelMetalBO *fBO;
    uint64_t fWindow;
    uint32_t fLastFence;
    bool fClosed, fTunnel, fQueues[NativeAdrenoAbiMaxQueues];
    RaphaelMetalBO *find(uint64_t h) {
        return h && h<=RM_BO_COUNT && fBO[h-1].memory && !fBO[h-1].pending ? &fBO[h-1] : NULL;
    }
    void releaseBO(RaphaelMetalBO *b) {
        if (!b->memory) return;
        if (b->iova) hakios_gpu_unmap(b->iova,b->bytes);
        if (b->kernelMap) b->kernelMap->release();
        if (b->imported) b->memory->complete();
        b->memory->release();
        bzero(b,sizeof(*b));
    }
    void reap() {
        for (unsigned i=0;i<RM_BO_COUNT;i++)
            if (fBO[i].memory && fBO[i].pending && hakios_gpu_done(fBO[i].fence)) releaseBO(&fBO[i]);
    }
    IOReturn method(uint32_t m,const void *in,size_t is,void *out,size_t *os);
public:
    bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *props) APPLE_KEXT_OVERRIDE {
        /* NativeAdreno direct type 0; ios-flame/Turnip accelerator types. */
        if ((type!=0 && type!=2 && type!=0x4147) || !IOUserClient::initWithTask(task,security,type,props)) return false;
        fTunnel=type!=0;
        fTask=task;
        fLock=IOLockAlloc();
        fBO=IONewZero(RaphaelMetalBO,RM_BO_COUNT);
        return fLock && fBO;
    }
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (!IOUserClient::start(provider) || !hakios_gpu_ready() || gGPUCached) return false;
        IOLockLock(gGPULock);
        fWindow=hakios_gpu_address(RM_WINDOW_BYTES);
        IOLockUnlock(gGPULock);
        return fWindow!=0;
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        IOLockLock(fLock);
        if (!fClosed) {
            fClosed=true;
            IOLockLock(gGPULock);
            bool done=!fLastFence || gpuWait(fLastFence,2000);
            if (done) {
                for (unsigned i=0;i<RM_BO_COUNT;i++) releaseBO(&fBO[i]);
                if (fWindow) hakios_gpu_address_free(fWindow,RM_WINDOW_BYTES);
                fWindow=0;
            } else {
                /* In-flight memory and its VA reservation remain owned until reboot.
                 * Never return physical pages that the stalled GPU can still write. */
                IOLog("RaphaelMetal: GPU did not retire client; retaining its memory and VA reservation\n");
            }
            IOLockUnlock(gGPULock);
        }
        IOLockUnlock(fLock);
        if (!isInactive()) terminate();
        return kIOReturnSuccess;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fBO) IODelete(fBO,RaphaelMetalBO,RM_BO_COUNT);
        if (fLock) IOLockFree(fLock);
        IOUserClient::free();
    }
    IOReturn clientMemoryForType(UInt32 token,IOOptionBits *options,IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE {
        IOLockLock(fLock);
        RaphaelMetalBO *b=fClosed ? NULL : find(token);
        IOReturn r=kIOReturnBadArgument;
        if (b && !b->imported) {
            b->memory->retain();*memory=b->memory;*options=0;r=kIOReturnSuccess;
        }
        IOLockUnlock(fLock);return r;
    }
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE {
        if (!a || a->scalarInputCount || a->scalarOutputCount || a->structureInputDescriptor || a->structureOutputDescriptor)
            return kIOReturnBadArgument;
        IOLockLock(fLock);
        if (fClosed) { IOLockUnlock(fLock);return kIOReturnNotOpen; }
        size_t output=a->structureOutputSize;
        const void *input=a->structureInput;
        size_t inputBytes=a->structureInputSize;
        uint32_t header[2]={0,0};
        bool tunnel=false;
        if (input && inputBytes>=sizeof(header)) {
            memcpy(header,input,sizeof(header));
            tunnel=header[0]==0x4e544e4cu; /* upstream NTNL framing */
        }
        if (fTunnel && !tunnel) {IOLockUnlock(fLock);return kIOReturnBadArgument;}
        if (tunnel) {
            input=(const uint8_t *)input+sizeof(header);
            inputBytes-=sizeof(header);
            if (header[1]==0xffffffffu) {
                IOReturn r=(inputBytes==0 && output==0)?kIOReturnSuccess:kIOReturnBadArgument;
                a->structureOutputSize=0;IOLockUnlock(fLock);return r;
            }
            selector=header[1];
        }
        IOReturn r=method(selector,input,inputBytes,a->structureOutput,&output);
        a->structureOutputSize=(uint32_t)(r==kIOReturnSuccess?output:0);
        IOLockUnlock(fLock);return r;
    }
};
OSDefineMetaClassAndStructors(RaphaelNativeGPUUserClient,IOUserClient);

IOReturn RaphaelNativeGPUUserClient::method(uint32_t m,const void *in,size_t is,void *out,size_t *os)
{
#define RM_IN(T) if (!in || is!=sizeof(T)) return kIOReturnBadArgument; const T rqValue=*(const T *)in; const T *rq=&rqValue
#define RM_OUT(T) if (!out || *os<sizeof(T)) return kIOReturnNoSpace; *os=sizeof(T); T *rp=(T *)out; bzero(rp,sizeof(*rp))
#define RM_NO_IN() if (is) return kIOReturnBadArgument
    switch (m) {
    case NativeAdrenoMethodQuery: {
        RM_NO_IN();RM_OUT(NativeAdrenoAbiParams);
        rp->abi_version=1;rp->input_bits=39;rp->output_bits=36;
        rp->flags=NativeAdrenoParamsHostImport;rp->max_bo_bytes=GPU_BUFFER_MAX;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodGetDeviceInfo: {
        RM_NO_IN();RM_OUT(NativeAdrenoAbiDeviceInfo);uint64_t v=0;
        hakios_gpu_parameter(3,&v);rp->chip_id=(uint32_t)v;
        hakios_gpu_parameter(2,&v);rp->gmem_bytes=(uint32_t)v;
        hakios_gpu_parameter(6,&v);rp->gmem_base=v;
        hakios_gpu_parameter(0x10,&v);rp->highest_bank_bit=(uint32_t)v;
        hakios_gpu_parameter(0x12,&v);rp->ubwc_swizzle=(uint32_t)v;
        hakios_gpu_parameter(0x13,&v);rp->macrotile_mode=(uint32_t)v;
        rp->uche_trap_base=0x0001fffffffff000ull;
        rp->ubwc_version=3;rp->flags=NativeAdrenoDeviceClientIova;
        rp->va_start=fWindow;rp->va_size=RM_WINDOW_BYTES;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoAlloc:
    case NativeAdrenoMethodBoImport: {
        uint64_t bytes=0,address=0;
        bool imported=m==NativeAdrenoMethodBoImport;
        if (imported) {
            RM_IN(NativeAdrenoAbiBoImport);bytes=rq->bytes;address=rq->address;
            if (!address || (address&4095) || address+bytes<address) return kIOReturnBadArgument;
        } else {
            RM_IN(NativeAdrenoAbiBoAlloc);bytes=rq->bytes;
            if (rq->align_log2!=12 || rq->contiguous) return kIOReturnUnsupported;
        }
        if (!bytes || bytes>GPU_BUFFER_MAX || (bytes&4095)) return kIOReturnBadArgument;
        RM_OUT(NativeAdrenoAbiBoAllocReply);
        IOLockLock(gGPULock);reap();IOLockUnlock(gGPULock);
        unsigned slot=0;for (;slot<RM_BO_COUNT && fBO[slot].memory;slot++) {}
        if (slot==RM_BO_COUNT) return kIOReturnNoMemory;
        IOMemoryDescriptor *memory=NULL;IOMemoryMap *mapping=NULL;
        if (imported) {
            memory=IOMemoryDescriptor::withAddressRange(address,bytes,kIODirectionInOut,fTask);
            if (!memory) return kIOReturnNoMemory;
            IOReturn r=memory->prepare();
            if (r!=kIOReturnSuccess) {memory->release();return r;}
            mapping=memory->map();
            if (!mapping) {memory->complete();memory->release();return kIOReturnNoMemory;}
        } else {
            memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,round_page_64(bytes),PAGE_SIZE);
            if (!memory) return kIOReturnNoMemory;
        }
        RaphaelMetalBO *b=&fBO[slot];bzero(b,sizeof(*b));
        b->memory=memory;b->kernelMap=mapping;b->bytes=bytes;b->imported=imported;
        rp->handle=slot+1;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoMap: {
        RM_IN(NativeAdrenoAbiBoMap);RM_OUT(NativeAdrenoAbiBoMapReply);
        RaphaelMetalBO *b=find(rq->handle);
        if (!b || b->iova || rq->reserved || !(rq->permissions&1) || (rq->permissions&~3u) || rq->writeback || rq->shareability!=2 ||
            (rq->iova&4095) || rq->iova<fWindow || rq->iova-fWindow>RM_WINDOW_BYTES || b->bytes>RM_WINDOW_BYTES-(rq->iova-fWindow))
            return kIOReturnBadArgument;
        for (unsigned i=0;i<RM_BO_COUNT;i++) {
            RaphaelMetalBO *other=&fBO[i];
            if (other->memory && other->iova && rq->iova<other->iova+other->bytes && other->iova<rq->iova+b->bytes)
                return kIOReturnExclusiveAccess;
        }
        bool ok=true;IOLockLock(gGPULock);
        if (!hakios_gpu_ready()) {IOLockUnlock(gGPULock);return kIOReturnNotReady;}
        for (uint64_t offset=0;offset<b->bytes && ok;offset+=4096) {
            IOByteCount length=0;addr64_t pa=b->memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            ok=pa && length>=4096 && !(pa&4095) && hakios_gpu_map(rq->iova+offset,pa,!(rq->permissions&2));
        }
        hakios_gpu_mapped();
        if (!ok) hakios_gpu_unmap(rq->iova,b->bytes);
        IOLockUnlock(gGPULock);
        if (!ok) return kIOReturnNoMemory;
        b->iova=rq->iova;rp->iova=b->iova;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoFree: {
        RM_IN(NativeAdrenoAbiBoFree);RaphaelMetalBO *b=find(rq->handle);
        if (!b) return kIOReturnBadArgument;
        IOLockLock(gGPULock);b->fence=fLastFence;b->pending=true;reap();IOLockUnlock(gGPULock);
        *os=0;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoCpuMap: {
        RM_IN(NativeAdrenoAbiBoCpuMap);RM_OUT(NativeAdrenoAbiBoCpuMapReply);
        RaphaelMetalBO *b=find(rq->handle);if (!b || b->imported) return kIOReturnBadArgument;
        rp->token=(uint32_t)rq->handle;rp->bytes=b->bytes;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoCpuUnmap: {
        RM_IN(NativeAdrenoAbiBoCpuUnmap);
        if (!find(rq->handle) || rq->token!=rq->handle || rq->reserved) return kIOReturnBadArgument;
        *os=0;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodSubmit: {
        RM_IN(NativeAdrenoAbiSubmit);RM_OUT(NativeAdrenoAbiSubmitReply);
        if (!rq->buffer_count || rq->buffer_count>16 || rq->reserved || rq->queue>8 || (rq->queue && !fQueues[rq->queue-1]) || rq->wait_fence>UINT32_MAX)
            return kIOReturnBadArgument;
        uint64_t addresses[16];uint32_t words[16];
        for (unsigned i=0;i<rq->buffer_count;i++) {
            const NativeAdrenoAbiSubmitBuffer *s=&rq->buffers[i];RaphaelMetalBO *b=find(s->handle);
            if (!b || !b->iova || !s->bytes || (s->bytes&3) || (s->iova&3) || s->iova<b->iova || s->iova-b->iova>b->bytes || s->bytes>b->bytes-(s->iova-b->iova))
                return kIOReturnBadArgument;
            addresses[i]=s->iova;words[i]=(uint32_t)(s->bytes/4);
        }
        IOLockLock(gGPULock);
        if (rq->wait_fence && (!fLastFence || (int32_t)((uint32_t)rq->wait_fence-fLastFence)>0 || !gpuWait((uint32_t)rq->wait_fence,MIN(rq->wait_timeout_ns/1000000,5000ull)))) {
            IOLockUnlock(gGPULock);return kIOReturnTimeout;
        }
        if (gGPUReported || !hakios_gpu_ready()) {IOLockUnlock(gGPULock);return kIOReturnNotReady;}
        for (unsigned i=0;i<RM_BO_COUNT;i++) if (fBO[i].memory && !fBO[i].pending && fBO[i].imported)
            hakios_gpu_to_memory((void *)fBO[i].kernelMap->getVirtualAddress(),fBO[i].bytes);
        /* Match the existing GPU client: publish WC command/data writes even
         * when there are no imported cached pages to flush. */
        hakios_gpu_to_memory(NULL,0);
        uint32_t fence=hakios_gpu_submit(addresses,words,rq->buffer_count);
        if (fence) fLastFence=fence;
        rp->fence=fence;reap();IOLockUnlock(gGPULock);
        return fence ? kIOReturnSuccess : kIOReturnBusy;
    }
    case NativeAdrenoMethodWaitFence: {
        RM_IN(NativeAdrenoAbiWaitFence);
        if (!rq->fence || rq->fence>UINT32_MAX || !fLastFence || (int32_t)((uint32_t)rq->fence-fLastFence)>0) return kIOReturnBadArgument;
        uint64_t ms=MIN(rq->timeout_ns/1000000+(rq->timeout_ns%1000000!=0),5000ull);
        IOLockLock(gGPULock);bool done=gpuWait((uint32_t)rq->fence,ms);reap();IOLockUnlock(gGPULock);
        *os=0;return done ? kIOReturnSuccess : kIOReturnTimeout;
    }
    case NativeAdrenoMethodGetTimestamp: {
        RM_NO_IN();RM_OUT(NativeAdrenoAbiTimestamp);
        IOLockLock(gGPULock);
        if (!hakios_gpu_ready()) {IOLockUnlock(gGPULock);return kIOReturnNotReady;}
        rp->ticks=hakios_gpu_timestamp_ticks();IOLockUnlock(gGPULock);return kIOReturnSuccess;
    }
    case NativeAdrenoMethodGetStatus: {
        RM_NO_IN();RM_OUT(NativeAdrenoAbiStatus);
        IOLockLock(gGPULock);rp->faults=(gGPUReported || !hakios_gpu_ready()) ? 1 : 0;IOLockUnlock(gGPULock);return kIOReturnSuccess;
    }
    case NativeAdrenoMethodCreateQueue: {
        RM_IN(NativeAdrenoAbiQueue);RM_OUT(NativeAdrenoAbiQueueReply);
        if (rq->queue) return kIOReturnBadArgument;
        for (unsigned i=0;i<8;i++) if (!fQueues[i]) {fQueues[i]=true;rp->queue=i+1;return kIOReturnSuccess;}
        return kIOReturnNoResources;
    }
    case NativeAdrenoMethodCloseQueue: {
        RM_IN(NativeAdrenoAbiQueue);if (!rq->queue || rq->queue>8 || !fQueues[rq->queue-1]) return kIOReturnBadArgument;
        fQueues[rq->queue-1]=false;*os=0;return kIOReturnSuccess;
    }
    case NativeAdrenoMethodSetMetadata:
    case NativeAdrenoMethodGetMetadata: {
        RM_IN(NativeAdrenoAbiMetadata);RaphaelMetalBO *b=find(rq->handle);
        if (!b || rq->reserved || rq->bytes>64) return kIOReturnBadArgument;
        if (m==NativeAdrenoMethodSetMetadata) {b->metadataBytes=rq->bytes;memcpy(b->metadata,rq->text,rq->bytes);*os=0;return kIOReturnSuccess;}
        RM_OUT(NativeAdrenoAbiMetadata);rp->handle=rq->handle;rp->bytes=MIN(rq->bytes,b->metadataBytes);memcpy(rp->text,b->metadata,rp->bytes);return kIOReturnSuccess;
    }
    default:return kIOReturnUnsupported;
    }
#undef RM_IN
#undef RM_OUT
#undef RM_NO_IN
}

class NativeAdrenoGpuPublished : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuPublished);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (!IOService::start(provider)) return false;
        setProperty(kIOUserClientClassKey,"RaphaelNativeGPUUserClient");
        registerService();return true;
    }
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOService);

static void RaphaelNativeMetalStart()
{
    /* Independent probe kernel only: current boot args already fill the loader limit.
     * No application receives a device unless its name is explicitly allowed. */
    uint32_t enabled=2;
    (void)PE_parse_boot_argn("raphael_metal",&enabled,sizeof(enabled));
    if (!enabled) return;
    NativeAdrenoGpuPublished *service=OSTypeAlloc(NativeAdrenoGpuPublished);
    IOService *provider=IOService::getPlatform();
    if (!service) return;
    if (provider && service->init() && service->attach(provider)) {
        if (service->start(provider)) IOLog("RaphaelMetal: experimental NativeAdreno ABI enabled; no Metal plugin published\n");
        else service->detach(provider);
    }
    service->release();
}
