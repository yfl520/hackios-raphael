/* Raphael SMB1390 identity/status probe, no charger configuration writes.
 * Live DT: GENI SE4 I2C at 0x890000, address 0x10, GPIO51 SDA/52 SCL.
 * Pinned SM8150 pinctrl: both pins on SOUTH (0x03d00000), GPIO OE bit9.
 * qcom-i2c-pmic uses 16-bit big-endian register indices, 8-bit values.
 * An I2C read sends only the two address bytes before repeated START.
 * Preserve/restore both pin configurations; no reset or bus recovery pulses.
 */
static vm_offset_t cp_gpio;
static bool cp_bus_bad;
static const unsigned cp_pins[2] = {51, 52};
static uint32_t cp_saved_cfg[2], cp_saved_io[2];

static uint32_t cp_gpio_get(unsigned pin, unsigned offset)
{
    return *(volatile uint32_t *)(cp_gpio + 0x1000u * pin + offset);
}

static void cp_gpio_set(unsigned pin, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(cp_gpio + 0x1000u * pin + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void cp_release(unsigned pin, bool release)
{
    /* Output LOW or high impedance only; never drive the bus HIGH. */
    cp_gpio_set(pin, 4, 0);
    cp_gpio_set(pin, 0, release ? 0 : 1u << 9);
}

static bool cp_level(unsigned pin) { return (cp_gpio_get(pin, 4) & 1) != 0; }

static void cp_clock_high(void)
{
    cp_release(52, true);
    for (unsigned n = 0; n < 20 && !cp_level(52); n++) delay(5);
    if (!cp_level(52)) cp_bus_bad = true;
    delay(5);
}

static void cp_start(void)
{
    cp_release(51, true); delay(5); cp_clock_high();
    if (cp_bus_bad) return;
    cp_release(51, false); delay(5); cp_release(52, false); delay(5);
}

static void cp_stop(void)
{
    cp_release(52, false); cp_release(51, false); delay(5);
    cp_clock_high(); cp_release(51, true); delay(5);
}

static bool cp_bit(bool release)
{
    if (cp_bus_bad) return true;
    cp_release(51, release); delay(5); cp_clock_high();
    bool bit = cp_level(51);
    cp_release(52, false); delay(5);
    return bit;
}

static bool cp_send(uint8_t value)
{
    for (int bit = 7; bit >= 0 && !cp_bus_bad; bit--) cp_bit((value >> bit) & 1);
    return !cp_bit(true) && !cp_bus_bad;
}

static bool cp_register_allowed(uint16_t reg)
{
    return reg == 0x0104 || reg == 0x0105 || reg == 0x0100 ||
        reg == 0x0101 || reg == 0x0102 || reg == 0x0103 ||
        (reg >= 0x1004 && reg <= 0x1008) || reg == 0x1010 ||
        reg == 0x1020 || reg == 0x1030 || reg == 0x1032 ||
        reg == 0x1033 || reg == 0x1034;
}

static bool cp_read_only(uint16_t reg, uint8_t *value)
{
    *value = 0xff;
    if (!cp_register_allowed(reg)) return false;
    cp_bus_bad = false;
    cp_start();
    bool ok = !cp_bus_bad && cp_send(0x20) && cp_send((uint8_t)(reg >> 8)) && cp_send((uint8_t)reg);
    if (ok) { cp_start(); ok = !cp_bus_bad && cp_send(0x21); }
    if (ok) {
        uint8_t result = 0;
        for (unsigned bit = 0; bit < 8; bit++) result = (uint8_t)((result << 1) | cp_bit(true));
        cp_bit(true); /* NACK: exactly one data byte requested */
        *value = result;
    }
    cp_stop();
    return ok && !cp_bus_bad;
}

static void cp_restore_pins(void)
{
    for (unsigned i = 0; i < 2; i++) {
        cp_gpio_set(cp_pins[i], 4, cp_saved_io[i] & 2);
        cp_gpio_set(cp_pins[i], 0, cp_saved_cfg[i]);
    }
}

static void raphael_cp_probe_readonly(void)
{
    if (!hakios_is_raphael()) return;
    cp_gpio = ml_io_map(0x03d00000, 0x100000);
    if (!cp_gpio) return;
    for (unsigned i = 0; i < 2; i++) {
        cp_saved_cfg[i] = cp_gpio_get(cp_pins[i], 0);
        cp_saved_io[i] = cp_gpio_get(cp_pins[i], 4);
    }
    rc_tell("charge-cp1: GPIO51/52 saved cfg=%#x/%#x io=%#x/%#x; identity/status read only\n",
        cp_saved_cfg[0], cp_saved_cfg[1], cp_saved_io[0], cp_saved_io[1]);
    /* Unknown mux or non-idle bus: do not take over or try bus recovery. */
    for (unsigned i = 0; i < 2; i++) {
        unsigned function = (cp_saved_cfg[i] >> 2) & 15;
        if ((function != 0 && function != 1) || !(cp_saved_io[i] & 1)) {
            rc_tell("charge-cp1: probe skipped, pin%u mux=%u level=%u\n",
                cp_pins[i], function, cp_saved_io[i] & 1);
            return;
        }
    }
    cp_release(51, true); cp_release(52, true); delay(20);
    if (!cp_level(51) || !cp_level(52)) {
        cp_restore_pins();
        rc_tell("charge-cp1: probe skipped, bus not idle after release; pins restored\n");
        return;
    }
    uint8_t type = 0xff, subtype = 0xff;
    bool id = cp_read_only(0x0104, &type) && cp_read_only(0x0105, &subtype);
    bool accepted = id && type == 0x51 && subtype == 0x23;
    rc_tell("charge-cp1: I2C0x10 type=%#x subtype=%#x read_ok=%u SMB1390=%u\n",
        type, subtype, id, accepted);
    if (accepted) {
        static const uint16_t regs[] = {0x0100,0x0101,0x0102,0x0103,
            0x1004,0x1005,0x1006,0x1007,0x1008,0x1010,0x1020,0x1030,0x1032,0x1033,0x1034};
        for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
            uint8_t value = 0xff;
            bool ok = cp_read_only(regs[i], &value);
            rc_tell("charge-cp1: read r%04x=%#x ok=%u\n", regs[i], value, ok);
            if (!ok) break;
        }
    }
    cp_restore_pins();
    rc_tell("charge-cp1: probe ended, pins restored; CP not enabled, no QC voltage request\n");
}
