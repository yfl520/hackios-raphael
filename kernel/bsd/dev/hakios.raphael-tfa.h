/* Raphael TFA9874 transport. Board live DT: SE1 GPIO114/115, reset GPIO59,
 * all on SM8150 TLMM SOUTH. Volatile board settings and bounded playback;
 * no OTP/MTP programming or calibration. */
#define RTFA_SDA 114
#define RTFA_SCL 115
#define RTFA_RESET 59
static bool rtfa_bad;
static bool rtfa_taken, rtfa_prepared, rtfa_on;
static uint32_t rtfa_saved_cfg[3], rtfa_saved_io[3];
static const unsigned rtfa_pins[3] = { RTFA_SDA, RTFA_SCL, RTFA_RESET };

static void rtfa_half(void) { delay(5); }
static void rtfa_line(unsigned pin, bool high)
{
	wr(tlmm.base + GPIO_IO(pin), 0);
	wr(tlmm.base + GPIO_CFG(pin), high ? 3u : 3u | CFG_OUTPUT);
}
static bool rtfa_high(unsigned pin) { return (rd(tlmm.base + GPIO_IO(pin)) & 1) != 0; }
static void rtfa_clock(void)
{
	rtfa_line(RTFA_SCL, true);
	for (unsigned n=0; !rtfa_high(RTFA_SCL) && n<100; n++) rtfa_half();
	if (!rtfa_high(RTFA_SCL)) rtfa_bad=true;
	rtfa_half();
}
static void rtfa_start(void)
{
	rtfa_line(RTFA_SDA,true); rtfa_clock();
	rtfa_line(RTFA_SDA,false);rtfa_half();rtfa_line(RTFA_SCL,false);rtfa_half();
}
static void rtfa_stop(void)
{
	rtfa_line(RTFA_SDA,false);rtfa_half();rtfa_clock();
	rtfa_line(RTFA_SDA,true);rtfa_half();
}
static bool rtfa_bit(bool out)
{
	rtfa_line(RTFA_SDA,out);rtfa_half();rtfa_clock();
	bool bit=rtfa_high(RTFA_SDA);rtfa_line(RTFA_SCL,false);return bit;
}
static bool rtfa_byte(uint8_t data)
{
	for(int b=7;b>=0;b--) rtfa_bit((data>>b)&1);
	return !rtfa_bit(true) && !rtfa_bad;
}
static uint8_t rtfa_receive(bool more)
{
	uint8_t v=0;for(unsigned b=0;b<8;b++) v=(uint8_t)((v<<1)|rtfa_bit(true));
	rtfa_bit(!more);return v;
}
static bool rtfa_get(uint8_t reg, uint16_t *value)
{
	rtfa_bad=false;rtfa_start();
	bool ack_address=rtfa_byte(0x68), ack_register=false, ack_read=false;
	if(ack_address) ack_register=rtfa_byte(reg);
	if(ack_register){rtfa_start();ack_read=rtfa_byte(0x69);}
	bool ok=ack_address && ack_register && ack_read;
	uint8_t hi=0,lo=0;
	if(ok){hi=rtfa_receive(true);lo=rtfa_receive(false);}
	rtfa_stop();*value=(uint16_t)((hi<<8)|lo);
	if(!ok||rtfa_bad) say("tfa9874: read %#x ACK address/register/read=%u/%u/%u stretch_fault=%u\n",
	    reg,ack_address,ack_register,ack_read,rtfa_bad);
	return ok && !rtfa_bad;
}
static void rtfa_restore(void)
{
	for(unsigned n=0;n<3;n++) {
		wr(tlmm.base+GPIO_IO(rtfa_pins[n]),rtfa_saved_io[n]&2);
		wr(tlmm.base+GPIO_CFG(rtfa_pins[n]),rtfa_saved_cfg[n]);
	}
	rtfa_taken=false;
}
static bool rtfa_take(void)
{
	if(rtfa_taken) return true;
	for(unsigned n=0;n<3;n++) {
		rtfa_saved_cfg[n]=rd(tlmm.base+GPIO_CFG(rtfa_pins[n]));
		rtfa_saved_io[n]=rd(tlmm.base+GPIO_IO(rtfa_pins[n]));
	}
	rtfa_taken=true;
	say("tfa9874: GPIO114/115 I2C, reset59 cfg=%#x io=%#x; volatile access\n",rtfa_saved_cfg[2],rtfa_saved_io[2]);
	/* tfa98xx_ext_reset(): active HIGH for 10 ms, release LOW for 10 ms. */
	wr(tlmm.base+GPIO_IO(RTFA_RESET),2);
	wr(tlmm.base+GPIO_CFG(RTFA_RESET),CFG_OUTPUT);
	sleep_ms(10);
	wr(tlmm.base+GPIO_IO(RTFA_RESET),0);
	sleep_ms(10);
	rtfa_line(RTFA_SDA,true);rtfa_line(RTFA_SCL,true);rtfa_half();
	if(!rtfa_high(RTFA_SDA)||!rtfa_high(RTFA_SCL)) {
		say("tfa9874: I2C bus not idle, probe aborted\n");rtfa_restore();return false;
	}
	return true;
}
static bool rtfa_probe(void)
{
	if(!rtfa_take()) return false;
	uint16_t id=0;
	bool ok=rtfa_get(3,&id) && (id==0x0a74||id==0x0b74||id==0x0c74);
	say("tfa9874: revision=%#x, %s\n",id,ok?"recognized":"not recognized; no amplifier register writes");
	if(ok) {
		static const uint8_t regs[]={0,1,2,4,0x11,0x14,0x15,0x16,0x20,0x21,0x22,0x23,0x26,0x27,0x50,0x51,0x61};
		for(unsigned n=0;n<sizeof(regs);n++) {
			uint16_t v=0;
			if(!rtfa_get(regs[n],&v)){ok=false;break;}
			say("tfa9874: read register %#x = %#x\n",regs[n],v);
		}
	}
	rtfa_restore();say("tfa9874: probe ended, GPIO/reset restored; amplifier not enabled\n");
	return ok;
}

#include "hakios.raphael-tfa-settings.h"
/* This exact set contains volatile board controls and the reference driver's
 * access keys. No MTP address/program/copy command, OTP, or calibration API. */
static bool rtfa_put(uint8_t reg,uint16_t value)
{
	bool allowed=reg==0 || reg==1 || reg==0x0f || reg==0xa0 || reg==0xa1 ||
	    reg==0x20 || reg==0x21 || reg==0x22;
	for(unsigned i=0;i<sizeof(rtfa_optimal)/sizeof(rtfa_optimal[0]);i++) allowed|=rtfa_optimal[i].reg==reg;
	for(unsigned i=0;i<sizeof(rtfa_board)/sizeof(rtfa_board[0]);i++) allowed|=rtfa_board[i].reg==reg;
	if(!allowed || !rtfa_taken) return false;
	/* The FAIM/MTP control bit is never enabled. */
	if(reg==1 && (value&0x4000)) return false;
	rtfa_bad=false;rtfa_start();
	bool ok=rtfa_byte(0x68)&&rtfa_byte(reg)&&rtfa_byte((uint8_t)(value>>8))&&rtfa_byte((uint8_t)value);
	rtfa_stop();
	if(!ok||rtfa_bad) say("tfa9874: write %#x failed, stretch=%u\n",reg,rtfa_bad);
	return ok&&!rtfa_bad;
}
static bool rtfa_update(uint8_t reg,uint16_t mask,uint16_t value)
{
	uint16_t old=0,got=0;
	if(!rtfa_get(reg,&old)||!rtfa_put(reg,(uint16_t)((old&~mask)|(value&mask)))) return false;
	if(!rtfa_get(reg,&got)||(got&mask)!=(value&mask)) {
		say("tfa9874: readback reg=%#x mask=%#x requested=%#x got=%#x\n",reg,mask,value,got);return false;
	}
	return true;
}
static bool rtfa_settings(const struct rtfa_setting *s,unsigned n)
{
	for(unsigned i=0;i<n;i++) {
		bool key=s[i].reg==0x0f||s[i].reg==0xa0||s[i].reg==0xa1;
		if(!(key?rtfa_put(s[i].reg,s[i].value):rtfa_update(s[i].reg,s[i].mask,s[i].value))) return false;
	}
	return true;
}
static void rtfa_shutdown(void)
{
	if(!rtfa_taken) return;
	(void)rtfa_update(0,0x0009,1); /* PWDN=1, AMPE=0: tfa98xx_powerdown */
	/* The physical reset also covers a lost I2C ACK. POR leaves PWDN=1. */
	wr(tlmm.base+GPIO_IO(RTFA_RESET),2);
	wr(tlmm.base+GPIO_CFG(RTFA_RESET),CFG_OUTPUT);
	sleep_ms(10);
	rtfa_restore();rtfa_on=rtfa_prepared=false;
	say("tfa9874: amplifier powered down and reset, GPIO restored\n");
}
static void rtfa_quiesce(void)
{
	if(!rtfa_taken||!tlmm.base) return;
	wr(tlmm.base+GPIO_IO(RTFA_RESET),2);
	wr(tlmm.base+GPIO_CFG(RTFA_RESET),CFG_OUTPUT);
	rtfa_on=false;
}
static bool rtfa_prepare(void)
{
	uint16_t id=0,key=0,flags=0;
	if(!rtfa_take()||!rtfa_get(3,&id)||id!=0x0c74) goto fail;
	/* Board reset already supplied POR; no nonvolatile programming is performed. */
	if(!rtfa_put(0x0f,0x5a6b)||!rtfa_get(0xfb,&key)||!rtfa_put(0xa0,key^0x005a)||
	    !rtfa_put(0xa1,0x005a)||!rtfa_put(0x0f,0)) goto fail;
	if(!rtfa_settings(rtfa_optimal,sizeof(rtfa_optimal)/sizeof(rtfa_optimal[0]))||
	    !rtfa_settings(rtfa_board,sizeof(rtfa_board)/sizeof(rtfa_board[0]))) goto fail;
	/* Match pinned tfa_dev_start(pcm_format=24): 64 BCLK/frame, 32-bit
	 * slots AND 32-bit receive size. POR receive size is only 16 bits. */
	if(!rtfa_update(0x20,0xf000,0x2000)||!rtfa_update(0x21,0x01f0,0x01f0)||
	    !rtfa_update(0x22,0x007c,0x007c)) goto fail;
	/* Hardware ready, but amplifier off; source follows pinned tfa_dev_set_state. */
	if(!rtfa_update(0,0x0009,1)||!rtfa_update(1,0x0004,4)||!rtfa_update(0,1,0)) goto fail;
	for(unsigned n=0;n<50;n++) {
		if(!rtfa_get(0x11,&flags)) goto fail;
		if(flags&0x0080) break;
		sleep_ms(2);
	}
	if(!(flags&0x0080)) {say("tfa9874: clocks not stable, status=%#x\n",flags);goto fail;}
	(void)rtfa_get(0x10,&flags); /* clear POR's sticky flags before monitoring */
	rtfa_prepared=true;
	static const uint8_t route_regs[]={0,1,2,0x20,0x21,0x22,0x23,0x26,0x61};
	for (unsigned n=0; n<sizeof(route_regs); n++) {
		uint16_t value=0;
		if (!rtfa_get(route_regs[n],&value)) goto fail;
		say("tfa9874: prepared route reg=%#x value=%#x\n",route_regs[n],value);
	}
	say("tfa9874: 0x0c74 optimal + board speaker configuration read back; clocks stable; AMPE=0\n");
	return true;
fail:
	say("tfa9874: preparation failed; no tone\n");rtfa_shutdown();return false;
}
static bool rtfa_start_amp(void)
{
	uint16_t status=0,state=0;
	if(!rtfa_prepared||!rtfa_update(0,0x0009,0x0008)) return false;
	rtfa_on=true;sleep_ms(20);
	if(!rtfa_get(0x11,&status)||!rtfa_get(0x14,&state)) {rtfa_shutdown();return false;}
	say("tfa9874: AMPE requested, status=%#x state=%#x; physical sound still needs confirmation\n",status,state);
	return true;
}
static bool rtfa_healthy(void)
{
	uint16_t alarms=0,raw=0;
	if(!rtfa_on||!rtfa_get(0x10,&alarms)||!rtfa_get(0x16,&raw)) {rtfa_shutdown();return false;}
	int temp=(raw&0x100)?(int)(raw&0x1ff)-512:(raw&0x1ff);
	/* NXP tfa_status(): OTDS/UVDS are positive 'OK' levels. 0x16 is
	 * DCOCPOK|OTDS|UVDS, not three asserted faults. OCDS/TDMERR/NOCLK
	 * are active high. Preserve shutdown on either actual alarm or lost OK. */
	if((alarms&0x0016)!=0x0016 || (alarms&0x00c8) || temp>=85||temp< -40) {
		say("tfa9874: stopping: alarm=%#x temperature=%d\n",alarms,temp);rtfa_shutdown();return false;
	}
	return true;
}
