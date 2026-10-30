/*
 * Raphael EA8076: use the running DSI0 command engine, without panel/PHY setup.
 * Register sequence: LineageOS/android_kernel_xiaomi_sm8150 at
 * 59220a048d04516e7fe1dfc094507930166a1e60, dsi_ctrl_hw_cmn.c,
 * dsi_ctrl.c and drm_mipi_dsi.c. SS panel brightness is an 11-bit,
 * MSB-first DCS 0x51 long write, not the one-byte Sailfish command.
 * Caller holds brightness_lock AND the framebuffer's scanout lock.
 */
static vm_offset_t raphael_dsi;
static bool dsi_failed;
extern int raphael_display_command_begin(uint32_t *refresh, uint32_t *sync);
extern void raphael_display_command_end(uint32_t refresh, uint32_t sync);

static uint32_t
dsi_read(uint32_t offset)
{
	return *(volatile uint32_t *)(raphael_dsi + offset);
}

static void
dsi_write(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(raphael_dsi + offset) = value;
}

static void
dsi_barrier(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
dsi_wait(uint32_t offset, uint32_t mask, uint32_t expected, unsigned milliseconds)
{
	uint64_t start = mach_absolute_time(), elapsed;
	for (;;) {
		if ((dsi_read(offset) & mask) == expected) return 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= (uint64_t)milliseconds * NSEC_PER_MSEC) return ETIMEDOUT;
		delay_for_interval(50, NSEC_PER_USEC);
	}
}

/* W1C only our DMA/BTA status; retain enables and other pending statuses. */
static void
dsi_ack(void)
{
	/* Status bits 0,8,10,12,14,16,20,24,28,30: native get/clear_interrupt_status. */
	const uint32_t statuses = 0x51115501;
	dsi_write(0x110, (dsi_read(0x110) & ~statuses) | 1u | (1u << 20));
}

/* Qualcomm's embedded header is {data0, data1, type/VC, flags}. */
static int
dsi_packet(const uint32_t *words, unsigned count, bool readback)
{
	uint32_t phase;
	if (dsi_failed || !raphael_display_scan_counter(&phase)) return ENXIO;
	if (!raphael_dsi) raphael_dsi = ml_io_map(0x0ae94000, 0x400);
	if (!raphael_dsi) return ENOMEM;
	if ((dsi_read(0x004) & 7) != 5 || dsi_read(0x15c) != 0) return EBUSY;
	if (dsi_read(0x110) & (1u << 24)) return EIO;
	uint32_t refresh, sync;
	int error = raphael_display_command_begin(&refresh, &sync);
	if (error) return error;
	/* No fresh TE/MDP kickoff may race the idle check or the DMA command. */
	error = dsi_wait(0x008, (1u << 2) | (1u << 1), 0, 200);
	if (error) {
		raphael_display_command_end(refresh, sync);
		return error;
	}
	uint32_t dma = dsi_read(0x03c), length = dsi_read(0x04c), trigger = dsi_read(0x084);
	if (readback) {
		dsi_write(0x1d4, 1);
		dsi_barrier();
		dsi_write(0x1d4, 0);
	}
	dsi_ack();
	/* Software DMA trigger; preserve the MDP/TE trigger selection. */
	dsi_write(0x084, (trigger & ~7u) | 4u);
	dsi_write(0x15c, (1u << 1) | (1u << 2) | (3u << 16));
	for (unsigned i = 0; i < count; i++) dsi_write(0x17c, words[i]);
	if (count & 1) dsi_write(0x17c, 0);
	dsi_write(0x03c, (dma & ~((1u << 31) | (1u << 30))) | (1u << 28) | (1u << 26));
	dsi_write(0x04c, count * 4);
	dsi_barrier();
	dsi_write(0x090, 1);
	error = dsi_wait(0x110, 1, 1, 200);
	if (!error && readback) error = dsi_wait(0x110, 1u << 20, 1u << 20, 200);
	if (!error && (dsi_read(0x110) & (1u << 24))) error = EIO;
	/* Same bounded FIFO cleanup as the native driver's timeout path. */
	dsi_ack();
	dsi_write(0x15c, 0);
	dsi_write(0x1ec, 1);
	dsi_barrier();
	delay_for_interval(1, NSEC_PER_USEC);
	dsi_write(0x1ec, 0);
	dsi_write(0x04c, length);
	dsi_write(0x03c, dma);
	dsi_write(0x084, trigger);
	dsi_barrier();
	raphael_display_command_end(refresh, sync);
	if (error) {
		dsi_failed = true; /* Don't hammer a stalled controller on every fade tick. */
		printf("display: DSI command failed %d, status %#x IRQ %#x ACK %#x\n",
		    error, dsi_read(0x008), dsi_read(0x110), dsi_read(0x068));
	}
	return error;
}

static int
dsi_brightness_write(uint32_t percent)
{
	/* 1..100 maps to 20..2047: never send display-off brightness here. */
	uint32_t level = (percent * 2047u + 50u) / 100u;
	uint32_t words[] = { 0xc0390003, 0xff000051 | ((level >> 8) << 8) | ((level & 255) << 16) };
	return dsi_packet(words, 2, false);
}

static int
dsi_brightness_read(uint32_t *percent)
{
	uint32_t maximum = 0x80370002, request = 0xa0060052;
	int error = dsi_packet(&maximum, 1, false);
	if (!error) error = dsi_packet(&request, 1, true);
	if (error) return error;
	/* Two-byte DCS short response, reversed DWORD as in get_cmd_read_data. */
	uint32_t count = dsi_read(0x1d4) >> 16, data = __builtin_bswap32(dsi_read(0x06c));
	if ((count != 4 && count != 8) || (data & 255) != 0x22) return EIO;
	uint32_t level = ((data >> 8) & 255) << 8 | ((data >> 16) & 255);
	if (level > 2047) return ERANGE;
	*percent = (level * 100u + 1023u) / 2047u;
	return 0;
}
