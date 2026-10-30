/* Separate front geometry; original rear frame implementation unchanged. */
static bool
raphael_front_frame_capture(struct raphael_front_sensor *sensor, struct raphael_i2c_bus *bus, bool *retain)
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
		say("raphael-front: frame preflight busy TPG=%#x IFE-mask=%#x/%#x BUS-mask=%#x/%#x/%#x CSID-mask=%#x/%#x WMheader=%#x\n",
		    rd(csid + 0x600), rd(ife + IFE_IRQ_MASK0), rd(ife + IFE_IRQ_MASK1),
		    rd(ife + 0x2044), rd(ife + 0x2048), rd(ife + 0x204c),
		    rd(csid + 0x24), rd(csid + 0x74), rd(ife + 0x2210));
		return false;
	}
	for (unsigned i = 0; i < 4; i++)
		if ((rd(ife + 0x2208 + 0x100 * i) & 1) ||
		    (rd(csid + 0x200 + 0x100 * i) & 0x80000000u) ||
		    rd(csid + 0x34 + 0x10 * i)) {
			say("raphael-front: frame preflight WM/RDI%u busy cfg=%#x/%#x mask=%#x\n",
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
	if (!frames_alloc(2592, 1940, RAW10_STRIDE(2592), 1)) return false;
	head = (struct cam_header *)frames;
	strlcpy(head->sensor, "S5K3T2", sizeof(head->sensor)); head->bayer = 1;
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
		    csid_config(1, 4, 0x3210, 0, 2592, 1940, false) &&
		    ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, head->frame_bytes, 0);
	}
	uint32_t done = 0, top = 0, completed = 0, crc_before = rd(csid + CSID_CSI2_RX_CRC);
	if (ok) {
		/* Clear stale completion before starting the real sensor. */
		wr(ife + BUS_IRQ_CLEAR1, 1); wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
		__sync_synchronize();
		ok = !(rd(ife + BUS_IRQ_STATUS1) & 1) && raphael_front_start(sensor, bus);
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
		    rd(csid + CSID_RDI0_BYTES_PING) == 6285600 &&
		    rd(csid + CSID_RDI0_BYTES_PONG) == 6285600;
		uint16_t shutter=0, gain=0, length=0;
		bool readback = raphael_front_read(bus, 0x202, &shutter) &&
		    raphael_front_read(bus, 0x204, &gain) && raphael_front_read(bus, 0x340, &length);
		say("raphael-front: capture frames=%u exposure-readback=%u shutter=%u gain=%#x frame-lines=%u\n",
		    completed, readback, shutter, gain, length);
	}
	/* Stop the sensor, wait for its final rolling frame, then the masters. */
	stopped = raphael_front_stop(sensor, bus);
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
		for (unsigned y = 0; y < 1940; y++) {
			unsigned row_changes = 0, row_written = 0;
			for (unsigned x = 0; x < 3240; x++) {
				uint8_t v = raw[y * head->stride + x];
				changed += v != 0xa5; zero += v == 0;
				row_written += v != 0xa5;
				row_changes += v != raw[y * head->stride];
			}
			varied_rows += row_changes > 100;
			written_rows += row_written > 2592;
		}
	}
	/* Texture is scene content, not proof of DMA integrity. The rejected v9
	 * frame was mostly saturated 0xff, with every row overwritten. Require
	 * completion, packet sizes, no bus faults and complete cleanup above;
	 * retain stale/zero rejection and require every row to replace its sentinel.
	 * Do not publish a rejected buffer, but distinguish it from a hardware fault. */
	bool content = changed > 4190400 && zero < 6200000 && written_rows == 1940;
	raphael_frame_content_rejected = ok && clean && !content;
	ok = ok && clean && content;
	head->frames = ok ? 1 : 0; head->last = 0;
	raphael_frame_cache(frames, CAM_HEADER_BYTES, 1);
	say("raphael-front: real RAW DMA %s WMdone=%#x top=%#x completed=%u changed=%u zeros=%u varied-rows=%u written-rows=%u content-retry=%u sensor-off=%u paths=%u SMMU=%u bridges=%u; export frames=%u\n",
	    ok ? "CANDIDATE" : "FAILED", done, top, completed, changed, zero, varied_rows,
	    written_rows, raphael_frame_content_rejected, stopped, paths_clean, mmu_clean, bridge_clean, head->frames);
	return ok;
}
