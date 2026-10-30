/* cam=4: one bounded rear IMX586 identification, then shutdown/restore.
 * Uses the supply/pin helpers above. No streaming, capture or lens/motor.
 */
static bool
raphael_camera_rear_probe(bool (*operation)(struct raphael_i2c_bus *, void *), void *context,
    bool *retain_power)
{
	struct raphael_probe_pin pins[5] = { 0 };
	const uint32_t addresses[] = { 0x0351c000, 0x0395a000, 0x03961000, 0x03911000, 0x03912000 };
	struct raphael_camera_clocks clocks = { 0 };
	struct raphael_i2c_bus bus = { 0 };
	bool ok = true, clean = true;
	uint8_t id[2] = { 0 }, where[2] = { 0, 0x16 };
	say("raphael-camera: bounded rear IMX586 ID probe; no capture or front motor\n");
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
	if (ok) ok = hakios_raphael_camera_power_step(2, false);
	if (ok) ok = hakios_raphael_camera_power_step(3, false);
	if (ok) ok = raphael_probe_gpio(&pins[1], true);
	if (ok) ok = raphael_probe_gpio(&pins[2], true);
	if (ok) { spin_us(135); ok = raphael_camera_clocks_begin(&clocks); }
	if (ok) { spin_us(10000); ok = raphael_probe_gpio(&pins[0], true); }
	if (ok) {
		spin_us(2000);
		enum raphael_i2c_result result = RAPHAEL_I2C_NACK;
		unsigned attempt;
		for (attempt = 0; attempt < 20; attempt++) {
			result = raphael_i2c_transfer(&bus, 0x1a, where, 2, id, 2);
			if (result != RAPHAEL_I2C_NACK) break;
			spin_us(2000);
		}
		ok = result == RAPHAEL_I2C_OK && id[0] == 5 && id[1] == 0x86;
		say("raphael-camera: IMX586 real ID result=%u bytes=%02x%02x attempts=%u %s\n",
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
	spin_us(2000);
	if (!raphael_camera_clocks_end(&clocks)) clean = false;
	if (!raphael_probe_gpio(&pins[2], false)) clean = false;
	if (!raphael_probe_gpio(&pins[1], false)) clean = false;
	for (unsigned rail = 4; rail--;) {
		if (rail == 0) spin_us(20000);
		if (!hakios_raphael_camera_power_step(rail, true)) clean = false;
	}
	if (!hakios_raphael_camera_power_end()) clean = false;
	for (unsigned i = 0; i < 5; i++) {
		if (i < 3) {
			wr(pins[i].base + 4, pins[i].io & 2u);
			wr(pins[i].base, pins[i].cfg);
		}
		if (rd(pins[i].base) != pins[i].cfg ||
		    (rd(pins[i].base + 4) & 2u) != (pins[i].io & 2u)) clean = false;
	}
	say("raphael-camera: rear ID probe %s; restore %s\n", ok ? "MATCHED" : "FAILED",
	    clean ? "verified" : "FAILED; reboot required");
	return ok && clean;
}

static bool
raphael_camera_id_probe(void)
{
	return raphael_camera_rear_probe(NULL, NULL, NULL);
}
