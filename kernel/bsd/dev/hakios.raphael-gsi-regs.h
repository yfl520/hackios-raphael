/* Generated from pinned Xiaomi gsi_reg_v1.h for GSI_VER_2_0.
 * Register version is not the GSI hardware generation.
 * EE0 only. Global firmware, peripheral/IEP configuration belongs to TZ. */
#ifndef HAKIOS_RAPHAEL_GSI_REGS_H
#define HAKIOS_RAPHAEL_GSI_REGS_H
#define GSI_FROM 0x1c000u
#define GSI_SIZE 0x4000u
#define GSI_STATUS 0x1f000u
#define GSI_SW_VERSION 0x1f044u
#define CH_CMD 0x1f008u
#define EV_CH_CMD 0x1f010u
#define CNTXT_TYPE_IRQ 0x1f080u
#define CNTXT_TYPE_IRQ_MSK 0x1f088u
#define CNTXT_SRC_CH_IRQ 0x1f090u
#define CNTXT_SRC_EV_CH_IRQ 0x1f094u
#define CNTXT_SRC_CH_IRQ_MSK 0x1f098u
#define CNTXT_SRC_EV_CH_IRQ_MSK 0x1f09cu
#define CNTXT_SRC_CH_IRQ_CLR 0x1f0a0u
#define CNTXT_SRC_EV_CH_IRQ_CLR 0x1f0a4u
#define CNTXT_SRC_IEOB_IRQ 0x1f0b0u
#define CNTXT_SRC_IEOB_IRQ_MSK 0x1f0b8u
#define CNTXT_SRC_IEOB_IRQ_CLR 0x1f0c0u
#define CNTXT_GLOB_IRQ_STTS 0x1f100u
#define CNTXT_GLOB_IRQ_EN 0x1f108u
#define CNTXT_GLOB_IRQ_CLR 0x1f110u
#define CNTXT_GSI_IRQ_STTS 0x1f118u
#define CNTXT_GSI_IRQ_EN 0x1f120u
#define CNTXT_GSI_IRQ_CLR 0x1f128u
#define CNTXT_INTSET 0x1f180u
#define ERROR_LOG 0x1f200u
#define ERROR_LOG_CLR 0x1f210u
#define CH_CNTXT(c, n) (0x1c000u + 0x80 * (c) + 4 * (n))
#define CH_QOS(c) (0x1c05cu + 0x80 * (c))
#define CH_SCRATCH(c, n) (0x1c060u + 0x80 * (c) + 4 * (n))
#define EV_CNTXT(e, n) (0x1d000u + 0x80 * (e) + 4 * (n))
#define CH_DOORBELL(c) (0x1e000u + 8 * (c))
#define EV_DOORBELL(e) (0x1e100u + 8 * (e))
#endif
