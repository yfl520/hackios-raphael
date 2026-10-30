/* S5K3T2 uses 16-bit register data, including Samsung's indirect 0x6f12
 * port. Keep each OEM word write intact, with the original bank sequence. */
#include "hakios.raphael-front-table.h"
struct raphael_front_sensor { bool prepared, dirty; };

static bool
raphael_front_write(struct raphael_i2c_bus *bus, uint16_t address, uint16_t value)
{
    const uint8_t out[] = {address >> 8, address & 255, value >> 8, value & 255};
    enum raphael_i2c_result r = raphael_i2c_transfer(bus, 0x56, out, sizeof(out), NULL, 0);
    if (r) say("raphael-front: word write %#x=%#x failed result=%u\n", address, value, r);
    return r == RAPHAEL_I2C_OK;
}

static bool
raphael_front_read(struct raphael_i2c_bus *bus, uint16_t address, uint16_t *value)
{
    const uint8_t out[] = {address >> 8, address & 255};
    uint8_t in[2] = {0};
    if (raphael_i2c_transfer(bus, 0x56, out, 2, in, 2) != RAPHAEL_I2C_OK) return false;
    *value = (uint16_t)in[0] << 8 | in[1];
    return true;
}

static bool
raphael_front_apply(struct raphael_i2c_bus *bus, const struct raphael_front_entry *table, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!raphael_front_write(bus, table[i].address, table[i].value)) return false;
        if (table[i].delay_us) spin_us(table[i].delay_us);
    }
    return true;
}
#define RAPHAEL_FRONT_APPLY(bus, stage) \
    raphael_front_apply((bus), raphael_front_##stage, sizeof(raphael_front_##stage)/sizeof(raphael_front_##stage[0]))

static bool
raphael_front_stop(struct raphael_front_sensor *sensor, struct raphael_i2c_bus *bus)
{
    if (!sensor->dirty) { sensor->prepared = false; return true; }
    uint16_t back = 0xffff;
    if (!RAPHAEL_FRONT_APPLY(bus, off) || !raphael_front_read(bus, 0x100, &back) || back) return false;
    sensor->dirty = false; sensor->prepared = false;
    return true;
}

static bool
raphael_front_prepare(struct raphael_front_sensor *sensor, struct raphael_i2c_bus *bus)
{
    uint16_t id = 0;
    if (sensor->prepared || sensor->dirty || !raphael_front_read(bus, 0, &id) || id != 0x3142) return false;
    sensor->dirty = true; /* Any partially received write still needs stream-off. */
    if (!raphael_front_stop(sensor, bus)) return false;
    sensor->dirty = true;
    if (!RAPHAEL_FRONT_APPLY(bus, init) || !RAPHAEL_FRONT_APPLY(bus, mode)) return false;
    const struct {uint16_t address, value;} checks[] = {
        {0x34c,2592}, {0x34e,1940}, {0x342,10880}, {0x340,2064}, {0x114,0x300}, {0x100,0}
    };
    for (unsigned i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
        uint16_t value = 0;
        if (!raphael_front_read(bus, checks[i].address, &value) || value != checks[i].value) {
            say("raphael-front: register verify %#x got=%#x expected=%#x\n", checks[i].address, value, checks[i].value);
            return false;
        }
    }
    sensor->dirty = false; sensor->prepared = true;
    say("raphael-front: S5K3T2 OEM mode1 verified 2592x1940 DPHY4; stream off\n");
    return true;
}

static bool
raphael_front_start(struct raphael_front_sensor *sensor, struct raphael_i2c_bus *bus)
{
    if (!sensor->prepared || sensor->dirty) return false;
    sensor->dirty = true;
    uint16_t back = 0;
    return RAPHAEL_FRONT_APPLY(bus, on) && raphael_front_read(bus, 0x100, &back) && back == 0x100;
}

static bool
raphael_front_configuration_probe(struct raphael_i2c_bus *bus, __unused void *context)
{
    struct raphael_front_sensor sensor = {0};
    bool ok = raphael_front_prepare(&sensor, bus);
    bool stopped = raphael_front_stop(&sensor, bus);
    say("raphael-front: sensor configuration=%u stopped=%u; not a preview test\n", ok, stopped);
    return ok && stopped;
}
