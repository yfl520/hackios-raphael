/* SM8150 RPMh bounded single-command transaction, included by hakios.wifi.c.
 * Register/protocol source: qcom-rpmh-mailbox.c and rpmh.c, LineageOS
 * android_kernel_xiaomi_sm8150 59220a048d04516e7fe1dfc094507930166a1e60.
 * Caller holds rpmh_lock; writes through the existing Wi-Fi path use it too.
 * READ and WRITE preserve the TCS configuration after a completed transfer.
 * A timed out command is never overwritten, including by the Wi-Fi writer.
 * Resource and voltage restrictions belong to the board power manager.
 */
static bool raphael_rpmh_faulted;

/* VRM VOTE values include RESP_REQ at bit31. It is control metadata, not
 * voltage/enable/mode. Field masks: Qualcomm AOP vrm_vt_hwio.h (670) and
 * downstream rpmh-regulator.c value limits; confirmed by SM8150 ICNSS readback. */
static bool
raphael_rpmh_decode_vrm(uint32_t raw, unsigned field, uint32_t *value)
{
	static const uint32_t masks[] = { 0x1fff, 1, 7 };
	if (!value || field > 2 || (raw & ~(masks[field] | 0x80000000u))) return false;
	*value = raw & masks[field];
	return true;
}

static bool
raphael_rpmh_transfer_locked(uint32_t address, uint32_t data, bool write, uint32_t *value)
{
	vm_offset_t tcs = rsc.base + RSC_TCS(1);
	uint32_t ctl = rd(tcs + TCS_CONTROL), enabled = rd(tcs + TCS_ENABLED);
	if (raphael_rpmh_faulted || !value || (address & 3) ||
	    rd(tcs + TCS_STATUS) == 0 || enabled || (ctl & (1u << 24)) ||
	    (rd(rsc.base + RSC_TCS(0) + 4) & 2)) return false;
	uint32_t old_msg = rd(tcs + TCS_COMMAND(0));
	uint32_t old_addr = rd(tcs + TCS_COMMAND(0) + 4);
	uint32_t old_data = rd(tcs + TCS_COMMAND(0) + 8);
	uint32_t old_wait = rd(tcs + 0x10);
	uint32_t old_irq = rd(rsc.base + RSC_TCS(0));
	wr(tcs + TCS_CONTROL, ctl & ~((1u << 24) | (1u << 16)));
	/* Length=8, response requested; WRITE is bit16. */
	wr(tcs + TCS_COMMAND(0), write ? 0x10108 : 0x108);
	wr(tcs + TCS_COMMAND(0) + 4, address);
	wr(tcs + TCS_COMMAND(0) + 8, write ? data : 0);
	wr(tcs + 0x10, 1);
	wr(tcs + TCS_ENABLED, 1);
	wr(rsc.base + RSC_TCS(0), old_irq | 2);
	settle();
	wr(tcs + TCS_CONTROL, 1u << 16);
	wr(tcs + TCS_CONTROL, (1u << 16) | (1u << 24));
	bool done = false;
	for (unsigned i = 0; i < 2000; i++) {
		if (rd(rsc.base + RSC_TCS(0) + 4) & 2) { done = true; break; }
		sleep_us(50);
	}
	uint32_t status = rd(tcs + TCS_COMMAND(0) + 12);
	bool ok = done && (status & 0x10100) == 0x10100;
	if (ok) *value = write ? data : rd(tcs + TCS_COMMAND(0) + 16);
	/* An unfinished transfer must not have its command overwritten or reused.
	 * Keep this TCS quarantined until reboot, including for Wi-Fi writes. */
	if (!done) {
		raphael_rpmh_faulted = true;
		tell("camera: RPMh %s %#x timed out; TCS1 quarantined until reboot\n", write ? "write" : "read", address);
		wr(rsc.base + RSC_TCS(0), old_irq);
		return false;
	}
	wr(tcs + TCS_CONTROL, 1u << 16);
	wr(tcs + TCS_CONTROL, 0);
	wr(tcs + TCS_ENABLED, 0);
	wr(rsc.base + RSC_TCS(0) + 8, 2);
	wr(tcs + TCS_COMMAND(0), old_msg);
	wr(tcs + TCS_COMMAND(0) + 4, old_addr);
	wr(tcs + TCS_COMMAND(0) + 8, old_data);
	wr(tcs + 0x10, old_wait);
	wr(tcs + TCS_CONTROL, ctl);
	wr(rsc.base + RSC_TCS(0), old_irq);
	return ok;
}

static bool
raphael_rpmh_read_locked(uint32_t address, uint32_t *value)
{
	return raphael_rpmh_transfer_locked(address, 0, false, value);
}

static bool
raphael_rpmh_write_locked(uint32_t address, uint32_t value)
{
	uint32_t accepted;
	return raphael_rpmh_transfer_locked(address, value, true, &accepted);
}
