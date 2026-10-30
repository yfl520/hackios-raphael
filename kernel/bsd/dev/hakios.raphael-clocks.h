/* Raphael sensor-ID clocks only. SM8150 camcc/gcc and live pinctrl, not
 * POCO addresses. No PLL, HF AXI, PHY, CSID, VFE or front motor changes.
 * Keep parent votes if a child cannot be stopped/collapsed on rollback.
 */
extern bool hakios_raphael_camera_clock_votes(bool restore);

struct raphael_camera_clocks {
	vm_offset_t cc, gc, pin;
	uint32_t ahb, xo, gdsc, cfg, gpio, branch;
	bool votes, ahb_dirty, xo_dirty, domain_dirty, cfg_dirty, branch_dirty, pin_dirty;
};

static bool
raphael_clock_wait(vm_offset_t address, uint32_t mask, uint32_t expected)
{
	for (unsigned i = 0; i < 1000; i++) {
		if ((rd(address) & mask) == expected) return true;
		spin_us(10);
	}
	say("raphael-camera: clock timeout at %#llx raw=%#x mask=%#x expected=%#x\n",
	    (unsigned long long)address, rd(address), mask, expected);
	return false;
}

static bool
raphael_clock_bits(vm_offset_t address, uint32_t mask, uint32_t value)
{
	wr(address, (rd(address) & ~mask) | (value & mask));
	uint32_t actual = rd(address);
	bool ok = (actual & mask) == (value & mask);
	if (!ok) say("raphael-camera: clock readback %#llx got=%#x mask=%#x expected=%#x\n",
	    (unsigned long long)address, actual, mask, value);
	return ok;
}

static bool
raphael_camera_clocks_begin(struct raphael_camera_clocks *c)
{
	c->cc = raphael_camera_map(0x0ad00000, 0x10000);
	c->gc = raphael_camera_map(0x00100000, 0x10000);
	c->pin = raphael_camera_map(0x0390d000, 0x1000); /* GPIO13 NORTH */
	if (!c->cc || !c->gc || !c->pin) return false;
	c->ahb = rd(c->gc + 0xb008); c->xo = rd(c->gc + 0xb044);
	c->gdsc = rd(c->cc + 0xc1bc); c->cfg = rd(c->cc + 0x5008);
	c->branch = rd(c->cc + 0x501c); c->gpio = rd(c->pin);
	/* Require an idle software-controlled domain and unowned MCLK/pin.
	 * A live owner or pending RCG update must not be commandeered. */
	if ((c->gdsc & 0x80000003u) != 1 || (c->branch & 1) ||
	    (rd(c->cc + 0x5004) & 3) || (c->gpio & 0x23cu) || (rd(c->pin + 4) & 2)) {
		say("raphael-camera: clock snapshot busy gdsc=%#x branch=%#x gpio=%#x\n", c->gdsc, c->branch, c->gpio);
		return false;
	}
	/* Mark before calling: a request may have taken effect even if its
	 * completion/readback failed. End must attempt the saved vote. */
	c->votes = true;
	if (!hakios_raphael_camera_clock_votes(false)) return false;
	c->ahb_dirty = true;
	if (!raphael_clock_bits(c->gc + 0xb008, 1, 1)) return false;
	c->xo_dirty = true;
	if (!raphael_clock_bits(c->gc + 0xb044, 1, 1)) return false;
	/* These GCC branches use BRANCH_HALT_DELAY, not a halt-bit poll. */
	spin_us(10);
	c->domain_dirty = true;
	if ((rd(c->cc + 0xc1bc) & 2) || !raphael_clock_bits(c->cc + 0xc1bc, 1, 0) ||
	    (rd(c->cc + 0xc1bc) & 3)) return false;
	spin_us(1); /* >= eight XO cycles */
	if (!raphael_clock_wait(c->cc + 0xc1bc, 0x80000000u, 0x80000000u)) return false;
	spin_us(2); /* staggered memory powerup */
	c->cfg_dirty = true;
	/* source BI_TCXO=0, HID=2*1-1, M/N bypass: 19.2 MHz. */
	if (!raphael_clock_bits(c->cc + 0x5008, 0x371f, 1)) return false;
	wr(c->cc + 0x5004, rd(c->cc + 0x5004) | 1);
	if (!raphael_clock_wait(c->cc + 0x5004, 1, 0)) return false;
	c->branch_dirty = true;
	if (!raphael_clock_bits(c->cc + 0x501c, 1, 1) ||
	    !raphael_clock_wait(c->cc + 0x501c, 0x80000000u, 0)) return false;
	c->pin_dirty = true;
	/* live DT: cam_mclk function1, no pull, 2mA, output-enable clear. */
	if (!raphael_clock_bits(c->pin, 0x3ff, 4)) return false;
	say("raphael-camera: Titan powered, MCLK0 configured XO19.2MHz GPIO13 2mA\n");
	return true;
}

static bool
raphael_camera_clocks_end(struct raphael_camera_clocks *c)
{
	bool ok = true;
	if (c->pin_dirty) {
		if (!raphael_clock_bits(c->pin, 0x3ff, c->gpio)) ok = false;
		else c->pin_dirty = false;
	}
	if (c->branch_dirty) {
		if (!raphael_clock_bits(c->cc + 0x501c, 1, c->branch) ||
		    !raphael_clock_wait(c->cc + 0x501c, 0x80000000u, 0x80000000u)) return false;
		c->branch_dirty = false;
	}
	if (c->cfg_dirty) {
		/* Do not rewrite a command while a previous update is pending. */
		if (!raphael_clock_wait(c->cc + 0x5004, 1, 0) ||
		    !raphael_clock_bits(c->cc + 0x5008, 0x371f, c->cfg)) return false;
		wr(c->cc + 0x5004, rd(c->cc + 0x5004) | 1);
		if (!raphael_clock_wait(c->cc + 0x5004, 1, 0)) return false;
		c->cfg_dirty = false;
	}
	if (c->domain_dirty) {
		if ((rd(c->cc + 0xc1bc) & 2) || !raphael_clock_bits(c->cc + 0xc1bc, 1, c->gdsc) ||
		    (rd(c->cc + 0xc1bc) & 3) != 1) return false;
		spin_us(1);
		if (!raphael_clock_wait(c->cc + 0xc1bc, 0x80000000u, 0)) return false;
		c->domain_dirty = false;
	}
	if (c->xo_dirty) {
		if (!raphael_clock_bits(c->gc + 0xb044, 1, c->xo)) ok = false;
		else c->xo_dirty = false;
	}
	if (c->ahb_dirty) {
		if (!raphael_clock_bits(c->gc + 0xb008, 1, c->ahb)) ok = false;
		else c->ahb_dirty = false;
	}
	spin_us(10); /* GCC BRANCH_HALT_DELAY on disable as well as enable. */
	if (c->votes) {
		if (!hakios_raphael_camera_clock_votes(true)) ok = false;
		else c->votes = false;
	}
	return ok;
}
