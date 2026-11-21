/* Real generated GSI2.0 readiness code, mock read-only MMIO. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "hakios.raphael-gsi-regs.h"
#define RAPHAEL_GSI_BASE 0x1e04000u
#define hakios_tl(...) ((void)0)
struct block {const char *name;uint32_t phys,size;uintptr_t base;};
static struct block ripa_gsi={"EE0",RAPHAEL_GSI_BASE+GSI_STATUS,0x1000,0};
static bool map_fails;
static unsigned reads;
static uint32_t status;
static void need(struct block *b) {b->base=map_fails?0:b->phys;}
static uint32_t rd(uintptr_t address) {
    ++reads;
    assert(address==RAPHAEL_GSI_BASE+GSI_STATUS || address==RAPHAEL_GSI_BASE+GSI_SW_VERSION);
    return address==RAPHAEL_GSI_BASE+GSI_STATUS?status:0x20000;
}
/* Deliberately no write helper: global configuration must remain TZ-owned. */
#include "hakios.raphael-gsi2-start.h"
int main(void) {
    _Static_assert(GSI_STATUS==0x1f000,"GSI2.0 uses register layout V1");
    _Static_assert(GSI_SW_VERSION==0x1f044,"SW version address");
    _Static_assert(CH_CMD==0x1f008 && EV_CH_CMD==0x1f010,"EE0 commands");
    _Static_assert(CH_CNTXT(4,0)==0x1c200,"command channel address");
    _Static_assert(EV_CNTXT(3,13)==0x1d1b4,"WAN event channel address");
    _Static_assert(CH_DOORBELL(6)==0x1e030 && EV_DOORBELL(3)==0x1e118,"doorbells");
    _Static_assert(ERROR_LOG==0x1f200 && CNTXT_SRC_IEOB_IRQ==0x1f0b0,"interrupts");
    map_fails=true;assert(!ripa_gsi_ready() && !reads);
    map_fails=false;status=4;assert(!ripa_gsi_ready());
    status=1;assert(ripa_gsi_ready());
    for(unsigned c=3;c<=6;c++) {
        assert(CH_CNTXT(c,3)>=GSI_FROM && CH_SCRATCH(c,3)<GSI_FROM+GSI_SIZE);
        assert(CH_DOORBELL(c)>=GSI_FROM && CH_DOORBELL(c)<GSI_FROM+GSI_SIZE);
    }
    puts("PASS actual GSI2 readiness: vendor V1 addresses, bounded EE0 map, failed map, disabled/ready status; no global writes");
    return 0;
}
