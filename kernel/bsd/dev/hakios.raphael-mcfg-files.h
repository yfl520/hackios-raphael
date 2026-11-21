/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef RAPHAEL_MCFG_FILES_H
#define RAPHAEL_MCFG_FILES_H
/* Exact request-to-image names. No substring, traversal or write access. */
static inline const char *raphael_mcfg_image_name(const char *path, int writing)
{
 if(!path || writing)return 0;
 if(!strcmp(path,"readonly/firmware/image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig"))return "mcfg-hw.dig";
 if(!strcmp(path,"readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.dig"))return "mcfg-sw.dig";
 return 0;
}
#endif
