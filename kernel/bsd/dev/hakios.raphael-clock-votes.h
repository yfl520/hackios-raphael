/* Included by Wi-Fi after command-db/RPMh helpers. This sensor-ID stage
 * requires an already established full CN0 configuration-path vote and
 * adequate existing MX/CX requests. It never changes shared bus votes.
 * Only MMCX may be raised and is restored after the Titan domain collapses.
 */
static uint32_t raphael_camera_mmcx_address, raphael_camera_mmcx_saved;
static bool raphael_camera_mmcx_dirty;

bool hakios_raphael_camera_capture_ready(void);
void hakios_raphael_camera_data_inspect(void);
bool hakios_raphael_camera_data_ready(void);

static bool
raphael_camera_arc_level(const struct cmd_db_entry *entry, unsigned minimum,
    unsigned *selected, unsigned *count, uint16_t levels[16])
{
	if (!entry || !selected || !count || !levels || ((entry->address >> 16) & 15) != 3 ||
	    (entry->address & 15) || !entry->length || (entry->length & 1) || entry->length > 32) return false;
	const unsigned limit = CMD_DB_SIZE - __offsetof(struct cmd_db_header, data);
	if (!cmd_db) return false;
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		if (!slave->id || slave->count > 512 || (slave->header_offset & 3) ||
		    slave->header_offset + slave->count * sizeof(*entry) > limit) continue;
		uintptr_t first = (uintptr_t)cmd_db->data + slave->header_offset;
		uintptr_t at = (uintptr_t)entry;
		if (at < first || at - first >= slave->count * sizeof(*entry) ||
		    (at - first) % sizeof(*entry)) continue;
		unsigned offset = slave->data_offset + entry->offset;
		if (offset > limit || entry->length > limit - offset) return false;
		const uint8_t *aux = cmd_db->data + offset;
		*count = 0; *selected = 16;
		for (unsigned i = 0; i < entry->length / 2u; i++) {
			unsigned level = aux[2 * i] | aux[2 * i + 1] << 8;
			if (i && !level) break;
			if ((!i && level) || (i && level <= levels[i - 1])) return false;
			levels[i] = (uint16_t)level;
			(*count)++;
			if (*selected == 16 && level >= minimum) *selected = i;
		}
		return *selected < *count;
	}
	return false;
}

bool
hakios_raphael_camera_clock_votes(bool restore)
{
	if (!hakios_is_raphael()) return false;
	lck_mtx_lock(&rpmh_lock);
	bool ok = false;
	uint32_t raw = 0;
	if (restore) {
		ok = !raphael_camera_mmcx_dirty ||
		    (raphael_rpmh_write_locked(raphael_camera_mmcx_address, raphael_camera_mmcx_saved) &&
		    raphael_rpmh_read_locked(raphael_camera_mmcx_address, &raw) &&
		    !(raw & ~0x8000000fu) && (raw & 15) == raphael_camera_mmcx_saved);
		if (ok) raphael_camera_mmcx_dirty = false;
		tell("raphael-camera: MMCX original request restore %s\n", ok ? "verified" : "FAILED");
		goto done;
	}
	if (raphael_camera_mmcx_dirty || !raphael_camera_power.active || raphael_rpmh_faulted) goto done;
	const struct cmd_db_entry *cn0 = rpmh_resource("CN0", 0);
	/* Full existing CN0 peak vote was read as 0x20003fff on this phone.
	 * Require it rather than guessing a new bandwidth or disturbing USB. */
	if (!cn0 || ((cn0->address >> 16) & 15) != 5 ||
	    !raphael_rpmh_read_locked(cn0->address, &raw) || (raw & 0x20003fff) != 0x20003fff) goto done;
	static const char *const names[] = { "mx.lvl", "cx.lvl", "mmcx.lvl" };
	const unsigned minimums[] = { 128, 64, 64 };
	for (unsigned i = 0; i < 3; i++) {
		const struct cmd_db_entry *entry = rpmh_resource(names[i], 0);
		unsigned selected = 16, count = 0;
		uint16_t levels[16] = { 0 };
		if (!raphael_camera_arc_level(entry, minimums[i], &selected, &count, levels) ||
		    !raphael_rpmh_read_locked(entry->address, &raw) || (raw & ~0x8000000fu) || (raw & 15) >= count) goto done;
		unsigned saved = raw & 15;
		tell("raphael-camera: %s request index=%u level=%u minimum=%u index=%u\n",
		    names[i], saved, levels[saved], minimums[i], selected);
		if (i < 2) { if (levels[saved] < minimums[i]) goto done; continue; }
		raphael_camera_mmcx_address = entry->address;
		raphael_camera_mmcx_saved = saved;
		if (saved < selected) {
			raphael_camera_mmcx_dirty = true;
			if (!raphael_rpmh_write_locked(entry->address, selected) ||
			    !raphael_rpmh_read_locked(entry->address, &raw) || (raw & ~0x8000000fu) || (raw & 15) != selected) goto done;
		}
	}
	ok = true;
done:
	if (!ok && !restore) tell("raphael-camera: clock prerequisite rejected before GDSC/MCLK\n");
	lck_mtx_unlock(&rpmh_lock);
	return ok;
}

/* PLL0 1200 MHz and its ~1300 MHz calibration require MX SVS_L1=192;
 * lite IFE400/CAMNOC266 require MMCX SVS=128 (raw command DB levels).
 * This probe only accepts existing votes; it never changes shared requests.
 * CN0 covers configuration, not a claim that DMA bandwidth is established.
 */
bool
hakios_raphael_camera_capture_ready(void)
{
	if (!hakios_is_raphael()) return false;
	lck_mtx_lock(&rpmh_lock);
	bool ok = raphael_camera_power.active && !raphael_rpmh_faulted;
	const char *names[] = { "mx.lvl", "mmcx.lvl" };
	const unsigned minimum[] = { 192, 128 };
	for (unsigned i = 0; i < 2 && ok; i++) {
		const struct cmd_db_entry *entry = rpmh_resource(names[i], 0);
		unsigned selected = 16, count = 0;
		uint16_t levels[16] = { 0 };
		uint32_t raw = 0;
		ok = raphael_camera_arc_level(entry, minimum[i], &selected, &count, levels) &&
		    raphael_rpmh_read_locked(entry->address, &raw) &&
		    !(raw & ~0x8000000fu) && (raw & 15) < count &&
		    levels[raw & 15] >= minimum[i];
		tell("raphael-camera: capture prerequisite %s raw=%#x minimum=%u %s\n",
		    names[i], raw, minimum[i], ok ? "verified" : "FAILED");
	}
	lck_mtx_unlock(&rpmh_lock);
	return ok;
}

/* BCM auxiliary bytes are unit32, width16, VCD8, reserved8. Read only.
 * Resolve through command DB; never guess addresses or saturate requests.
 */
static bool
raphael_camera_data_query(bool require)
{
	if (!hakios_is_raphael()) return false;
	lck_mtx_lock(&rpmh_lock);
	bool ok = raphael_camera_power.active && !raphael_rpmh_faulted;
	const char *names[] = { "MM1", "MM0", "SH0", "MC0", "ACV" };
	const unsigned node_width[] = { 32, 32, 16, 4, 0 };
	const unsigned limit = CMD_DB_SIZE - __offsetof(struct cmd_db_header, data);
	for (unsigned n = 0; n < 5; n++) {
		const struct cmd_db_entry *e = rpmh_resource(names[n], 0);
		const uint8_t *aux = NULL;
		if (!e || !cmd_db || e->length != 8 || ((e->address >> 16) & 15) != 5) { ok = false; continue; }
		for (unsigned i = 0; i < 8; i++) {
			const struct cmd_db_slave *s = &cmd_db->slave[i];
			if (!s->id || s->count > 512 || (s->header_offset & 3) ||
			    s->header_offset + s->count * sizeof(*e) > limit) continue;
			uintptr_t first = (uintptr_t)cmd_db->data + s->header_offset, at = (uintptr_t)e;
			if (at < first || at - first >= s->count * sizeof(*e) || (at-first) % sizeof(*e)) continue;
			unsigned offset = s->data_offset + e->offset;
			if (offset <= limit && e->length <= limit - offset) aux = cmd_db->data + offset;
		}
		uint32_t raw = 0;
		if (!aux || !raphael_rpmh_read_locked(e->address, &raw)) { ok = false; continue; }
		uint32_t unit = aux[0] | (uint32_t)aux[1] << 8 | (uint32_t)aux[2] << 16 | (uint32_t)aux[3] << 24;
		unsigned width = aux[4] | aux[5] << 8;
		tell("raphael-camera: data BCM %s address=%#x unit=%u width=%u VCD=%u reserved=%u original=%#x READ ONLY\n",
		    names[n], e->address, unit, width, aux[6], aux[7], raw);
		if (require) {
			/* Invert msm_bus_arb_rpmh.c max_ib scaling for each path
			 * node: rawY * unit * node_buswidth / bcm_width. Reuse an
			 * adequate original request; never change shared bus votes. */
			uint64_t capacity = n < 4 && width ?
			    (uint64_t)(raw & 0x3fff) * unit * node_width[n] / width : 0;
			bool enough = (raw & 0x20000000u) && !aux[7] &&
			    (n == 4 ? (raw & 8) != 0 : unit && width && capacity >= 3000000000ull);
			tell("raphael-camera: data BCM %s original peak-equivalent=%llu B/s minimum3GB/s %s\n",
			    names[n], (unsigned long long)capacity, enough ? "verified" : "FAILED");
			ok &= enough;
		}
	}
	lck_mtx_unlock(&rpmh_lock);
	return ok;
}

void
hakios_raphael_camera_data_inspect(void)
{
	(void)raphael_camera_data_query(false);
}

bool
hakios_raphael_camera_data_ready(void)
{
	return raphael_camera_data_query(true);
}
