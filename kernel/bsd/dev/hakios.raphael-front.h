/* Raphael S5K3T2 front sensor: verified live DT and vendor module power/ID.
 * First bring-up: bounded ID only; no motor or CSI writes in cam=11.
 */
struct raphael_front_clocks {
	vm_offset_t cc, gc, pin;
	uint32_t ahb, xo, gdsc, cfg, gpio, branch;
	bool votes, ahb_dirty, xo_dirty, domain_dirty, cfg_dirty, branch_dirty, pin_dirty;
};

static bool
raphael_front_clocks_begin(struct raphael_front_clocks *c)
{
	c->cc = raphael_camera_map(0x0ad00000, 0x10000);
	c->gc = raphael_camera_map(0x00100000, 0x10000);
	c->pin = raphael_camera_map(0x0390f000, 0x1000); /* GPIO15 NORTH */
	if (!c->cc || !c->gc || !c->pin) return false;
	c->ahb = rd(c->gc + 0xb008); c->xo = rd(c->gc + 0xb044);
	c->gdsc = rd(c->cc + 0xc1bc); c->cfg = rd(c->cc + 0x5048);
	c->branch = rd(c->cc + 0x505c); c->gpio = rd(c->pin);
	/* Require an idle software-controlled domain and unowned MCLK/pin.
	 * A live owner or pending RCG update must not be commandeered. */
	if ((c->gdsc & 0x80000003u) != 1 || (c->branch & 1) ||
	    (rd(c->cc + 0x5044) & 3) || (c->gpio & 0x23cu) || (rd(c->pin + 4) & 2)) {
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
	if (!raphael_clock_bits(c->cc + 0x5048, 0x371f, 1)) return false;
	wr(c->cc + 0x5044, rd(c->cc + 0x5044) | 1);
	if (!raphael_clock_wait(c->cc + 0x5044, 1, 0)) return false;
	c->branch_dirty = true;
	if (!raphael_clock_bits(c->cc + 0x505c, 1, 1) ||
	    !raphael_clock_wait(c->cc + 0x505c, 0x80000000u, 0)) return false;
	c->pin_dirty = true;
	/* live DT: cam_mclk function1, no pull, 2mA, output-enable clear. */
	if (!raphael_clock_bits(c->pin, 0x3ff, 4)) return false;
	say("raphael-camera: Titan powered, MCLK2 configured XO19.2MHz GPIO15 2mA\n");
	return true;
}

static bool
raphael_front_clocks_end(struct raphael_front_clocks *c)
{
	bool ok = true;
	if (c->pin_dirty) {
		if (!raphael_clock_bits(c->pin, 0x3ff, c->gpio)) ok = false;
		else c->pin_dirty = false;
	}
	if (c->branch_dirty) {
		if (!raphael_clock_bits(c->cc + 0x505c, 1, c->branch) ||
		    !raphael_clock_wait(c->cc + 0x505c, 0x80000000u, 0x80000000u)) return false;
		c->branch_dirty = false;
	}
	if (c->cfg_dirty) {
		/* Do not rewrite a command while a previous update is pending. */
		if (!raphael_clock_wait(c->cc + 0x5044, 1, 0) ||
		    !raphael_clock_bits(c->cc + 0x5048, 0x371f, c->cfg)) return false;
		wr(c->cc + 0x5044, rd(c->cc + 0x5044) | 1);
		if (!raphael_clock_wait(c->cc + 0x5044, 1, 0)) return false;
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

static bool
raphael_camera_front_probe(bool (*operation)(struct raphael_i2c_bus *, void *), void *context,
    bool *retain_power)
{
	struct raphael_probe_pin pins[5] = { 0 };
	const uint32_t addresses[] = { 0x0390c000, 0x0395f000, 0x03961000, 0x03911000, 0x03912000 };
	struct raphael_front_clocks clocks = { 0 };
	struct raphael_i2c_bus bus = { 0 };
	bool ok = true, clean = true;
	uint8_t id[2] = { 0 }, where[2] = { 0, 0 };
	say("raphael-camera: bounded front S5K3T2 ID probe; no capture or front motor\n");
	for (unsigned i = 0; i < 5; i++) {
		pins[i].base = raphael_camera_map(addresses[i], 0x1000);
		if (!pins[i].base) return false;
		pins[i].cfg = rd(pins[i].base); pins[i].io = rd(pins[i].base + 4);
		if ((pins[i].cfg & 0x23cu) || (pins[i].io & 2u)) {
			say("raphael-camera: ID pin %#x busy; no supply changes\n", addresses[i]);
			return false;
		}
	}
	bus.sda = pins[3].base; bus.scl = pins[4].base;
	hakios_raphael_camera_clock_inspect();
	if (!hakios_raphael_camera_power_begin()) return false;
	for (unsigned i = 0; i < 3 && ok; i++) ok = raphael_probe_gpio(&pins[i], false);
	if (ok) ok = hakios_raphael_camera_power_step(0, false);
	if (ok) spin_us(5000);
	if (ok) ok = hakios_raphael_camera_power_step(1, false);
	if (ok) ok = raphael_probe_gpio(&pins[2], true);
	if (ok) spin_us(135);
	if (ok) ok = hakios_raphael_camera_power_step(3, false);
	if (ok) ok = raphael_probe_gpio(&pins[1], true);
	if (ok) { spin_us(135); ok = raphael_front_clocks_begin(&clocks); }
	if (ok) { spin_us(10000); ok = raphael_probe_gpio(&pins[0], true); }
	if (ok) {
		spin_us(2000);
		enum raphael_i2c_result result = RAPHAEL_I2C_NACK;
		unsigned attempt;
		for (attempt = 0; attempt < 20; attempt++) {
			result = raphael_i2c_transfer(&bus, 0x56, where, 2, id, 2);
			if (result != RAPHAEL_I2C_NACK) break;
			spin_us(2000);
		}
		ok = result == RAPHAEL_I2C_OK && id[0] == 0x31 && id[1] == 0x42;
		say("raphael-camera: S5K3T2 real ID result=%u bytes=%02x%02x attempts=%u %s\n",
		    result, id[0], id[1], attempt < 20 ? attempt + 1 : 20, ok ? "MATCH" : "FAILED");
	}
	if (ok && operation) ok = operation(&bus, context);
	if (retain_power && *retain_power) {
		(void)raphael_probe_gpio(&pins[0], false);
		spin_us(2000); /* hold the sensor reset even if stream-off failed */
		say("raphael-camera: capture cleanup FAILED; keep parent clocks/power; reboot required\n");
		return false;
	}
	/* Sensor shutdown order from this phone's module, including failures. */
	if (!raphael_probe_gpio(&pins[0], false)) clean = false;
	spin_us(1000);
	if (!raphael_front_clocks_end(&clocks)) clean = false;
	spin_us(1000);
	if (!raphael_probe_gpio(&pins[1], false)) clean = false;
	if (!hakios_raphael_camera_power_step(3, true)) clean = false;
	if (!raphael_probe_gpio(&pins[2], false)) clean = false;
	if (!hakios_raphael_camera_power_step(1, true)) clean = false;
	spin_us(20000);
	if (!hakios_raphael_camera_power_step(0, true)) clean = false;
	if (!hakios_raphael_camera_power_end()) clean = false;
	for (unsigned i = 0; i < 5; i++) {
		if (i < 3) {
			wr(pins[i].base + 4, pins[i].io & 2u);
			wr(pins[i].base, pins[i].cfg);
		}
		if (rd(pins[i].base) != pins[i].cfg ||
		    (rd(pins[i].base + 4) & 2u) != (pins[i].io & 2u)) clean = false;
	}
	say("raphael-camera: front ID probe %s; restore %s\n", ok ? "MATCHED" : "FAILED",
	    clean ? "verified" : "FAILED; reboot required");
	return ok && clean;
}

