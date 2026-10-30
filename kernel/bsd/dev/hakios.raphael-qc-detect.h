/* QC3 candidate: authentication prerequisite for later high-power charging.
 * Pinned SM8150 smb5-reg.h/smb5-lib.c; PLAN.md records provenance/limits.
 * This stage restricts adapter allowance and QC2 ceiling to 5V, disables
 * autonomous mode, and never sends DP increments or 9V/12V requests.
 * Register acceptance is NOT proof of protocol success or measured power.
 * Callbacks must verify AP ownership and every persistent masked write.
 */
enum { RQ_IDLE, RQ_AUTH, RQ_QC2, RQ_QC3, RQ_FALLBACK };
struct raphael_qc_detect {
    unsigned phase, used, modified, restore_failed, stopped, eligible_class;
    unsigned long long eligible_since, started;
    unsigned char saved[3];
};
static const unsigned char rq_regs[3] = {0x62, 0x60, 0x5b};
static const unsigned char rq_masks[3] = {0x64, 0x0f, 0xc0};

static unsigned rq_start_class(unsigned apsd, unsigned result)
{
    if ((apsd & 0x11) != 1) return 0;
    if (result == 8 || result == 2) return 1; /* DCP / Android OCP-as-DCP */
    /* A timed-out FLOAT attachment is only a candidate for hardware protocol
     * identification, never relabelled as DCP or granted extra current. */
    if (result == 0x10 && (apsd & 0x20)) return 2;
    return 0;
}

static int rq_stop(struct raphael_qc_detect *s)
{
    if (!s->modified || s->stopped) return 1;
    /* Keep the 5V caps while attached, including when a write fails. Never
     * restore inherited high-voltage allowances during an active session. */
    int ok = rq_mask(0x62, 0x20, 0);
    unsigned char result = 0;
    int read = rq_read(0x08, &result);
    if (!read || result == 0x28 || result == 0x48)
        if (!rq_command(0x43, 0x08)) ok = 0;
    if (!rq_mask(0x62, 0x64, 0)) ok = 0;
    s->phase = RQ_FALLBACK;
    s->stopped = ok;
    return ok;
}

static int rq_detached(struct raphael_qc_detect *s)
{
    if (s->modified) {
        /* Disable negotiation first; restore options last, and only after a
         * positive detach. A read failure is not treated as a detach. */
        int ok = rq_mask(0x62, 0x64, 0);
        if (!ok) { s->restore_failed = 1; return 0; }
        for (unsigned i = 1; i < 3; i++)
            if (!rq_mask(rq_regs[i], rq_masks[i], s->saved[i])) ok = 0;
        if (ok && !rq_mask(0x62, 0x64, s->saved[0])) ok = 0;
        if (!ok) { s->restore_failed = 1; return 0; }
    }
    *s = (struct raphael_qc_detect){0};
    return 1;
}

static int rq_begin(struct raphael_qc_detect *s, unsigned long long now)
{
    s->used = 1;
    unsigned char current = 0, override = 0;
    if (!rq_read(0x70, &current) || !current || current > 20) goto reject;
    if (!rq_read(0x44, &override)) goto reject;
    for (unsigned i = 0; i < 3; i++)
        if (!rq_read(rq_regs[i], &s->saved[i])) goto reject;
    /* Do not take over an inherited QC/PD contract or unknown allowance. */
    if ((s->saved[0] & 0x64) || (override & 0x0f)) goto reject;
    unsigned allow = s->saved[1] & 0x0f;
    if (allow != 0 && allow != 2 && allow != 3 && allow != 4 &&
        allow != 5 && allow != 6 && allow != 7 && allow != 8 && allow != 12)
        goto reject;
    s->modified = 1;
    /* Non-PD mode keeps adapter override NULL; never write reg44 here.
     * Both allowance and QC2 maximum retain their 5V caps.
     * Every persistent operation includes readback. Authentication starts only if
     * every cap succeeds. The inherited current limit remains <=1A. */
    if (!rq_mask(0x62, 0x64, 0) || !rq_mask(0x60, 0x0f, 0) ||
        !rq_mask(0x5b, 0xc0, 0) ||
        !rq_mask(0x62, 0x64, 0x44) || !rq_command(0x41, 1)) {
        rq_stop(s);
        return 0;
    }
    s->started = now;
    s->phase = RQ_AUTH;
    return 1;
reject:
    s->phase = RQ_FALLBACK;
    return 0;
}

static void rq_tick(struct raphael_qc_detect *s, unsigned long long now,
    int connection_known, int present, int safe, int eligible,
    unsigned apsd, unsigned result)
{
    if (connection_known && !present) { rq_detached(s); return; }
    if (s->restore_failed) { rq_stop(s); return; }
    if (!safe) {
        s->eligible_since = 0;
        s->eligible_class = 0;
        if (s->modified) rq_stop(s);
        return;
    }
    if (s->modified && s->phase == RQ_FALLBACK) {
        /* Reassert disable if a previous bus error prevented cleanup. */
        rq_stop(s);
        return;
    }
    if (s->phase == RQ_AUTH || s->phase == RQ_QC2 || s->phase == RQ_QC3) {
        unsigned char qc_voltage = 0xff;
        if (!rq_read(0x09, &qc_voltage) || (qc_voltage & 6) || (apsd & 0x10)) {
            rq_stop(s);
            return;
        }
        if (((apsd & 3) == 3 && result == 0x28) ||
            ((apsd & 5) == 5 && result == 0x48)) {
            s->phase = result == 0x48 ? RQ_QC3 : RQ_QC2;
            return;
        }
        if (s->phase != RQ_AUTH || now < s->started ||
            now - s->started >= 25000000000ULL) rq_stop(s);
        return;
    }
    unsigned start_class = rq_start_class(apsd, result);
    if (s->used || !eligible || !start_class) {
        s->eligible_since = 0;
        s->eligible_class = 0;
        return;
    }
    if (!s->eligible_since || now < s->eligible_since ||
        s->eligible_class != start_class) {
        s->eligible_since = now;
        s->eligible_class = start_class;
        return;
    }
    unsigned long long wait = start_class == 2 ? 60000000000ULL : 15000000000ULL;
    if (now - s->eligible_since >= wait) rq_begin(s, now);
}
