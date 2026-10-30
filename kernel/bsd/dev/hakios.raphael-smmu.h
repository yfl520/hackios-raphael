/* Camera IFE stream group from live DT: 0840/0620 (8 aliases, one SMR).
 * Caller holds hakios_smmu_lock through begin/use/end and publishes its 4K
 * tables before begin. Stop all camera DMA before end. No global SCR0 writes.
 * On failure keep tables/buffers allocated until end verifies restoration
 * or a documented disabled, fault-routed, pointer-free retirement state.
 */
static const uint16_t raphael_smmu_cb_regs[] = {
	0, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38, 0x3c
};
struct raphael_smmu {
	vm_offset_t base, gr1, cb;
	uint32_t match, context, saved_smr, saved_s2cr, saved_cbar, saved_cba2r, saved_cb[9];
	uint16_t cb_dirty;
	bool cbar_dirty, cba2r_dirty, smr_dirty, s2cr_dirty, ready, retired;
};

static void
raphael_smmu_store(vm_offset_t at, uint32_t value)
{
#if defined(__aarch64__)
	/* Qualcomm's mediated SMMU access may mishandle WZR stores. Require
	 * an actual general register even for a compile-time zero. */
	__asm__ volatile ("" : "+r" (value));
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (at) : "memory");
#else
	wr(at, value);
#endif
}

static bool
raphael_smmu_put(vm_offset_t at, uint32_t value)
{
	raphael_smmu_store(at, value);
	__sync_synchronize();
	uint32_t got = rd(at);
	if (got != value) say("raphael-camera: SMMU readback %#llx got=%#x expected=%#x\n",
	    (unsigned long long)at, got, value);
	return got == value;
}

static bool
raphael_smmu_flush(struct raphael_smmu *s)
{
	/* Forget both our ASID and the saved context's old ASID. */
	raphael_smmu_store(s->cb + 0x610, s->context);
	raphael_smmu_store(s->cb + 0x610, s->saved_cb[3] >> 16);
	__sync_synchronize();
	raphael_smmu_store(s->cb + 0x7f0, ~0u); /* downstream QCOM_DUMMY_VAL */
	__sync_synchronize();
	for (unsigned i = 0; i < 1000; i++) {
		if (!(rd(s->cb + 0x7f4) & 1)) return true;
		spin_us(10);
	}
	say("raphael-camera: SMMU context TLB sync timed out\n");
	return false;
}

static bool
raphael_smmu_cb_put(struct raphael_smmu *s, unsigned i, uint32_t value)
{
	s->cb_dirty |= 1u << i;
	if (i == 1) {
		/* Preserve implementation/reserved bits exposed in the active
		 * context format; only alter PASIZE/AS/SEP fields we use. */
		value = (rd(s->cb + 0x10) & ~0x38017u) | (value & 0x38017u);
	}
	return raphael_smmu_put(s->cb + raphael_smmu_cb_regs[i], value);
}

static bool
raphael_smmu_begin(struct raphael_smmu *s, uint64_t root_pa)
{
	if (!s || s->ready || s->cb_dirty || s->cbar_dirty || s->cba2r_dirty ||
	    s->smr_dirty || s->s2cr_dirty || !root_pa || (root_pa & 4095) ||
	    root_pa >= (1ull << 36)) return false;
	s->retired = false;
	s->base = raphael_camera_map(0x15000000, 0x1000);
	if (!s->base) return false;
	uint32_t idr0 = rd(s->base + 0x20), idr1 = rd(s->base + 0x24);
	unsigned page = idr1 >> 31 ? 0x10000 : 0x1000;
	unsigned pages = 2u << ((idr1 >> 28) & 7);
	unsigned matches = idr0 & 255, contexts = idr1 & 255;
	if (!matches || !contexts || contexts > pages ||
	    2ull * page * pages != 0x100000) return false;
	bool used[256] = { false };
	s->match = s->context = ~0u;
	for (unsigned i = 0; i < matches; i++) {
		uint32_t smr = rd(s->base + 0x800 + 4 * i);
		uint32_t s2cr = rd(s->base + 0xc00 + 4 * i);
		if (smr & 0x80000000u) {
			used[s2cr & 255] = true;
			if (!(((smr & 0x7fff) ^ 0x840u) &
			    ~(((smr >> 16) & 0x7fff) | 0x620u))) {
				say("raphael-camera: IFE stream group already owned by SMR%u\n", i);
				return false;
			}
		} else if (s->match == ~0u) s->match = i;
	}
	if (s->match == ~0u) return false;
	s->gr1 = raphael_camera_map(0x15000000 + page, 0x1000);
	if (!s->gr1) return false;
	for (unsigned i = 0; i < contexts; i++) {
		if (used[i]) continue;
		vm_offset_t cb = raphael_camera_map(0x15000000 + (uint64_t)(pages + i) * page, 0x1000);
		if (!cb) return false;
		/* Disabled, unreferenced, no old fault or pending sync. */
		if ((rd(cb) & 1) || (rd(cb + 0x58) & 0xc00001feu) || (rd(cb + 0x7f4) & 1)) continue;
		s->context = i; s->cb = cb; break;
	}
	if (s->context == ~0u) return false;
	s->saved_smr = rd(s->base + 0x800 + 4 * s->match);
	s->saved_s2cr = rd(s->base + 0xc00 + 4 * s->match);
	s->saved_cbar = rd(s->gr1 + 4 * s->context);
	s->saved_cba2r = rd(s->gr1 + 0x800 + 4 * s->context);
	for (unsigned i = 0; i < 9; i++) s->saved_cb[i] = rd(s->cb + raphael_smmu_cb_regs[i]);
	say("raphael-camera: SMMU selected SMR=%u CB=%u saved route=%#x/%#x CBAR=%#x CBA2R=%#x SCTLR=%#x TCR2=%#x IDR=%#x/%#x\n",
	    s->match, s->context, s->saved_smr, s->saved_s2cr, s->saved_cbar,
	    s->saved_cba2r, s->saved_cb[0], s->saved_cb[1], idr0, idr1);
	/* Same 4K S1 table format used by this phone's validated WiFi path,
	 * with independent context and 36-bit physical-address limit. */
	s->cba2r_dirty = true;
	if (!raphael_smmu_put(s->gr1 + 0x800 + 4 * s->context, 1)) return false;
	s->cbar_dirty = true;
	if (!raphael_smmu_put(s->gr1 + 4 * s->context, 0x1f300)) return false;
	uint64_t ttbr = root_pa | (uint64_t)s->context << 48;
	const uint32_t values[9] = {
		0, 1u | 1u << 4 | 7u << 15, (uint32_t)ttbr, (uint32_t)(ttbr >> 32),
		0, 0, 25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23, 0xf404ff44, 0
	};
	for (unsigned i = 0; i < 9; i++)
		if (!raphael_smmu_cb_put(s, i, values[i])) return false;
	/* ASIDPNE bit12 is not required: this helper explicitly invalidates
	 * contexts on each transition. Handset reads that optional bit as zero. */
	if (!raphael_smmu_flush(s) || !raphael_smmu_cb_put(s, 0, 0x127)) return false;
	s->s2cr_dirty = true;
	if (!raphael_smmu_put(s->base + 0xc00 + 4 * s->match, s->context)) return false;
	s->smr_dirty = true;
	if (!raphael_smmu_put(s->base + 0x800 + 4 * s->match, 0x86200840u)) return false;
	s->ready = true;
	return true;
}

static bool
raphael_smmu_retire(struct raphael_smmu *s)
{
	/* Linux disables unassigned context banks instead of requiring their
	 * original uninitialised register image. Only use this for an originally
	 * unreferenced/disabled bank, after restoring the exact inactive route.
	 * Remove all references to allocated tables, flush, and keep M=0. */
	if (s->smr_dirty || s->s2cr_dirty || (s->saved_smr & 0x80000000u) ||
	    (s->saved_cb[0] & 1) ||
	    (rd(s->base + 0x800 + 4 * s->match) & 0x80000000u) ||
	    rd(s->base + 0xc00 + 4 * s->match) != s->saved_s2cr ||
	    (s->saved_s2cr & 0x30000u) != 0x20000u) return false;
	if (!raphael_smmu_put(s->cb, 0) ||
	    !raphael_smmu_put(s->gr1 + 0x800 + 4 * s->context, 1) ||
	    !raphael_smmu_put(s->gr1 + 4 * s->context, 0x1f300)) return false;
	for (unsigned i = 2; i < 6; i++)
		if (!raphael_smmu_put(s->cb + raphael_smmu_cb_regs[i], 0)) return false;
	if (!raphael_smmu_flush(s) || rd(s->cb) ||
	    (rd(s->cb + 0x58) & 0xc00001feu)) return false;
	s->cb_dirty = 0; s->cbar_dirty = s->cba2r_dirty = false;
	s->retired = true;
	say("raphael-camera: SMMU retired inactive bank: M=0, exact FAULT route, SMR invalid, TTBR0/1=0, TLB synced; configuration not byte-restored\n");
	return true;
}

static bool
raphael_smmu_end(struct raphael_smmu *s)
{
	if (!s) return false;
	s->ready = false;
	/* Remove routing before touching the translation context. */
	if (s->smr_dirty) {
		if (!raphael_smmu_put(s->base + 0x800 + 4 * s->match, s->saved_smr)) return false;
		s->smr_dirty = false;
	}
	if (s->s2cr_dirty) {
		vm_offset_t reg = s->base + 0xc00 + 4 * s->match;
		if (!raphael_smmu_put(reg, s->saved_s2cr)) {
			/* Linux arm-smmu-qcom.c bypass_quirk: some firmware ignores
			 * FAULT writes and translates BYPASS writes into FAULT.
			 * Only try after our SMR is invalid; verify the EXACT saved
			 * fault value, never accept a real bypass as restoration. */
			if ((s->saved_smr & 0x80000000u) ||
			    (s->saved_s2cr & 0x30000u) != 0x20000u ||
			    (rd(s->base + 0x800 + 4 * s->match) & 0x80000000u)) return false;
			raphael_smmu_store(reg, (s->saved_s2cr & ~0x30000u) | 0x10000u);
			__sync_synchronize();
			if (rd(reg) != s->saved_s2cr) return false;
			say("raphael-camera: Qualcomm S2CR FAULT quirk restore verified\n");
		}
		s->s2cr_dirty = false;
	}
	if (s->cb_dirty) {
		if (!raphael_smmu_cb_put(s, 0, 0) || !raphael_smmu_flush(s)) return false;
	}
	/* CBAR/CBA2R select the interpretation of context registers. Restore
	 * that format before writing its saved register values. */
	if (s->cba2r_dirty) {
		if (!raphael_smmu_put(s->gr1 + 0x800 + 4 * s->context, s->saved_cba2r)) return false;
		s->cba2r_dirty = false;
	}
	if (s->cbar_dirty) {
		if (!raphael_smmu_put(s->gr1 + 4 * s->context, s->saved_cbar)) {
			/* A bare reset CBAR=0 can be rejected by the mediated SMMU.
			 * Do not call this an exact restoration. A private disabled
			 * bank can be retired under the stronger pointer-free checks. */
			if (!s->saved_cbar && !s->saved_cba2r) return raphael_smmu_retire(s);
			return false;
		}
		s->cbar_dirty = false;
	}
	if (s->cb_dirty) {
		/* Keep SCTLR disabled until all other context/group registers match. */
		for (unsigned i = 1; i < 9; i++) if (s->cb_dirty & (1u << i)) {
			if (!raphael_smmu_put(s->cb + raphael_smmu_cb_regs[i], s->saved_cb[i])) return false;
			s->cb_dirty &= ~(1u << i);
		}
	}
	if (s->cb_dirty & 1) {
		if (!raphael_smmu_put(s->cb, s->saved_cb[0])) return false;
		s->cb_dirty &= ~1u;
	}
	return true;
}
