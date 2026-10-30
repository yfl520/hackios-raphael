#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hakios.raphael-bt-power.h"

int main(void)
{
    assert(raphael_bt_rail_index(NULL) == -1);
    assert(raphael_bt_rail_index("ldoc10") == -1);
    assert(raphael_bt_rail_index("ldoa7") == 0);
    assert(raphael_bt_rail_index("ldoc2") == 1);
    assert(raphael_bt_rail_index("ldoc11") == 2);
    for (unsigned mask = 0; mask < 8; mask++) {
        for (unsigned rail = 0; rail < 3; rail++) {
            const char *name = raphael_bt_rail_names[rail];
            assert(raphael_bt_shared_enable(name, 0, mask) == !!(mask & (1u << rail)));
            assert(raphael_bt_shared_enable(name, 1, mask) == 1);
            assert(raphael_bt_shared_mv(name, 0, mask) == ((mask & (1u << rail)) ? raphael_bt_rail_mv[rail] : 0));
            assert(raphael_bt_shared_mv(name, 4000, mask) == 4000);
        }
        assert(raphael_bt_shared_mv("ldoc11", 3104, mask) == ((mask & 4) ? 3312 : 3104));
        assert(raphael_bt_shared_enable("ldoc10", 0, mask) == 0);
        assert(raphael_bt_shared_mv("ldoc10", 3312, mask) == 3312);
    }
    puts("PASS: 8 Bluetooth rail combinations; WLAN on/off and shared voltage preservation");
    return 0;
}
