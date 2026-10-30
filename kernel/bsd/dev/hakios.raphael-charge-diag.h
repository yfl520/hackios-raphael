/* Read-only SMB5 observer diagnostics. Board: live DT SID2 0x1000..0x16ff.
 * Definitions: pinned LineageOS SM8150 smb5-reg.h in research/sources.json.
 * No PMIC register data writes: the only MMIO write issues SPMI READ commands.
 */
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern void loader_log_putc(char);

static vm_offset_t rc_core;
static struct { uint16_t ppid; vm_offset_t observer; } rc_blocks[] = {
	{0x210,0}, {0x211,0}, {0x212,0}, {0x213,0}, {0x216,0},
};
static bool rc_ready;
static vm_offset_t rc_typec_obs;

static bool
rc_typec_read(uint8_t reg, uint8_t *out)
{
    *out = 0xff;
    if (!rc_typec_obs) return false;
    uint64_t duration, start;
    nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &duration);
    hakios_spmi_lock();
    start = mach_absolute_time();
    *(volatile uint32_t *)rc_typec_obs = (1u << 27) | (uint32_t)reg << 4;
    uint32_t status;
    do { status = *(volatile uint32_t *)(rc_typec_obs + 8); }
    while (!(status & 1) && mach_absolute_time() - start < duration);
    bool ok = (status & 0xf) == 1;
    if (ok) *out = (uint8_t)*(volatile uint32_t *)(rc_typec_obs + 0x18);
    hakios_spmi_unlock();
    return ok;
}


static void
rc_tell(const char *format, ...) __printflike(1,2);

static void
rc_tell(const char *format, ...)
{
	char line[420];
	va_list list;
	va_start(list,format);
	vsnprintf(line,sizeof(line),format,list);
	va_end(list);
	for (const char *p=line; *p; p++) loader_log_putc(*p);
}

static bool
rc_read(unsigned block, uint8_t reg, uint8_t *out)
{
	vm_offset_t ch=rc_blocks[block].observer;
	uint64_t interval, begin;
	uint32_t status=0;
	*out=0xff;
	if (!ch) return false;
	nanoseconds_to_absolutetime(2000ULL*NSEC_PER_USEC,&interval);
	hakios_spmi_lock();
	begin=mach_absolute_time();
	/* extended read long, count=1; no write-channel mapping exists here */
	*(volatile uint32_t *)ch=(1u<<27)|(uint32_t)reg<<4;
	do { status=*(volatile uint32_t *)(ch+8); }
	while (!(status&1) && mach_absolute_time()-begin<interval);
	bool ok=(status&0xf)==1;
	if (ok) *out=(uint8_t)*(volatile uint32_t *)(ch+0x18);
	hakios_spmi_unlock();
	return ok;
}

static void
rc_probe(void)
{
	if (!hakios_is_raphael()) return;
	rc_core=ml_io_map(0x0c440000,0x1100);
	if (!rc_core) return;
	uint32_t version=*(volatile uint32_t *)rc_core;
	if (version<0x50000000 || version>=0x70000000) return;
	vm_offset_t cfg=ml_io_map(0x0c40a000,0x1000);
	if (!cfg) return;
	for (unsigned b=0;b<sizeof(rc_blocks)/sizeof(rc_blocks[0]);b++) {
		unsigned selected=512;
		for (unsigned n=0;n<512;n++) {
			uint32_t map=*(volatile uint32_t *)(rc_core+0x900+4*n);
			if (((map>>8)&0xfff)!=rc_blocks[b].ppid) continue;
			if (selected==512) selected=n;
			if ((*(volatile uint32_t *)(cfg+0x700+4*n)&7)==0) { selected=n; break; }
		}
		if (selected==512) continue;
		rc_blocks[b].observer=ml_io_map(0x0e600000UL+0x80UL*selected,0x20);
		uint8_t type=0xff,subtype=0xff;
		bool ok=rc_read(b,4,&type) && rc_read(b,5,&subtype);
		rc_tell("charge-diag: read-only PPID=%#x APID=%u type=%#x subtype=%#x ok=%u\n",rc_blocks[b].ppid,selected,type,subtype,ok);
		if (!ok) rc_blocks[b].observer=0;
	}

    /* TYPEC at SID2/0x1500: observer READ commands only, never its write channel. */
    for (unsigned n = 0; n < 512; n++) {
        if (((*(volatile uint32_t *)(rc_core + 0x900 + 4*n) >> 8) & 0xfff) != 0x215) continue;
        rc_typec_obs = ml_io_map(0x0e600000UL + 0x80UL*n, 0x20);
        uint8_t type = 0xff, subtype = 0xff;
        bool ok = rc_typec_read(4, &type) && rc_typec_read(5, &subtype) && type == 2 && subtype == 0x85;
        rc_tell("charge-diag5: TYPEC PPID=0x215 APID=%u type=%#x subtype=%#x accepted=%u\n", n, type, subtype, ok);
        if (!ok) rc_typec_obs = 0;
        break;
    }
	rc_ready=true;
}

static void
rc_poll(void)
{
	if (!rc_ready) return;
	static const uint8_t registers[5][12]={
		{0x06,0x07,0x0b,0x0d,0x42,0x43,0x61,0x70,0x90,0xa2,0x04,0x05},
		{0x06,0x07,0x08,0x0a,0x0b,0x40,0x04,0x05,0x50,0x53,0x04,0x05},
		{0x10,0x04,0x05,0x82,0x10,0x04,0x05,0x82,0x10,0x04,0x05,0x82},
		{0x07,0x08,0x10,0x40,0x42,0x60,0x62,0x63,0x65,0x66,0x70,0x80},
		{0x06,0x07,0x08,0x09,0x0a,0x48,0x51,0x53,0x70,0x90,0x10,0x04},
	};
	for (unsigned b=0;b<5;b++) {
		uint8_t values[12];
		unsigned valid=0;
		for (unsigned i=0;i<12;i++) if (rc_read(b,registers[b][i],&values[i])) valid|=1u<<i;
		rc_tell("charge-diag: PPID=%#x valid=%03x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x r%02x=%02x\n",
		    rc_blocks[b].ppid,valid,
		    registers[b][0],values[0],registers[b][1],values[1],registers[b][2],values[2],
		    registers[b][3],values[3],registers[b][4],values[4],registers[b][5],values[5],
		    registers[b][6],values[6],registers[b][7],values[7],registers[b][8],values[8],
		    registers[b][9],values[9],registers[b][10],values[10],registers[b][11],values[11]);
	}
}
