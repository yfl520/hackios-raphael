/* Rear IMX586 sessions. Reuse upstream SOF/address/WM_DONE flow and cdev
 * ABI, but keep the verified Raphael parent/PHY/SMMU transactions. The worker
 * stays inside rear_probe: its supply/clock snapshots never outlive a stack.
 * Enabled explicitly by cam_session=1 while cold-read stays available. */
#include "hakios.raphael-front-queue.h"
#include "hakios.raphael-rear-engine.h"
#include "hakios.raphael-session-content.h"
#define RAPHAEL_RAW_BYTES (5008u * 3000u)
#define RAPHAEL_SLOT_BYTES (round_page(RAPHAEL_RAW_BYTES) + PAGE_SIZE)
static bool raphael_session_wanted, raphael_session_active;
static bool raphael_session_streaming, raphael_session_reading, raphael_session_copying;
static volatile bool raphael_session_exit;
static bool rear_stop_requested;
static uint64_t raphael_session_last_read, raphael_session_delivered, raphael_session_number;
static vm_offset_t raphael_session_pool, raphael_session_reset;
static vm_size_t raphael_session_bytes;
static struct raphael_queue raphael_session_queue;
static uint32_t raphael_session_slot_focus[RAPHAEL_SLOTS];
static bool raphael_session_focus_ready, raphael_session_focus_pending;
static uint32_t raphael_session_focus_target;
static struct {
    struct raphael_sensor sensor;
    struct raphael_rear_engine engine;
    struct raphael_capture_clocks clocks;
    struct raphael_phy phy;
    struct raphael_actuator_state actuator;
    bool retain;
} raphael_session_hw;

/* Caller holds preview_lock; hardware writes stay on the one session worker. */
static int
raphael_session_focus_command(uint32_t target)
{
    if (!raphael_session_enabled || raphael_actuator_mode != 3) return ENOTSUP;
    if (!raphael_session_focus_ready) return EAGAIN;
    if (target < 180 || target > 579) return EINVAL;
    raphael_session_focus_target = target;
    raphael_session_focus_pending = true;
    return 0;
}

static bool
raphael_session_controls(struct raphael_sensor *s, struct raphael_i2c_bus *bus,
    uint32_t lines, uint32_t gain, uint32_t length)
{
    uint16_t code = (uint16_t)(1024u - 262144u / gain);
    const struct { uint16_t address, value; } controls[] = {
        {0x340, (uint16_t)length}, {0x202, (uint16_t)lines}, {0x204, code}
    };
    bool ok = raphael_sensor_apply(s, bus, R586_HOLDON);
    for (unsigned i = 0; i < 3 && ok; i++)
        ok = raphael_sensor_write(bus, controls[i].address, controls[i].value >> 8) &&
            raphael_sensor_write(bus, controls[i].address + 1, controls[i].value & 255);
    bool released = raphael_sensor_apply(s, bus, R586_HOLDOFF);
    if (!ok || !released) return false;
    for (unsigned i = 0; i < 3; i++) {
        uint8_t v[2] = {0};
        if (!raphael_sensor_read(bus, controls[i].address, v, 2) ||
            ((unsigned)v[0] << 8 | v[1]) != controls[i].value) return false;
    }
    return true;
}

static bool
raphael_session_run(struct raphael_i2c_bus *bus, __unused void *opaque)
{
    struct raphael_sensor *sensor = &raphael_session_hw.sensor;
    struct raphael_rear_engine *engine = &raphael_session_hw.engine;
    engine->stopped=engine->paths_clean=engine->mmu_clean=engine->bridge_clean=true;
    uint32_t lines, gain, length, next_lines, next_gain, next_length;
    uint64_t apply_from = 0, focus_from = 0, began = ios_ms(), last_frame = began;
    uint32_t focus = HAKIOS_CAMERA_NO_FOCUS, next_focus = focus;
    lck_mtx_lock(&raphael_preview_lock);
    lines = raphael_preview_lines; gain = raphael_preview_gain; length = raphael_preview_frame;
    next_lines = lines; next_gain = gain; next_length = length;
    raphael_session_pending = false;
    raphael_session_focus_ready = raphael_session_focus_pending = false;
    raphael_queue_init(&raphael_session_queue);
    lck_mtx_unlock(&raphael_preview_lock);
    memset((void *)raphael_session_pool, 0xa5, raphael_session_bytes);
    raphael_frame_cache(raphael_session_pool, raphael_session_bytes, 0);
    bool ok = raphael_actuator_begin(&raphael_session_hw.actuator, bus) && raphael_sensor_prepare(sensor, bus) &&
        raphael_session_controls(sensor, bus, lines, gain, length) &&
        raphael_capture_clocks_begin(&raphael_session_hw.clocks) &&
        raphael_phy_begin(&raphael_session_hw.phy) &&
        raphael_rear_engine_map(engine, raphael_session_pool, raphael_session_bytes,
            FRAME_ADDRESSES, RAPHAEL_RAW_BYTES);
    if (ok) ok = raphael_rear_engine_share(engine);
    uint32_t crc = ok ? rd(csid + CSID_CSI2_RX_CRC) : 0;
    if (ok) {
        wr(ife + IFE_IRQ_CLEAR0, RDI0_SOF); wr(ife + IFE_IRQ_GLOBAL_CLEAR, 1);
        wr(ife + BUS_IRQ_CLEAR1, WM0_DONE); wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
        __sync_synchronize();
        ok = !(rd(ife + BUS_IRQ_STATUS1) & WM0_DONE) && raphael_sensor_start(sensor, bus);
    }
    lck_mtx_lock(&raphael_preview_lock);
    raphael_session_streaming = ok;
    raphael_session_focus_ready = ok && raphael_actuator_mode == 3 && raphael_session_hw.actuator.active;
    if (raphael_session_focus_ready) focus = raphael_session_hw.actuator.dac;
    lck_mtx_unlock(&raphael_preview_lock);
    wakeup(&raphael_session_queue);
    say("raphael-camera: session=%llu start=%u, persistent sensor/PHY, three reserved slots; setup=%llu ms\n",
        raphael_session_number, ok, ios_ms() - began);
    last_frame = ios_ms();
    while (ok) {
        wait_ms(1);
        uint64_t now = ios_ms();
        lck_mtx_lock(&raphael_preview_lock);
        bool end = raphael_session_exit || rear_stop_requested || raphael_selected_sensor != 1 || !ios_open || broken ||
            now - raphael_session_last_read > 3000 ||
            (raphael_preview_cutoff_ms && now >= raphael_preview_cutoff_ms);
        lck_mtx_unlock(&raphael_preview_lock);
        if (end) break;
        uint32_t top = rd(ife + IFE_IRQ_STATUS0), bus_irq = rd(ife + BUS_IRQ_STATUS1);
        if ((rd(engine->mmu.cb + 0x58) & 0xc00001feu) || rd(ife + IFE_VIOLATION_STATUS) ||
            rd(csid + CSID_CSI2_RX_CRC) != crc) { ok = false; break; }
        if (!(top & RDI0_SOF) && !(bus_irq & WM0_DONE)) {
            if (now - last_frame > FRAME_TIMEOUT_MS) ok = false;
            continue;
        }
        /* Publish only complete frames with the expected real CSI packet length. */
        if ((bus_irq & WM0_DONE) &&
            (rd(csid + CSID_RDI0_BYTES_PING) != 15000000 ||
            (raphael_session_queue.completed > 0 && rd(csid + CSID_RDI0_BYTES_PONG) != 15000000))) {
            ok = false; break;
        }
        wr(ife + IFE_IRQ_CLEAR0, top & RDI0_SOF); wr(ife + IFE_IRQ_GLOBAL_CLEAR, 1);
        wr(ife + BUS_IRQ_CLEAR1, bus_irq & WM0_DONE); wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
        __sync_synchronize();
        lck_mtx_lock(&raphael_preview_lock);
        if (bus_irq & WM0_DONE) {
            last_frame = now;
            if (apply_from && raphael_session_queue.completed + 1 >= apply_from) {
                lines = next_lines; gain = next_gain; length = next_length; apply_from = 0;
            }
            if (focus_from && raphael_session_queue.completed + 1 >= focus_from) {
                focus = next_focus; focus_from = 0;
            }
            raphael_session_slot_focus[raphael_session_queue.writing] = focus;
            (void)raphael_queue_done(&raphael_session_queue, mach_absolute_time(),
                lines, gain, length, raphael_frame_warmup);
        }
        if (top & RDI0_SOF) {
            int next = raphael_queue_sof(&raphael_session_queue);
            if (next < 0) ok = false;
            else {
                wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * RAPHAEL_SLOT_BYTES));
                wr(ife + IFE_REG_UPDATE, 2); __sync_synchronize();
            }
        }
        /* One in-flight exposure update. Metadata becomes effective two frames
         * later, as in the upstream session; do not overwrite a pending epoch. */
        bool update = ok && (bus_irq & WM0_DONE) && raphael_session_pending && !apply_from;
        if (update) {
            next_lines = raphael_preview_lines; next_gain = raphael_preview_gain;
            next_length = raphael_preview_frame; raphael_session_pending = false;
        }
        bool focus_update = ok && (bus_irq & WM0_DONE) && raphael_session_focus_ready &&
            raphael_session_focus_pending && !focus_from;
        uint32_t focus_target = raphael_session_focus_target;
        if (focus_update) raphael_session_focus_pending = false;
        uint64_t count = raphael_session_queue.completed;
        lck_mtx_unlock(&raphael_preview_lock);
        if (bus_irq & WM0_DONE) wakeup(&raphael_session_queue);
        if (update && (next_lines != lines || next_gain != gain || next_length != length)) {
            ok = raphael_session_controls(sensor, bus, next_lines, next_gain, next_length);
            if (ok) apply_from = count + 2;
        }
        if (ok && focus_update && focus_target != focus) {
            bool moved = raphael_actuator_move(&raphael_session_hw.actuator, bus, focus_target);
            if (moved) {
                next_focus = raphael_session_hw.actuator.dac; focus_from = count + 2;
            } else {
                raphael_session_hw.actuator.active = false;
                focus = HAKIOS_CAMERA_NO_FOCUS; focus_from = 0;
                lck_mtx_lock(&raphael_preview_lock);
                raphael_session_focus_ready = raphael_session_focus_pending = false;
                lck_mtx_unlock(&raphael_preview_lock);
            }
            say("raphael-af: session=%llu move target=%u actual=%u success=%u effective-frame=%llu\n",
                raphael_session_number, focus_target, raphael_session_hw.actuator.dac, moved, focus_from);
        }
        if ((bus_irq & WM0_DONE) && count % 300 == 0)
            say("raphael-camera: session=%llu completed=%llu delivered=%llu elapsed=%llu ms gain=%u lines=%u frame-lines=%u\n",
                raphael_session_number, count, raphael_session_delivered, now - began, gain, lines, length);
    }
    /* Reader memory remains pinned; no new reader is admitted during teardown. */
    lck_mtx_lock(&raphael_preview_lock);
    raphael_session_streaming = false;
    raphael_session_focus_ready = raphael_session_focus_pending = false;
    lck_mtx_unlock(&raphael_preview_lock);
    wakeup(&raphael_session_queue);
    bool stopped = raphael_sensor_stop(sensor, bus);
    bool lens_clean = raphael_actuator_end(&raphael_session_hw.actuator, bus);
    bool clean = raphael_rear_engine_end(engine, stopped);
    if (clean) clean = raphael_phy_end(&raphael_session_hw.phy);
    if (clean) clean = raphael_capture_clocks_end(&raphael_session_hw.clocks);
    clean = clean && lens_clean;
    raphael_session_hw.retain = !clean;
    say("raphael-camera: session=%llu end ok=%u stopped=%u cleanup=%u completed=%llu delivered=%llu elapsed=%llu ms\n",
        raphael_session_number, ok, stopped, clean, raphael_session_queue.completed,
        raphael_session_delivered, ios_ms() - began);
    return ok && clean;
}

static void
raphael_session_thread(__unused void *parameter, __unused wait_result_t result)
{
    for (;;) {
        lck_mtx_lock(&raphael_preview_lock);
        while (!raphael_session_exit && !raphael_preview_fault &&
            !(ios_open && raphael_selected_sensor == 1 && !rear_stop_requested && raphael_session_wanted && !raphael_session_copying)) {
            struct timespec ts = {0, 100 * NSEC_PER_MSEC};
            (void)msleep(&raphael_session_wanted, &raphael_preview_lock, PZERO, "raphael-camera-idle", &ts);
        }
        if (raphael_session_exit || raphael_preview_fault) {
            lck_mtx_unlock(&raphael_preview_lock); break;
        }
        raphael_session_wanted = false; raphael_session_active = true;
        raphael_session_number++; raphael_session_delivered = 0;
        bzero(&raphael_session_hw, sizeof(raphael_session_hw));
        lck_mtx_unlock(&raphael_preview_lock);
        uint64_t bytes = 0;
        const uint8_t *table = hakios_wifi_file("raphael-imx586.table", &bytes);
        bool ok = raphael_sensor_load(&raphael_session_hw.sensor, table, bytes) &&
            raphael_camera_rear_probe(raphael_session_run, NULL, &raphael_session_hw.retain);
        lck_mtx_lock(&raphael_preview_lock);
        raphael_session_active = false; raphael_session_streaming = false;
        if (!ok) raphael_preview_fault = true;
        lck_mtx_unlock(&raphael_preview_lock);
        wakeup(&raphael_session_queue); wakeup(&raphael_session_active);
    }
    thread_terminate(current_thread());
}

static bool
raphael_session_init(void)
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("cam_session", &enabled, sizeof(enabled)) || !enabled) return true;
    uint32_t af = 0;
    if (PE_parse_boot_argn("cam_af", &af, sizeof(af)) && af >= 1 && af <= 3) raphael_actuator_mode = af;
    uint32_t dac = 0;
    if (PE_parse_boot_argn("cam_dac", &dac, sizeof(dac)) && dac >= 180 && dac <= 579)
        raphael_actuator_fixed = dac;
    raphael_session_bytes = RAPHAEL_SLOTS * RAPHAEL_SLOT_BYTES;
    if (raphael_session_bytes > RAPHAEL_MAX_FRAME_BYTES ||
        kmem_alloc_contig(kernel_map, &raphael_session_pool, raphael_session_bytes, PAGE_MASK, 0, 0,
            KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
    raphael_session_reset = raphael_camera_map(0x0351c000, 0x1000);
    if (!raphael_session_reset) return false;
    thread_t thread;
    if (kernel_thread_start(raphael_session_thread, NULL, &thread) != KERN_SUCCESS) return false;
    thread_deallocate(thread); raphael_session_enabled = true;
    say("raphael-camera: persistent rear session enabled; pool=%llu, slot=%llu bytes\n",
        (uint64_t)raphael_session_bytes, (uint64_t)RAPHAEL_SLOT_BYTES);
    return true;
}

static int
raphael_session_read(struct uio *uio)
{
    int slot = -1, error = 0;
    uint64_t began = ios_ms(), acquired = began, cached = began, checked = began, copied = began;
    struct hakios_camera_frame f = {0};
    lck_mtx_lock(&raphael_preview_lock);
    /* Serialize readers, including time spent asleep waiting for a frame. */
    if (raphael_session_reading) { lck_mtx_unlock(&raphael_preview_lock); return EBUSY; }
    raphael_session_reading = true;
    raphael_session_last_read = began;
    for (;;) {
        if (!ios_open || broken || raphael_preview_fault || raphael_session_exit || rear_stop_requested || raphael_selected_sensor != 1 ||
            (raphael_preview_cutoff_ms && ios_ms() >= raphael_preview_cutoff_ms)) { error = EIO; break; }
        if (raphael_session_streaming) slot = raphael_queue_take(&raphael_session_queue);
        if (slot >= 0) break;
        if (!raphael_session_active) {
            raphael_session_wanted = true; wakeup(&raphael_session_wanted);
        }
        if (ios_ms() - began > 5000) { error = EAGAIN; break; }
        struct timespec ts = {0, 100 * NSEC_PER_MSEC};
        error = msleep(&raphael_session_queue, &raphael_preview_lock, PZERO | PCATCH, "raphael-camera-frame", &ts);
        if (error && error != EWOULDBLOCK) break;
        error = 0; raphael_session_last_read = ios_ms();
    }
    if (slot >= 0) {
        raphael_session_copying = true;
        const struct raphael_queue_slot *p = &raphael_session_queue.slot[slot];
        memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8); f.header_bytes = sizeof(f);
        f.width = 4000; f.height = 3000; f.stride = 5008; f.format = 1; f.black = 64;
        f.sequence = ++raphael_preview_sequence; f.time = p->time;
        f.exposure_lines = p->lines; f.gain = p->gain; f.frame_lines = p->length;
        f.line_ns = (uint32_t)(1000000000000ull / (30010ull * 3064));
        f.gain_unity = 256; f.gain_max = 4096; f.frame_lines_min = 3064; f.frame_lines_max = 6112;
        f.margin = 48; f.sensor = 1; f.focus = raphael_session_slot_focus[slot];
        if (f.focus != HAKIOS_CAMERA_NO_FOCUS) {
            f.reserved[0] = HAKIOS_CAMERA_FOCUS_RANGE_MAGIC;
            f.reserved[1] = 180; f.reserved[2] = 579;
        }
    }
    lck_mtx_unlock(&raphael_preview_lock);
    if (slot >= 0) {
        vm_offset_t at = raphael_session_pool + (uint64_t)slot * RAPHAEL_SLOT_BYTES;
        acquired = ios_ms();
        raphael_frame_cache(at, RAPHAEL_RAW_BYTES, 2);
        cached = ios_ms();
        const uint8_t *raw = (const uint8_t *)at;
        struct raphael_content_counts counts = raphael_content_count(raw, 5008, 3000, 5000, 4000);
        checked = ios_ms();
        /* Flat/overexposed scenes remain valid. Stale sentinel rows and zero
         * buffers do not reach the ISP; this read can retry without a latch. */
        if (counts.zero >= 14000000 || counts.written_rows != 3000) error = EAGAIN;
        if (!error) error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
        if (!error && uio_resid(uio) > 0)
            error = uiomove((caddr_t)at, (int)MIN((uint64_t)RAPHAEL_RAW_BYTES, (uint64_t)uio_resid(uio)), uio);
        copied = ios_ms();
    }
    lck_mtx_lock(&raphael_preview_lock);
    if (slot >= 0) {
        raphael_queue_release(&raphael_session_queue, (unsigned)slot);
        if (!error) raphael_session_delivered++;
    }
    raphael_session_reading = raphael_session_copying = false; raphael_session_last_read = ios_ms();
    if (raphael_session_wanted) wakeup(&raphael_session_wanted);
    uint64_t delivered = raphael_session_delivered;
    lck_mtx_unlock(&raphael_preview_lock);
    if (!error && (delivered <= 3 || delivered % 100 == 0)) {
        say("raphael-camera: session read=%llu seq=%llu slot=%d total=%llu ms gain=%u lines=%u\n",
            delivered, f.sequence, slot, ios_ms() - began, f.gain, f.exposure_lines);
        say("raphael-camera: read phases seq=%llu wait=%llu cache=%llu content=%llu copy=%llu ms\n",
            f.sequence, acquired - began, cached - acquired, checked - cached, copied - checked);
        raphael_camera_map_status(f.sequence);
    }
    return error;
}

/* Called by orderly thread-call exits before Wi-Fi shutdown and SMP stop. */
void
hakios_camera_stop(void)
{
    if (!raphael_session_enabled) return;
    lck_mtx_lock(&raphael_preview_lock);
    raphael_session_exit = true;
    wakeup(&raphael_session_wanted); wakeup(&raphael_session_queue);
    for (unsigned tries = 0; raphael_session_active && tries < 50; tries++) {
        struct timespec ts = {0, 100 * NSEC_PER_MSEC};
        (void)msleep(&raphael_session_active, &raphael_preview_lock, PZERO, "raphael-camera-stop", &ts);
    }
    bool done = !raphael_session_active;
    lck_mtx_unlock(&raphael_preview_lock);
    say("raphael-camera: orderly session drain=%u\n", done);
}

/* Panic fallback after other CPUs stopped: never take their mutexes or free
 * allocations. Hold the rear sensor in reset and halt DMA before RAM reuse.
 * This is containment, NOT verified normal power/SMMU restoration. */
static void
raphael_session_quiesce(void)
{
    if (!raphael_session_enabled || (!raphael_session_active && !raphael_session_hw.retain)) return;
    raphael_session_exit = true;
    if (raphael_session_reset) {
        wr(raphael_session_reset + 4, rd(raphael_session_reset + 4) & ~2u);
        wr(raphael_session_reset, (rd(raphael_session_reset) & ~0x3ffu) | 0x200u);
        spin_us(2000);
    }
    if (raphael_session_hw.engine.path_dirty) { paths_down(); spin_us(40000); }
    say("raphael-camera: emergency rear reset/DMA halt; memory retained, full restore unverified\n");
}

/* Call with preview_lock held; sensor switch waits for full restoration. */
static bool
rear_session_drain_locked(void)
{
    rear_stop_requested=true; raphael_session_wanted=false;
    wakeup(&raphael_session_wanted); wakeup(&raphael_session_queue);
    for (unsigned i=0; raphael_session_active && i<50; i++) {
        struct timespec ts={0,100*NSEC_PER_MSEC};
        (void)msleep(&raphael_session_active,&raphael_preview_lock,PZERO,"rear-drain",&ts);
    }
    say("raphael-camera: rear switch drain=%u\n",!raphael_session_active);
    return !raphael_session_active;
}
