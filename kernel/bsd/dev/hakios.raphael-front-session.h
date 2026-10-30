/* S5K3T2 persistent worker: same proven SOF/WM_DONE three-slot protocol
 * as the rear session, isolated front state and original parent transaction.
 * preview_lock protects queues/selection, one worker owns all hardware. */
#include "hakios.raphael-front-engine.h"
#include "hakios.raphael-front-queue.h"
#define FRONT_RAW_BYTES (3248u * 1940u)
#define FRONT_SLOT_BYTES (round_page(FRONT_RAW_BYTES) + PAGE_SIZE)
static bool front_enabled, front_active, front_wanted, front_streaming;
static bool front_reading, front_copying, front_stop_requested;
static volatile bool front_exit;
static uint64_t front_last_read, front_delivered, front_number;
static vm_offset_t front_pool, front_reset;
static vm_size_t front_pool_bytes;
static struct raphael_queue front_queue;
static struct {
    struct raphael_front_sensor sensor;
    struct raphael_front_engine engine;
    struct raphael_front_capture_clocks clocks;
    struct raphael_front_phy phy;
    bool retain;
} front_hw;

static bool
front_session_run(struct raphael_i2c_bus *bus, __unused void *context)
{
    struct raphael_front_sensor *sensor=&front_hw.sensor;
    struct raphael_front_engine *engine=&front_hw.engine;
    engine->stopped=engine->paths_clean=engine->mmu_clean=engine->bridge_clean=true;
    uint32_t lines,gain,length,next_lines,next_gain,next_length;
    uint64_t apply_from=0, began=ios_ms(), last_frame=began;
    lck_mtx_lock(&raphael_preview_lock);
    lines=next_lines=raphael_front_lines; gain=next_gain=raphael_front_gain;
    length=next_length=raphael_front_length;
    raphael_queue_init(&front_queue);
    lck_mtx_unlock(&raphael_preview_lock);
    memset((void *)front_pool,0xa5,front_pool_bytes);
    raphael_frame_cache(front_pool,front_pool_bytes,0);
    bool ok=raphael_front_prepare(sensor,bus) &&
        raphael_front_controls(bus,lines,gain,length) &&
        raphael_front_capture_clocks_begin(&front_hw.clocks) &&
        raphael_front_phy_begin(&front_hw.phy) &&
        raphael_front_engine_map(engine,front_pool,front_pool_bytes,FRAME_ADDRESSES,FRONT_RAW_BYTES);
    if (ok) ok=raphael_front_engine_share(engine);
    uint32_t crc=ok?rd(csid+CSID_CSI2_RX_CRC):0;
    if (ok) {
        wr(ife+IFE_IRQ_CLEAR0,RDI0_SOF); wr(ife+IFE_IRQ_GLOBAL_CLEAR,1);
        wr(ife+BUS_IRQ_CLEAR1,WM0_DONE); wr(ife+BUS_IRQ_GLOBAL_CLEAR,1);
        __sync_synchronize();
        ok=!(rd(ife+BUS_IRQ_STATUS1)&WM0_DONE) && raphael_front_start(sensor,bus);
    }
    lck_mtx_lock(&raphael_preview_lock); front_streaming=ok; lck_mtx_unlock(&raphael_preview_lock);
    wakeup(&front_queue);
    say("raphael-front: session=%llu start=%u persistent sensor/PHY, three slots, setup=%llu ms\n",front_number,ok,ios_ms()-began);
    last_frame=ios_ms();
    while (ok) {
        wait_ms(1);
        uint64_t now=ios_ms();
        lck_mtx_lock(&raphael_preview_lock);
        bool end=front_exit || front_stop_requested || !ios_open || broken ||
            raphael_selected_sensor!=2 || now-front_last_read>3000 ||
            (raphael_preview_cutoff_ms && now>=raphael_preview_cutoff_ms);
        lck_mtx_unlock(&raphael_preview_lock);
        if (end) break;
        uint32_t top=rd(ife+IFE_IRQ_STATUS0), irq=rd(ife+BUS_IRQ_STATUS1);
        if ((rd(engine->mmu.cb+0x58)&0xc00001feu) || rd(ife+IFE_VIOLATION_STATUS) || rd(csid+CSID_CSI2_RX_CRC)!=crc) {
            say("raphael-front: persistent DMA fault fsr=%#x violation=%#x crc=%#x/%#x\n",rd(engine->mmu.cb+0x58),rd(ife+IFE_VIOLATION_STATUS),rd(csid+CSID_CSI2_RX_CRC),crc);
            ok=false; break;
        }
        if (!(top&RDI0_SOF) && !(irq&WM0_DONE)) {
            if (now-last_frame>FRAME_TIMEOUT_MS) { say("raphael-front: session frame timeout\n"); ok=false; }
            continue;
        }
        if ((irq&WM0_DONE) && (rd(csid+CSID_RDI0_BYTES_PING)!=6285600 ||
            (front_queue.completed>0 && rd(csid+CSID_RDI0_BYTES_PONG)!=6285600))) {
            say("raphael-front: bad packet bytes=%u/%u\n",rd(csid+CSID_RDI0_BYTES_PING),rd(csid+CSID_RDI0_BYTES_PONG));
            ok=false; break;
        }
        wr(ife+IFE_IRQ_CLEAR0,top&RDI0_SOF); wr(ife+IFE_IRQ_GLOBAL_CLEAR,1);
        wr(ife+BUS_IRQ_CLEAR1,irq&WM0_DONE); wr(ife+BUS_IRQ_GLOBAL_CLEAR,1);
        __sync_synchronize();
        lck_mtx_lock(&raphael_preview_lock);
        if (irq&WM0_DONE) {
            last_frame=now;
            if (apply_from && front_queue.completed+1>=apply_from) {
                lines=next_lines; gain=next_gain; length=next_length; apply_from=0;
            }
            (void)raphael_queue_done(&front_queue,mach_absolute_time(),lines,gain,length,raphael_frame_warmup);
        }
        if (top&RDI0_SOF) {
            int next=raphael_queue_sof(&front_queue);
            if (next<0) ok=false;
            else {
                wr(ife+WM0_IMAGE_ADDR,(uint32_t)(FRAME_ADDRESSES+(uint64_t)next*FRONT_SLOT_BYTES));
                wr(ife+IFE_REG_UPDATE,2); __sync_synchronize();
            }
        }
        bool update=ok && (irq&WM0_DONE) && !apply_from &&
            (lines!=raphael_front_lines || gain!=raphael_front_gain || length!=raphael_front_length);
        if (update) { next_lines=raphael_front_lines; next_gain=raphael_front_gain; next_length=raphael_front_length; }
        uint64_t completed=front_queue.completed;
        lck_mtx_unlock(&raphael_preview_lock);
        if (irq&WM0_DONE) wakeup(&front_queue);
        if (update) {
            ok=raphael_front_controls(bus,next_lines,next_gain,next_length);
            if (ok) apply_from=completed+2;
        }
        if ((irq&WM0_DONE) && completed%150==0)
            say("raphael-front: session=%llu completed=%llu delivered=%llu elapsed=%llu ms lines=%u gain=%u\n",front_number,completed,front_delivered,now-began,lines,gain);
    }
    lck_mtx_lock(&raphael_preview_lock); front_streaming=false; lck_mtx_unlock(&raphael_preview_lock);
    wakeup(&front_queue);
    bool stopped=raphael_front_stop(sensor,bus);
    bool clean=raphael_front_engine_end(engine,stopped);
    if (clean) clean=raphael_front_phy_end(&front_hw.phy);
    if (clean) clean=raphael_front_capture_clocks_end(&front_hw.clocks);
    front_hw.retain=!clean;
    say("raphael-front: session=%llu end ok=%u stopped=%u cleanup=%u completed=%llu delivered=%llu elapsed=%llu ms\n",front_number,ok,stopped,clean,front_queue.completed,front_delivered,ios_ms()-began);
    return ok && clean;
}

static void
front_session_thread(__unused void *parameter, __unused wait_result_t result)
{
    for (;;) {
        lck_mtx_lock(&raphael_preview_lock);
        while (!front_exit && !raphael_preview_fault && !(front_wanted && ios_open && raphael_selected_sensor==2 && !front_copying)) {
            struct timespec ts={0,100*NSEC_PER_MSEC};
            (void)msleep(&front_wanted,&raphael_preview_lock,PZERO,"front-idle",&ts);
        }
        if (front_exit || raphael_preview_fault) { lck_mtx_unlock(&raphael_preview_lock); break; }
        front_wanted=false; front_active=true; front_stop_requested=false;
        front_number++; front_delivered=0; bzero(&front_hw,sizeof(front_hw));
        lck_mtx_unlock(&raphael_preview_lock);
        bool ok=raphael_camera_front_probe(front_session_run,NULL,&front_hw.retain);
        lck_mtx_lock(&raphael_preview_lock);
        front_active=front_streaming=false;
        if (!ok) raphael_preview_fault=true;
        lck_mtx_unlock(&raphael_preview_lock);
        wakeup(&front_queue); wakeup(&front_active);
    }
    thread_terminate(current_thread());
}

/* Called with preview_lock held. Pool is pinned until kernel exits. */
static bool
front_session_init(void)
{
    uint32_t enabled=0;
    if (front_enabled || !PE_parse_boot_argn("cam_front_session",&enabled,sizeof(enabled)) || !enabled) return true;
    front_pool_bytes=RAPHAEL_SLOTS*FRONT_SLOT_BYTES;
    if (front_pool_bytes>RAPHAEL_MAX_FRAME_BYTES || kmem_alloc_contig(kernel_map,&front_pool,front_pool_bytes,PAGE_MASK,0,0,
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) return false;
    front_reset=raphael_camera_map(0x0390c000,0x1000);
    if (!front_reset) { kmem_free(kernel_map,front_pool,front_pool_bytes); front_pool=0; return false; }
    thread_t thread;
    if (kernel_thread_start(front_session_thread,NULL,&thread)!=KERN_SUCCESS) {
        kmem_free(kernel_map,front_pool,front_pool_bytes); front_pool=0; return false;
    }
    thread_deallocate(thread); front_enabled=true;
    say("raphael-front: persistent session enabled pool=%llu slot=%llu\n",(uint64_t)front_pool_bytes,(uint64_t)FRONT_SLOT_BYTES);
    return true;
}

static bool
front_session_drain_locked(void)
{
    front_stop_requested=true; front_wanted=false; wakeup(&front_wanted); wakeup(&front_queue);
    for (unsigned i=0; front_active && i<50; i++) {
        struct timespec ts={0,100*NSEC_PER_MSEC};
        (void)msleep(&front_active,&raphael_preview_lock,PZERO,"front-drain",&ts);
    }
    say("raphael-front: persistent drain=%u\n",!front_active);
    return !front_active;
}

/* Enter and return with preview_lock held; reader pins a READY slot. */
static int
front_session_read_locked(struct uio *uio)
{
    if (front_reading) return EBUSY;
    front_reading=true; front_last_read=ios_ms();
    uint64_t began=front_last_read; int slot=-1,error=0;
    struct hakios_camera_frame f={0};
    for (;;) {
        if (!ios_open || broken || raphael_preview_fault || front_exit || front_stop_requested || raphael_selected_sensor!=2) { error=EIO; break; }
        if (front_streaming) slot=raphael_queue_take(&front_queue);
        if (slot>=0) break;
        if (!front_active) { front_wanted=true; wakeup(&front_wanted); }
        if (ios_ms()-began>5000) { error=EAGAIN; break; }
        struct timespec ts={0,100*NSEC_PER_MSEC};
        error=msleep(&front_queue,&raphael_preview_lock,PZERO|PCATCH,"front-frame",&ts);
        if (error && error!=EWOULDBLOCK) break;
        error=0; front_last_read=ios_ms();
    }
    if (slot>=0) {
        front_copying=true;
        const struct raphael_queue_slot *p=&front_queue.slot[slot];
        memcpy(f.magic,HAKIOS_CAMERA_MAGIC,8); f.header_bytes=sizeof(f);
        f.width=2592; f.height=1940; f.stride=3248; f.format=1; f.bayer=1; f.black=64;
        f.sequence=++raphael_preview_sequence; f.time=p->time;
        f.exposure_lines=p->lines; f.gain=p->gain; f.frame_lines=p->length;
        f.line_ns=(uint32_t)(1000000000000ull/(30130ull*2064));
        f.gain_unity=256; f.gain_max=4096; f.frame_lines_min=2064; f.frame_lines_max=4128;
        f.margin=16; f.sensor=2; f.focus=HAKIOS_CAMERA_NO_FOCUS;
    }
    lck_mtx_unlock(&raphael_preview_lock);
    if (slot>=0) {
        vm_offset_t at=front_pool+(uint64_t)slot*FRONT_SLOT_BYTES;
        raphael_frame_cache(at,FRONT_RAW_BYTES,2);
        const uint8_t *raw=(const uint8_t *)at;
        unsigned written=0,zero=0;
        for (unsigned y=0;y<1940;y++) {
            unsigned row=0;
            for (unsigned x=0;x<3240;x++) { uint8_t v=raw[y*3248+x]; row+=v!=0xa5; zero+=v==0; }
            written+=row>2592;
        }
        if (zero>=6200000 || written!=1940) error=EAGAIN;
        if (!error) error=uiomove((caddr_t)&f,(int)MIN(sizeof(f),(size_t)uio_resid(uio)),uio);
        if (!error && uio_resid(uio)>0) error=uiomove((caddr_t)at,(int)MIN((uint64_t)FRONT_RAW_BYTES,(uint64_t)uio_resid(uio)),uio);
    }
    lck_mtx_lock(&raphael_preview_lock);
    if (slot>=0) { raphael_queue_release(&front_queue,(unsigned)slot); if (!error) front_delivered++; }
    front_reading=front_copying=false; front_last_read=ios_ms();
    if (!error && (front_delivered<=3 || front_delivered%100==0))
        say("raphael-front: persistent read=%llu sequence=%llu total=%llu ms gain=%u lines=%u\n",front_delivered,f.sequence,ios_ms()-began,f.gain,f.exposure_lines);
    return error;
}

static void
front_session_quiesce(void)
{
    if (!front_enabled || (!front_active && !front_hw.retain)) return;
    front_exit=true;
    if (front_reset) {
        wr(front_reset+4,rd(front_reset+4)&~2u); wr(front_reset,(rd(front_reset)&~0x3ffu)|0x200u); spin_us(2000);
    }
    if (front_hw.engine.path_dirty) { paths_down(); spin_us(40000); }
    say("raphael-front: emergency sensor reset/DMA halt, memory retained\n");
}
