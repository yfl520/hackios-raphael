/* ml_io_map() consumes permanent VA in XNU's bounded io_submap. Repeated
 * rear captures must reuse those mappings, not allocate them every frame.
 * Cache virtual mappings only: each operation still snapshots/validates the
 * live registers and retains the existing power/clock/DMA rollback order.
 */
static LCK_GRP_DECLARE(raphael_mmio_group, "raphael-camera-mmio");
static LCK_MTX_DECLARE(raphael_mmio_lock, &raphael_mmio_group);
static struct {
	vm_offset_t physical, virtual;
	vm_size_t bytes;
} raphael_mmio[272]; /* up to 256 SMMU contexts plus camera register blocks */
static unsigned raphael_mmio_count;
static uint64_t raphael_mmio_bytes;

static vm_offset_t
raphael_camera_map(vm_offset_t physical, vm_size_t bytes)
{
	if (!bytes || physical > ~(vm_offset_t)0 - bytes) return 0;
	vm_offset_t base = trunc_page(physical);
	vm_size_t offset = physical - base;
	if (bytes > ~(vm_size_t)0 - offset - PAGE_MASK) return 0;
	vm_size_t span = round_page(bytes + offset);
	lck_mtx_lock(&raphael_mmio_lock);
	for (unsigned i = 0; i < raphael_mmio_count; i++) {
		if (base < raphael_mmio[i].physical) continue;
		vm_offset_t delta = base - raphael_mmio[i].physical;
		if (delta <= raphael_mmio[i].bytes && span <= raphael_mmio[i].bytes - delta) {
			vm_offset_t result = raphael_mmio[i].virtual + delta + offset;
			lck_mtx_unlock(&raphael_mmio_lock);
			return result;
		}
	}
	if (raphael_mmio_count == sizeof(raphael_mmio) / sizeof(raphael_mmio[0])) {
		lck_mtx_unlock(&raphael_mmio_lock);
		say("raphael-camera: MMIO cache full; refuse another permanent map\n");
		return 0;
	}
	vm_offset_t mapped = ml_io_map(base, span);
	if (mapped) {
		unsigned i = raphael_mmio_count++;
		raphael_mmio[i].physical = base;
		raphael_mmio[i].virtual = mapped;
		raphael_mmio[i].bytes = span;
		raphael_mmio_bytes += span;
		say("raphael-camera: MMIO cached physical=%#llx bytes=%llu maps=%u total=%llu\n",
		    (unsigned long long)base, (unsigned long long)span,
		    raphael_mmio_count, (unsigned long long)raphael_mmio_bytes);
	}
	lck_mtx_unlock(&raphael_mmio_lock);
	return mapped ? mapped + offset : 0;
}

static void
raphael_camera_map_status(uint64_t sequence)
{
	lck_mtx_lock(&raphael_mmio_lock);
	say("raphael-camera: MMIO status frame=%llu maps=%u total=%llu\n",
	    (unsigned long long)sequence, raphael_mmio_count,
	    (unsigned long long)raphael_mmio_bytes);
	lck_mtx_unlock(&raphael_mmio_lock);
}
