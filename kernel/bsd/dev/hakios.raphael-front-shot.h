struct raphael_front_shot { struct raphael_front_sensor sensor; bool retain; uint32_t lines, gain, length; };

static bool
raphael_front_controls(struct raphael_i2c_bus *bus, uint32_t lines, uint32_t gain, uint32_t length)
{
    if (length < 2064 || length > 4128 || lines < 4 || lines > length - 16 || gain < 256 || gain > 4096) return false;
    const struct {uint16_t address, value;} controls[] = {
        {0x340,length}, {0x202,lines}, {0x204,gain / 8}
    };
    bool ok = RAPHAEL_FRONT_APPLY(bus, holdon);
    for (unsigned i = 0; i < 3 && ok; i++) ok = raphael_front_write(bus, controls[i].address, controls[i].value);
    bool released = RAPHAEL_FRONT_APPLY(bus, holdoff);
    if (!ok || !released) return false;
    for (unsigned i = 0; i < 3; i++) {
        uint16_t back = 0;
        if (!raphael_front_read(bus, controls[i].address, &back) || back != controls[i].value) return false;
    }
    return true;
}

static bool
raphael_front_shot_run(struct raphael_i2c_bus *bus, void *context)
{
    struct raphael_front_shot *shot = context;
    struct raphael_front_capture_clocks clocks = {0};
    struct raphael_front_phy phy = {0};
    bool ok = raphael_front_prepare(&shot->sensor, bus);
    if (ok) ok = raphael_front_controls(bus, shot->lines ? shot->lines : 2000,
        shot->gain ? shot->gain : 512, shot->length ? shot->length : 2064);
    if (ok) ok = raphael_front_capture_clocks_begin(&clocks);
    if (ok) ok = raphael_front_phy_begin(&phy);
    if (ok) ok = raphael_front_frame_capture(&shot->sensor, bus, &shot->retain);
    bool stopped = raphael_front_stop(&shot->sensor, bus);
    bool clean = !shot->retain && stopped && raphael_front_phy_end(&phy);
    clean = clean && raphael_front_capture_clocks_end(&clocks);
    shot->retain |= !clean;
    say("raphael-front: bounded shot=%u sensor-off=%u receiver-clean=%u; not Camera.app preview\n", ok, stopped, clean);
    return ok && clean;
}
