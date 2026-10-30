/* Reused verified rear session DMA transaction, front PHY1/geometry only. */
static bool
raphael_front_smmu_owned(const struct raphael_smmu *s, uint64_t root_pa)
{
    uint64_t ttbr = root_pa | (uint64_t)s->context << 48;
    return s->ready &&
        rd(s->base + 0x800 + 4 * s->match) == 0x86200840u &&
        rd(s->base + 0xc00 + 4 * s->match) == s->context &&
        rd(s->gr1 + 4 * s->context) == 0x1f300 &&
        rd(s->gr1 + 0x800 + 4 * s->context) == 1 &&
        rd(s->cb) == 0x127 && rd(s->cb + 0x20) == (uint32_t)ttbr &&
        rd(s->cb + 0x24) == (uint32_t)(ttbr >> 32);
}

enum { RAPHAEL_FRONT_REG_COUNT = sizeof(raphael_frame_registers) / sizeof(raphael_frame_registers[0]) };
struct raphael_front_engine {
    vm_offset_t gc, tables; uint64_t root_pa;
    vm_size_t table_alloc;
    struct raphael_frame_reg saved[RAPHAEL_FRONT_REG_COUNT];
    uint32_t bridge_saved[2];
    bool bridge_dirty[2], path_dirty, locked, shared, ok;
    bool stopped, paths_clean, mmu_clean, bridge_clean;
    struct raphael_smmu mmu;
};

static bool
raphael_front_engine_map(struct raphael_front_engine *e, vm_offset_t buffer, uint64_t buffer_bytes,
    uint32_t first_address, uint32_t frame_bytes)
{
    e->stopped = e->paths_clean = e->mmu_clean = e->bridge_clean = true;
	e->gc = raphael_camera_map(0x00100000, 0x10000);
	csid = raphael_camera_map(CSID_PHYS, 0x1000);
	ife = raphael_camera_map(IFE_PHYS, 0x4000);
	if (!e->gc || !csid || !ife || !hakios_raphael_camera_data_ready()) return false;
	/* Reset-done masks alone (top31, bus0 bit0) are idle reset defaults.
	 * Save/mask/restore these; all DMA/other IRQ ownership stays refused. */
	if ((rd(csid + 0x600) & 1) || (rd(ife + IFE_IRQ_MASK0) & ~0x80000000u) || rd(ife + IFE_IRQ_MASK1) ||
	    (rd(ife + 0x2044) & ~1u) || rd(ife + 0x2048) || rd(ife + 0x204c) ||
	    rd(csid + 0x24) || rd(csid + 0x74) || (rd(ife + 0x2210) & 1)) {
		say("raphael-camera: frame preflight busy TPG=%#x IFE-mask=%#x/%#x BUS-mask=%#x/%#x/%#x CSID-mask=%#x/%#x WMheader=%#x\n",
		    rd(csid + 0x600), rd(ife + IFE_IRQ_MASK0), rd(ife + IFE_IRQ_MASK1),
		    rd(ife + 0x2044), rd(ife + 0x2048), rd(ife + 0x204c),
		    rd(csid + 0x24), rd(csid + 0x74), rd(ife + 0x2210));
		return false;
	}
	for (unsigned i = 0; i < 4; i++)
		if ((rd(ife + 0x2208 + 0x100 * i) & 1) ||
		    (rd(csid + 0x200 + 0x100 * i) & 0x80000000u) ||
		    rd(csid + 0x34 + 0x10 * i)) {
			say("raphael-camera: frame preflight WM/RDI%u busy cfg=%#x/%#x mask=%#x\n",
			    i, rd(ife + 0x2208 + 0x100*i), rd(csid + 0x200 + 0x100*i),
			    rd(csid + 0x34 + 0x10*i));
			return false;
		}
	for (unsigned i = 0; i < RAPHAEL_FRONT_REG_COUNT; i++) {
		e->saved[i].vfe = raphael_frame_registers[i].vfe;
		e->saved[i].offset = raphael_frame_registers[i].offset;
		e->saved[i].saved = rd((e->saved[i].vfe ? ife : csid) + e->saved[i].offset);
	}
	e->bridge_saved[0] = rd(e->gc + 0xb030); e->bridge_saved[1] = rd(e->gc + 0xb034);
	e->ok = true;
	unsigned count = 2 + (unsigned)((buffer_bytes + 0x1fffff) >> 21);
	e->table_alloc = round_page(count * 4096);
	uint64_t physical[RAPHAEL_MAX_TABLE_PAGES] = {0};
	if (count > RAPHAEL_MAX_TABLE_PAGES ||
	    kmem_alloc(kernel_map, &e->tables, e->table_alloc, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) e->ok = false;
	if (e->ok) {
		for (unsigned i = 0; i < count; i++) physical[i] = kvtophys(e->tables + 4096 * i);
		e->ok = raphael_camera_pages((uint64_t *)e->tables, count * 4096, physical, count,
		    kvtophys(buffer), buffer_bytes);
	}
	if (e->ok) {
		raphael_frame_cache(e->tables, e->table_alloc, 1);
		lck_mtx_lock(&hakios_smmu_lock); e->locked = true;
		e->root_pa = physical[0];
        e->ok = raphael_smmu_begin(&e->mmu, physical[0]);
	}
	for (unsigned i = 0; i < 2 && e->ok; i++) {
		vm_offset_t at = e->gc + 0xb030 + 4 * i;
		/* Preserve an already enabled bridge; only undo our own enables. */
		if (!(e->bridge_saved[i] & 1)) {
			e->bridge_dirty[i] = true;
			wr(at, rd(at) | 1); __sync_synchronize();
		}
		e->ok = raphael_clock_wait(at, 0x80000001u, 1);
	}
	if (e->ok) {
		/* Mark before invoking existing helpers, including partial setup. */
		e->path_dirty = true; csid_running = ife_running = true;
		e->ok = blk_put(ife, "IFE reset-mask", IFE_IRQ_MASK0, 0, true) &&
		    blk_put(ife, "IFE bus reset-mask", 0x2044, 0, true) &&
		    csid_config(1, 4, 0x3210, 0, 2592, 1940, false) &&
		    ife_config(first_address, frame_bytes, 0);
	}
    return e->ok;
}

/* A valid SMR reserves this CB for other allocators while the mutex is free.
 * Verify that route and translation root before handing it back. */
static bool
raphael_front_engine_share(struct raphael_front_engine *e)
{
    if (!e->ok || !e->locked || !raphael_front_smmu_owned(&e->mmu, e->root_pa)) return false;
    e->shared = true; e->locked = false;
    lck_mtx_unlock(&hakios_smmu_lock);
    return true;
}

static bool
raphael_front_engine_begin(struct raphael_front_engine *e, uint32_t first_address, uint32_t frame_bytes)
{
    return raphael_front_engine_map(e, frames, frames_bytes, first_address, frame_bytes);
}

static bool
raphael_front_engine_end(struct raphael_front_engine *e, bool stopped)
{
    e->stopped = stopped;
    if (e->shared) {
        lck_mtx_lock(&hakios_smmu_lock); e->locked = true;
        if (!raphael_front_smmu_owned(&e->mmu, e->root_pa)) {
            lck_mtx_unlock(&hakios_smmu_lock); e->locked = false;
            say("raphael-camera: lost SMMU lease; no foreign context restore, retain memory\n");
            return false;
        }
    }
	if (e->path_dirty) {
		spin_us(40000);
		show_paths("Raphael DMA end");
		paths_down();
		e->paths_clean = e->stopped && !rd(ife + WM0_CFG) &&
		    !(rd(csid + CSID_RDI0_CFG0) & 0x80000000u) &&
		    wait_reg(csid, CSID_RDI0_STATUS, 1, 1, 100000);
		if (e->paths_clean) for (unsigned i = RAPHAEL_FRONT_REG_COUNT; i--;) {
			vm_offset_t base = e->saved[i].vfe ? ife : csid;
			/* All saved enables were preflighted inactive. */
			if (!blk_put(base, e->saved[i].vfe ? "IFE restore" : "CSID restore",
			    e->saved[i].offset, e->saved[i].saved, true)) e->paths_clean = false;
		}
		wr(ife + IFE_REG_UPDATE, 2); __sync_synchronize();
	}
	if (e->locked) {
		e->mmu_clean = e->paths_clean && e->stopped && raphael_smmu_end(&e->mmu);
		if (e->mmu_clean || e->shared) {
            lck_mtx_unlock(&hakios_smmu_lock); e->locked = false;
        }
	}
	if (e->mmu_clean && e->paths_clean && e->stopped) {
		for (unsigned i = 2; i--;) if (e->bridge_dirty[i]) {
			vm_offset_t at = e->gc + 0xb030 + 4 * i;
			wr(at, (rd(at) & ~1u) | (e->bridge_saved[i] & 1)); __sync_synchronize();
			if (!raphael_clock_wait(at, 0x80000001u, 0x80000000u)) e->bridge_clean = false;
		}
	}
	bool clean = e->stopped && e->paths_clean && e->mmu_clean && e->bridge_clean;
	if (clean && e->tables) kmem_free(kernel_map, e->tables, e->table_alloc);
    return clean;
}

