/* Build bounded 4K, 39-bit stage-1 tables for one contiguous camera buffer.
 * All physical addresses are supplied by the caller, checked below, and
 * must stay allocated until SMMU restoration succeeds. No MMIO here.
 */
#define RAPHAEL_FRAME_IOVA 0x40000000ull
#define RAPHAEL_MAX_FRAME_BYTES (64ull << 20)
#define RAPHAEL_MAX_TABLE_PAGES 34
static bool
raphael_camera_pages(uint64_t *tables, uint64_t table_bytes, const uint64_t *table_pa,
    unsigned table_count, uint64_t frame_pa, uint64_t frame_bytes)
{
	if (!tables || ((uintptr_t)tables & 4095) || !table_pa || !frame_bytes ||
	    frame_bytes > RAPHAEL_MAX_FRAME_BYTES || (frame_bytes & 4095) ||
	    !frame_pa || (frame_pa & 4095) || frame_pa >= (1ull << 36) ||
	    frame_bytes > (1ull << 36) - frame_pa) return false;
	unsigned thirds = (unsigned)((frame_bytes + 0x1fffff) >> 21);
	unsigned count = 2 + thirds;
	if (table_count != count || count > RAPHAEL_MAX_TABLE_PAGES || table_bytes != count * 4096ull)
		return false;
	for (unsigned i = 0; i < count; i++) {
		uint64_t p = table_pa[i];
		if (!p || (p & 4095) || p >= (1ull << 36) ||
		    (p >= frame_pa && p - frame_pa < frame_bytes)) return false;
		for (unsigned j = 0; j < i; j++) if (table_pa[j] == p) return false;
	}
	bzero(tables, (size_t)table_bytes);
	tables[(RAPHAEL_FRAME_IOVA >> 30) & 511] = table_pa[1] | 3;
	uint64_t *second = tables + 512;
	for (unsigned t = 0; t < thirds; t++) {
		second[((RAPHAEL_FRAME_IOVA >> 21) & 511) + t] = table_pa[2 + t] | 3;
		uint64_t *third = tables + (2 + t) * 512;
		for (unsigned p = 0; p < 512 && (uint64_t)(t * 512 + p) * 4096 < frame_bytes; p++)
			/* Attr0 normal NC, EL1 RW, inner-shareable, AF/not-global. */
			third[p] = (frame_pa + (uint64_t)(t * 512 + p) * 4096) |
			    3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11;
	}
	return true;
}
