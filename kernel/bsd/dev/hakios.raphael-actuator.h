/* Rear wide DW9763 only. The live DT links cam-sensor@0 -> actuator@0 ->
 * fixed 2.8V regulator on GPIO83. SM8150 NORTH tile, TLMM 0x03000000 +
 * 0x00900000 + 83*0x1000 = 0x03953000. BOB parent is held by rear_probe.
 * cam_af=1 enables identification only: no actuator register/DAC writes.
 * No front sensor, pop-up motor, tele actuator or PMIC configuration here.
 */
static unsigned raphael_actuator_mode;
static unsigned raphael_actuator_fixed = 180;

static bool
raphael_actuator_identify(struct raphael_i2c_bus *bus)
{
    if (raphael_actuator_mode != 1) return true;
    struct raphael_probe_pin pin = {0};
    pin.base = raphael_camera_map(0x03953000, 0x1000);
    if (!pin.base) return true; /* Optional focus must not disable preview. */
    pin.cfg = rd(pin.base); pin.io = rd(pin.base + 4);
    if ((pin.cfg & 0x23cu) || (pin.io & 2u)) {
        say("raphael-af: GPIO83 busy cfg=%#x io=%#x; untouched\n", pin.cfg, pin.io);
        return true;
    }
    bool ok = raphael_probe_gpio(&pin, true);
    spin_us(1000); /* OEM module VAF up delay: 1ms, DT ramp: 100us. */
    uint8_t address = 0, registers[8] = {0};
    enum raphael_i2c_result result = RAPHAEL_I2C_ARGUMENT;
    if (ok) result = raphael_i2c_transfer(bus, 0x0c, &address, 1, registers, sizeof(registers));
    ok = ok && result == RAPHAEL_I2C_OK && registers[0] == 0xf9;
    say("raphael-af: identify mode=%u transfer=%u id=%02x match=%u control=%02x dac=%u status=%02x sac=%02x period=%02x; no register writes\n",
        raphael_actuator_mode, result, registers[0], ok, registers[2],
        ((unsigned)registers[3] << 8) | registers[4], registers[5], registers[6], registers[7]);
    bool clean = raphael_probe_gpio(&pin, false);
    spin_us(1000);
    wr(pin.base + 4, pin.io & 2u); wr(pin.base, pin.cfg);
    clean = clean && rd(pin.base) == pin.cfg && (rd(pin.base + 4) & 2u) == (pin.io & 2u);
    say("raphael-af: identify power/pin restore=%u\n", clean);
    /* Identification/NACK is optional. Failure to restore ownership is not. */
    return clean;
}

/* cam_af=2: verified OEM initialization and bounded fixed rear lens position.
 * Remains hidden from userland AF until movement/settling is tested. */
struct raphael_actuator_state {
    struct raphael_probe_pin pin;
    bool owned, initialized, active;
    uint16_t dac;
};

static bool
raphael_actuator_read(struct raphael_i2c_bus *bus, uint8_t reg, uint8_t *in, unsigned count)
{
    return count > 0 && count <= 8 &&
        raphael_i2c_transfer(bus, 0x0c, &reg, 1, in, count) == RAPHAEL_I2C_OK;
}

static bool
raphael_actuator_write(struct raphael_i2c_bus *bus, uint8_t reg, uint16_t value)
{
    uint8_t out[3] = {reg, (uint8_t)value, 0};
    if (reg == 3) {
        if (value > 579) return false; /* OEM's 400 entries: 180..579; parking may use 0..179. */
        out[1] = value >> 8; out[2] = (uint8_t)value;
    } else if (!((reg == 2 && value <= 2) || (reg == 6 && value == 0x60) ||
                (reg == 7 && value == 2))) return false;
    return raphael_i2c_transfer(bus, 0x0c, out, reg == 3 ? 3 : 2, NULL, 0) == RAPHAEL_I2C_OK;
}

static bool
raphael_actuator_idle(struct raphael_i2c_bus *bus)
{
    for (unsigned n = 0; n < 20; n++) {
        uint8_t status = 0;
        if (!raphael_actuator_read(bus, 5, &status, 1)) return false;
        if (!(status & 1)) return true;
        spin_us(1000);
    }
    return false;
}

static bool
raphael_actuator_move(struct raphael_actuator_state *s, struct raphael_i2c_bus *bus, unsigned target)
{
    if (!s->initialized || target > 579) return false;
    for (unsigned n = 0; n < 20 && s->dac != target; n++) {
        unsigned next = s->dac < target ? s->dac + MIN(32u, target - s->dac) :
            s->dac - MIN(32u, s->dac - target);
        if (!raphael_actuator_idle(bus) || !raphael_actuator_write(bus, 3, (uint16_t)next)) return false;
        spin_us(1000);
        uint8_t readback[2] = {0};
        if (!raphael_actuator_idle(bus) || !raphael_actuator_read(bus, 3, readback, 2) ||
            (((unsigned)readback[0] << 8) | readback[1]) != next) return false;
        s->dac = next;
    }
    return s->dac == target;
}

static bool
raphael_actuator_end(struct raphael_actuator_state *s, struct raphael_i2c_bus *bus)
{
    if (!s->owned) return true;
    bool parked = true;
    if (s->initialized) {
        /* Match upstream's gradual return to zero before disconnecting VAF.
         * OEM deInitSettings then writes control=0. */
        parked = raphael_actuator_move(s, bus, 0);
        if (!raphael_actuator_write(bus, 2, 0)) parked = false;
    }
    bool clean = raphael_probe_gpio(&s->pin, false);
    spin_us(1000);
    wr(s->pin.base + 4, s->pin.io & 2u); wr(s->pin.base, s->pin.cfg);
    clean = clean && rd(s->pin.base) == s->pin.cfg &&
        (rd(s->pin.base + 4) & 2u) == (s->pin.io & 2u);
    say("raphael-af: end parked=%u power/pin-restore=%u last-dac=%u\n", parked, clean, s->dac);
    s->owned = !clean; s->active = s->initialized = false;
    return clean;
}

static bool
raphael_actuator_begin(struct raphael_actuator_state *s, struct raphael_i2c_bus *bus)
{
    if (raphael_actuator_mode != 2 && raphael_actuator_mode != 3) return raphael_actuator_identify(bus);
    s->pin.base = raphael_camera_map(0x03953000, 0x1000);
    if (!s->pin.base) return true;
    s->pin.cfg = rd(s->pin.base); s->pin.io = rd(s->pin.base + 4);
    if ((s->pin.cfg & 0x23cu) || (s->pin.io & 2u)) {
        say("raphael-af: GPIO83 occupied; fixed lens unavailable, preview retained\n");
        return true;
    }
    s->owned = true;
    bool ok = raphael_probe_gpio(&s->pin, true);
    spin_us(1000);
    uint8_t identity[8] = {0};
    if (ok) ok = raphael_actuator_read(bus, 0, identity, sizeof(identity)) && identity[0] == 0xf9;
    /* A power-cycled, idle chip was verified in the ID-only candidate. */
    if (ok) ok = identity[2] == 0 && identity[3] == 0 && identity[4] == 0 && !(identity[5] & 1);
    if (ok) ok = raphael_actuator_write(bus, 2, 1) && raphael_actuator_write(bus, 2, 0);
    if (ok) spin_us(1000);
    if (ok) ok = raphael_actuator_write(bus, 2, 2) && raphael_actuator_write(bus, 6, 0x60) &&
        raphael_actuator_write(bus, 7, 2);
    uint8_t control = 0, mode[2] = {0};
    if (ok) ok = raphael_actuator_read(bus, 2, &control, 1) && control == 2 &&
        raphael_actuator_read(bus, 6, mode, 2) && mode[0] == 0x60 && mode[1] == 2;
    if (ok) {
        s->initialized = true; s->dac = 0;
        ok = raphael_actuator_move(s, bus, raphael_actuator_fixed);
    }
    say("raphael-af: fixed begin id=%02x init=%u target=%u actual=%u success=%u\n",
        identity[0], s->initialized, raphael_actuator_fixed, s->dac, ok);
    s->active = ok;
    return ok ? true : raphael_actuator_end(s, bus);
}
