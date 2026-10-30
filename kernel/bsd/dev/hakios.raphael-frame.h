/* One bounded real RAW capture. Reuse HackiOS frames_alloc(), csid_config(),
 * ife_config(), show_paths(), paths_down() and RWCAM001 export unchanged.
 * Only the board/DMA backend and transaction restoration are Raphael-specific.
 */
extern bool hakios_raphael_camera_data_ready(void);
static bool raphael_frame_content_rejected;
static unsigned raphael_frame_warmup = 8;

static void
raphael_frame_cache(vm_offset_t start, uint64_t bytes, unsigned operation)
{
#if defined(__aarch64__)
	for (vm_offset_t at = start; at < start + bytes; at += 64) {
		if (operation == 0) __asm__ volatile ("dc civac, %0" : : "r" (at));
		else if (operation == 1) __asm__ volatile ("dc cvac, %0" : : "r" (at));
		else __asm__ volatile ("dc ivac, %0" : : "r" (at));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	(void)start; (void)bytes; (void)operation;
	__sync_synchronize(); /* host control-flow harness */
#endif
}

struct raphael_frame_reg { bool vfe; uint16_t offset; uint32_t saved; };
static const struct { bool vfe; uint16_t offset; } raphael_frame_registers[] = {
	{false,0x100},{false,0x104},{false,0x108},{false,0x034},
	{false,0x200},{false,0x204},{false,0x208},{false,0x20c},{false,0x210},
	{false,0x214},{false,0x218},{false,0x224},{false,0x228},{false,0x22c},{false,0x230},
	{true,0x5c},{true,0x2044},{true,0x200c},{true,0x2080},{true,0x2084},{true,0x2048},
	{true,0x2208},{true,0x2214},{true,0x225c},{true,0x221c},{true,0x2220},
	{true,0x2224},{true,0x2228},{true,0x2250},{true,0x2254},{true,0x2248},{true,0x224c},{true,0x2258}
};

static bool
raphael_frame_capture(struct raphael_sensor *sensor, struct raphael_i2c_bus *bus, bool *retain)
{
	raphael_frame_content_rejected = false;
	vm_offset_t gc = raphael_camera_map(0x00100000, 0x10000);
	csid = raphael_camera_map(CSID_PHYS, 0x1000);
	ife = raphael_camera_map(IFE_PHYS, 0x4000);
	if (!gc || !csid || !ife || !hakios_raphael_camera_data_ready()) return false;
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
	enum { COUNT = sizeof(raphael_frame_registers) / sizeof(raphael_frame_registers[0]) };
	struct raphael_frame_reg saved[COUNT];
	for (unsigned i = 0; i < COUNT; i++) {
		saved[i].vfe = raphael_frame_registers[i].vfe;
		saved[i].offset = raphael_frame_registers[i].offset;
		saved[i].saved = rd((saved[i].vfe ? ife : csid) + saved[i].offset);
	}
	uint32_t bridge_saved[2] = {rd(gc + 0xb030),rd(gc + 0xb034)};
	bool bridge_dirty[2] = {false,false}, path_dirty = false, locked = false;
	bool ok = true, stopped = true, paths_clean = true, mmu_clean = true, bridge_clean = true;
	vm_offset_t tables = 0;
	vm_size_t table_alloc = 0;
	struct raphael_smmu mmu = {0};
	struct cam_header *head = NULL;
	/* Existing 2-slot contiguous capture format, reused for host tools. */
	if (!frames_alloc(4000, 3000, RAW10_STRIDE(4000), 1)) return false;
	head = (struct cam_header *)frames;
	strlcpy(head->sensor, "IMX586", sizeof(head->sensor)); head->bayer = 0;
	memset((void *)(frames + CAM_HEADER_BYTES), 0xa5, frames_bytes - CAM_HEADER_BYTES);
	raphael_frame_cache(frames, frames_bytes, 0);
	unsigned count = 2 + (unsigned)((frames_bytes + 0x1fffff) >> 21);
	table_alloc = round_page(count * 4096);
	uint64_t physical[RAPHAEL_MAX_TABLE_PAGES] = {0};
	if (count > RAPHAEL_MAX_TABLE_PAGES ||
	    kmem_alloc(kernel_map, &tables, table_alloc, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) ok = false;
	if (ok) {
		for (unsigned i = 0; i < count; i++) physical[i] = kvtophys(tables + 4096 * i);
		ok = raphael_camera_pages((uint64_t *)tables, count * 4096, physical, count,
		    kvtophys(frames), frames_bytes);
	}
	if (ok) {
		raphael_frame_cache(tables, table_alloc, 1);
		lck_mtx_lock(&hakios_smmu_lock); locked = true;
		ok = raphael_smmu_begin(&mmu, physical[0]);
	}
	for (unsigned i = 0; i < 2 && ok; i++) {
		vm_offset_t at = gc + 0xb030 + 4 * i;
		/* Preserve an already enabled bridge; only undo our own enables. */
		if (!(bridge_saved[i] & 1)) {
			bridge_dirty[i] = true;
			wr(at, rd(at) | 1); __sync_synchronize();
		}
		ok = raphael_clock_wait(at, 0x80000001u, 1);
	}
	if (ok) {
		/* Mark before invoking existing helpers, including partial setup. */
		path_dirty = true; csid_running = ife_running = true;
		ok = blk_put(ife, "IFE reset-mask", IFE_IRQ_MASK0, 0, true) &&
		    blk_put(ife, "IFE bus reset-mask", 0x2044, 0, true) &&
		    csid_config(0, 4, 0x3210, 0, 4000, 3000, false) &&
		    ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, head->frame_bytes, 0);
	}
	uint32_t done = 0, top = 0, completed = 0, crc_before = rd(csid + CSID_CSI2_RX_CRC);
	if (ok) {
		/* Clear stale completion before starting the real sensor. */
		wr(ife + BUS_IRQ_CLEAR1, 1); wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
		__sync_synchronize();
		ok = !(rd(ife + BUS_IRQ_STATUS1) & 1) && raphael_sensor_start(sensor, bus);
	}
	if (ok) {
		for (unsigned i = 0; i < 1000; i++) {
			spin_us(1000);
			done = rd(ife + BUS_IRQ_STATUS1); top = rd(ife + IFE_IRQ_STATUS0);
			if (done & 1) {
				completed++;
				wr(ife + BUS_IRQ_CLEAR1, 1); wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
				__sync_synchronize();
				if (completed == raphael_frame_warmup) break;
				/* Reuse upstream capture_sensor's warm-up policy:
				 * initial frames can precede the programmed exposure.
				 * Keep the same bounded slot and latch its address again. */
				wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES));
				wr(ife + IFE_REG_UPDATE, 2);
				__sync_synchronize();
			}
			if ((rd(mmu.cb + 0x58) & 0xc00001feu) || rd(ife + IFE_VIOLATION_STATUS)) break;
		}
		ok = completed == raphael_frame_warmup && !(rd(mmu.cb + 0x58) & 0xc00001feu) &&
		    !rd(ife + IFE_VIOLATION_STATUS) &&
		    rd(csid + CSID_CSI2_RX_CRC) == crc_before &&
		    rd(csid + CSID_RDI0_BYTES_PING) == 15000000 &&
		    rd(csid + CSID_RDI0_BYTES_PONG) == 15000000;
		uint8_t shutter[2]={0}, gain[2]={0}, length[2]={0};
		bool readback = raphael_sensor_read(bus, 0x202, shutter, 2) &&
		    raphael_sensor_read(bus, 0x204, gain, 2) &&
		    raphael_sensor_read(bus, 0x340, length, 2);
		say("raphael-camera: capture warmup frames=%u exposure-readback=%u shutter=%u gain=%#x frame-lines=%u\n",
		    completed, readback, shutter[0]<<8|shutter[1], gain[0]<<8|gain[1], length[0]<<8|length[1]);
	}
	/* Stop the sensor, wait for its final rolling frame, then the masters. */
	stopped = raphael_sensor_stop(sensor, bus);
	if (path_dirty) {
		spin_us(40000);
		show_paths("Raphael DMA end");
		paths_down();
		paths_clean = stopped && !rd(ife + WM0_CFG) &&
		    !(rd(csid + CSID_RDI0_CFG0) & 0x80000000u) &&
		    wait_reg(csid, CSID_RDI0_STATUS, 1, 1, 100000);
		if (paths_clean) for (unsigned i = COUNT; i--;) {
			vm_offset_t base = saved[i].vfe ? ife : csid;
			/* All saved enables were preflighted inactive. */
			if (!blk_put(base, saved[i].vfe ? "IFE restore" : "CSID restore",
			    saved[i].offset, saved[i].saved, true)) paths_clean = false;
		}
		wr(ife + IFE_REG_UPDATE, 2); __sync_synchronize();
	}
	if (locked) {
		mmu_clean = paths_clean && stopped && raphael_smmu_end(&mmu);
		if (mmu_clean) lck_mtx_unlock(&hakios_smmu_lock);
	}
	if (mmu_clean && paths_clean && stopped) {
		for (unsigned i = 2; i--;) if (bridge_dirty[i]) {
			vm_offset_t at = gc + 0xb030 + 4 * i;
			wr(at, (rd(at) & ~1u) | (bridge_saved[i] & 1)); __sync_synchronize();
			if (!raphael_clock_wait(at, 0x80000001u, 0x80000000u)) bridge_clean = false;
		}
	}
	bool clean = stopped && paths_clean && mmu_clean && bridge_clean;
	if (clean && tables) kmem_free(kernel_map, tables, table_alloc);
	*retain |= !clean;
	/* CPU reads only after the writer stopped. Preserve the existing export
	 * format but publish frames=1 only if real completion and content agree. */
	unsigned changed = 0, zero = 0, varied_rows = 0, written_rows = 0;
	if (clean) {
		raphael_frame_cache(frames, frames_bytes, 2);
		const uint8_t *raw = (const uint8_t *)frames + CAM_HEADER_BYTES;
		for (unsigned y = 0; y < 3000; y++) {
			unsigned row_changes = 0, row_written = 0;
			for (unsigned x = 0; x < 5000; x++) {
				uint8_t v = raw[y * head->stride + x];
				changed += v != 0xa5; zero += v == 0;
				row_written += v != 0xa5;
				row_changes += v != raw[y * head->stride];
			}
			varied_rows += row_changes > 100;
			written_rows += row_written > 4000;
		}
	}
	/* Texture is scene content, not proof of DMA integrity. The rejected v9
	 * frame was mostly saturated 0xff, with every row overwritten. Require
	 * completion, packet sizes, no bus faults and complete cleanup above;
	 * retain stale/zero rejection and require every row to replace its sentinel.
	 * Do not publish a rejected buffer, but distinguish it from a hardware fault. */
	bool content = changed > 10000000 && zero < 14000000 && written_rows == 3000;
	raphael_frame_content_rejected = ok && clean && !content;
	ok = ok && clean && content;
	head->frames = ok ? 1 : 0; head->last = 0;
	raphael_frame_cache(frames, CAM_HEADER_BYTES, 1);
	say("raphael-camera: real RAW DMA %s WMdone=%#x top=%#x completed=%u changed=%u zeros=%u varied-rows=%u written-rows=%u content-retry=%u sensor-off=%u paths=%u SMMU=%u bridges=%u; export frames=%u\n",
	    ok ? "CANDIDATE" : "FAILED", done, top, completed, changed, zero, varied_rows,
	    written_rows, raphael_frame_content_rejected, stopped, paths_clean, mmu_clean, bridge_clean, head->frames);
	return ok;
}
