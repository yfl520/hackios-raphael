
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint8_t delivered[20000];
static unsigned delivered_len, calls;
static void bt_deliver_to_host(const uint8_t *bytes, uint32_t count) {
    assert(delivered_len + count <= sizeof(delivered));
    memcpy(delivered + delivered_len, bytes, count); delivered_len += count; calls++;
}

/* UART reads are chunks, not H4 packets. Never deliver a partial real packet:
 * a locally generated Command Complete may be sent before the next UART poll,
 * and would otherwise be inserted into that unfinished packet on the host.
 * One IO thread owns both real RX and synthetic replies, so whole-packet
 * delivery serializes them without another lock or changing packet contents. */
#define BT_HOST_PACKET_MAX 4101u
static uint8_t g_host_rx_packet[BT_HOST_PACKET_MAX];
static uint32_t g_host_rx_got, g_host_rx_want, g_host_rx_hdr;
static unsigned g_host_rx_oversize;
static unsigned g_host_rx_diag;

static void
bt_host_rx_packets(const uint8_t *bytes, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        if (!g_host_rx_got) {
            switch (b) {
            case 0x04: g_host_rx_hdr = 3; break;
            case 0x02: case 0x05: g_host_rx_hdr = 5; break;
            case 0x03: g_host_rx_hdr = 4; break;
            default: continue;
            }
            g_host_rx_want = 0;
        }
        if (g_host_rx_got < BT_HOST_PACKET_MAX)
            g_host_rx_packet[g_host_rx_got] = b;
        g_host_rx_got++;
        if (g_host_rx_got == g_host_rx_hdr) {
            uint32_t payload;
            if (g_host_rx_packet[0] == 0x04) payload = g_host_rx_packet[2];
            else if (g_host_rx_packet[0] == 0x03) payload = g_host_rx_packet[3];
            else {
                payload = (uint32_t)g_host_rx_packet[3] |
                    ((uint32_t)g_host_rx_packet[4] << 8);
                if (g_host_rx_packet[0] == 0x05) payload &= 0x3fff;
            }
            g_host_rx_want = g_host_rx_hdr + payload;
        }
        if (g_host_rx_want && g_host_rx_got == g_host_rx_want) {
            /* Linux v6.12 hci_qca.c:qca_recv_acl_data diverts exactly
             * QCA_DEBUG_HANDLE=0x2edc to hci_recv_diag, not the ACL stack.
             * Match the full on-wire handle, including PB bits. */
            if (g_host_rx_packet[0] == 0x02 && g_host_rx_packet[1] == 0xdc &&
                g_host_rx_packet[2] == 0x2e) {
                if (++g_host_rx_diag <= 4)
                    printf("raphael-bt: QCA diagnostic ACL separated, %u bytes\n",
                        (unsigned)g_host_rx_want);
            } else if (g_host_rx_want <= BT_HOST_PACKET_MAX)
                bt_deliver_to_host(g_host_rx_packet, g_host_rx_want);
            else if (++g_host_rx_oversize <= 4)
                printf("raphael-bt: whole RX packet too large: %u; dropped intact\n",
                    (unsigned)g_host_rx_want);
            g_host_rx_got = g_host_rx_want = 0;
        }
    }
}

static void reset(void) {
    delivered_len = calls = g_host_rx_got = g_host_rx_want = g_host_rx_hdr = 0;
}
int main(void) {
    /* Same 486-byte ACL size observed while pairing on the phone. */
    uint8_t acl[491] = {2, 1, 0x20, 0xe6, 1};
    for (unsigned i = 5; i < sizeof(acl); i++) acl[i] = (uint8_t)i;
    uint8_t synthetic[11] = {4, 0x0e, 8, 1, 0xe2, 0xfc, 1, 0x36, 0, 0, 0};
    for (unsigned split = 1; split < sizeof(acl); split++) {
        reset(); bt_host_rx_packets(acl, split); assert(calls == 0);
        bt_deliver_to_host(synthetic, sizeof(synthetic));
        bt_host_rx_packets(acl + split, sizeof(acl) - split);
        assert(calls == 2 && delivered_len == sizeof(synthetic) + sizeof(acl));
        assert(memcmp(delivered, synthetic, sizeof(synthetic)) == 0);
        assert(memcmp(delivered + sizeof(synthetic), acl, sizeof(acl)) == 0);
    }
    uint8_t frames[] = {4, 1, 0, 3, 1, 0, 2, 0xaa, 0xbb, 2, 1, 0, 0, 0,
        5, 2, 0, 2, 0, 0xab, 0xcd};
    reset();
    for (unsigned i = 0; i < sizeof(frames); i++) bt_host_rx_packets(frames + i, 1);
    assert(calls == 4 && delivered_len == sizeof(frames));
    assert(memcmp(delivered, frames, sizeof(frames)) == 0);
    uint8_t diag[491]; memcpy(diag, acl, sizeof(diag)); diag[1] = 0xdc; diag[2] = 0x2e;
    reset();
    for (unsigned i = 0; i < sizeof(diag); i++) bt_host_rx_packets(diag + i, 1);
    assert(calls == 0); bt_host_rx_packets(synthetic, sizeof(synthetic));
    assert(calls == 1 && delivered_len == sizeof(synthetic));
    /* Reject an oversized packet as a unit, recover on the next frame. */
    uint8_t oversized[5005] = {2, 0, 0, 0x88, 0x13};
    memset(oversized + 5, 4, 5000); reset();
    bt_host_rx_packets(oversized, sizeof(oversized)); assert(calls == 0);
    bt_host_rx_packets(synthetic, sizeof(synthetic)); assert(calls == 1);
    assert(delivered_len == sizeof(synthetic));
    puts("PASS: H4 RX atomicity, ACL split points, QCA diagnostic separation, mixed frames, oversize recovery.");
    return 0;
}
