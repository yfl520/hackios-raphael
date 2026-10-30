/* cam9: establish/restore a private IFE SMMU context with an empty page
 * table. No WM/RDI/DMA starts. Also inspect BCM metadata, never vote here.
 */
extern void hakios_raphael_camera_data_inspect(void);

static bool
raphael_camera_smmu_probe(bool *retain)
{
	vm_offset_t vfe = raphael_camera_map(0x0acc4000, 0x4000);
	vm_offset_t cid = raphael_camera_map(0x0acc8000, 0x1000);
	if (!vfe || !cid) return false;
	for (unsigned i = 0; i < 4; i++)
		if ((rd(vfe + 0x2208 + 0x100 * i) & 1) ||
		    (rd(cid + 0x200 + 0x100 * i) & 0x80000000u)) {
			say("raphael-camera: SMMU probe refused active WM/RDI%u\n", i);
			return false;
		}
	hakios_raphael_camera_data_inspect();
	vm_offset_t table = 0;
	if (kmem_alloc(kernel_map, &table, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
	bzero((void *)table, PAGE_SIZE);
	for (vm_offset_t line = table; line < table + PAGE_SIZE; line += 64)
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	__asm__ volatile ("dsb sy" ::: "memory");
	struct raphael_smmu mmu = { 0 };
	lck_mtx_lock(&hakios_smmu_lock);
	bool ok = raphael_smmu_begin(&mmu, kvtophys(table));
	uint32_t raw_fault = mmu.cb ? rd(mmu.cb + 0x58) : 0;
	uint32_t fault = raw_fault & 0xc00001feu;
	bool clean = raphael_smmu_end(&mmu);
	say("raphael-camera: IFE SMMU empty-table probe %s SMR=%u CB=%u fault=%#x safe-release=%u; NO DMA\n",
	    ok && !fault && clean ? "verified" : "FAILED", mmu.match, mmu.context, fault, clean);
	if (mmu.retired) say("raphael-camera: SMMU release used disabled-bank retirement, not original configuration restore\n");
	if (!clean) {
		/* Quarantine under the shared lock until the bounded reboot:
		 * no mapper may reuse a partly restored context or freed tables. */
		*retain = true;
		say("raphael-camera: SMMU rollback FAILED; retain lock/tables until reboot\n");
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	kmem_free(kernel_map, table, PAGE_SIZE);
	return ok && !fault;
}
