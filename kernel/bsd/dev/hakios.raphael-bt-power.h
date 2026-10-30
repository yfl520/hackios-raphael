/* Raphael WCN3990 supplies, from the phone's live DT (2026-10-28).
 * Pure policy shared with the host tests; hardware access stays under rpmh_lock.
 */
#ifndef HAKIOS_RAPHAEL_BT_POWER_H
#define HAKIOS_RAPHAEL_BT_POWER_H

static const char *const raphael_bt_rail_names[] = { "ldoa7", "ldoc2", "ldoc11" };
static const unsigned raphael_bt_rail_mv[] = { 1800, 1304, 3312 };

static inline int
raphael_bt_rail_index(const char *name)
{
    if (!name) return -1;
    for (unsigned i = 0; i < 3; i++)
        if (!strcmp(name, raphael_bt_rail_names[i])) return (int)i;
    return -1;
}

static inline unsigned
raphael_bt_shared_mv(const char *name, unsigned wifi_mv, unsigned bt_mask)
{
    int rail = raphael_bt_rail_index(name);
    return rail >= 0 && (bt_mask & (1u << rail)) && wifi_mv < raphael_bt_rail_mv[rail]
        ? raphael_bt_rail_mv[rail] : wifi_mv;
}

static inline unsigned
raphael_bt_shared_enable(const char *name, unsigned wifi_on, unsigned bt_mask)
{
    int rail = raphael_bt_rail_index(name);
    return wifi_on || (rail >= 0 && (bt_mask & (1u << rail))) ? 1 : 0;
}
#endif
