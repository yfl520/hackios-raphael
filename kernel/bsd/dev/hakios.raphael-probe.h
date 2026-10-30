/* Bounded rear-sensor supply test. cam=3 leaves MCLK/reset unreleased.
 * No front motor or sensor register writes. All pin changes are restored.
 */
extern bool hakios_raphael_camera_power_begin(void);
extern bool hakios_raphael_camera_power_step(unsigned rail, bool restore);
extern bool hakios_raphael_camera_power_end(void);
extern void hakios_raphael_camera_clock_inspect(void);

struct raphael_probe_pin {
	vm_offset_t base;
	uint32_t cfg, io;
};

static bool
raphael_probe_gpio(struct raphael_probe_pin *p, bool high)
{
	wr(p->base + 4, (p->io & ~2u) | (high ? 2u : 0));
	wr(p->base, (p->cfg & ~0x3ffu) | 0x200u);
	spin_us(5);
	return (rd(p->base) & 0x3ffu) == 0x200u && ((rd(p->base + 4) >> 1) & 1) == high;
}

static void
raphael_camera_supply_probe(void)
{
	struct raphael_probe_pin pins[3] = { 0 };
	const uint32_t addresses[] = { 0x0351c000, 0x0395a000, 0x03961000 };
	bool gpio_dirty = false, ok = true, clean = true;
	say("raphael-camera: bounded rear supply probe; reset held low, MCLK remains off\n");
	for (unsigned i = 0; i < 3; i++) {
		pins[i].base = raphael_camera_map(addresses[i], 0x1000);
		if (!pins[i].base) return;
		pins[i].cfg = rd(pins[i].base);
		pins[i].io = rd(pins[i].base + 4);
		/* Do not take over a pin with a live function or active output. */
		if ((pins[i].cfg & 0x23cu) || (pins[i].io & 2u)) {
			say("raphael-camera: rear pin %#x already in use; supplies untouched\n", addresses[i]);
			return;
		}
	}
	vm_offset_t cc = raphael_camera_map(0x0ad00000, 0x10000);
	vm_offset_t gc = raphael_camera_map(0x00100000, 0x10000);
	if (!cc || !gc || (rd(cc + 0x501c) & 1)) return;
	say("raphael-camera: GCC camera AHB=%#x XO=%#x HF=%#x\n", rd(gc + 0xb008), rd(gc + 0xb044), rd(gc + 0xb030));
	hakios_raphael_camera_clock_inspect();
	if (!hakios_raphael_camera_power_begin()) {
		say("raphael-camera: supply snapshot rejected; no power changes\n");
		return;
	}
	gpio_dirty = true;
	for (unsigned i = 0; i < 3 && ok; i++) ok = raphael_probe_gpio(&pins[i], false);
	if (ok) ok = hakios_raphael_camera_power_step(0, false);
	if (ok) spin_us(5000);
	if (ok) ok = hakios_raphael_camera_power_step(1, false);
	if (ok) ok = hakios_raphael_camera_power_step(2, false);
	if (ok) ok = hakios_raphael_camera_power_step(3, false);
	if (ok) ok = raphael_probe_gpio(&pins[1], true);
	if (ok) ok = raphael_probe_gpio(&pins[2], true);
	if (ok) {
		spin_us(135);
		say("raphael-camera: rear supplies verified; reset is low, no ID read attempted\n");
	}
	/* Follow the board's shutdown order even after a partial operation. */
	if (gpio_dirty) {
		if (!raphael_probe_gpio(&pins[0], false)) clean = false;
		spin_us(2000);
		if (!raphael_probe_gpio(&pins[2], false)) clean = false;
		if (!raphael_probe_gpio(&pins[1], false)) clean = false;
	}
	for (unsigned rail = 4; rail--;) {
		if (rail == 0) spin_us(20000);
		if (!hakios_raphael_camera_power_step(rail, true)) clean = false;
	}
	if (!hakios_raphael_camera_power_end()) clean = false;
	if (gpio_dirty) for (unsigned i = 0; i < 3; i++) {
		wr(pins[i].base + 4, pins[i].io & 2u);
		wr(pins[i].base, pins[i].cfg);
		if (rd(pins[i].base) != pins[i].cfg || (rd(pins[i].base + 4) & 2u) != (pins[i].io & 2u)) clean = false;
	}
	say("raphael-camera: supply probe %s; restore %s\n", ok ? "completed" : "FAILED", clean ? "verified" : "FAILED; reboot required");
}
