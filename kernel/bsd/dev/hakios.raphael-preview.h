/* Rear-only first preview backend. Reuse HackiOS cdev/ISP/FigCapture ABI,
 * acquiring one NEW bounded RAW per read (4..8 warm-up frames). Low rate;
 * no POCO hardware/session, AF or pop-up motor calls. Pool is reused.
 */
static LCK_GRP_DECLARE(raphael_preview_group, "raphael-camera-preview");
static LCK_MTX_DECLARE(raphael_preview_lock, &raphael_preview_group);
static bool raphael_preview_ready, raphael_preview_fault;
static uint64_t raphael_preview_sequence;
static uint64_t raphael_preview_cutoff_ms;
static unsigned raphael_preview_rejections;
static unsigned raphael_preview_content_drops;
static uint32_t raphael_preview_lines=3016, raphael_preview_gain=2048, raphael_preview_frame=3064;

#include "hakios.raphael-front-preview.h"

struct raphael_preview_shot {
	struct raphael_sensor sensor;
	bool retain;
	bool content_rejected;
	uint32_t lines, gain, length;
	uint64_t began_ms, prepared_ms, controls_ms, receiver_ms, capture_ms, ended_ms;
};

static bool
raphael_preview_shot(struct raphael_i2c_bus *bus, void *opaque)
{
	struct raphael_preview_shot *p=opaque;
	p->began_ms=ios_ms();
	if (!raphael_sensor_prepare(&p->sensor,bus)) return false;
	p->prepared_ms=ios_ms();
	/* IMX586 analog gain =1024/(1024-code). Expose linear Q8 to the
	 * existing AE algorithm, translate here instead of treating code linear.
	 * Bounded 1.125x..16x, shutter/frame lengths in sensor line units. */
	uint16_t code=(uint16_t)(1024u-262144u/p->gain);
	const struct {uint16_t address,value;} controls[]={
		{0x340,(uint16_t)p->length},{0x202,(uint16_t)p->lines},{0x204,code}
	};
	bool ok=raphael_sensor_apply(&p->sensor,bus,R586_HOLDON);
	for (unsigned i=0;i<3&&ok;i++)
		ok=raphael_sensor_write(bus,controls[i].address,controls[i].value>>8)&&
		    raphael_sensor_write(bus,controls[i].address+1,controls[i].value&255);
	bool released=raphael_sensor_apply(&p->sensor,bus,R586_HOLDOFF);
	if (!ok||!released) return false;
	for (unsigned i=0;i<3;i++) {
		uint8_t b[2]={0};
		if (!raphael_sensor_read(bus,controls[i].address,b,2)||
		    ((unsigned)b[0]<<8|b[1])!=controls[i].value) return false;
	}
	/* Same verified stopped-sensor capture clocks and PHY as cam10. */
	p->controls_ms=ios_ms();
	struct raphael_capture_clocks clocks={0};
	struct raphael_phy phy={0};
	ok=raphael_capture_clocks_begin(&clocks);
	if (ok) ok=raphael_phy_begin(&phy);
	p->receiver_ms=ios_ms();
	if (ok) {
		ok=raphael_frame_capture(&p->sensor,bus,&p->retain);
		p->content_rejected=!ok&&raphael_frame_content_rejected&&!p->retain;
	}
	p->capture_ms=ios_ms();
	bool stopped=raphael_sensor_stop(&p->sensor,bus);
	bool clean=!p->retain&&stopped&&raphael_phy_end(&phy);
	clean=clean&&raphael_capture_clocks_end(&clocks);
	p->retain|=!clean;
	p->ended_ms=ios_ms();
	/* Complete the outer power/GPIO restoration before allowing a retry. */
	return (ok||p->content_rejected)&&stopped&&clean;
}

static bool
raphael_preview_init(void)
{
	uint32_t seconds=0;
	uint32_t warmup=8;
	if (PE_parse_boot_argn("cam_warmup",&warmup,sizeof(warmup)) && warmup>=4 && warmup<=8)
		raphael_frame_warmup=warmup;
	if (PE_parse_boot_argn("loader_return_after",&seconds,sizeof(seconds)) && seconds>30)
		raphael_preview_cutoff_ms=ios_ms()+(uint64_t)(seconds-20)*1000;
	uint64_t size=0;
	struct raphael_sensor sensor={0};
	const uint8_t *table=hakios_wifi_file("raphael-imx586.table",&size);
	if (!raphael_sensor_load(&sensor,table,size)) return false;
	/* Allocate while memory is still available, before mediaserverd. */
	keep_bytes=32ull<<20;
	if (kmem_alloc_contig(kernel_map,&keep,keep_bytes,PAGE_MASK,0,0,
	    KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) {
		keep=0;keep_bytes=0;return false;
	}
	camdev_make();
	raphael_preview_ready=camdev_made && raphael_session_init();
	say("raphael-camera: rear live-read preview %s; fresh RAW per read, bounded warmup, low frame rate\n",
	    raphael_preview_ready?"registered":"FAILED");
	say("raphael-camera: preview start-ms=%llu cutoff-ms=%llu\n",
	    ios_ms(),raphael_preview_cutoff_ms);
	say("raphael-camera: preview warmup=%u; clean content rejection is retryable\n",raphael_frame_warmup);
	return raphael_preview_ready;
}

static int
raphael_camdev_read(struct uio *uio)
{
	lck_mtx_lock(&raphael_preview_lock);
	if (!raphael_preview_ready||raphael_preview_fault||broken||!ios_open||
	    (raphael_preview_cutoff_ms&&ios_ms()>=raphael_preview_cutoff_ms)) {
		if (raphael_preview_rejections++<5)
			say("raphael-camera: preview read refused ready=%u fault=%u broken=%u open=%u now-ms=%llu cutoff=%llu\n",
			    raphael_preview_ready,raphael_preview_fault,broken,ios_open,ios_ms(),raphael_preview_cutoff_ms);
		lck_mtx_unlock(&raphael_preview_lock);return EIO;
	}
	if (raphael_selected_sensor == 2) {
		int error = raphael_front_read_locked(uio);
		lck_mtx_unlock(&raphael_preview_lock);
		return error;
	}
	if (raphael_session_enabled) { lck_mtx_unlock(&raphael_preview_lock); return raphael_session_read(uio); }
	struct raphael_preview_shot p={.lines=raphael_preview_lines,
	    .gain=raphael_preview_gain,.length=raphael_preview_frame};
	uint64_t read_begin_ms=ios_ms();
	uint64_t size=0;
	const uint8_t *table=hakios_wifi_file("raphael-imx586.table",&size);
	bool ok=raphael_sensor_load(&p.sensor,table,size)&&
	    raphael_camera_rear_probe(raphael_preview_shot,&p,&p.retain);
	uint64_t read_end_ms=ios_ms();
	if (!ok) {
		raphael_preview_fault=true;
		lck_mtx_unlock(&raphael_preview_lock);return EIO;
	}
	if (p.content_rejected) {
		if (++raphael_preview_content_drops<=3||raphael_preview_content_drops%30==0)
			say("raphael-camera: preview content drop=%u; full restoration verified, retry next read\n",
			    raphael_preview_content_drops);
		lck_mtx_unlock(&raphael_preview_lock);return EAGAIN;
	}
	struct cam_header *h=(struct cam_header *)frames;
	if (h->frames!=1||h->width!=4000||h->height!=3000||h->stride!=5008) {
		raphael_preview_fault=true;
		lck_mtx_unlock(&raphael_preview_lock);return EIO;
	}
	struct hakios_camera_frame f={0};
	memcpy(f.magic,HAKIOS_CAMERA_MAGIC,8);f.header_bytes=sizeof(f);
	f.width=4000;f.height=3000;f.stride=5008;f.format=1;f.bayer=0;f.black=64;
	f.sequence=++raphael_preview_sequence;f.time=mach_absolute_time();
	f.exposure_lines=p.lines;
	/* Timing from the mode's declared fps and frame length, not output
	 * pixel clock (which is not its VT exposure clock). */
	f.line_ns=(uint32_t)(1000000000000ull/(30010ull*3064));
	f.gain=p.gain;f.gain_unity=256;f.gain_max=4096;
	f.frame_lines=p.length;f.frame_lines_min=3064;f.frame_lines_max=6112;
	f.margin=48;f.sensor=1;f.focus=HAKIOS_CAMERA_NO_FOCUS;
	int error=uiomove((caddr_t)&f,(int)MIN(sizeof(f),(size_t)uio_resid(uio)),uio);
	if (!error&&uio_resid(uio)>0) error=uiomove((caddr_t)(frames+CAM_HEADER_BYTES),
	    (int)MIN((uint64_t)h->frame_bytes,(uint64_t)uio_resid(uio)),uio);
	if (f.sequence<=3||f.sequence%30==0) {
		/* Profile the existing transaction. Do not infer that the ISP is
		 * slow from total read time, or change power/DMA lifetime to guess. */
		say("raphael-camera: read phases frame=%llu total=%llu parent-up=%llu sensor-table=%llu controls=%llu receiver-up=%llu capture=%llu receiver-down=%llu parent-down=%llu ms\n",
		    f.sequence,read_end_ms-read_begin_ms,p.began_ms-read_begin_ms,
		    p.prepared_ms-p.began_ms,p.controls_ms-p.prepared_ms,
		    p.receiver_ms-p.controls_ms,p.capture_ms-p.receiver_ms,
		    p.ended_ms-p.capture_ms,read_end_ms-p.ended_ms);
		say("raphael-camera: live read frame=%llu result=%d lines=%u linear-gainQ8=%u frame-lines=%u\n",
		    f.sequence,error,p.lines,p.gain,p.length);
		raphael_camera_map_status(f.sequence);
	}
	lck_mtx_unlock(&raphael_preview_lock);
	return error;
}

static int
raphael_camdev_write(struct uio *uio)
{
	struct hakios_camera_command c={0};
	if (uio_resid(uio)<(user_ssize_t)sizeof(c)||uiomove((caddr_t)&c,sizeof(c),uio)) return EINVAL;
	lck_mtx_lock(&raphael_preview_lock);
	int error=0;
	if (!raphael_preview_ready||raphael_preview_fault) error=EIO;
	else if (c.command==HAKIOS_CAMERA_EXPOSURE && raphael_selected_sensor==2) {
		uint32_t length=c.c?c.c:2064;
		length=length<2064?2064:length>4128?4128:length;
		raphael_front_length=length;
		raphael_front_lines=c.a<4?4:c.a>length-16?length-16:c.a;
		raphael_front_gain=c.b<256?256:c.b>4096?4096:c.b;
	} else if (c.command==HAKIOS_CAMERA_EXPOSURE) {
		uint32_t length=c.c?c.c:3064;
		length=length<3064?3064:length>6112?6112:length;
		raphael_preview_frame=length;
		raphael_preview_lines=c.a<4?4:c.a>length-48?length-48:c.a;
		raphael_preview_gain=c.b<288?288:c.b>4096?4096:c.b;
		if (raphael_session_enabled) raphael_session_pending=true;
	} else if (c.command==HAKIOS_CAMERA_SENSOR) {
		if (c.b!=~0u&&c.b!=0) error=ENODEV;
		else error=raphael_front_select(c.a);
	} else if (c.command==HAKIOS_CAMERA_KEEP) error=keep_picture(&c,uio);
	else if (c.command==HAKIOS_CAMERA_FOCUS) error=raphael_selected_sensor==1 ? raphael_session_focus_command(c.a) : ENOTSUP;
	else error=EINVAL;
	lck_mtx_unlock(&raphael_preview_lock);return error;
}
