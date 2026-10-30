/* Rear-only IMX586 table transport. Caller owns the board supplies/reset/MCLK.
 * This does not program CSI/DMA, operate a lens, or claim a frame was received.
 * R586TB01 is made only by tools/raphael-imx586-tables.py from pinned V1 blobs.
 */
enum { R586_INIT, R586_MODE, R586_ON, R586_OFF, R586_HOLDON, R586_HOLDOFF, R586_STAGES };
struct raphael_sensor_header {
	char magic[8];
	uint32_t bytes, width, height, lane_assign, lanes, pixel_clock, line_length,
	    frame_length, settle, fps_milli, cfa, entries, stages, reserved;
};
struct raphael_sensor_stage { uint32_t first, count; };
struct raphael_sensor_entry { uint16_t address; uint8_t value, reserved; uint32_t delay_us; };
struct raphael_sensor {
	const struct raphael_sensor_header *head;
	const struct raphael_sensor_stage *stage;
	const struct raphael_sensor_entry *entry;
	bool prepared, stream_dirty;
};
_Static_assert(sizeof(struct raphael_sensor_header) == 64, "R586 header ABI");
_Static_assert(sizeof(struct raphael_sensor_stage) == 8, "R586 stage ABI");
_Static_assert(sizeof(struct raphael_sensor_entry) == 8, "R586 entry ABI");

static bool
raphael_sensor_load(struct raphael_sensor *s, const void *data, uint64_t size)
{
	/* A live session cannot be replaced even by an invalid table. */
	if (!s || s->prepared || s->stream_dirty) return false;
	s->head = NULL; s->stage = NULL; s->entry = NULL;
	if (!data || ((uintptr_t)data & 3) || size < 64 + R586_STAGES * 8) return false;
	const struct raphael_sensor_header *h = data;
	if (memcmp(h->magic, "R586TB01", 8) || h->bytes != size || h->reserved ||
	    h->width != 4000 || h->height != 3000 || h->lane_assign != 0x3210 ||
	    h->lanes != 4 || h->pixel_clock != 459520000 || h->line_length != 8976 ||
	    h->frame_length != 3064 || h->settle != 14 || h->fps_milli != 30010 ||
	    h->cfa != 3 || h->entries != 199 || h->stages != R586_STAGES ||
	    size != sizeof(*h) + R586_STAGES * 8 + (uint64_t)h->entries * 8) return false;
	const struct raphael_sensor_stage *stage = (const void *)(h + 1);
	const struct raphael_sensor_entry *entry = (const void *)(stage + R586_STAGES);
	const unsigned counts[R586_STAGES] = { 103, 92, 1, 1, 1, 1 };
	unsigned next = 0;
	/* Validate every stage before allowing even its first bus transaction. */
	for (unsigned i = 0; i < R586_STAGES; i++) {
		if (stage[i].first != next || stage[i].count != counts[i]) return false;
		next += counts[i];
		for (unsigned j = stage[i].first; j < next; j++) {
			if (entry[j].reserved || entry[j].delay_us > 100000) return false;
			if (i < R586_ON && (entry[j].address == 0x100 ||
			    entry[j].address == 0x103 || entry[j].address == 0x104)) return false;
		}
		if (i >= R586_ON) {
			const struct raphael_sensor_entry *e = &entry[stage[i].first];
			if (e->address != (i < R586_HOLDON ? 0x100 : 0x104) ||
			    e->value != (i == R586_ON || i == R586_HOLDON) || e->delay_us) return false;
		}
	}
	s->head = h; s->stage = stage; s->entry = entry;
	return true;
}

static bool
raphael_sensor_write(struct raphael_i2c_bus *bus, uint16_t address, uint8_t value)
{
	const uint8_t out[3] = { address >> 8, address & 255, value };
	enum raphael_i2c_result r = raphael_i2c_transfer(bus, 0x1a, out, 3, NULL, 0);
	if (r) say("raphael-camera: IMX586 write %#x failed (%u)\n", address, r);
	return r == RAPHAEL_I2C_OK;
}

static bool
raphael_sensor_read(struct raphael_i2c_bus *bus, uint16_t address, uint8_t *in, unsigned n)
{
	const uint8_t out[2] = { address >> 8, address & 255 };
	return raphael_i2c_transfer(bus, 0x1a, out, 2, in, n) == RAPHAEL_I2C_OK;
}

static bool
raphael_sensor_apply(struct raphael_sensor *s, struct raphael_i2c_bus *bus, unsigned stage)
{
	if (!s || !s->head || stage >= R586_STAGES) return false;
	const struct raphael_sensor_stage *p = &s->stage[stage];
	for (unsigned i = 0; i < p->count; i++) {
		const struct raphael_sensor_entry *e = &s->entry[p->first + i];
		if (!raphael_sensor_write(bus, e->address, e->value)) return false;
		if (e->delay_us) spin_us(e->delay_us);
	}
	return true;
}

static bool
raphael_sensor_stop(struct raphael_sensor *s, struct raphael_i2c_bus *bus)
{
	if (!s) return false;
	if (!s->stream_dirty) { s->prepared = false; return true; }
	uint8_t value = 0xff;
	if (!raphael_sensor_apply(s, bus, R586_OFF) ||
	    !raphael_sensor_read(bus, 0x100, &value, 1) || value) return false;
	s->stream_dirty = false; s->prepared = false;
	return true;
}

static bool
raphael_sensor_prepare(struct raphael_sensor *s, struct raphael_i2c_bus *bus)
{
	if (!s || !s->head || s->prepared || s->stream_dirty) return false;
	uint8_t id[2] = { 0 };
	if (!raphael_sensor_read(bus, 0x16, id, 2) || id[0] != 5 || id[1] != 0x86) return false;
	/* A failed write can still have reached the sensor: cleanup stays required. */
	s->stream_dirty = true;
	if (!raphael_sensor_stop(s, bus)) return false;
	if (!raphael_sensor_apply(s, bus, R586_INIT) || !raphael_sensor_apply(s, bus, R586_MODE)) return false;
	/* Critical final values, compared to the reviewed module. No stream-on here. */
	const struct { uint16_t address; uint8_t value; } checks[] = {
		{0x136,0x13}, {0x137,0x33}, {0x112,10}, {0x113,10}, {0x114,3},
		{0x340,0x0b}, {0x341,0xf8}, {0x342,0x23}, {0x343,0x10},
		{0x34c,0x0f}, {0x34d,0xa0}, {0x34e,0x0b}, {0x34f,0xb8}, {0x100,0},
	};
	for (unsigned i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
		uint8_t value = 0;
		if (!raphael_sensor_read(bus, checks[i].address, &value, 1) || value != checks[i].value) {
			say("raphael-camera: IMX586 verify %#x got %#x expected %#x\n",
			    checks[i].address, value, checks[i].value);
			return false;
		}
	}
	s->prepared = true;
	say("raphael-camera: IMX586 configured 4000x3000 RAW10 DPHY4; stream still off\n");
	return true;
}

static bool
raphael_sensor_start(struct raphael_sensor *s, struct raphael_i2c_bus *bus)
{
	if (!s || !s->prepared || s->stream_dirty) return false;
	s->stream_dirty = true;
	uint8_t value = 0;
	return raphael_sensor_apply(s, bus, R586_ON) &&
	       raphael_sensor_read(bus, 0x100, &value, 1) && value == 1;
}
