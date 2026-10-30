/* Added only to the baseline's complete-frame ABI. Rear reads stay in the
 * original implementation; sensor switching is serialized by preview_lock. */
static uint32_t raphael_selected_sensor = 1;
static uint32_t raphael_front_lines = 2000, raphael_front_gain = 512, raphael_front_length = 2064;
static bool raphael_session_enabled, raphael_session_pending;
#include "hakios.raphael-session-actuator.h"
#include "hakios.raphael-front-session.h"
#include "hakios.raphael-rear-session.h"

static int
raphael_front_select(uint32_t sensor)
{
    if (sensor != 1 && sensor != 2) return ENODEV;
    if (sensor == raphael_selected_sensor) {
        if (sensor==1) rear_stop_requested=false;
        else front_stop_requested=false;
        return 0;
    }
    bool ok;
    if (sensor == 2) {
        if (!rear_session_drain_locked()) return EIO;
        ok = front_session_init() && raphael_motor_init() && raphael_motor_move(true);
        if (ok) front_stop_requested=false;
    }
    else {
        if (!front_session_drain_locked()) return EIO;
        ok = !raphael_motor.up || raphael_motor_move(false);
        if (raphael_motor.dirty) ok = raphael_motor_restore() && ok;
    }
    if (!ok) return EIO;
    raphael_selected_sensor = sensor;
    if (sensor==1) rear_stop_requested=false;
    say("raphael-front: selected sensor=%u; original rear pipeline preserved\n", sensor);
    return 0;
}

static int
raphael_front_read_locked(struct uio *uio)
{
    if (!raphael_motor.ready || !raphael_motor.up || raphael_motor.fault) return EIO;
    if (front_enabled) return front_session_read_locked(uio);
    struct raphael_front_shot shot = {.lines=raphael_front_lines, .gain=raphael_front_gain, .length=raphael_front_length};
    bool ok = raphael_camera_front_probe(raphael_front_shot_run, &shot, &shot.retain);
    if (!ok) {
        if (shot.retain) raphael_preview_fault = true;
        return raphael_frame_content_rejected && !shot.retain ? EAGAIN : EIO;
    }
    struct cam_header *head = (struct cam_header *)frames;
    if (head->frames != 1 || head->width != 2592 || head->height != 1940 || head->stride != 3248 || head->bayer != 1) return EIO;
    struct hakios_camera_frame f = {0};
    memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8); f.header_bytes = sizeof(f);
    f.width=2592; f.height=1940; f.stride=3248; f.format=1; f.bayer=1; f.black=64;
    f.sequence=++raphael_preview_sequence; f.time=mach_absolute_time();
    f.exposure_lines=shot.lines; f.line_ns=(uint32_t)(1000000000000ull/(30130ull*2064));
    f.gain=shot.gain; f.gain_unity=256; f.gain_max=4096;
    f.frame_lines=shot.length; f.frame_lines_min=2064; f.frame_lines_max=4128; f.margin=16;
    f.sensor=2; f.focus=HAKIOS_CAMERA_NO_FOCUS;
    int error=uiomove((caddr_t)&f, (int)MIN(sizeof(f),(size_t)uio_resid(uio)),uio);
    if (!error && uio_resid(uio)>0) error=uiomove((caddr_t)(frames+CAM_HEADER_BYTES),
        (int)MIN((uint64_t)head->frame_bytes,(uint64_t)uio_resid(uio)),uio);
    say("raphael-front: complete-frame sequence=%llu result=%d lines=%u gainQ8=%u\n", f.sequence,error,shot.lines,shot.gain);
    return error;
}

static void
raphael_front_close(void)
{
    lck_mtx_lock(&raphael_preview_lock);
    if (!rear_session_drain_locked()) {
        raphael_motor_emergency_stop(); lck_mtx_unlock(&raphael_preview_lock); return;
    }
    if (!front_session_drain_locked()) {
        raphael_motor_emergency_stop(); lck_mtx_unlock(&raphael_preview_lock); return;
    }
    bool ok = true;
    if (raphael_motor.up) ok = raphael_motor_move(false);
    if (raphael_motor.dirty) ok = raphael_motor_restore() && ok;
    raphael_selected_sensor = 1;
    if (!ok) raphael_motor_emergency_stop();
    lck_mtx_unlock(&raphael_preview_lock);
    say("raphael-front: close/retract sequence=%u\n", ok);
}

void
hakios_raphael_front_stop(void)
{
    raphael_front_close();
}
