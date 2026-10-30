/* V5 current control plus QC2 bounded authentication at configured 5V.
 * See qc-runtime/detect for additional USBIN caps and one APSD rerun.
 * Input ceiling 1A, original AICL and FCC/FV/JEITA/watchdog/thermal settings
 * retained. No PHY or CP configuration writes, no positive QC pulses.
 */
#include "hakios.raphael-charge-policy.h"
static vm_offset_t cm_cfg, cm_obs, cm_chan;
static unsigned cm_apid;
static bool cm_enabled, cm_ready, cm_modified, cm_bad, cm_leaving;
static uint8_t cm_saved[4];
static const uint8_t cm_regs[4] = {0x70, 0x66, 0x42, 0x65};
static const uint8_t cm_masks[4] = {0xff, 0x01, 0x01, 0x10};
static uint32_t cm_last_sequence;
static uint64_t cm_last_update;
static struct raphael_charge_gate cm_gate;
static unsigned cm_adopted_source;

static bool cm_owner(void)
{
    return cm_ready &&
        ((*(volatile uint32_t *)(rc_core + 0x900 + 4 * cm_apid) >> 8) & 0xfff) == 0x213 &&
        (*(volatile uint32_t *)(cm_cfg + 0x700 + 4 * cm_apid) & 7) == 0;
}

static bool cm_done(vm_offset_t channel)
{
    uint64_t duration, begin = mach_absolute_time();
    nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &duration);
    uint32_t status;
    do { status = *(volatile uint32_t *)(channel + 8); }
    while (!(status & 1) && mach_absolute_time() - begin < duration);
    return (status & 0xf) == 1;
}

static bool cm_get(uint8_t reg, uint8_t *value)
{
    if (!cm_obs) return false;
    if (!cm_leaving) hakios_spmi_lock();
    *(volatile uint32_t *)cm_obs = (1u << 27) | (uint32_t)reg << 4;
    bool ok = cm_done(cm_obs);
    if (ok) *value = (uint8_t)*(volatile uint32_t *)(cm_obs + 0x18);
    if (!cm_leaving) hakios_spmi_unlock();
    return ok;
}

static bool cm_put(unsigned index, uint8_t value)
{
    if (index >= 4 || !cm_owner()) return false;
    if (index == 0 && value != 20 && value != cm_saved[0]) return false;
    uint8_t before, back;
    if (!cm_get(cm_regs[index], &before)) return false;
    uint8_t next = (before & ~cm_masks[index]) | (value & cm_masks[index]);
    if (before == next) return true;
    if (!cm_leaving) hakios_spmi_lock();
    *(volatile uint32_t *)(cm_chan + 0x10) = next;
    *(volatile uint32_t *)cm_chan = (uint32_t)cm_regs[index] << 4;
    bool ok = cm_done(cm_chan);
    if (!cm_leaving) hakios_spmi_unlock();
    return ok && cm_get(cm_regs[index], &back) &&
        (back & cm_masks[index]) == (next & cm_masks[index]);
}

#include "hakios.raphael-qc-runtime.h"

static void cm_restore(void)
{
    if (!cm_modified) return;
    bool ok = cm_put(0, cm_saved[0]);
    for (unsigned i = 1; i < 4; i++) if (!cm_put(i, cm_saved[i])) ok = false;
    rc_tell("charge-control5: restore ok=%u saved_icl_ma=%u\n", ok, cm_saved[0] * 50);
    if (ok) { cm_modified = false; cm_adopted_source = 0; }
    else cm_bad = true;
}

static void cm_probe(void)
{
    if (!cm_enabled || !rc_ready || !hakios_is_raphael()) return;
    cm_cfg = ml_io_map(0x0c40a000, 0x1000);
    if (!cm_cfg) return;
    for (unsigned n = 0; n < 512; n++) {
        if (((*(volatile uint32_t *)(rc_core + 0x900 + 4 * n) >> 8) & 0xfff) != 0x213 ||
            (*(volatile uint32_t *)(cm_cfg + 0x700 + 4 * n) & 7) != 0) continue;
        cm_apid = n;
        cm_obs = ml_io_map(0x0e600000UL + 0x80UL * n, 0x20);
        cm_chan = ml_io_map(0x0c600000UL + 0x10000UL * n, 0x20);
        uint8_t type = 0xff, subtype = 0xff;
        cm_ready = cm_chan && cm_get(4, &type) && cm_get(5, &subtype) && type == 2 && subtype == 0x83;
        rc_tell("charge-control5: USBIN APID=%u ready=%u type=%#x subtype=%#x; 1A cap SW_HC; TypeC read-only; QC2 may enable authentication/rerun APSD; no PHY writes\n",
            n, cm_ready, type, subtype);
        return;
    }
    rc_tell("charge-control5: no AP-owned USBIN; no writes\n");
}

static void cm_poll(void)
{
    if (!cm_ready || cm_leaving) return;
    if (cm_bad) { cm_restore(); return; }
    struct loader_battery sample = {0};
    bool valid = battery_snapshot(&sample) && (sample.flags & LOADER_BATTERY_VALID) &&
        (sample.flags & LOADER_BATTERY_PRESENT) && !(sample.flags & LOADER_BATTERY_SOC_ESTIMATED);
    uint64_t now = mach_absolute_time(), freshness, now_ns;
    nanoseconds_to_absolutetime(10ULL * NSEC_PER_SEC, &freshness);
    absolutetime_to_nanoseconds(now, &now_ns);
    if (valid && sample.sequence != cm_last_sequence) {
        cm_last_sequence = sample.sequence;
        cm_last_update = now;
    }
    valid = valid && cm_last_update && now - cm_last_update < freshness;
    uint8_t plugin = 0, apsd = 0, result = 0, thermal = 0xff, enable = 0, pause = 1, aicl = 0;
    uint8_t suspend = 1, otg = 1, options = 0, snk = 0, misc = 0, legacy = 0;
    bool read = cm_get(0x10, &plugin) && cm_get(7, &apsd) && cm_get(8, &result) &&
        rc_read(0, 0x0d, &thermal) && rc_read(0, 0x42, &enable) &&
        rc_read(0, 0x43, &pause) && cm_get(0x80, &aicl) &&
        cm_get(0x40, &suspend) && rc_read(1, 0x40, &otg) && cm_get(0x62, &options);
    bool typec_valid = rc_typec_read(6, &snk) && rc_typec_read(0x0b, &misc) && rc_typec_read(0x0d, &legacy);
    unsigned source = read ? raphael_charge_class(plugin, apsd, result, thermal, enable, pause,
        aicl, valid, valid ? sample.temperature_c : -273, valid ? sample.voltage_mv : 0,
        suspend, otg, options, typec_valid, snk, misc, legacy) : 0;
    /* QC2 lets the hardware identify a stable timed-out FLOAT or an OCP
     * attachment as well as a DCP. It does not relabel FLOAT or change V5's
     * current gate. SDP/CDP/mixed/enumerated hosts remain excluded.
     * Keep the hardware TypeC VBUS_ERROR indication in diagnostics;
     * it appears on this board even with measured working 5V DCP charging.
     * It is not relabelled as a successful 27W connection. */
    unsigned rp = snk & 0x7f;
    bool qc_safe = read && typec_valid && valid &&
        (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) &&
        (plugin & 0x1f) == 0x10 && !(apsd & 0x10) &&
        !(thermal & 0x3f) && (enable & 1) && !(pause & 1) &&
        (aicl & 4) && !(suspend & 1) && !(otg & 1) && (options & 8) &&
        raphael_charge_temperature_ok(sample.temperature_c) &&
        sample.voltage_mv >= 3400 && sample.voltage_mv <= 4200 &&
        (misc & 9) == 9 && !(misc & 0xc0) && !(legacy & 1) &&
        (rp == 2 || rp == 4 || rp == 8) && !cm_bad;
    unsigned old_phase = rq_state.phase;
    rq_tick(&rq_state, now_ns, read, (plugin & 0x10) != 0, qc_safe,
        rq_start_class(apsd, result) != 0, apsd, result);
    if (old_phase != rq_state.phase)
        rc_tell("charge-qc3t: phase=%u -> %u APSD=%#x result=%#x used=%u modified=%u stopped=%u restore_failed=%u; non-PD identification, 5V caps, no 27W claim\n",
            old_phase, rq_state.phase, apsd, result, rq_state.used,
            rq_state.modified, rq_state.stopped, rq_state.restore_failed);
    static unsigned tick;
    if (tick++ % 30 == 0) {
        uint8_t q[4] = {0xff,0xff,0xff,0xff};
        const uint8_t qr[4] = {0x09,0x44,0x5b,0x60};
        unsigned qvalid = 0;
        for (unsigned i = 0; i < 4; i++) if (cm_get(qr[i], &q[i])) qvalid |= 1u << i;
        rc_tell("charge-qc3t: phase=%u safe=%u used=%u stopped=%u restore_failed=%u valid=%#x qc09=%#x override44=%#x max5b=%#x allow60=%#x options62=%#x; CP off\n",
            rq_state.phase, qc_safe, rq_state.used, rq_state.stopped,
            rq_state.restore_failed, qvalid, q[0], q[1], q[2], q[3], options);
        rc_tell("charge-control5: source=%u APSD=%#x result=%#x plugin=%#x gauge=%u TypeC_valid=%u snk=%#x misc=%#x legacy=%#x adopted=%u bad=%u\n",
            source, apsd, result, plugin, valid, typec_valid, snk, misc, legacy, cm_modified, cm_bad);
        /* Diagnostic reads only. Keep V5 eligibility and current limits
         * byte-for-byte unchanged until the anomalous VBUS state is understood.
         * 09/0a/10/44: FSM state, SM comparators, RT interrupt status, role cfg.
         * 0a comparator interpretation is supplementary upstream evidence,
         * not a validated per-board charging permission. */
        uint8_t tc[4] = {0xff, 0xff, 0xff, 0xff};
        const uint8_t regs[4] = {0x09, 0x0a, 0x10, 0x44};
        unsigned tc_valid = 0;
        for (unsigned i = 0; i < 4; i++)
            if (rc_typec_read(regs[i], &tc[i])) tc_valid |= 1u << i;
        rc_tell("charge-guard6: read=%u gauge=%u flags=%#x polls=%u T=%d V=%d thermal=%#x enable=%#x pause=%#x aicl=%#x suspend=%#x otg=%#x options=%#x tc_valid=%#x tc09=%#x tc0a=%#x tc10=%#x tc44=%#x\n",
            read, valid, sample.flags, sample.polls, sample.temperature_c,
            sample.voltage_mv, thermal, enable, pause, aicl, suspend, otg,
            options, tc_valid, tc[0], tc[1], tc[2], tc[3]);
    }
    if (cm_modified && source != cm_adopted_source) cm_restore();
    if (cm_bad || !raphael_charge_settled(&cm_gate, source, now_ns) || cm_modified) return;
    for (unsigned i = 0; i < 4; i++) if (!cm_get(cm_regs[i], &cm_saved[i])) { cm_bad = true; return; }
    if (!cm_saved[0] || cm_saved[0] > 10) return;
    cm_modified = true;
    cm_adopted_source = source;
    /* Android: program ICL, then USBIN_MODE_CHG, clear USB51 override, set
     * ICL_OVERRIDE_AFTER_APSD. Keep unrelated USB2/3 selection bits intact. */
    bool ok = cm_put(0, 20) && cm_put(1, 1) && cm_put(2, 0) && cm_put(3, 0x10);
    uint8_t back[4] = {0};
    for (unsigned i = 0; i < 4; i++) if (!cm_get(cm_regs[i], &back[i])) ok = false;
    rc_tell("charge-control5: source=%u apply=%u saved=%umA requested=1000mA readback70=%#x 66=%#x 42=%#x 65=%#x; actual current needs ADC\n",
        source, ok, cm_saved[0] * 50, back[0], back[1], back[2], back[3]);
    if (!ok) { cm_restore(); cm_bad = true; }
}

void hakios_battery_quiesce(void);
void hakios_battery_quiesce(void)
{
    cm_leaving = true;
    bool qc_stopped = rq_stop(&rq_state);
    rc_tell("charge-qc3t: quiesce disabled=%u; attached session keeps 5V caps\n", qc_stopped);
    cm_restore();
}
