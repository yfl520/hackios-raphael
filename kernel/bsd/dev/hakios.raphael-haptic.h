/* Raphael PM8150B ERM haptics. Included by hakios.haptic.c after its PCM sink.
 * Register protocol: LineageOS/android_kernel_xiaomi_sm8150, commit
 * 59220a048d04516e7fe1dfc094507930166a1e60, qti-haptics.c and spmi-pmic-arb.c.
 * Board values come from this phone's live DT: SID3:c000, ERM, 3000mV, 6667us.
 * No GPIOs, charger registers, secure-access or PMIC reset writes are used.
 */
#define RH_PPID 0x3c0u
#define RH_MAX_US 80000u
#define RH_SETTLE_US 5000u
#define RH_VMIN_MV 2000u
#define RH_VMAX_MV 2600u
/* Two driven periods (~13.3ms), then zero; no overdrive or repeat.
 * This is the electrical envelope, not a claim about ERM mechanical decay. */
static const uint8_t rh_click[8] = {0x3e,0x26,0,0,0,0,0,0};
static vm_offset_t rh_core, rh_cfg, rh_obs, rh_chan;
static unsigned rh_apid;
static bool rh_found, rh_owner, rh_configured, rh_bad;
static volatile bool rh_leaving;
static unsigned rh_events, rh_capped;
static uint8_t rh_type, rh_subtype;
/* Set only after the read-only hardware probe has identified this board. */
#define RH_VERIFIED_TYPE 0x15u
#define RH_VERIFIED_SUBTYPE 0x09u
static const uint8_t rh_regs[] = {0x46,0x48,0x4a,0x4b,0x4c,0x4e,0x51,0x52,0x54,0x55,0x5a,0x5c,0x5e,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x70};
static uint8_t rh_saved[sizeof(rh_regs)];

static bool
rh_done(vm_offset_t ch)
{
	uint64_t until = now_us() + 2000;
	uint32_t status;
	do { status = rd(ch + 8); } while (!(status & 1) && now_us() < until);
	if ((status & 0xf) == 1) return true;
	say("raphael-haptic: SPMI transfer failed status=%#x\n", status);
	return false;
}

static bool
rh_get(uint8_t reg, uint8_t *value)
{
	if (!rh_found || !rh_obs) return false;
	if (!rh_leaving) hakios_spmi_lock();
	wr(rh_obs, (1u << 27) | (uint32_t)reg << 4);
	bool ok = rh_done(rh_obs);
	if (ok) *value = (uint8_t)rd(rh_obs + 0x18);
	if (!rh_leaving) hakios_spmi_unlock();
	return ok;
}

/* Even emergency writes are restricted to this peripheral and the allowlist. */
static bool
rh_write(uint8_t reg, uint8_t value)
{
	bool allowed = false;
	for (unsigned i=0; i<sizeof(rh_regs); i++) allowed |= rh_regs[i] == reg;
	if (!allowed || !rh_owner || !rh_chan ||
	    ((rd(rh_core + 0x900 + 4*rh_apid) >> 8) & 0xfff) != RH_PPID ||
	    (rd(rh_cfg + 0x700 + 4*rh_apid) & 7) != 0) return false;
	if (!rh_leaving) hakios_spmi_lock();
	wr(rh_chan + 0x10, value);
	wr(rh_chan, (uint32_t)reg << 4);
	bool ok = rh_done(rh_chan);
	if (!rh_leaving) hakios_spmi_unlock();
	uint8_t back = 0xff;
	ok = ok && rh_get(reg, &back);
	if (ok && reg==0x70 && value==0x80 && back==0) {
		uint8_t status=0xff;
		bool read_status=rh_get(0x0a,&status);
		say("raphael-haptic: PLAY accepted, reads cleared; STATUS1=%#x read_ok=%u (physical pulse not inferred)\n",status,read_status);
		/* A finite buffer may finish before readback. Preserve the transaction
		 * evidence; only physical observation can certify that it moved. */
		return read_status && !(status&8);
	}
	ok = ok && back == value;
	if (!ok) say("raphael-haptic: write reg=%#x value=%#x read=%#x failed\n",reg,value,back);
	return ok;
}

static void
rh_stop(void)
{
	if (!rh_configured) return;
	bool play = rh_write(0x70, 0);
	bool module = rh_write(0x46, 0);
	if (!play || !module) rh_bad = true;
}

static bool
rh_put(uint8_t reg, uint8_t value)
{
	if (rh_bad || rh_leaving) return false;
	if (rh_write(reg,value)) return true;
	rh_bad = true;
	rh_stop();
	return false;
}

static bool
rh_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t before;
	if (!rh_get(reg,&before)) { rh_bad=true; rh_stop(); return false; }
	return rh_put(reg,(before & ~mask) | (value & mask));
}

static bool
rh_probe(void)
{
	rh_core=ml_io_map(0x0c440000,0x1100);
	rh_cfg=ml_io_map(0x0c40a000,0x1000);
	if (!rh_core || !rh_cfg) return false;
	uint32_t version=rd(rh_core);
	say("raphael-haptic: arbiter=%#x PPID=%#x (SID3:c000), probe reads only\n",version,RH_PPID);
	if (version < 0x50000000 || version >= 0x70000000) return false;
	for (unsigned n=0;n<512;n++) {
		uint32_t map=rd(rh_core+0x900+4*n);
		if (((map>>8)&0xfff)!=RH_PPID) continue;
		unsigned owner=rd(rh_cfg+0x700+4*n)&7;
		say("raphael-haptic: APID=%u owner=%u mapping=%#x\n",n,owner,map);
		if (!rh_found || owner==0) { rh_apid=n; rh_found=true; rh_owner=owner==0; }
	}
	if (!rh_found) return false;
	rh_obs=ml_io_map(0x0e600000UL+0x80UL*rh_apid,0x20);
	if (!rh_obs || !rh_get(4,&rh_type) || !rh_get(5,&rh_subtype)) return false;
	say("raphael-haptic: type=%#x subtype=%#x writable=%u\n",rh_type,rh_subtype,rh_owner);
	for (unsigned i=0;i<sizeof(rh_regs);i++) {
		if (!rh_get(rh_regs[i],&rh_saved[i])) return false;
		say("raphael-haptic: initial %#x=%#x\n",rh_regs[i],rh_saved[i]);
	}
	uint8_t status;
	if (!rh_get(0x0a,&status)) return false;
	say("raphael-haptic: initial STATUS1=%#x, DT ERM maximum=3000mV period=6667us\n",status);
	return true;
}

static bool
rh_configure(void)
{
	if (!RH_VERIFIED_TYPE || rh_type!=RH_VERIFIED_TYPE || rh_subtype!=RH_VERIFIED_SUBTYPE || !rh_owner) {
		say("raphael-haptic: hardware identity has not been approved for writes\n"); return false;
	}
	/* Avoid adopting an already running motor. */
	uint8_t play,enable,status;
	if (!rh_get(0x70,&play) || !rh_get(0x46,&enable) || !rh_get(0x0a,&status) ||
	    (play&0x80) || (enable&0x80) || (status&0x08)) return false;
	rh_chan=ml_io_map(0x0c600000UL+0x10000UL*rh_apid,0x20);
	if (!rh_chan) return false;
	rh_configured=true;
	/* Gentler click envelope at the board's DT period. Hardware ends the
	 * finite buffer even if the application processor stops scheduling. */
	if (!(rh_put(0x70,0) && rh_put(0x46,0) && rh_put(0x4c,1) &&
	    /* Keep the bootloader's readable current limit; bit 1 from the vendor
	     * source does not stick on this peripheral revision (probe evidence). */
	    rh_put(0x4a,0xfd) && rh_bits(0x5a,7,3) &&
	    rh_put(0x54,(6667/5)&0xff) && rh_put(0x55,(6667/5)>>8) &&
	    rh_bits(0x4b,0x80,0) && rh_bits(0x4e,0x31,0x10) &&
	    rh_bits(0x48,1,1) && rh_put(0x5c,1) && rh_bits(0x5e,0x73,0) &&
	    rh_put(0x51,(RH_VMAX_MV/116)<<1))) return false;
	for (unsigned i=0;i<8;i++) if (!rh_put(0x60+i,rh_click[i])) return false;
	return true;
}

static bool
rh_start(uint32_t peak)
{
	uint8_t status;
	if (!rh_get(0x0a,&status) || (status&8)) { rh_bad=true; rh_stop(); return false; }
	/* ERM translates the envelope; it cannot reproduce a linear Taptic waveform. */
	unsigned mv=RH_VMIN_MV+(RH_VMAX_MV-RH_VMIN_MV)*(peak>32767?32767:peak)/32767;
	return rh_put(0x51,(mv/116)<<1) && rh_put(0x46,0x80) && rh_put(0x70,0x80);
}

static void
rh_restore(void)
{
	if (!rh_configured) return;
	rh_stop();
	/* Restore only while disabled; never restore PLAY/MODULE enable bits. */
	for (unsigned i=0;i<sizeof(rh_regs);i++) {
		if (rh_regs[i]!=0x46 && rh_regs[i]!=0x70 && !rh_write(rh_regs[i],rh_saved[i])) rh_bad=true;
	}
	rh_stop();
}

static void
rh_serve(void)
{
	int major=cdevsw_add(-1,&haptic_cdevsw);
	if (major<0 || !devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0666,"hakios-haptic")) return;
	say("raphael-haptic: iOS Actuator sink ready; refine1: drive~13.3ms, VMAX=1972..2552mV, watchdog=80ms, cap rest=80ms, settle=5ms\n");
	bool playing=false;
	uint64_t since=0,rest=0,last_onset=0;
	while (!rh_bad && !rh_leaving) {
		uint64_t now=now_us();
		bool active=heard_us && now-heard_us<ACTIVE_US;
		if (playing && ((!active && now-since>=20000) || now-since>=RH_MAX_US)) {
			if (now-since>=RH_MAX_US) { rh_capped++; rest=now+RH_MAX_US; }
			rh_stop(); playing=false;
		}
		if (!playing && active && onset_us!=last_onset && now>=rest &&
		    now>=onset_us && now-onset_us>=RH_SETTLE_US) {
			last_onset=onset_us;
			if (!rh_start(heard_peak)) break;
			playing=true; since=now; rh_events++;
			if (rh_events<=24) say("raphael-haptic: iOS pulse %u peak=%u (refine1 finite envelope)\n",rh_events,heard_peak);
		}
		if (playing) {
			uint8_t status;
			if (!rh_get(0x0a,&status) || (status&8)) { rh_bad=true; rh_stop(); }
		}
		wait_ms(4);
	}
	rh_stop();
}

static void
rh_thread(__unused void *parameter,__unused wait_result_t result)
{
	if (rh_probe() && level>=2 && rh_configure()) {
		say("raphael-haptic: ERM configured, standby readback passed\n");
		if (level==4 && rh_start(16384)) {
			wait_ms(60); rh_stop();
			say("raphael-haptic: bounded 60ms test completed; physical result requires observation\n");
		} else if (level==5) rh_serve();
	}
	if (level!=5 || rh_bad) rh_restore();
	say("raphael-haptic: worker done events=%u capped=%u fault=%u PCM=%llu\n",rh_events,rh_capped,rh_bad,written_frames);
	thread_terminate(current_thread());
}

static void
rh_init(void)
{
	thread_t thread;
	if (!PE_parse_boot_argn("haptic",&level,sizeof(level)) || !level) return;
	if (level>5) level=5;
	if (kernel_thread_start(rh_thread,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
}

static void
rh_quiesce(void)
{
	rh_leaving=true;
	/* Panic/stop can interrupt a bus lock holder; no lock acquisition here. */
	rh_stop();
	say("raphael-haptic: quiesced events=%u capped=%u PCM=%llu fault=%u\n",rh_events,rh_capped,written_frames,rh_bad);
}
