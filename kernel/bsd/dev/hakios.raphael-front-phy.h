/* Raphael PHY1 v1.1, rear IMX586 mode1, four-lane D-PHY only.
 * Register sequence follows pinned SM8150 cam_csiphy_1_1_hwreg.h/core.c.
 * Caller owns the idle PHY and keeps capture clocks/domain on until end
 * succeeds; sensor must be stopped before end. No IRQ handler is installed:
 * mask IRQ delivery and let a future capture session poll status registers.
 * cam7 currently exercises stopped-sensor configuration only.
 */
#define RAPHAEL_FRONT_PHY_REGS 77
struct raphael_front_phy {
	vm_offset_t base;
	uint16_t offset[RAPHAEL_FRONT_PHY_REGS];
	uint32_t saved[RAPHAEL_FRONT_PHY_REGS];
	unsigned count;
	bool dirty, ready;
};

static bool
raphael_front_phy_write(struct raphael_front_phy *p, unsigned offset, uint32_t value)
{
	wr(p->base + offset, value);
	uint32_t actual = rd(p->base + offset);
	if (actual != value)
		say("raphael-camera: PHY1 offset=%#x got=%#x expected=%#x\n", offset, actual, value);
	return actual == value;
}

static bool
raphael_front_phy_irq_drain(struct raphael_front_phy *p)
{
	uint32_t pending[11];
	bool any = false;
	for (unsigned i = 0; i < 11; i++) {
		pending[i] = rd(p->base + 0x8b0 + 4 * i);
		if (pending[i] & ~0xffu) return false;
		any |= pending[i] != 0;
	}
	if (!any) return true;
	/* Match mainline csiphy_isr(): keep sampled clear bits asserted
	 * across the global command pulse, then clear strobes. Never enable IRQ delivery,
	 * write a status register, or put W1C state in a restore snapshot. */
	for (unsigned i = 0; i < 11; i++) {
		wr(p->base + 0x858 + 4 * i, pending[i]);
		__sync_synchronize();
	}
	wr(p->base + 0x828, 1); __sync_synchronize();
	wr(p->base + 0x828, 0); __sync_synchronize();
	for (unsigned i = 0; i < 11; i++) {
		wr(p->base + 0x858 + 4 * i, 0);
		__sync_synchronize();
	}
	for (unsigned attempt = 0; attempt < 100; attempt++) {
		any = false;
		for (unsigned i = 0; i < 11; i++) any |= rd(p->base + 0x8b0 + 4 * i) != 0;
		if (!any) {
			say("raphael-camera: PHY1 masked residual IRQ status cleared\n");
			return true;
		}
		spin_us(10);
	}
	say("raphael-camera: PHY1 IRQ clear timed out\n");
	return false;
}

static bool
raphael_front_phy_begin(struct raphael_front_phy *p)
{
	if (!p || p->dirty || p->ready) return false;
	p->base = raphael_camera_map(0x0ac66000, 0x1000);
	if (!p->base) return false;
	/* Do not commandeer enabled lanes, reset or IRQ delivery. Masked
	 * residual status on an idle PHY is acknowledged below. */
	if (rd(p->base + 0x814) || rd(p->base + 0x818) ||
	    rd(p->base + 0x81c) || rd(p->base + 0x800)) {
		say("raphael-camera: PHY1 busy common=%#x/%#x/%#x reset=%#x\n",
		    rd(p->base + 0x814), rd(p->base + 0x818),
		    rd(p->base + 0x81c), rd(p->base + 0x800));
		return false;
	}
	for (unsigned i = 0; i < 11; i++)
		if ((rd(p->base + 0x8b0 + 4 * i) & ~0xffu) ||
		    rd(p->base + 0x82c + 4 * i)) {
			say("raphael-camera: PHY1 IRQ%u busy status=%#x mask=%#x\n",
			    i, rd(p->base + 0x8b0 + 4 * i), rd(p->base + 0x82c + 4 * i));
			return false;
		}
	static const uint16_t lanes[] = { 0, 0x700, 0x200, 0x400, 0x600 };
	static const uint16_t regs[] = { 4, 0x2c, 0x34, 0x1c, 0x14, 0x28, 0x3c,
	    0, 8, 0x0c, 0x10, 0x38, 0x60, 0x64 };
	static const uint8_t values[] = { 0x0c, 1, 0x0f, 0x0a, 0x60, 4, 0xb8,
	    0x90, 19, 0xa5, 0x52, 0xfe, 0, 0x7f };
	p->count = 0;
	for (unsigned lane = 0; lane < 5; lane++) {
		for (unsigned j = 0; j < 14; j++) {
			if (lane != 1 && (regs[j] == 0x28 || regs[j] == 0x0c)) continue;
			p->offset[p->count++] = lanes[lane] + regs[j];
		}
	}
	for (unsigned i = 0; i < 11; i++) p->offset[p->count++] = 0x82c + 4 * i;
	p->offset[p->count++] = 0x814;
	p->offset[p->count++] = 0x818;
	p->offset[p->count++] = 0x81c;
	p->offset[p->count++] = 0x800;
	if (p->count != RAPHAEL_FRONT_PHY_REGS) return false;
	for (unsigned i = 0; i < p->count; i++)
		p->saved[i] = rd(p->base + p->offset[i]);
	/* Mark before any mutation, including a write with failed readback. */
	p->dirty = true;
	for (unsigned i = 0; i < 11; i++)
		if (!raphael_front_phy_write(p, 0x82c + 4 * i, 0)) return false;
	if (!raphael_front_phy_write(p, 0x814, 0xd5) ||
	    !raphael_front_phy_write(p, 0x818, 1) ||
	    !raphael_front_phy_write(p, 0x81c, 2) ||
	    !raphael_front_phy_write(p, 0x800, 1)) return false;
	spin_us(1000);
	if (!raphael_front_phy_write(p, 0x800, 0)) return false;
	for (unsigned lane = 0; lane < 5; lane++) {
		for (unsigned j = 0; j < 14; j++) {
			if (lane != 1 && (regs[j] == 0x28 || regs[j] == 0x0c)) continue;
			uint32_t value = values[j];
			if (lane == 1 && regs[j] == 0) value = 0x80;
			if (lane == 1 && regs[j] == 0x38) value = 0x1f;
			if (!raphael_front_phy_write(p, lanes[lane] + regs[j], value)) return false;
		}
	}
	/* Status is sampled only after COMMON_PWRDN_B=1 and lane setup.
	 * Requiring it to clear while the analog common block is powered down
	 * is not a valid ownership/quiescence check. IRQ masks stay zero. */
	if (!raphael_front_phy_irq_drain(p)) return false;
	p->ready = true;
	return true;
}

static bool
raphael_front_phy_end(struct raphael_front_phy *p)
{
	if (!p) return false;
	if (!p->dirty) return true;
	p->ready = false;
	/* Stop lane reception before touching saved configuration. If any stop
	 * write fails, retain dirty and require the caller to keep clocks on. */
	if (!raphael_front_phy_write(p, 0x814, 0)) return false;
	spin_us(5000);
	if (!raphael_front_phy_write(p, 0x818, 0) ||
	    !raphael_front_phy_write(p, 0x81c, 0) ||
	    !raphael_front_phy_write(p, 0x800, 1)) return false;
	spin_us(1000);
	if (!raphael_front_phy_write(p, 0x800, 0)) return false;
	/* Common power is now down; status need not be meaningful. Quiescence
	 * is established by masked IRQs and verified lane/reset/common controls,
	 * matching the upstream shutdown reset sequence, not raw IRQ=0. */
	/* Restore only readable configuration, never W1C/IRQ status or command.
	 * Common controls were required to be zero at begin. */
	for (unsigned i = 0; i < p->count; i++)
		if (!raphael_front_phy_write(p, p->offset[i], p->saved[i])) return false;
	p->dirty = false;
	return true;
}
