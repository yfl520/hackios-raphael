#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char registers[256];
static unsigned calls, fail_at, persistent_failure, reruns, force5, auths;
static int connected;
static int transfer_ok(void)
{
    calls++;
    return !fail_at || (persistent_failure ? calls < fail_at : calls != fail_at);
}
static int rq_read(unsigned char reg, unsigned char *out)
{
    if (!transfer_ok()) return 0;
    *out = registers[reg];
    return 1;
}
static int rq_mask(unsigned char reg, unsigned char mask, unsigned char value)
{
    assert((reg == 0x62 && (mask == 0x20 || mask == 0x64)) ||
        (reg == 0x60 && mask == 15) ||
        (reg == 0x5b && mask == 0xc0));
    assert(reg != 0x44); /* PD override is read-only in QC3 */
    if (!transfer_ok()) return 0;
    if (reg == 0x62 && (value & 0x44)) {
        assert((value & 0x64) == 0x44);
        assert((registers[0x60] & 15) == 0);
        assert((registers[0x44] & 15) == 0);
        assert((registers[0x5b] & 0xc0) == 0);
        auths++;
    }
    if (connected && reg != 0x62) {
        assert((value & mask) == 0);
    }
    registers[reg] = (registers[reg] & ~mask) | (value & mask);
    /* Include the case where a write may have reached hardware but its
     * readback/transport failed: cleanup must still be attempted. */
    return transfer_ok();
}
static int rq_command(unsigned char reg, unsigned char value)
{
    assert((reg == 0x41 && value == 1) || (reg == 0x43 && value == 8));
    if (!transfer_ok()) return 0;
    if (reg == 0x41) reruns++; else force5++;
    assert(reruns <= 1);
    return 1;
}
#include "hakios.raphael-qc-detect.h"

static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0x62] = 0x18;
    registers[0x60] = 0xa7; /* preserve unrelated bits */
    registers[0x5b] = 0xbb;
    registers[0x44] = 0xa0;
    registers[0x70] = 10;
    registers[0x08] = 8;
    registers[0x09] = 1;
    calls = fail_at = persistent_failure = reruns = force5 = auths = 0;
    connected = 1;
}
static void start(struct raphael_qc_detect *s)
{
    rq_tick(s, 1000000000ULL, 1, 1, 1, 1, 1, 8);
    rq_tick(s, 16000000000ULL, 1, 1, 1, 1, 1, 8);
}
int main(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        reset();
        struct raphael_qc_detect s = {0};
        start(&s);
        assert(s.phase == RQ_AUTH && reruns == 1 && auths == 1);
        assert(registers[0x60] == 0xa0 && registers[0x5b] == 0x3b);
        assert(registers[0x44] == 0xa0);
        registers[8] = mode ? 0x48 : 0x28;
        rq_tick(&s, 17000000000ULL, 1, 1, 1, 0, mode ? 5 : 3, registers[8]);
        assert(s.phase == (mode ? RQ_QC3 : RQ_QC2));
        rq_tick(&s, 20000000000ULL, 0, 1, 0, 0, 0, 0);
        assert(s.phase == RQ_FALLBACK && !(registers[0x62] & 0x64));
        unsigned before = calls;
        rq_tick(&s, 21000000000ULL, 1, 1, 1, 1, 1, 8);
        assert(calls == before); /* no repeated command once cleanup succeeds */
        assert(registers[0x60] == 0xa0);
        connected = 0;
        rq_tick(&s, 22000000000ULL, 1, 0, 0, 0, 0, 0);
        assert(!s.used && !s.modified && registers[0x60] == 0xa7);
        assert(registers[0x5b] == 0xbb && registers[0x44] == 0xa0);
    }
    reset();
    struct raphael_qc_detect s = {0};
    start(&s);
    rq_tick(&s, 41000000000ULL, 1, 1, 1, 1, 1, 8);
    assert(s.phase == RQ_FALLBACK && s.stopped && reruns == 1);
    for (unsigned t = 42; t < 200; t++)
        rq_tick(&s, t * 1000000000ULL, 1, 1, 1, 1, 1, 8);
    assert(reruns == 1 && auths == 1);

    /* Exhaustive source bytes: an unenumerated exact DCP/OCP may identify at
     * 15s; FLOAT additionally needs hardware slow-plugin timeout and 60s.
     * A mistaken eligibility argument cannot authenticate SDP/CDP/mixed. */
    for (unsigned apsd = 0; apsd < 256; apsd++)
        for (unsigned result = 0; result < 256; result++) {
            reset(); s = (struct raphael_qc_detect){0};
            rq_tick(&s, 1000000000ULL, 1, 1, 1, 1, apsd, result);
            rq_tick(&s, 16000000000ULL, 1, 1, 1, 1, apsd, result);
            int dedicated = (apsd & 0x11) == 1 && (result == 8 || result == 2);
            int slow_float = (apsd & 0x11) == 1 && result == 0x10 && (apsd & 0x20);
            assert((reruns == 1) == dedicated);
            rq_tick(&s, 61000000000ULL, 1, 1, 1, 1, apsd, result);
            assert((auths == 1) == (dedicated || slow_float));
        }

    reset(); s = (struct raphael_qc_detect){0};
    rq_tick(&s, 1000000000ULL, 1, 1, 1, 1, 1, 8);
    rq_tick(&s, 16000000000ULL, 1, 1, 1, 1, 0x21, 0x10);
    rq_tick(&s, 61000000000ULL, 1, 1, 1, 1, 0x21, 0x10);
    assert(!auths); /* changed source must start a new full settling interval */
    rq_tick(&s, 76000000000ULL, 1, 1, 1, 1, 0x21, 0x10);
    assert(auths == 1 && reruns == 1);

    for (unsigned persistent = 0; persistent < 2; persistent++)
        for (unsigned fault = 1; fault <= 40; fault++) {
            reset(); s = (struct raphael_qc_detect){0};
            fail_at = fault; persistent_failure = persistent;
            start(&s);
            rq_tick(&s, 17000000000ULL, 0, 1, 0, 0, 0, 0);
            fail_at = 0;
            rq_tick(&s, 18000000000ULL, 0, 1, 0, 0, 0, 0);
            assert(!(registers[0x62] & 0x64));
            connected = 0;
            rq_tick(&s, 19000000000ULL, 1, 0, 0, 0, 0, 0);
            assert(!s.modified && !s.restore_failed);
        }
    reset(); s = (struct raphael_qc_detect){0};
    start(&s);
    connected = 0; fail_at = calls + 1;
    rq_tick(&s, 17000000000ULL, 1, 0, 0, 0, 0, 0);
    assert(s.restore_failed && registers[0x60] == 0xa0);
    fail_at = 0;
    rq_tick(&s, 18000000000ULL, 1, 0, 0, 0, 0, 0);
    assert(!s.restore_failed && registers[0x60] == 0xa7);

    for (unsigned inherited = 0; inherited < 3; inherited++) {
        reset(); s = (struct raphael_qc_detect){0};
        registers[inherited == 0 ? 0x62 : inherited == 1 ? 0x44 : 0x70] =
            inherited == 0 ? 0x5c : inherited == 1 ? 2 : 21;
        start(&s);
        assert(s.phase == RQ_FALLBACK && !s.modified && !reruns);
    }
    puts("PASS: QC2/QC3 detection; 65,536 source pairs; timeout; read/temperature guard failure; partial-write and persistent I/O faults; detach restoration; PD override never written; no high voltage or CP enable.");
    return 0;
}
