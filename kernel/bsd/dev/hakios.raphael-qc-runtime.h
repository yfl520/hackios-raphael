/* USBIN-only bus adapter. No DCDC, TYPEC, PHY, or CP configuration writes. */
static int rq_read(unsigned char reg, unsigned char *out)
{
    return cm_get(reg, out);
}

static int rq_write_byte(unsigned char reg, unsigned char value)
{
    if (!cm_owner()) return 0;
    if (!cm_leaving) hakios_spmi_lock();
    *(volatile uint32_t *)(cm_chan + 0x10) = value;
    *(volatile uint32_t *)cm_chan = (uint32_t)reg << 4;
    int ok = cm_done(cm_chan);
    if (!cm_leaving) hakios_spmi_unlock();
    return ok;
}

static int rq_mask(unsigned char reg, unsigned char mask, unsigned char value)
{
    /* Explicit stage-specific whitelist, including disabling autonomous mode. */
    if (!((reg == 0x62 && (mask == 0x64 || mask == 0x20)) ||
          (reg == 0x60 && mask == 0x0f) ||
          (reg == 0x5b && mask == 0xc0))) return 0;
    unsigned char before, back;
    if (!cm_owner() || !cm_get(reg, &before)) return 0;
    unsigned char next = (before & ~mask) | (value & mask);
    if (next != before && !rq_write_byte(reg, next)) return 0;
    return cm_get(reg, &back) && (back & mask) == (next & mask);
}

static int rq_command(unsigned char reg, unsigned char value)
{
    /* Self-clearing command bits: report transport completion, not a fake
     * persistent readback. No positive QC pulse or high-voltage command. */
    if (!((reg == 0x41 && value == 1) || (reg == 0x43 && value == 8))) return 0;
    return rq_write_byte(reg, value);
}

#include "hakios.raphael-qc-detect.h"
static struct raphael_qc_detect rq_state;
