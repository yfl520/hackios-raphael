#ifndef SYS_HAKIOS_RAPHAEL_BT_H
#define SYS_HAKIOS_RAPHAEL_BT_H
#include <stdbool.h>
#include <stdint.h>
void raphael_bt_init(void);
uint32_t hakios_raphael_bt_rpmh_address(const char *name);
bool hakios_raphael_bt_rpmh_send(const uint32_t *, const uint32_t *, uint32_t, const char *);
bool hakios_raphael_bt_rpmh_claim(const char *name);
#endif
