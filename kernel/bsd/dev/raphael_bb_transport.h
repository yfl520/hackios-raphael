/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef RAPHAEL_BB_TRANSPORT_H
#define RAPHAEL_BB_TRANSPORT_H
#include <stdint.h>
#define RBB_PORT_FIRST 0x4100u
#define RBB_PORT_COUNT 64u
#define RBB_QRTR_MAX 8192u
static inline int rbb_reserved_port(uint32_t p) {
    return p>=RBB_PORT_FIRST && p-RBB_PORT_FIRST<RBB_PORT_COUNT;
}
static inline uint32_t rbb_next_general_port(uint32_t p) {
    return rbb_reserved_port(p)?RBB_PORT_FIRST+RBB_PORT_COUNT:p;
}
/* Match the selected real server, including its port; no cross-service replies. */
static inline int rbb_peer_ok(uint32_t n,uint32_t p,uint32_t expected_n,uint32_t expected_p) {
    return n==expected_n && p==expected_p;
}
static inline int rbb_sdu_ok(const uint8_t *p,uint32_t n,int response) {
    if(!p || n<7 || n>RBB_QRTR_MAX || (uint32_t)(p[5]|p[6]<<8)+7!=n) return 0;
    if(response?(p[0]!=2 && p[0]!=4):p[0]!=0) return 0;
    uint32_t at=7;
    while(at<n) {
        if(n-at<3)return 0;
        uint32_t len=(uint32_t)(p[at+1]|p[at+2]<<8);
        if(len>n-at-3)return 0;
        at+=3+len;
    }
    return 1;
}
#endif
