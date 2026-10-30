/* Rear CCI0 bus over SM8150 GPIO17/18. No front-camera/motor access.
 * Pin bases are supplied by the board layer; SM8150 has several TLMM tiles.
 * Open drain: drive LOW or release to input, never drive HIGH. All changes
 * are restored after success, NACK, stuck bus, or clock-stretch timeout.
 * This file does not switch supplies, clocks or reset. Write-only transfers
 * are used by the separate, validated rear-sensor table executor.
 */
struct raphael_i2c_bus {
	vm_offset_t sda, scl;
	uint32_t cfg[2], io[2];
};

enum raphael_i2c_result {
	RAPHAEL_I2C_OK, RAPHAEL_I2C_ARGUMENT, RAPHAEL_I2C_BUSY,
	RAPHAEL_I2C_STRETCH, RAPHAEL_I2C_NACK,
};

static void
raphael_i2c_line(struct raphael_i2c_bus *b, unsigned pin, bool release)
{
	vm_offset_t base = pin ? b->scl : b->sda;
	wr(base + 4, 0);
	/* Pull up, function GPIO, 2mA; preserve unrelated high configuration bits. */
	wr(base, (b->cfg[pin] & ~0x3ffu) | 3u | (release ? 0 : 0x200u));
}

static bool
raphael_i2c_level(struct raphael_i2c_bus *b, unsigned pin)
{
	return (rd((pin ? b->scl : b->sda) + 4) & 1) != 0;
}

static bool
raphael_i2c_clock_high(struct raphael_i2c_bus *b)
{
	raphael_i2c_line(b, 1, true);
	for (unsigned i = 0; i < 100; i++) {
		if (raphael_i2c_level(b, 1)) { spin_us(5); return true; }
		spin_us(5);
	}
	return false;
}

static bool
raphael_i2c_start(struct raphael_i2c_bus *b)
{
	raphael_i2c_line(b, 0, true);
	spin_us(5);
	if (!raphael_i2c_clock_high(b)) return false;
	if (!raphael_i2c_level(b, 0)) return false;
	raphael_i2c_line(b, 0, false);
	spin_us(5);
	raphael_i2c_line(b, 1, false);
	spin_us(5);
	return true;
}

static bool
raphael_i2c_stop(struct raphael_i2c_bus *b)
{
	raphael_i2c_line(b, 1, false);
	raphael_i2c_line(b, 0, false);
	spin_us(5);
	if (!raphael_i2c_clock_high(b)) return false;
	raphael_i2c_line(b, 0, true);
	spin_us(5);
	return raphael_i2c_level(b, 0);
}

static enum raphael_i2c_result
raphael_i2c_bit(struct raphael_i2c_bus *b, bool out, bool *in)
{
	raphael_i2c_line(b, 0, out);
	spin_us(5);
	if (!raphael_i2c_clock_high(b)) return RAPHAEL_I2C_STRETCH;
	*in = raphael_i2c_level(b, 0);
	raphael_i2c_line(b, 1, false);
	spin_us(5);
	return RAPHAEL_I2C_OK;
}

static enum raphael_i2c_result
raphael_i2c_send(struct raphael_i2c_bus *b, uint8_t byte)
{
	bool in;
	for (int bit = 7; bit >= 0; bit--) {
		enum raphael_i2c_result result = raphael_i2c_bit(b, (byte >> bit) & 1, &in);
		if (result) return result;
	}
	enum raphael_i2c_result result = raphael_i2c_bit(b, true, &in);
	return result ? result : in ? RAPHAEL_I2C_NACK : RAPHAEL_I2C_OK;
}

static enum raphael_i2c_result
raphael_i2c_receive(struct raphael_i2c_bus *b, bool more, uint8_t *value)
{
	uint8_t byte = 0;
	bool in;
	for (unsigned bit = 0; bit < 8; bit++) {
		enum raphael_i2c_result result = raphael_i2c_bit(b, true, &in);
		if (result) return result;
		byte = (uint8_t)((byte << 1) | in);
	}
	enum raphael_i2c_result result = raphael_i2c_bit(b, !more, &in);
	if (!result) *value = byte;
	return result;
}

static enum raphael_i2c_result
raphael_i2c_transfer(struct raphael_i2c_bus *b, uint8_t address,
    const uint8_t *out, unsigned out_count, uint8_t *in, unsigned in_count)
{
	if (!b || !b->sda || !b->scl || b->sda == b->scl || address > 0x7f ||
	    !out || !out_count || out_count > 32 || (in_count && !in) || in_count > 32)
		return RAPHAEL_I2C_ARGUMENT;
	b->cfg[0] = rd(b->sda); b->cfg[1] = rd(b->scl);
	b->io[0] = rd(b->sda + 4); b->io[1] = rd(b->scl + 4);
	enum raphael_i2c_result result = RAPHAEL_I2C_BUSY;
	bool started = false;
	raphael_i2c_line(b, 0, true);
	raphael_i2c_line(b, 1, true);
	spin_us(5);
	if (!raphael_i2c_level(b, 0) || !raphael_i2c_level(b, 1)) goto restore;
	if (!raphael_i2c_start(b)) goto restore;
	started = true;
	result = raphael_i2c_send(b, (uint8_t)(address << 1));
	for (unsigned i = 0; i < out_count && !result; i++) result = raphael_i2c_send(b, out[i]);
	if (!result && in_count && !raphael_i2c_start(b)) result = RAPHAEL_I2C_STRETCH;
	if (!result && in_count) result = raphael_i2c_send(b, (uint8_t)((address << 1) | 1));
	for (unsigned i = 0; i < in_count && !result; i++)
		result = raphael_i2c_receive(b, i + 1 < in_count, &in[i]);
restore:
	if (started && !raphael_i2c_stop(b) && !result) result = RAPHAEL_I2C_STRETCH;
	/* Restore the output latch before the original output-enable/pin function. */
	wr(b->sda + 4, b->io[0] & 2); wr(b->scl + 4, b->io[1] & 2);
	wr(b->sda, b->cfg[0]); wr(b->scl, b->cfg[1]);
	return result;
}
