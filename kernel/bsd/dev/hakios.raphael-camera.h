/* Raphael rear-camera platform inspection. No register writes or motor action.
 * Wiring: saved live-device.dtb camera0/CCI0/clock_camcc/titan_top_gdsc.
 * GPIO tiles: LineageOS/android_kernel_xiaomi_sm8150,
 * 59220a048d04516e7fe1dfc094507930166a1e60, pinctrl-sm8150.c.
 * Clock offsets: same commit, camcc-sm8150.c (MCLK0 19.2 MHz from XO).
 * This is a diagnostic stage, not a sensor-powered or image-capture driver.
 */
extern bool hakios_raphael_camera_rpmh_read(const char *name, unsigned field, uint32_t *value);

static void
raphael_camera_inspect(void)
{
	static const struct {
		unsigned gpio;
		uint32_t physical;
		const char *role;
	} pins[] = {
		{ 13, 0x0390d000, "rear MCLK0" },
		{ 17, 0x03911000, "CCI0 SDA" },
		{ 18, 0x03912000, "CCI0 SCL" },
		{ 28, 0x0351c000, "rear reset" },
		{ 90, 0x0395a000, "rear VDIG enable" },
		{ 97, 0x03961000, "camera VIO enable" },
	};
	say("raphael-camera: read-only rear platform inspection; front motor untouched\n");
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		vm_offset_t p = raphael_camera_map(pins[i].physical, 0x1000);
		if (!p) {
			say("raphael-camera: gpio%u map failed\n", pins[i].gpio);
			return;
		}
		uint32_t cfg = rd(p), io = rd(p + 4);
		say("raphael-camera: gpio%u %s at %#x cfg=%#x io=%#x function=%u output=%u level=%u\n",
		    pins[i].gpio, pins[i].role, pins[i].physical, cfg, io, (cfg >> 2) & 15, (cfg >> 9) & 1, io & 1);
	}
	vm_offset_t cc = raphael_camera_map(0x0ad00000, 0x10000);
	if (!cc) {
		say("raphael-camera: CAMCC map failed\n");
		return;
	}
	say("raphael-camera: CAMCC titan_top=%#x MCLK0 cmd=%#x cfg=%#x branch=%#x\n",
	    rd(cc + 0xc1bc), rd(cc + 0x5004), rd(cc + 0x5008), rd(cc + 0x501c));
	say("raphael-camera: inspection complete; sensor ID and frames not tested\n");
}

/* cam=2 adds ePCB READs of the camera's VRM resources. Writing the TCS command
 * mailbox is necessary to read RPMh; its WRITE flag remains clear. */
static void
raphael_camera_power_inspect(void)
{
	static const char *const names[] = { "ldoa17", "bobc1", "smpa5", "smpc8" };
	for (unsigned i = 0; i < 4; i++) {
		uint32_t values[3] = { 0 };
		bool ok = true;
		for (unsigned field = 0; field < 3 && ok; field++)
			ok = hakios_raphael_camera_rpmh_read(names[i], field, &values[field]);
		if (!ok) {
			say("raphael-camera: %s RPMh read failed; no power vote made\n", names[i]);
			return;
		}
		say("raphael-camera: %s RPMh read voltage=%#x enable=%#x mode=%#x; interpretation unverified\n",
		    names[i], values[0], values[1], values[2]);
	}
	say("raphael-camera: RPMh read-only stage complete; sensor remains unpowered by this driver\n");
}
