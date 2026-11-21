/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef RAPHAEL_BB_POLICY_H
#define RAPHAEL_BB_POLICY_H
#include <stdint.h>
/* SIM-only Raphael: WDS also owns registration profiles, not just IPA data.
 * Pass a bounded set of standard control requests to the real modem. Never
 * acknowledge an unavailable IPA endpoint or start a packet-data session.
 * Message definitions: linux-mobile-broadband/libqmi data/qmi-service-wds.json.
 */
static inline int rbb_control_request_allowed(uint32_t service, uint32_t message)
{
    if (service == 0x04u || service == 0x1au || service == 0x2fu) return 0;
    if (service != 0x01u) return 1;
    switch (message) {
    case 0x0001: /* Set Event Report */
    case 0x0003: /* Indication Register */
    case 0x001e: /* Get Supported Messages */
    case 0x0022: /* Get Packet Service Status */
    case 0x0027: /* Create Profile */
    case 0x0028: /* Modify Profile */
    case 0x002a: /* Get Profile List */
    case 0x002b: /* Get Profile Settings */
    case 0x0030: /* Get Dormancy Status: query only */
    case 0x0049: /* Get Default Profile Number */
    case 0x004a: /* Set Default Profile Number */
    case 0x004d: /* Set IP Family */
    case 0x0085: /* Get LTE Attach Parameters */
    case 0x0092: /* Get Max LTE Attach PDN Number */
    case 0x0093: /* Set LTE Attach PDN List */
    case 0x0094: /* Get LTE Attach PDN List */
    case 0x00a7: /* Configure Profile Event List */
    case 0x00af: /* Bind Subscription: logical SIM, not hardware data port */
    case 0x00b0: /* Get Bind Subscription */
    case 0x00c8: /* iOS16.5 Mav16 SetAttachProfileInfo3GPP. Send unchanged;
                   the real Qualcomm result decides support, never fake success. */
        return 1;
    default:
        return 0;
    }
}
#endif
