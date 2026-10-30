/* CSID-lite0 receive-only probe: no RDI path, VFE, DMA or SMMU programming.
 * RX/capture offsets and VC/DT/WC decode follow pinned lite17x/core.c.
 * Caller keeps PHY clocks alive and stops the sensor before end.
 */
struct raphael_csid_rx {
	vm_offset_t base;
	uint32_t cfg[3];
	bool dirty, ready;
};

static bool
raphael_csid_put(struct raphael_csid_rx *c, unsigned offset, uint32_t value)
{
	wr(c->base + offset, value);
	__sync_synchronize();
	uint32_t got = rd(c->base + offset);
	if (got != value)
		say("raphael-camera: CSID RX offset=%#x got=%#x expected=%#x\n", offset, got, value);
	return got == value;
}

static bool
raphael_csid_begin(struct raphael_csid_rx *c)
{
	if (!c || c->dirty || c->ready) return false;
	c->base = raphael_camera_map(0x0acc8000, 0x1000);
	if (!c->base) return false;
	uint32_t version = rd(c->base);
	if (!version || version == 0xffffffffu) {
		say("raphael-camera: CSID-lite0 invalid version=%#x\n", version);
		return false;
	}
	/* Caller owns the board/domain that was off before this probe. Stale
	 * lane selection and ECC correction are not enables; preserve them.
	 * Reject MISR capture, TPG start bit0, RDI enables and IRQ delivery.
	 * Never reset an unknown owner's receiver or enable physical IRQs. */
	if ((rd(c->base + 0x104) & 0x40) ||
	    rd(c->base + 0x108) || rd(c->base + 0x024) ||
	    rd(c->base + 0x074) || (rd(c->base + 0x600) & 1)) {
		say("raphael-camera: CSID-lite0 busy cfg=%#x/%#x capture=%#x masks=%#x/%#x TPG=%#x\n",
		    rd(c->base + 0x100), rd(c->base + 0x104), rd(c->base + 0x108),
		    rd(c->base + 0x024), rd(c->base + 0x074), rd(c->base + 0x600));
		return false;
	}
	for (unsigned i = 0; i < 4; i++)
		if ((rd(c->base + 0x200 + i * 0x100) & 0x80000000u) ||
		    rd(c->base + 0x034 + i * 0x10)) {
			say("raphael-camera: CSID-lite0 RDI%u busy\n", i);
			return false;
		}
	for (unsigned i = 0; i < 3; i++) c->cfg[i] = rd(c->base + 0x100 + i * 4);
	c->dirty = true;
	/* lane_num-1=3, lane assignment3210, PHY0, DPHY; ECC+MISR. */
	if (!raphael_csid_put(c, 0x100, 0x32103) ||
	    !raphael_csid_put(c, 0x104, 0x41) ||
	    !raphael_csid_put(c, 0x108, 1 | (0x2bu << 4))) return false;
	c->ready = true;
	say("raphael-camera: CSID-lite0 receive-only configured version=%#x VC0 RAW10\n", version);
	return true;
}

static bool
raphael_csid_end(struct raphael_csid_rx *c)
{
	if (!c) return false;
	c->ready = false;
	if (!c->dirty) return true;
	/* Disable capture and RX before restoring their idle snapshot. */
	if (!raphael_csid_put(c, 0x108, 0) ||
	    !raphael_csid_put(c, 0x100, 0) ||
	    !raphael_csid_put(c, 0x104, 0)) return false;
	for (unsigned i = 0; i < 3; i++)
		if (!raphael_csid_put(c, 0x100 + i * 4, c->cfg[i])) return false;
	c->dirty = false;
	return true;
}

struct raphael_csid_sample {
	uint32_t packets, ecc, crc, header, footer, irq;
};

static struct raphael_csid_sample
raphael_csid_sample(struct raphael_csid_rx *c)
{
	struct raphael_csid_sample s = {
		rd(c->base + 0x160), rd(c->base + 0x164), rd(c->base + 0x168),
		rd(c->base + 0x130), rd(c->base + 0x138), rd(c->base + 0x020)
	};
	return s;
}

static bool
raphael_csid_stream_probe(struct raphael_csid_rx *c, struct raphael_sensor *s,
    struct raphael_i2c_bus *bus)
{
	struct raphael_csid_sample before = raphael_csid_sample(c);
	spin_us(20000);
	if (raphael_csid_sample(c).packets != before.packets) {
		say("raphael-camera: CSID count moves with sensor stopped; reject evidence\n");
		return false;
	}
	bool started = raphael_sensor_start(s, bus);
	struct raphael_csid_sample a = before, b = before;
	if (started) {
		spin_us(100000); a = raphael_csid_sample(c);
		spin_us(100000); b = raphael_csid_sample(c);
	}
	bool stopped = raphael_sensor_stop(s, bus);
	/* A rolling frame can take 33ms; allow it to drain before off baseline. */
	spin_us(40000);
	uint32_t off = raphael_csid_sample(c).packets;
	spin_us(20000);
	bool still = raphael_csid_sample(c).packets == off;
	unsigned vc = b.header >> 22, dt = (b.header >> 16) & 63, wc = b.header & 65535;
	uint32_t da = a.packets - before.packets, db = b.packets - a.packets;
	bool ok = started && stopped && still && da && db && da < 1000000 && db < 1000000 &&
	    vc == 0 && dt == 0x2b && wc == 5000 &&
	    b.crc == before.crc && b.ecc == before.ecc &&
	    (b.footer >> 16) == (b.footer & 65535);
	say("raphael-camera: CSID real-stream evidence %s packets=%u/%u/%u delta=%u/%u VC=%u DT=%#x WC=%u ECC=%u->%u CRC=%u->%u footer=%#x IRQ=%#x stop=%u stable-off=%u\n",
	    ok ? "MATCH" : "FAILED", before.packets, a.packets, b.packets, da, db,
	    vc, dt, wc, before.ecc, b.ecc, before.crc, b.crc, b.footer, b.irq, stopped, still);
	/* A packet match is not a DMA frame or an image. */
	return ok;
}
