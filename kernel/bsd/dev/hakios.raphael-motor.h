/* Raphael DRV8846. Pin/rail route comes from this phone's DT; waveform
 * timing follows pinned Xiaomi drv8846.c and PMIC LPG register contracts.
 * PWM is PM8150L GPIO10, not SoC GPIO10. No battery/charger registers. */
struct raphael_motor_channel { uint16_t ppid, channel; vm_offset_t observer, writer; };
static struct {
    vm_offset_t core, config;
    struct raphael_motor_channel pwm, pin;
    struct raphael_probe_pin gpio[5]; /* mode0, mode1, sleep, direction, power */
    uint8_t pwm_saved[7], pin_saved[6], subtype;
    bool ready, dirty, up, fault;
} raphael_motor;
static const uint8_t raphael_motor_pwm_regs[] = {0x40,0x41,0x42,0x43,0x44,0x45,0x46};
static const uint8_t raphael_motor_pin_regs[] = {0x40,0x41,0x42,0x44,0x45,0x46};

static bool
raphael_motor_owned(struct raphael_motor_channel *c)
{
    return c->observer && c->writer &&
        ((rd(raphael_motor.core + ARB_CHANNEL(c->channel)) >> 8) & 0xfff) == c->ppid &&
        (rd(raphael_motor.config + ARB_OWNER(c->channel)) & 7) == ARB_SYSTEM;
}

static bool
raphael_motor_find(struct raphael_motor_channel *c, uint16_t ppid)
{
    c->ppid = ppid;
    for (unsigned i = 0; i < ARB_MAX_CHANNELS; i++) {
        if (((rd(raphael_motor.core + ARB_CHANNEL(i)) >> 8) & 0xfff) != ppid ||
            (rd(raphael_motor.config + ARB_OWNER(i)) & 7) != ARB_SYSTEM) continue;
        c->channel = i;
        c->observer = raphael_camera_map(ARB_OBSERVER_PHYS + 0x80ul * i, 0x80);
        c->writer = raphael_camera_map(ARB_CHNLS_PHYS + 0x10000ul * i, 0x1000);
        say("raphael-motor: PPID=%#x channel=%u AP-owned=%u\n", ppid, i, raphael_motor_owned(c));
        return raphael_motor_owned(c);
    }
    say("raphael-motor: no AP-owned channel for PPID=%#x; no writes\n", ppid);
    return false;
}

static bool
raphael_motor_read(struct raphael_motor_channel *c, uint8_t reg, uint8_t *value)
{
    if (!raphael_motor_owned(c)) return false;
    const char *problem = NULL;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        lock();
        wr(c->observer + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
        problem = wait_done(c->observer);
        *value = (uint8_t)rd(c->observer + OBSERVER_DATA0);
        unlock();
        if (!problem || strcmp(problem, DROPPED)) break;
    }
    return problem == NULL;
}

static bool
raphael_motor_write(struct raphael_motor_channel *c, uint8_t reg, uint8_t value)
{
    if (!raphael_motor_owned(c)) return false;
    bool allowed = false;
    if (c == &raphael_motor.pwm) allowed = reg >= 0x40 && reg <= 0x47;
    if (c == &raphael_motor.pin)
        for (unsigned i = 0; i < 6; i++) allowed |= reg == raphael_motor_pin_regs[i];
    if (!allowed) return false;
    const char *problem = NULL;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        lock();
        wr(c->writer + CHANNEL_WDATA0, value);
        wr(c->writer + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
        problem = wait_done(c->writer);
        unlock();
        if (!problem || strcmp(problem, DROPPED)) break;
    }
    uint8_t back = 0;
    /* PWM_SYNC is a command, not a persistent register to read back. */
    bool ok = !problem && (reg == 0x47 || (raphael_motor_read(c, reg, &back) && back == value));
    if (!ok) say("raphael-motor: PPID=%#x reg=%#x write=%#x read=%#x failed transfer=%s status=%#x\n", c->ppid, reg, value, back, problem ? problem : "completed", rd(c->writer + CHANNEL_STATUS));
    return ok;
}

static bool
raphael_motor_init(void)
{
    if (raphael_motor.ready) return true;
    raphael_motor.core = raphael_camera_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
    raphael_motor.config = raphael_camera_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
    if (!raphael_motor.core || !raphael_motor.config ||
        !raphael_motor_find(&raphael_motor.pwm, 0x5bd) ||
        !raphael_motor_find(&raphael_motor.pin, 0x4c9)) return false;
    uint8_t type = 0, pin_type = 0, pin_subtype = 0;
    if (!raphael_motor_read(&raphael_motor.pwm, 4, &type) ||
        !raphael_motor_read(&raphael_motor.pwm, 5, &raphael_motor.subtype) ||
        !raphael_motor_read(&raphael_motor.pin, 4, &pin_type) ||
        !raphael_motor_read(&raphael_motor.pin, 5, &pin_subtype)) return false;
    if ((raphael_motor.subtype != 0x0b && raphael_motor.subtype != 0x11) ||
        pin_type != 0x10 || (pin_subtype != 0x10 && pin_subtype != 0x11)) {
        say("raphael-motor: unsupported PWM=%#x/%#x GPIO=%#x/%#x; no writes\n",
            type, raphael_motor.subtype, pin_type, pin_subtype);
        return false;
    }
    for (unsigned i = 0; i < 7; i++)
        if (!raphael_motor_read(&raphael_motor.pwm, raphael_motor_pwm_regs[i], &raphael_motor.pwm_saved[i])) return false;
    for (unsigned i = 0; i < 6; i++)
        if (!raphael_motor_read(&raphael_motor.pin, raphael_motor_pin_regs[i], &raphael_motor.pin_saved[i])) return false;
    say("raphael-motor: saved PWM40..46=%02x %02x %02x %02x %02x %02x %02x\n",
        raphael_motor.pwm_saved[0], raphael_motor.pwm_saved[1], raphael_motor.pwm_saved[2],
        raphael_motor.pwm_saved[3], raphael_motor.pwm_saved[4], raphael_motor.pwm_saved[5], raphael_motor.pwm_saved[6]);
    if (raphael_motor.pwm_saved[6] & 0x82) {
        say("raphael-motor: PWM/ramp already active; refusing takeover\n"); return false;
    }
    const uint32_t physical[] = {0x03515000,0x03921000,0x03923000,0x03909000,0x03d82000};
    for (unsigned i = 0; i < 5; i++) {
        struct raphael_probe_pin *p = &raphael_motor.gpio[i];
        p->base = raphael_camera_map(physical[i], 0x1000);
        if (!p->base) return false;
        p->cfg = rd(p->base); p->io = rd(p->base + 4);
        if (p->cfg & 0x3c) return false;
    }
    if (raphael_motor.gpio[2].io & 2) { say("raphael-motor: sleep pin high; motor busy\n"); return false; }
    raphael_motor.ready = true;
    say("raphael-motor: idle snapshot verified PWM-subtype=%#x GPIO-subtype=%#x; no movement\n",
        raphael_motor.subtype, pin_subtype);
    return true;
}

static void
raphael_motor_emergency_stop(void)
{
    if (!raphael_motor.dirty) return;
    /* The physical driver's sleep and power gates stop steps even if an
     * SPMI transaction cannot complete during panic/loader shutdown. */
    for (unsigned i = 2; i < 5; i += 2) if (raphael_motor.gpio[i].base) {
        wr(raphael_motor.gpio[i].base + 4, rd(raphael_motor.gpio[i].base + 4) & ~2u);
        wr(raphael_motor.gpio[i].base, (rd(raphael_motor.gpio[i].base) & ~0x3ffu) | 0x200u);
    }
    __sync_synchronize();
}

static bool
raphael_motor_pwm(unsigned period_ns)
{
    const unsigned sizes[] = {6,9}, clocks[] = {1024,32768,19200000}, divisors[] = {1,3,5,6};
    uint64_t best_error = ~0ull, best_period = 0;
    unsigned size = 0, clock = 0, divider = 0, exponent = 0;
    for (unsigned s = 0; s < 2; s++) for (unsigned c = 0; c < 3; c++)
        for (unsigned d = 0; d < 4; d++) for (unsigned e = 0; e < 8; e++) {
            uint64_t actual = ((1ull << (sizes[s] + e)) * divisors[d] * 1000000000ull + clocks[c]/2) / clocks[c];
            uint64_t error = actual > period_ns ? actual - period_ns : period_ns - actual;
            if (error <= best_error) {
                best_error = error; best_period = actual; size = s; clock = c; divider = d; exponent = e;
            }
        }
    if (best_error * 100 > (uint64_t)period_ns * 5) return false;
    unsigned shift = raphael_motor.subtype == 0x0b ? 2 : 4;
    uint8_t cfg = (raphael_motor.pwm_saved[1] & ~((1u << shift) | 3u)) | (size << shift) | (clock + 1);
    uint8_t freq = (raphael_motor.pwm_saved[2] & ~0x67u) | divider << 5 | exponent;
    unsigned value = 1u << (sizes[size] - 1);
    /* This phone's stand-alone PWM (0x0b) implements only bit7 here.
     * With motor sleep/power low, 0x04/0x20 read as zero, 0x80 reads
     * 0x80 and 0xa4 reads 0x80. Keep exact readback verification. */
    uint8_t source = raphael_motor.subtype == 0x0b ? 0u : 4u;
    uint8_t enable = (raphael_motor.pwm_saved[6] & ~0x86u) | source;
    bool ok = raphael_motor_write(&raphael_motor.pwm, 0x46, enable) &&
        raphael_motor_write(&raphael_motor.pwm, 0x41, cfg) &&
        raphael_motor_write(&raphael_motor.pwm, 0x42, freq) &&
        raphael_motor_write(&raphael_motor.pwm, 0x43, raphael_motor.pwm_saved[3] | 0x20) &&
        raphael_motor_write(&raphael_motor.pwm, 0x45, (raphael_motor.pwm_saved[5] & ~1u) | (value >> 8)) &&
        raphael_motor_write(&raphael_motor.pwm, 0x44, value & 255) &&
        raphael_motor_write(&raphael_motor.pwm, 0x47, 1) &&
        raphael_motor_write(&raphael_motor.pwm, 0x46, enable | 0x80);
    say("raphael-motor: requested period=%u actual=%llu ns configured=%u\n", period_ns, best_period, ok);
    return ok;
}

static bool
raphael_motor_restore(void)
{
    bool ok = true;
    raphael_motor_emergency_stop();
    if (!raphael_motor_write(&raphael_motor.pwm, 0x46, raphael_motor.pwm_saved[6] & ~0x82u)) ok = false;
    for (unsigned i = 0; i < 6; i++)
        if (!raphael_motor_write(&raphael_motor.pwm, raphael_motor_pwm_regs[i], raphael_motor.pwm_saved[i])) ok = false;
    if (!raphael_motor_write(&raphael_motor.pwm, 0x47, 1)) ok = false;
    for (unsigned i = 0; i < 6; i++)
        if (!raphael_motor_write(&raphael_motor.pin, raphael_motor_pin_regs[i], raphael_motor.pin_saved[i])) ok = false;
    for (unsigned i = 0; i < 5; i++) {
        struct raphael_probe_pin *p = &raphael_motor.gpio[i];
        wr(p->base + 4, p->io & 2); wr(p->base, p->cfg);
        if (rd(p->base) != p->cfg || ((rd(p->base + 4) ^ p->io) & 2)) ok = false;
    }
    if (!ok) raphael_motor_emergency_stop();
    else raphael_motor.dirty = false;
    return ok;
}

static bool
raphael_motor_pwm_diagnostic(void)
{
    if (!raphael_motor.ready || raphael_motor.up) return false;
    raphael_motor.dirty = true;
    bool safe = raphael_probe_gpio(&raphael_motor.gpio[2], false) &&
                raphael_probe_gpio(&raphael_motor.gpio[4], false);
    if (!safe) { raphael_motor_emergency_stop(); return false; }
    say("raphael-motor: PWM diagnostic: sleep=0 power=0; physical movement gated\n");
    bool cfg = raphael_motor_write(&raphael_motor.pwm, 0x41, raphael_motor.pwm_saved[1] ^ 1);
    bool reset = raphael_motor_write(&raphael_motor.pwm, 0x41, raphael_motor.pwm_saved[1]);
    const uint8_t controls[] = {0x04,0x20,0x80,0xa4,0};
    for (unsigned i = 0; i < sizeof(controls); i++) {
        bool ok = raphael_motor_write(&raphael_motor.pwm, 0x46, controls[i]);
        uint8_t back = 0xff;
        spin_us(1000);
        bool read = raphael_motor_read(&raphael_motor.pwm, 0x46, &back);
        say("raphael-motor: gated diagnostic control=%#x immediate=%u delayed-read=%u/%#x\n", controls[i], ok, read, back);
    }
    bool restored = raphael_motor_restore();
    say("raphael-motor: gated diagnostic config-write=%u config-reset=%u restored=%u; no UP/DOWN requested\n",cfg,reset,restored);
    return restored;
}

static bool
raphael_motor_move(bool up)
{
    if (!raphael_motor.ready || raphael_motor.fault || raphael_motor.up == up) return false;
    raphael_motor.dirty = true;
    bool ok = raphael_probe_gpio(&raphael_motor.gpio[2], false) &&
        raphael_probe_gpio(&raphael_motor.gpio[0], false) &&
        raphael_probe_gpio(&raphael_motor.gpio[1], true) &&
        raphael_probe_gpio(&raphael_motor.gpio[3], !up) &&
        raphael_probe_gpio(&raphael_motor.gpio[4], true);
    /* LV/MV FUNC1 selector is 2 (normal=0, paired=1). */
    uint8_t pin[] = {1,1,5,2,3,0x80};
    for (unsigned i = 0; i < 6 && ok; i++)
        ok = raphael_motor_write(&raphael_motor.pin, raphael_motor_pin_regs[i], pin[i]);
    spin_us(1000);
    if (ok) ok = raphael_motor_pwm(625000) && raphael_probe_gpio(&raphael_motor.gpio[2], true);
    if (ok) { wait_ms(50); ok = raphael_motor_pwm(52083); }
    if (ok) { wait_ms(up ? 720 : 725); ok = raphael_motor_pwm(625000); }
    if (ok) wait_ms(50);
    bool stopped = raphael_probe_gpio(&raphael_motor.gpio[2], false);
    stopped = raphael_motor_write(&raphael_motor.pwm, 0x46, (raphael_motor.pwm_saved[6] & ~0x86u) |
        (raphael_motor.subtype == 0x0b ? 0u : 4u)) && stopped;
    if (ok && stopped) raphael_motor.up = up;
    else { raphael_motor.fault = true; raphael_motor_emergency_stop(); }
    say("raphael-motor: direction=%s sequence=%u stopped=%u; mechanical position needs observation\n", up ? "UP" : "DOWN", ok, stopped);
    return ok && stopped;
}
