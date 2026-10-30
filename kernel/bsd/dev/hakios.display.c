/* Raphael command-mode TE phase for refresh notifications; page flips retain the existing latch path. */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.display.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/hakios.loader-display.h>

static struct hakios_display_scan scan;
extern int raphael_display_scan_counter(uint32_t *line);

uint32_t
hakios_display_line(uint32_t *frame)
{
	uint32_t line;
	{
		if (frame != NULL) *frame = 0; /* Firmware's command-mode frame counter does not advance. */
		if (!scan.ok || !raphael_display_scan_counter(&line) || line > scan.lines + scan.lines / 4) {
			scan.ok = 0;
			return 0;
		}
		return line < scan.lines ? line : scan.lines - 1;
	}
	return 0;
}

const struct hakios_display_scan *
hakios_display_scan(void)
{
	if (scan.ok) (void)hakios_display_line(NULL);
	return scan.ok ? &scan : NULL;
}

uint64_t
hakios_display_next_frame_ns(void)
{
	uint32_t line;

	if (!scan.ok) {
		return 0;
	}
	line = hakios_display_line(NULL);
	if (!scan.ok) return 0;
	return (uint64_t)(scan.lines - line) * scan.line_ns;
}

static int
command_scan_start(void)
{
	uint32_t before, line, edges = 0;
	uint64_t start = mach_absolute_time(), previous = start, first = 0, last = 0;
	uint64_t elapsed = 0, ticks = 0, ns, dt;

	if (!raphael_display_scan_counter(&before)) return 0;
	while (edges < 3) {
		delay_for_interval(50, NSEC_PER_USEC);
		uint64_t now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &ns);
		if (ns > 150 * NSEC_PER_MSEC || !raphael_display_scan_counter(&line)) return 0;
		absolutetime_to_nanoseconds(now - previous, &dt);
		if (line < before) {
			if (dt >= NSEC_PER_MSEC) return 0; /* A delayed sample cannot identify which TE edge it saw. */
			if (edges++ == 0) first = now;
			last = now;
		} else if (dt < NSEC_PER_MSEC) {
			elapsed += dt;
			ticks += line - before;
		}
		before = line;
		previous = now;
	}
	if (ticks == 0) return 0;
	absolutetime_to_nanoseconds(last - first, &ns);
	scan.frame_ns = ns / 2;
	scan.line_ns = elapsed / ticks;
	if (scan.line_ns == 0 || scan.frame_ns < 4 * NSEC_PER_MSEC || scan.frame_ns > 50 * NSEC_PER_MSEC) return 0;
	scan.lines = (uint32_t)((scan.frame_ns + scan.line_ns / 2) / scan.line_ns);
	scan.shown = 2340;
	if (scan.lines < scan.shown || scan.lines > 0xffff) return 0;
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	scan.ok = 1;
	printf("display: raphael command TE: %u.%03u Hz, %u ticks/frame, %llu ns/tick; page flips retained (fb_scan=1)\n",
	    scan.millihz / 1000, scan.millihz % 1000, scan.lines, scan.line_ns);
	return 1;
}

int
hakios_display_scan_start(void)
{
	uint32_t on = 0;
	if (scan.ok) return 1;
	if (!hakios_is_raphael() || !PE_parse_boot_argn("fb_scan", &on, sizeof(on)) || on != 1) return 0;
	return command_scan_start();
}

/* CPU0 owns firmware calls; XNU publishes requests on a separate cache line. */
static LCK_GRP_DECLARE(brightness_group, "hakios-brightness");
static LCK_MTX_DECLARE(brightness_lock, &brightness_group);
static volatile struct loader_display *brightness_shared;
static int brightness_status = -1;
SYSCTL_DECL(_kern_hakios);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_display_scanout_lock(void);
extern void hakios_display_scanout_unlock(void);

#include "hakios.raphael-dsi.h"
static uint32_t dsi_percent;
static int brightness_backend; /* 1: firmware mailbox, 2: running DSI0 engine */
static int brightness_feedback; /* 1 only when the panel itself returned DCS 0x52 */

static bool
brightness_mailbox_prepare(void)
{
	uint64_t physical = 0;
	uint32_t enabled = 0;
	if (brightness_shared) return true;
	if (!hakios_is_raphael() || !PE_parse_boot_argn("backlight", &enabled, sizeof(enabled)) || enabled != 1 ||
    !PE_parse_boot_argn("display_shared_pa", &physical, sizeof(physical)) || !physical || (physical & 63) ||
    (physical & 0x3fff) > 0x4000 - sizeof(struct loader_display))
		return false;
	vm_offset_t mapped = ml_io_map((vm_offset_t)(physical & ~0x3fffULL), 0x4000);
	if (!mapped) return false;
	volatile struct loader_display *shared = (volatile struct loader_display *)(mapped + (physical & 0x3fff));
	if (shared->magic != LOADER_DISPLAY_MAGIC || shared->version != LOADER_DISPLAY_VERSION ||
	    shared->size != sizeof(*shared) || shared->answer.status || shared->answer.percent > 100) return false;
	brightness_shared = shared;
	brightness_status = 0;
	printf("display: native brightness mailbox at %#llx, initial %u%%\n", physical, shared->answer.percent);
	return true;
}

static bool
brightness_prepare(void)
{
	uint32_t enabled = 0, initial = 0, readback = 0;
	if (brightness_backend) return true;
	if (!hakios_is_raphael() || !PE_parse_boot_argn("backlight", &enabled, sizeof(enabled)) || enabled != 1)
		return false;
	if (brightness_mailbox_prepare()) {
		brightness_backend = 1;
		return true;
	}
	/* A fallback initial value is explicit, and only published after a real DMA. */
	(void)PE_parse_boot_argn("backlight_initial", &initial, sizeof(initial));
	(void)PE_parse_boot_argn("backlight_readback", &readback, sizeof(readback));
	hakios_display_scanout_lock();
	int error = ENXIO;
	if (readback == 1) {
		error = dsi_brightness_read(&dsi_percent);
		if (!error) brightness_feedback = 1;
	}
	if (error && initial >= 1 && initial <= 100 && !dsi_failed) {
		error = dsi_brightness_write(initial);
		if (!error) dsi_percent = initial;
	}
	hakios_display_scanout_unlock();
	brightness_status = error;
	if (error) return false;
	brightness_backend = 2;
	printf("display: Raphael DSI brightness %u%%, panel readback %d\n", dsi_percent, brightness_feedback);
	return true;
}

int
hakios_display_brightness_get(uint32_t *percent)
{
	int error = 0;
	lck_mtx_lock(&brightness_lock);
	if (!brightness_prepare()) error = ENXIO;
	else *percent = brightness_backend == 2 ? dsi_percent : brightness_shared->answer.percent;
	lck_mtx_unlock(&brightness_lock);
	return error;
}

int
hakios_display_brightness_set(uint32_t percent)
{
	int error;
	if (percent > 100) return EINVAL;
	/* iOS brightness zero means minimum visible light; panel power is separate. */
	if (!percent) percent = 1;
	lck_mtx_lock(&brightness_lock);
	if (!brightness_prepare()) {
		lck_mtx_unlock(&brightness_lock);
		return ENXIO;
	}
	if (brightness_backend == 2) {
		hakios_display_scanout_lock();
		error = dsi_brightness_write(percent);
		if (!error) {
			dsi_percent = percent;
			brightness_feedback = 0; /* Last commanded level; no invented readback. */
		}
		brightness_status = error;
		hakios_display_scanout_unlock();
		lck_mtx_unlock(&brightness_lock);
		return error;
	}
	uint32_t previous = brightness_shared->request.sequence;
	if (previous != brightness_shared->answer.sequence) {
		lck_mtx_unlock(&brightness_lock);
		return EBUSY;
	}
	/* Frame submission completes its TE latch before releasing this lock. */
	hakios_display_scanout_lock();
	uint32_t next = previous + 1;
	brightness_shared->request.percent = (uint32_t)percent;
	__asm__ volatile ("dmb ishst" ::: "memory");
	brightness_shared->request.sequence = next;
	for (unsigned tick = 0; tick < 500 && brightness_shared->answer.sequence != next; tick++)
		delay_for_interval(1, NSEC_PER_MSEC);
	__asm__ volatile ("dmb ishld" ::: "memory");
	error = brightness_shared->answer.sequence != next ? ETIMEDOUT :
	    brightness_shared->answer.status ? EIO : 0;
	brightness_status = error;
	hakios_display_scanout_unlock();
	printf("display: brightness %d%% -> %u%%, error %d\n", percent, brightness_shared->answer.percent, error);
	lck_mtx_unlock(&brightness_lock);
	return error;
}

static int
brightness_sysctl SYSCTL_HANDLER_ARGS
{
	uint32_t value;
	int error = hakios_display_brightness_get(&value);
	if (error) return error;
	int percent = (int)value;
	error = sysctl_handle_int(oidp, &percent, 0, req);
	if (error || !req->newptr) return error;
	return percent < 0 ? EINVAL : hakios_display_brightness_set((uint32_t)percent);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, brightness_sysctl, "I", "native panel brightness percent, 0 requests minimum visible level");
SYSCTL_INT(_kern_hakios, OID_AUTO, brightness_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &brightness_status, 0, "last native brightness error, -1 before initialization");
SYSCTL_INT(_kern_hakios, OID_AUTO, brightness_backend, CTLFLAG_RD | CTLFLAG_LOCKED,
    &brightness_backend, 0, "brightness transport: 0 unavailable, 1 UEFI, 2 DSI0");
SYSCTL_INT(_kern_hakios, OID_AUTO, brightness_feedback, CTLFLAG_RD | CTLFLAG_LOCKED,
    &brightness_feedback, 0, "1: panel readback, 0: last successfully commanded level");
