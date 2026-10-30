/* Included by hakios.wifi.c after wifi_rpmh_write_unlocked.
 * Shares its CMD-DB, TCS timeout quarantine, and rpmh_lock with camera/WLAN.
 * Only the three verified BT rails can be raised; no shared rail is unvoted.
 */
#include "hakios.raphael-bt-power.h"
static unsigned raphael_bt_power_mask;

uint32_t
hakios_raphael_bt_rpmh_address(const char *name)
{
    if (!hakios_is_raphael() || raphael_bt_rail_index(name) < 0) return 0;
    lck_mtx_lock(&rpmh_lock);
    const struct cmd_db_entry *entry = rpmh_resource(name, 0);
    uint32_t address = entry && ((entry->address >> 16) & 0xf) == 4 &&
        !(entry->address & 0xff) ? entry->address : 0;
    lck_mtx_unlock(&rpmh_lock);
    return address;
}

bool
hakios_raphael_bt_rpmh_send(const uint32_t *addresses, const uint32_t *values,
    uint32_t count, const char *who)
{
    (void)who;
    if (!hakios_is_raphael() || !addresses || !values || count != 3) return false;
    lck_mtx_lock(&rpmh_lock);
    bool ok = false;
    int selected = -1;
    uint32_t out[3], current[3];
    for (unsigned rail = 0; rail < 3; rail++) {
        const struct cmd_db_entry *entry = rpmh_resource(raphael_bt_rail_names[rail], 0);
        if (entry && ((entry->address >> 16) & 0xf) == 4 && !(entry->address & 0xff) &&
            addresses[0] == entry->address && addresses[1] == entry->address + 8 &&
            addresses[2] == entry->address + 4 && values[0] == raphael_bt_rail_mv[rail] &&
            values[1] == 7 && values[2] == 1) selected = (int)rail;
    }
    if (selected < 0 || raphael_rpmh_faulted) goto done;
    need(&rsc);
    if (!rsc.base) goto done;
    /* Vote registers contain RESP_REQ metadata. Compare only decoded values. */
    for (unsigned i = 0; i < 3; i++) {
        unsigned field = i == 1 ? 2 : (i == 2 ? 1 : 0);
        uint32_t raw = 0;
        if (!raphael_rpmh_read_locked(addresses[i], &raw) ||
            !raphael_rpmh_decode_vrm(raw, field, &current[i])) goto done;
        out[i] = current[i] > values[i] ? current[i] : values[i];
    }
    ok = wifi_rpmh_write_unlocked(addresses, out, count);
    if (ok) raphael_bt_power_mask |= 1u << selected;
done:
    lck_mtx_unlock(&rpmh_lock);
    return ok;
}

/* Claim an already-powered rail as well: WLAN off must not remove its vote. */
bool
hakios_raphael_bt_rpmh_claim(const char *name)
{
    uint32_t address = hakios_raphael_bt_rpmh_address(name);
    int rail = raphael_bt_rail_index(name);
    if (!address || rail < 0) return false;
    uint32_t addresses[3] = { address, address + 8, address + 4 };
    uint32_t values[3] = { raphael_bt_rail_mv[rail], 7, 1 };
    return hakios_raphael_bt_rpmh_send(addresses, values, 3, "raphael-bt");
}
