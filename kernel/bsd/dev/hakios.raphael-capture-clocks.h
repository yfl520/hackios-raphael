/* SM8150-v2 capture clocks; caller holds the Titan/MCLK lifecycle above.
 * No streaming or DMA. All clocks must be unowned. A failed stop/restore
 * keeps dirty state and its PLL; caller must keep Titan and votes alive.
 */
extern bool hakios_raphael_camera_capture_ready(void);
static const uint16_t raphael_capture_pll_regs[] = {
	4, 8, 0x40, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x0c, 0x10, 0x14
};
static const uint32_t raphael_capture_pll_values[] = {
	0x3e, 0x44, 0x8000, 0x20485699, 0x2267, 0x24, 0, 0, 0x20, 0x3100, 0x805, 0xd0
};
static const struct { uint16_t cmd, cfg; } raphael_capture_roots[] = {
	{ 0x7058, 0x20e }, /* slow AHB 600/7.5=80 */
	{ 0xc170, 0x302 }, /* CAMNOC 400/1.5 */
	{ 0xa064, 0x301 }, /* CPHY RX 400 */
	{ 0x6004, 0x203 }, /* PHY0 timer 600/2 */
	{ 0xc020, 0x301 }, /* CSID-lite0 400 */
	{ 0xc004, 0x301 }, /* IFE-lite0 400 */
};
static const uint16_t raphael_capture_branches[] = {
	/* live CPAS/PHY0/lite0 DT clocks; CORE_AHB c1c8 is not a consumer
	 * dependency of this path and must not be programmed speculatively. */
	0xc168, 0xc18c, 0x601c, 0x6020, 0xc038, 0xc040, 0xc01c
};
#define RAPHAEL_CAPTURE_BRANCHES 7
struct raphael_capture_clocks {
	vm_offset_t cc;
	uint32_t pll[12], mode, opmode, cfg[6], branch[RAPHAEL_CAPTURE_BRANCHES];
	bool pll_dirty, cfg_dirty[6], branch_dirty[RAPHAEL_CAPTURE_BRANCHES], ready;
};

static void
raphael_capture_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__sync_synchronize(); /* host fault-injection harness */
#endif
}

static bool
raphael_capture_branch(struct raphael_capture_clocks *c, unsigned i, uint32_t value)
{
	vm_offset_t reg = c->cc + raphael_capture_branches[i];
	wr(reg, (rd(reg) & ~1u) | (value & 1));
	/* clk_branch_toggle barriers before the delay/poll. Do not reject
	 * an unpropagated write by reading immediately before that delay. */
	raphael_capture_barrier();
	if (!raphael_clock_wait(reg, 1, value & 1)) return false;
	return raphael_clock_wait(reg, 0x80000000u, value & 1 ? 0 : 0x80000000u);
}

static bool
raphael_capture_cfg(struct raphael_capture_clocks *c, unsigned i, uint32_t value)
{
	vm_offset_t cmd = c->cc + raphael_capture_roots[i].cmd;
	if (!raphael_clock_wait(cmd, 1, 0) ||
	    !raphael_clock_bits(cmd + 4, 0x371f, value)) return false;
	wr(cmd, rd(cmd) | 1);
	return raphael_clock_wait(cmd, 1, 0);
}

static bool
raphael_capture_clocks_begin(struct raphael_capture_clocks *c)
{
	if (!c || c->ready || c->pll_dirty || !hakios_raphael_camera_capture_ready()) return false;
	for (unsigned i = 0; i < 6; i++) if (c->cfg_dirty[i]) return false;
	for (unsigned i = 0; i < RAPHAEL_CAPTURE_BRANCHES; i++) if (c->branch_dirty[i]) return false;
	c->cc = raphael_camera_map(0x0ad00000, 0x10000);
	if (!c->cc) return false;
	if ((rd(c->cc + 0xc1bc) & 0x80000003u) != 0x80000000u) {
		say("raphael-camera: capture Titan prerequisite raw=%#x\n", rd(c->cc + 0xc1bc));
		return false;
	}
	c->mode = rd(c->cc); c->opmode = rd(c->cc + 0x38);
	/* Reject output, FSM control, update/ACK, active/locked or running PLL. */
	/* USER_CTL output selection may retain reset defaults while global
	 * output and OPMODE are off; upstream trion_pll_is_enabled() tests
	 * MODE/OPMODE, not USER_CTL. Preserve that inactive configuration. */
	if ((c->mode & 0xe0500001u) || c->opmode) {
		say("raphael-camera: capture PLL0 busy mode=%#x opmode=%#x\n", c->mode, c->opmode);
		return false;
	}
	for (unsigned i = 0; i < 12; i++) c->pll[i] = rd(c->cc + raphael_capture_pll_regs[i]);
	for (unsigned i = 0; i < 6; i++) {
		vm_offset_t cmd = c->cc + raphael_capture_roots[i].cmd;
		if (rd(cmd) & 3) {
			say("raphael-camera: capture root %#x busy cmd=%#x\n",
			    raphael_capture_roots[i].cmd, rd(cmd));
			return false;
		}
		c->cfg[i] = rd(cmd + 4);
	}
	for (unsigned i = 0; i < RAPHAEL_CAPTURE_BRANCHES; i++) {
		c->branch[i] = rd(c->cc + raphael_capture_branches[i]);
		if (c->branch[i] & 1) {
			say("raphael-camera: capture branch %#x busy value=%#x\n",
			    raphael_capture_branches[i], c->branch[i]);
			return false;
		}
	}
	c->pll_dirty = true;
	for (unsigned i = 0; i < 12; i++) {
		vm_offset_t reg = c->cc + raphael_capture_pll_regs[i];
		wr(reg, raphael_capture_pll_values[i]);
		if (rd(reg) != raphael_capture_pll_values[i]) {
			say("raphael-camera: PLL0 offset=%#x got=%#x expected=%#x\n",
			    raphael_capture_pll_regs[i], rd(reg), raphael_capture_pll_values[i]);
			return false;
		}
	}
	/* TRION v2 configure: bypass HW update, standby, release reset. */
	if (!raphael_clock_bits(c->cc, 0x800001u, 0x800000u)) return false;
	wr(c->cc + 0x38, 0);
	if (rd(c->cc + 0x38) || !raphael_clock_bits(c->cc, 4, 4)) return false;
	wr(c->cc + 0x38, 1);
	if (rd(c->cc + 0x38) != 1 ||
	    !raphael_clock_wait(c->cc, 0x80000000u, 0x80000000u) ||
	    !raphael_clock_wait(c->cc + 0x30, 0x04000000u, 0x04000000u)) return false;
	if (!raphael_clock_bits(c->cc + 0x0c, 7, 7) ||
	    !raphael_clock_bits(c->cc, 1, 1)) return false;
	for (unsigned i = 0; i < 6; i++) {
		c->cfg_dirty[i] = true;
		if (!raphael_capture_cfg(c, i, raphael_capture_roots[i].cfg)) return false;
	}
	for (unsigned i = 0; i < RAPHAEL_CAPTURE_BRANCHES; i++) {
		c->branch_dirty[i] = true;
		if (!raphael_capture_branch(c, i, 1)) {
			say("raphael-camera: capture branch %#x failed cfg=%#x slowcmd=%#x slowcfg=%#x PLL=%#x/%#x GDSC=%#x gdscclk=%#x\n",
			    raphael_capture_branches[i], rd(c->cc + raphael_capture_branches[i]),
			    rd(c->cc + 0x7058), rd(c->cc + 0x705c), rd(c->cc),
			    rd(c->cc + 0x0c), rd(c->cc + 0xc1bc), rd(c->cc + 0xc1e4));
			return false;
		}
	}
	c->ready = true;
	say("raphael-camera: capture PLL0 locked/calibrated; PHY0/lite0 clocks enabled\n");
	return true;
}

static bool
raphael_capture_clocks_end(struct raphael_capture_clocks *c)
{
	if (!c) return false;
	c->ready = false;
	for (unsigned i = RAPHAEL_CAPTURE_BRANCHES; i--;) if (c->branch_dirty[i]) {
		if (!raphael_capture_branch(c, i, c->branch[i])) return false;
		c->branch_dirty[i] = false;
	}
	for (unsigned i = 6; i--;) if (c->cfg_dirty[i]) {
		if (!raphael_capture_cfg(c, i, c->cfg[i])) return false;
		c->cfg_dirty[i] = false;
	}
	if (c->pll_dirty) {
		if (!raphael_clock_bits(c->cc, 1, 0) ||
		    !raphael_clock_bits(c->cc + 0x0c, 7, 0)) return false;
		wr(c->cc + 0x38, 0);
		if (rd(c->cc + 0x38) || !raphael_clock_wait(c->cc, 0x80000000u, 0)) return false;
		/* Stop/reset before restoring configuration, then restore the saved
		 * control bits. LOCK/ACTIVE/PCAL_DONE are hardware-owned state. */
		if (!raphael_clock_bits(c->cc, 4, 0)) return false;
		for (unsigned i = 0; i < 12; i++) {
			vm_offset_t reg = c->cc + raphael_capture_pll_regs[i];
			wr(reg, c->pll[i]);
			if (rd(reg) != c->pll[i]) return false;
		}
		if (!raphael_clock_bits(c->cc, 0x800005u, c->mode)) return false;
		c->pll_dirty = false;
	}
	return true;
}
