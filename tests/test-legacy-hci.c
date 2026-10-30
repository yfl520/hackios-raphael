
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned g_vsc_mode = 1;
static bool g_legacy_random_ready;

/* Restrict the host-visible LE interface to the legacy command family. QCA
 * rejects legacy commands after even the 0x204a periodic-list query. Never
 * convert a rejected scan command into success: actual scans pass to hardware.
 * Parse H4 framing so a prefix embedded in ACL data cannot change its payload.
 * State persists across UART read boundaries; no packet-sized buffer needed. */
static uint8_t g_sc_header[7];
static uint32_t g_sc_pos, g_sc_total, g_sc_header_len;

static void
bt_sc_rewrite(uint8_t *bytes, uint32_t count)
{
    if (g_vsc_mode == 0) return;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        if (g_sc_pos == 0) {
            switch (b) {
            case 0x04: g_sc_header_len = 3; break; /* HCI Event */
            case 0x02: case 0x05: g_sc_header_len = 5; break; /* ACL / ISO */
            case 0x03: g_sc_header_len = 4; break; /* SCO */
            default: continue;
            }
            g_sc_total = 0;
        }
        if (g_sc_pos < sizeof(g_sc_header)) g_sc_header[g_sc_pos] = b;
        if (g_sc_pos + 1 == g_sc_header_len) {
            uint32_t payload;
            if (g_sc_header[0] == 0x04) payload = g_sc_header[2];
            else if (g_sc_header[0] == 0x03) payload = g_sc_header[3];
            else {
                payload = (uint32_t)g_sc_header[3] | ((uint32_t)g_sc_header[4] << 8);
                if (g_sc_header[0] == 0x05) payload &= 0x3fff;
            }
            g_sc_total = g_sc_header_len + payload;
        }
        if (g_sc_pos == 6 && g_sc_header[0] == 0x04 &&
            g_sc_header[1] == 0x0e && g_sc_header[4] == 0x05 &&
            g_sc_header[5] == 0x20 && b == 0) g_legacy_random_ready = true;
        if (g_sc_pos >= 7 && g_sc_header[0] == 0x04 &&
            g_sc_header[1] == 0x0e && g_sc_header[6] == 0) {
            uint16_t opcode = (uint16_t)g_sc_header[4] | ((uint16_t)g_sc_header[5] << 8);
            if (opcode == 0x1002 && g_sc_total == 71) {
                unsigned octet = g_sc_pos - 7;
                if (octet == 18) bytes[i] = b | 0x08; /* existing 0x0c5b adapter */
                if (octet == 36) bytes[i] = b & 0x01;
                if (octet == 37) bytes[i] = 0;
                if (octet == 38) bytes[i] = b & 0x80;
                if (bytes[i] != b)
                    printf("raphael-bt: legacy LE: Supported_Commands[%u] %#x -> %#x\n",
                        octet, (unsigned)b, (unsigned)bytes[i]);
            } else if (opcode == 0x2003 && g_sc_total == 15 && g_sc_pos == 8) {
                bytes[i] = b & 0xcf; /* no Extended/Periodic Advertising, bits12/13 */
                printf("raphael-bt: legacy LE: feature octet1 %#x -> %#x\n",
                    (unsigned)b, (unsigned)bytes[i]);
            }
        }
        g_sc_pos++;
        if (g_sc_total && g_sc_pos >= g_sc_total) g_sc_pos = 0;
    }
}

static void reset(void) {
    g_sc_pos = g_sc_total = g_sc_header_len = 0;
    memset(g_sc_header, 0, sizeof(g_sc_header));
}
int main(void) {
    uint8_t event[71] = {4, 0x0e, 0x44, 1, 2, 0x10, 0};
    memset(event + 7, 0xff, 64);
    event[25] = 0;
    uint8_t expected[71]; memcpy(expected, event, 71);
    expected[25] = 8; expected[43] = 1; expected[44] = 0; expected[45] = 0x80;
    for (unsigned split = 0; split <= 71; split++) {
        uint8_t actual[71]; memcpy(actual, event, 71); reset();
        bt_sc_rewrite(actual, split); bt_sc_rewrite(actual + split, 71 - split);
        assert(memcmp(actual, expected, 71) == 0);
    }
    uint8_t feat[15] = {4, 0x0e, 12, 1, 3, 0x20, 0, 0xff, 0x7d};
    for (unsigned split = 0; split <= 15; split++) {
        uint8_t actual[15]; memcpy(actual, feat, 15); reset();
        bt_sc_rewrite(actual, split); bt_sc_rewrite(actual + split, 15 - split);
        assert(actual[8] == 0x4d); actual[8] = 0x7d;
        assert(memcmp(actual, feat, 15) == 0);
    }
    /* False prefix inside ACL must remain byte-for-byte unchanged. */
    uint8_t acl[76] = {2, 0, 0, 71, 0}, before[76];
    memcpy(acl + 5, event, 71); memcpy(before, acl, 76); reset();
    for (unsigned i = 0; i < 76; i++) bt_sc_rewrite(acl + i, 1);
    assert(memcmp(acl, before, 76) == 0);
    /* Failed and malformed-length replies are not success capabilities. */
    uint8_t failed[71]; memcpy(failed, event, 71); failed[6] = 1;
    reset(); bt_sc_rewrite(failed, 71); assert(failed[43] == 0xff);
    uint8_t seq[86]; memcpy(seq, event, 71); memcpy(seq + 71, feat, 15);
    reset(); for (unsigned i = 0; i < 86; i++) bt_sc_rewrite(seq + i, 1);
    assert(memcmp(seq, expected, 71) == 0 && seq[79] == 0x4d);
    g_vsc_mode = 0; reset(); memcpy(failed, event, 71);
    bt_sc_rewrite(failed, 71); assert(memcmp(failed, event, 71) == 0);
    puts("Legacy HCI capability stream tests passed: chunk splits, framing, errors, bypass.");
    return 0;
}
