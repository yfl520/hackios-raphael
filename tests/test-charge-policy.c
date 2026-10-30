#include <assert.h>
#include <stdio.h>
#include "hakios.raphael-charge-policy.h"

static int accepted(unsigned result, unsigned thermal, int fresh, int temperature, int voltage)
{
    return raphael_charge_class(0x10, 1, result, thermal, 1, 0, 4, fresh, temperature, voltage,
        0, 0, 8, 1, 2, 9, 0) != RAPHAEL_CHARGE_NONE;
}

int main(void)
{
    for (unsigned result = 0; result < 256; result++) {
        int expected = result == 0x02 || result == 0x08 || result == 0x04 || result == 0x28 || result == 0x48;
        assert(accepted(result, 0, 1, 25, 3800) == expected);
        if (result & 1) assert(!accepted(result, 0, 1, 25, 3800));
    }
    for (int temperature = -40; temperature <= 100; temperature++)
        assert(accepted(8, 0, 1, temperature, 3800) == (temperature >= 15 && temperature < 45));
    for (unsigned bit = 1; bit <= 0x20; bit <<= 1) assert(!accepted(8, bit, 1, 25, 3800));
    assert(!accepted(8, 0, 0, 25, 3800));
    assert(!accepted(8, 0, 1, 25, 3199));
    assert(!accepted(8, 0, 1, 25, 4301));
    assert(!raphael_charge_class(0, 1, 8, 0, 1, 0, 4, 1, 25, 3800, 0, 0, 8, 1, 2, 9, 0));
    assert(!raphael_charge_class(0x10, 0, 8, 0, 1, 0, 4, 1, 25, 3800, 0, 0, 8, 1, 2, 9, 0));
    assert(!raphael_charge_class(0x10, 1, 8, 0, 0, 0, 4, 1, 25, 3800, 0, 0, 8, 1, 2, 9, 0));
    assert(!raphael_charge_class(0x10, 1, 8, 0, 1, 1, 4, 1, 25, 3800, 0, 0, 8, 1, 2, 9, 0));
    assert(!raphael_charge_class(0x10, 1, 8, 0, 1, 0, 0, 1, 25, 3800, 0, 0, 8, 1, 2, 9, 0));
    struct raphael_charge_gate gate = {0};
    assert(!raphael_charge_settled(&gate, RAPHAEL_CHARGE_DEDICATED, 100));
    assert(!raphael_charge_settled(&gate, RAPHAEL_CHARGE_DEDICATED, 15000000099ULL));
    assert(raphael_charge_settled(&gate, RAPHAEL_CHARGE_DEDICATED, 15000000100ULL));
    puts("PASS: all 256 APSD results, SDP exclusion, thermal/stale/voltage/AICL/enable/pause guards");
    return 0;
}
