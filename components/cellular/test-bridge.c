/* Host model of the pending baseband bridge: QMUX and CTL against QRTR, the SAHARA device end, the AppleBaseband methods, the channel
   table, the data port, activation answers and eSIM Tape transport. Built with -fsanitize=address,undefined by tests/test_baseband_bridge.py. */
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_data.h"
#include "coachz_bb_pdp.h"
#include "coachz_bb_esim.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* a host CTL request: [QMUX][0, txn, id, len][TLV 0x01] */
static uint32_t ctl_request(uint8_t *f, uint8_t txn, uint16_t id, const uint8_t *v, uint32_t vlen)
{
    const uint32_t tlvs = v ? 3 + vlen : 0, n = CBQ_HDR + CBQ_CTL_HDR + tlvs;
    f[0] = 1; cbq_wr16(f + 1, n - 1); f[3] = 0; f[4] = 0; f[5] = 0;
    f[6] = 0; f[7] = txn; cbq_wr16(f + 8, id); cbq_wr16(f + 10, tlvs);
    if (v) { f[12] = 1; cbq_wr16(f + 13, vlen); memcpy(f + 15, v, vlen); }
    return n;
}
static uint32_t qmi_request(uint8_t *f, uint8_t service, uint8_t cid, uint16_t txn, uint16_t id)
{
    const uint8_t tlv[] = { 0x10, 0x01, 0x00, 0x05 };
    const uint32_t n = CBQ_HDR + CBQ_SDU_HDR + sizeof tlv;
    f[0] = 1; cbq_wr16(f + 1, n - 1); f[3] = 0; f[4] = service; f[5] = cid;
    f[6] = 0; cbq_wr16(f + 7, txn); cbq_wr16(f + 9, id); cbq_wr16(f + 11, sizeof tlv);
    memcpy(f + 13, tlv, sizeof tlv);
    return n;
}

static void test_qmux(void)
{
    static struct cbq_bridge b;
    uint8_t f[64], out[256];
    cbq_init(&b);
    CHECK(cbq_server_new(&b, 1, 1 | 0 << 8, 0, 0x10));           /* WDS */
    CHECK(cbq_server_new(&b, 3, 1 | 2 << 8, 0, 0x22));           /* NAS instance 2 */
    CHECK(cbq_server_new(&b, 3, 1 | 1 << 8, 0, 0x21));           /* NAS instance 1: preferred */
    CHECK(cbq_server_new(&b, 1, 1, 0, 0x10));                    /* repeated announcement */
    CHECK(!cbq_server_new(&b, 0x101, 1, 0, 0x30));               /* not expressible in QMUX */
    CHECK(cbq_server_find(&b, 3)->port == 0x21);

    /* GetClientId(WDS): exact answer bytes */
    uint8_t svc = 1;
    uint32_t n = ctl_request(f, 7, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    struct cbq_result r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    const uint8_t want[] = { 1, 0x17, 0, 0x80, 0, 0, 1, 7, 0x22, 0, 0x0c, 0, 2, 4, 0, 0, 0, 0, 0, 1, 2, 0, 1, 1 };
    CHECK(r.kind == CBQ_TO_HOST && r.len == sizeof want && !memcmp(out, want, sizeof want));
    CHECK(r.local_port == CBQ_PORT_BASE && r.cid == 1);

    /* a service the modem lacks, and one it lacks but the bridge serves */
    svc = 2;
    n = ctl_request(f, 8, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 12 + 3) == 1 && cbq_rd16(out + 12 + 5) == CBQ_ERR_INVALID_SERVICE_TYPE);
    svc = 0xe2;
    n = ctl_request(f, 9, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && out[22] == 0xe2 && out[23] == 1 && r.local_port == CBQ_PORT_BASE + 1);

    /* the same service on the other channel gets its own id space and port */
    svc = 1;
    n = ctl_request(f, 1, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 1, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.cid == 1 && r.local_port == CBQ_PORT_BASE + 2 && r.channel == 1);
    n = ctl_request(f, 2, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.cid == 2 && r.local_port == CBQ_PORT_BASE + 3);

    /* a WDS request goes to the modem as the SDU; the answer comes back framed for the right channel and client */
    n = qmi_request(f, 1, 2, 0x1234, 0x0020);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_MODEM && r.node == 0 && r.port == 0x10 && r.local_port == CBQ_PORT_BASE + 3 && r.len == n - CBQ_HDR);
    uint8_t sdu[16];
    memcpy(sdu, f + CBQ_HDR, n - CBQ_HDR);
    sdu[0] = CBQ_QMI_RESPONSE;
    r = cbq_from_modem(&b, CBQ_PORT_BASE + 3, 0, sdu, n - CBQ_HDR, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.channel == 0 && r.len == n && out[3] == 0x80 && out[4] == 1 && out[5] == 2 && !memcmp(out + 6, sdu, n - 6));
    CHECK(cbq_from_modem(&b, CBQ_PORT_BASE + 3, 5, sdu, n - CBQ_HDR, out, sizeof out).kind == CBQ_DROP);   /* wrong node */
    CHECK(cbq_from_modem(&b, CBQ_PORT_BASE + 9, 0, sdu, n - CBQ_HDR, out, sizeof out).kind == CBQ_DROP);   /* no such client */

    /* a private-service request comes back to the caller; the default answer is NOT_SUPPORTED with the same id and txn */
    n = qmi_request(f, 0xe2, 1, 0x0042, 0x0055);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_PRIVATE && r.service == 0xe2 && r.cid == 1);
    const uint32_t m = cbq_private_unsupported(f + CBQ_HDR, r.service, r.cid, out, sizeof out);
    const uint8_t want_ns[] = { 1, 0x13, 0, 0x80, 0xe2, 1, 2, 0x42, 0, 0x55, 0, 7, 0, 2, 4, 0, 1, 0, 94, 0 };
    CHECK(m == sizeof want_ns && !memcmp(out, want_ns, sizeof want_ns));

    /* malformed frames and host indications are dropped */
    n = qmi_request(f, 1, 2, 1, 1);
    f[1]++;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 2, 1, 1);
    f[3] = 0x80;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 2, 1, 1);
    f[6] = CBQ_QMI_INDICATION;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 9, 1, 1);
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    CHECK(b.dropped == 6);

    /* release: answer echoes the pair, the modem client's port needs DEL_CLIENT once */
    const uint8_t pair[2] = { 1, 2 };
    n = ctl_request(f, 3, CBQ_CTL_RELEASE_CLIENT_ID, pair, 2);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && out[19] == 1 && out[20] == 2 && out[22] == 1 && out[23] == 2);
    CHECK(cbq_next_release(&b) == CBQ_PORT_BASE + 3 && cbq_next_release(&b) == 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(cbq_rd16(out + 17) == CBQ_ERR_INVALID_CLIENT_ID);
    CHECK(cbq_release_complete(&b, CBQ_PORT_BASE + 3));
    /* Raphael retains the port until the DEL_CLIENT send is confirmed. */
    svc = 1;
    n = ctl_request(f, 4, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.cid == 3 && r.local_port == CBQ_PORT_BASE + 3);

    /* Sync on channel 0 frees its clients (WDS 1, BSP 1, WDS 3); only modem clients need DEL_CLIENT; channel 1 keeps its client */
    n = ctl_request(f, 5, CBQ_CTL_SYNC, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.len == 19 && cbq_rd16(out + 15) == 0);
    uint32_t released[4] = { 0 }, count = 0, p;
    while ((p = cbq_next_release(&b)) && count < 4) released[count++] = p;
    CHECK(count == 2 && released[0] == CBQ_PORT_BASE && released[1] == CBQ_PORT_BASE + 3);
    CHECK(cbq_client_find(&b, 1, 1, 1) == 2);

    /* GetVersionInfo lists each service once; other CTL requests are accepted and counted */
    n = ctl_request(f, 6, CBQ_CTL_GET_VERSION_INFO, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && out[19] == 1 && cbq_rd16(out + 20) == 1 + 2 * 5 && out[22] == 2 && out[23] == 1 && out[28] == 3);
    n = ctl_request(f, 7, 0x0028, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && b.ctl_other == 1);

    const uint8_t want_sync[] = { 1, 11, 0, 0x80, 0, 0, 2, 0, 0x27, 0, 0, 0 };
    CHECK(cbq_ctl_sync_indication(out, sizeof out) == sizeof want_sync && !memcmp(out, want_sync, sizeof want_sync));

    /* a vanished server: requests to it are dropped */
    cbq_server_del(&b, 1, 0, 0x10);
    n = qmi_request(f, 1, 1, 1, 1);
    CHECK(cbq_from_host(&b, 1, f, n, out, sizeof out).kind == CBQ_DROP);
    if (!failures) printf("PASS qmux and ctl\n");
}

/* ------------------------------------------------------------------ SAHARA: a host that serves two images */
static uint8_t image_elf32[0x400], image_elf64[0x600], image_blob[100];

static void build_images(void)
{
    uint8_t *h = image_elf32;
    memset(image_elf32, 0xa5, sizeof image_elf32);
    memset(h, 0, 52);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 1;
    cbs_put32(h + 0x1c, 0x40);                  /* phoff */
    h[0x2a] = 32; h[0x2c] = 3;                  /* phentsize, phnum */
    const uint32_t seg[3][2] = { { 0x100, 0x80 }, { 0x200, 0 }, { 0x200, 0x1f0 } };
    for (int i = 0; i < 3; i++) {
        uint8_t *e = h + 0x40 + i * 32;
        memset(e, 0, 32);
        cbs_put32(e + 4, seg[i][0]);
        cbs_put32(e + 16, seg[i][1]);
    }
    h = image_elf64;
    memset(image_elf64, 0x5a, sizeof image_elf64);
    memset(h, 0, 64);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 2;
    cbs_put32(h + 0x20, 0x40);
    h[0x36] = 56; h[0x38] = 1;
    memset(h + 0x40, 0, 56);
    cbs_put32(h + 0x40 + 8, 0x100);
    cbs_put32(h + 0x40 + 32, 0x500);
    memset(image_blob, 0x11, sizeof image_blob);
}

static const uint8_t *image_for(uint32_t id, uint32_t *size)
{
    if (id == 0x15) { *size = sizeof image_elf32; return image_elf32; }
    if (id == 0x1f) { *size = sizeof image_elf64; return image_elf64; }
    if (id == 0x4300) { *size = sizeof image_blob; return image_blob; }
    return 0;
}

static void test_sahara(void)
{
    static struct cbs_device d;
    struct cbs_out o;
    const uint32_t list[] = { 0x15, 0x1f, 0x4300 };
    build_images();
    cbs_init(&d, list, 3, 0x100);
    cbs_start(&d, &o);
    unsigned hellos = 0, ends = 0, done_resp = 0, final_status = 9, guard = 0;
    uint64_t served = 0;
    while (o.count && guard++ < 1000) {
        struct cbs_out next = { 0 };
        for (uint32_t k = 0; k < o.count; k++) {
            const uint8_t *p = o.data[k];
            const uint32_t command = cbs_get32(p);
            uint8_t reply[0x30] = { 0 };
            struct cbs_out step = { 0 };
            if (command == CBS_HELLO) {
                CHECK(o.len[k] == 0x30 && cbs_get32(p + 4) == 0x30 && cbs_get32(p + 8) == 2 && cbs_get32(p + 20) == 0);
                hellos++;
                cbs_put32(reply, CBS_HELLO_RESP); cbs_put32(reply + 4, 0x30); cbs_put32(reply + 8, 2); cbs_put32(reply + 12, 1);
                cbs_receive(&d, reply, 0x30, &step);
            } else if (command == CBS_READ_DATA) {
                uint32_t size = 0;
                const uint8_t *img = image_for(cbs_get32(p + 8), &size);
                const uint32_t off = cbs_get32(p + 12), len = cbs_get32(p + 16);
                CHECK(img && off + len <= size && len <= 0x100);
                if (!img || off + len > size) { failures++; return; }
                /* serve in two writes to exercise partial data */
                const uint32_t half = len / 2;
                uint32_t used = cbs_receive(&d, img + off, half, &step);
                CHECK(used == half && step.count == 0);
                used = cbs_receive(&d, img + off + half, len - half, &step);
                CHECK(used == len - half);
                served += len;
            } else if (command == CBS_END_IMAGE_TX) {
                CHECK(cbs_get32(p + 8) == list[ends] && cbs_get32(p + 12) == 0);
                ends++;
                cbs_put32(reply, CBS_DONE); cbs_put32(reply + 4, 8);
                cbs_receive(&d, reply, 8, &step);
            } else if (command == CBS_DONE_RESP) {
                done_resp++;
                final_status = cbs_get32(p + 8);
                continue;
            } else {
                CHECK(0);
            }
            for (uint32_t j = 0; j < step.count && next.count < 2; j++) {
                next.len[next.count] = step.len[j];
                memcpy(next.data[next.count++], step.data[j], CBS_PACKET_MAX);
            }
        }
        o = next;
    }
    /* image 1: probe 64, headers 96, segments 0x80 + 0x1f0 in 0x100 pieces; image 2: 64 + 56 + 0x500; image 3: probe only */
    CHECK(d.state == CBS_FINISHED && hellos == 3 && ends == 3 && done_resp == 3 && final_status == 1);
    CHECK(served == 64 + 96 + 0x80 + 0x1f0 + 64 + 56 + 0x500 + 64 && d.bytes == served);
    CHECK(d.requests == 1 + 1 + 1 + 2 + 1 + 1 + 5 + 1);

    /* a refused HELLO, a RESET, a stray packet */
    cbs_init(&d, list, 1, 0);
    cbs_start(&d, &o);
    uint8_t bad[0x30] = { 0 };
    cbs_put32(bad, CBS_HELLO_RESP); cbs_put32(bad + 4, 0x30); cbs_put32(bad + 16, 1);
    cbs_receive(&d, bad, 0x30, &o);
    CHECK(d.state == CBS_FAILED && o.count == 0);
    cbs_start(&d, &o);
    cbs_put32(bad, CBS_RESET); cbs_put32(bad + 4, 8);
    cbs_receive(&d, bad, 8, &o);
    CHECK(d.state == CBS_IDLE && o.count == 1 && cbs_get32(o.data[0]) == CBS_RESET_RESP);
    cbs_start(&d, &o);
    cbs_put32(bad, CBS_DONE); cbs_put32(bad + 4, 8);
    cbs_receive(&d, bad, 8, &o);
    CHECK(d.state == CBS_FAILED);
    cbs_init(&d, list, 0, 0);
    cbs_start(&d, &o);
    CHECK(d.state == CBS_FAILED && o.count == 0);
    if (!failures) printf("PASS sahara device\n");
}

/* BBUEUR10Loader's order: mode 3, chip id and serial number in command mode, then a switch to mode 0 and a new HELLO. */
static void test_sahara_command(void)
{
    static const uint32_t list[] = { 0x15 };
    static struct cbs_device d;
    struct cbs_out o = { 0 };
    uint8_t in[0x30] = { 0 };
    cbs_init(&d, list, 1, 0x100);
    cbs_start(&d, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_HELLO);
    cbs_put32(in, CBS_HELLO_RESP); cbs_put32(in + 4, 0x30); cbs_put32(in + 8, 2); cbs_put32(in + 12, 1); cbs_put32(in + 20, CBS_MODE_COMMAND);
    CHECK(cbs_receive(&d, in, 0x30, &o) == 0x30 && o.count == 1 && cbs_get32(o.data[0]) == CBS_COMMAND_READY && o.len[0] == 8 && d.state == CBS_COMMAND);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 4, 0xc); cbs_put32(in + 8, CBS_EXEC_HW_ID);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_EXECUTE_RESP && o.len[0] == 0x10 && cbs_get32(o.data[0] + 8) == CBS_EXEC_HW_ID && cbs_get32(o.data[0] + 12) == 8);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && o.len[0] == 8 && cbs_get32(o.data[0] + 4) == CBS_CHIP_ID);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 8, CBS_EXEC_SERIAL);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0] + 12) == 4);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && o.len[0] == 4 && d.executes == 2);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 8, 0x77);       /* unknown: answers empty */
    cbs_receive(&d, in, 0xc, &o);
    CHECK(cbs_get32(o.data[0] + 12) == 0);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 0);
    cbs_put32(in, CBS_SWITCH_MODE); cbs_put32(in + 8, CBS_MODE_IMAGE_PENDING);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_HELLO && cbs_get32(o.data[0] + 20) == CBS_MODE_IMAGE_PENDING && d.state == CBS_WAIT_HELLO_RESP);
    cbs_put32(in, CBS_HELLO_RESP); cbs_put32(in + 4, 0x30); cbs_put32(in + 20, CBS_MODE_IMAGE_PENDING);
    cbs_receive(&d, in, 0x30, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_READ_DATA && cbs_get32(o.data[0] + 8) == 0x15 && d.state == CBS_WAIT_ELF);
}

static void test_registers(void)
{
    uint8_t r[0x60];
    cbs_read_register(3, r, 0x18);
    CHECK(cbs_get32(r + 4) == CBS_CHIP_ID && cbs_get32(r) == 0);
    cbs_read_register(0, r, 4);
    CHECK(cbs_get32(r) == CBS_SERIAL);
    cbs_read_register(4, r, 0x60);
    CHECK(!memcmp(r, cbs_default_pk_hash, 32) && r[0x20] == 0 && r[0x5f] == 0);
    cbs_read_register(9, r, 8);
    CHECK(cbs_get32(r) == 0 && cbs_get32(r + 4) == 0);
}

static void test_control(void)
{
    static struct cbc_state s;
    uint64_t in[3] = { 0 }, out[2] = { 0 };
    uint32_t nout;
    cbc_init(&s);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_RESET, in, 0, out, &nout) == CBC_OK && nout == 1 && out[0] == 0);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PMU_EXTON, in, 0, out, &nout) == CBC_OK && out[0] == 1);
    in[0] = 0; nout = 0; CHECK(cbc_call(&s, CBC_POWER, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PMU_EXTON, in, 0, out, &nout) == CBC_OK && out[0] == 0);
    in[0] = 5; nout = 0; CHECK(cbc_call(&s, CBC_SET_BASEBAND_STATE, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_BASEBAND_STATE, in, 0, out, &nout) == CBC_OK && nout == 1 && out[0] == 5);
    in[0] = 3; nout = 0; CHECK(cbc_call(&s, CBC_SET_MANAGER_STATE, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_MANAGER_STATE, in, 0, out, &nout) == CBC_OK && out[0] == 3);
    in[0] = 7; in[1] = 70; nout = 0; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 2, out, &nout) == CBC_OK);
    in[1] = 71; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 2, out, &nout) == CBC_OK && s.properties == 1);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PROPERTY, in, 1, out, &nout) == CBC_OK && out[0] == 71);
    in[0] = 8; nout = 1; CHECK(cbc_call(&s, CBC_GET_PROPERTY, in, 1, out, &nout) == CBC_OK && out[0] == 0);
    nout = 0; CHECK(cbc_call(&s, CBC_GET_BASEBAND_STATE, in, 0, out, &nout) == CBC_BAD_ARGUMENT);
    nout = 0; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 1, out, &nout) == CBC_BAD_ARGUMENT);
    nout = 0; CHECK(cbc_call(&s, CBC_RESET_MODEM, in, 1, out, &nout) == CBC_OK && s.resets == 1);
    nout = 0; CHECK(cbc_call(&s, CBC_SPMI_RAW_READ, in, 3, out, &nout) == CBC_UNSUPPORTED && s.last_unsupported == CBC_SPMI_RAW_READ);
    nout = 0; CHECK(cbc_call(&s, CBC_TIMESYNC, in, 0, out, &nout) == CBC_UNSUPPORTED);
    /* ABM powercycles after an unclean stop, and every client names itself first (structure input, no scalars). */
    nout = 0; CHECK(cbc_call(&s, CBC_POWERCYCLE, in, 0, out, &nout) == CBC_OK && s.powered == 1);
    nout = 0; CHECK(cbc_call(&s, CBC_SELF_IDENTIFY, in, 0, out, &nout) == CBC_OK && nout == 0);
    if (!failures) printf("PASS baseband methods\n");
}

static void test_channels(void)
{
    CHECK(cbh_channel_id("qmux1") == 4 && cbh_channel_id("qmux2") == 5 && cbh_channel_id("sahara") == 1);
    CHECK(cbh_channel_id("boot_logger") == 20 && cbh_channel_id("qmux") == -1 && cbh_channel_id("qmux11") == -1);
    for (unsigned i = 0; i < CBH_PUBLISHED; i++) CHECK(cbh_channel_id(cbh_published[i]) >= 0);
    for (unsigned i = 0; i < sizeof cbh_interface_methods / sizeof cbh_interface_methods[0]; i++) CHECK(cbh_interface_methods[i].selector == i);
    for (unsigned i = 0; i < sizeof cbh_control_methods / sizeof cbh_control_methods[0]; i++) CHECK(cbh_control_methods[i].selector == i);
    CHECK(!strcmp(CBH_PROTOCOL_KEY, "ABPInterfaceProtocol") && !strcmp(CBH_INTERFACE_CLASS, "AppleBasebandPCIInterface"));
    if (!failures) printf("PASS channel table\n");
}

/* ------------------------------------------------------------------ data port and activation */
static void test_data(void)
{
    static struct cbq_bridge b;
    uint8_t out[256], sdu[128];
    uint32_t len = 0;
    cbq_init(&b);

    /* CommCenter's Bind Mux Data Port names Apple's PCIe endpoint: it becomes the embedded one, everything else kept */
    const uint8_t bind_pcie[] = { 0, 3, 0, 0xa2, 0, 15, 0, 0x10, 8, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0x11, 1, 0, 2 };
    struct cbd_fix f = cbd_endpoint_fix(CBD_SVC_WDS, bind_pcie, sizeof bind_pcie, out, sizeof out);
    const uint8_t bind_ipa[] = { 0, 3, 0, 0xa2, 0, 15, 0, 0x10, 8, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0x11, 1, 0, 2 };
    CHECK(f.len == sizeof bind_ipa && !memcmp(out, bind_ipa, sizeof bind_ipa) && f.before_type == 3 && f.before_interface == 4 && !f.inserted);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_ipa, sizeof bind_ipa, out, sizeof out).len);   /* already right */

    /* a mux id alone (the dormancy driver's bind, QOS bind): the endpoint is added */
    const uint8_t bind_mux[] = { 0, 9, 0, 0x2b, 0, 4, 0, 0x11, 1, 0, 1 };
    f = cbd_endpoint_fix(CBD_SVC_QOS, bind_mux, sizeof bind_mux, out, sizeof out);
    const uint8_t bind_mux_ipa[] = { 0, 9, 0, 0x2b, 0, 15, 0, 0x11, 1, 0, 1, 0x10, 8, 0, 4, 0, 0, 0, 1, 0, 0, 0 };
    CHECK(f.len == sizeof bind_mux_ipa && !memcmp(out, bind_mux_ipa, sizeof bind_mux_ipa) && f.inserted);
    /* a data port without a mux id, other messages and malformed ones go as they are */
    const uint8_t bind_sio[] = { 0, 9, 0, 0x2b, 0, 5, 0, 0x12, 2, 0, 1, 0x0e };
    CHECK(!cbd_endpoint_fix(CBD_SVC_QOS, bind_sio, sizeof bind_sio, out, sizeof out).len);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_mux, sizeof bind_mux, out, sizeof out).len);   /* WDS 0x2b is another message */
    const uint8_t broken[] = { 0, 9, 0, 0xa2, 0, 9, 0, 0x11, 9, 0, 1 };
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, broken, sizeof broken, out, sizeof out).len);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_pcie, sizeof bind_pcie, out, 16).len);         /* no room: unchanged */

    /* the setup: DPM Open Port, then WDA Set Data Format with QMAPv4, then QMAP when the modem keeps another */
    struct cbd_setup st;
    memset(&st, 0, sizeof st);
    st.has_dpm = st.has_wda = 1;
    uint32_t service = 0, n = cbd_next(&st, sdu, sizeof sdu, &service);
    const uint8_t dpm[] = { 0, 1, 0, 0x20, 0, 20, 0, 0x11, 17, 0, 1, 4, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 11, 0, 0, 0 };
    CHECK(service == CBD_SVC_DPM && n == sizeof dpm && !memcmp(sdu, dpm, sizeof dpm) && st.step == CBD_DPM);
    const uint8_t dpm_ok[] = { 2, 1, 0, 0x20, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0 };
    CHECK(!cbd_answer(&st, CBD_SVC_WDA, dpm_ok, sizeof dpm_ok) && cbd_answer(&st, CBD_SVC_DPM, dpm_ok, sizeof dpm_ok) && st.dpm_error == 0);
    n = cbd_next(&st, sdu, sizeof sdu, &service);
    CHECK(service == CBD_SVC_WDA && n == 7 + 5 * 7 + 11 && cbd_rd16(sdu + 3) == CBD_WDA_SET_DATA_FORMAT && cbd_u32(sdu, n, 0x11) == 2 &&
          cbd_u32(sdu, n, 0x12) == CBD_DAP_QMAP && cbd_u32(sdu, n, 0x13) == CBD_DAP_QMAP && cbd_u32(sdu, n, 0x15) == 32 &&
          cbd_u32(sdu, n, 0x16) == 16384 && cbd_find(sdu, n, 0x17, &len) && len == 8 && cbd_rd32(cbd_find(sdu, n, 0x17, &len)) == 4);
    uint8_t wda_qmap[] = { 2, 0, 0, 0x20, 0, 21, 0, 2, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 2, 0, 0, 0, 0x13, 4, 0, 5, 0, 0, 0 };
    wda_qmap[1] = (uint8_t)st.txn;
    CHECK(cbd_answer(&st, CBD_SVC_WDA, wda_qmap, sizeof wda_qmap) && st.dl_dap == CBD_DAP_QMAP);
    CHECK(!cbd_next(&st, sdu, sizeof sdu, &service) && st.step == CBD_DONE);
    /* without DPM on the modem the setup starts at WDA; without either it ends at once */
    memset(&st, 0, sizeof st);
    st.has_wda = 1;
    CHECK(cbd_next(&st, sdu, sizeof sdu, &service) && service == CBD_SVC_WDA && st.step == CBD_WDA);
    memset(&st, 0, sizeof st);
    CHECK(!cbd_next(&st, sdu, sizeof sdu, &service) && st.step == CBD_DONE);

    /* the bridge's own clients: answers come back on the internal channel, not to ABM */
    CHECK(cbq_server_new(&b, CBD_SVC_WDA, 1, 0, 0x30));
    const int slot = cbq_client_alloc(&b, CBQ_INTERNAL, CBD_SVC_WDA);
    CHECK(slot >= 0);
    struct cbq_result r = cbq_from_modem(&b, CBQ_PORT_BASE + (uint32_t)slot, 0, wda_qmap, sizeof wda_qmap, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.channel == CBQ_INTERNAL && r.service == CBD_SVC_WDA);

    /* activation register: success with the request's transaction, then the indication with kActivated */
    const uint8_t act[] = { 0, 0x34, 0x12, 0x0c, 0xb0, 0, 0 };
    n = cbq_private_answer(act, CBD_SVC_BSP, 3, 0, 0, 0, out, sizeof out);
    const uint8_t act_ok[] = { 1, 19, 0, 0x80, 0xe2, 3, 2, 0x34, 0x12, 0x0c, 0xb0, 7, 0, 2, 4, 0, 0, 0, 0, 0 };
    CHECK(n == sizeof act_ok && !memcmp(out, act_ok, sizeof act_ok));
    uint8_t tlvs[CBD_ACTIVATION_SIM_TLVS];
    const uint32_t t = cbd_activation_state(tlvs, CBD_ACTIVATION_TLVS, NULL, NULL);
    n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, 3, tlvs, t, out, sizeof out);
    /* kActivated, service provisioned (0x4f = 2), slot one's card provisioned (0x50 = 2), both slots accepted (0x56, 0x57 = 1) */
    const uint8_t act_ind[] = { 1, 32, 0, 0x80, 0xe2, 3, 4, 0, 0, 0x0c, 0xb0, 20, 0, 1, 1, 0, 0, 0x4f, 1, 0, 2, 0x50, 1, 0, 2,
                                0x56, 1, 0, 1, 0x57, 1, 0, 1 };
    CHECK(t == CBD_ACTIVATION_TLVS && n == sizeof act_ind && !memcmp(out, act_ind, sizeof act_ind));
    CHECK(!cbd_activation_state(tlvs, CBD_ACTIVATION_TLVS - 1, NULL, NULL));   /* no room: nothing to send */
    /* CommCenter's EF_ICCID and EF_IMSI reads (synthetic values) are recognized, then go into the indication in reading order. */
    const uint8_t iccid_req[] = { 0, 7, 0, 0x20, 0, 13, 0, 1, 2, 0, 0, 0, 2, 5, 0, 0xe2, 0x2f, 2, 0, 0x3f };
    const uint8_t imsi_req[] = { 0, 8, 0, 0x20, 0, 13, 0, 1, 2, 0, 0, 0, 2, 5, 0, 0x07, 0x6f, 2, 0, 0x3f };
    CHECK(cbd_uim_read_file(iccid_req, sizeof iccid_req) == CBD_EF_ICCID && cbd_uim_read_file(imsi_req, sizeof imsi_req) == CBD_EF_IMSI);
    uint8_t iccid_ans[] = { 2, 7, 0, 0x20, 0, 22, 0, 2, 4, 0, 0, 0, 0, 0, 0x11, 12, 0, 10, 0,
                            0x98, 0x10, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32, 0x54, 0x76 };
    uint8_t ef[16], iccid[CBD_ICCID_BYTES], imsi[CBD_IMSI_BYTES];
    uint32_t eflen = cbd_uim_read_content(iccid_ans, sizeof iccid_ans, ef, sizeof ef);
    CHECK(eflen == 10 && cbd_iccid_from_ef(ef, eflen, iccid) && iccid[0] == 0x89 && iccid[1] == 0x01 && iccid[9] == 0x67);
    iccid_ans[17] = 9;   /* a content length that does not match its TLV is not a read */
    CHECK(!cbd_uim_read_content(iccid_ans, sizeof iccid_ans, ef, sizeof ef));
    /* IMSI 001010123456789: length 8, digit 1 above parity 9, then pairs low nibble first */
    const uint8_t ef_imsi[9] = { 8, 0x09, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98 };
    CHECK(cbd_imsi_from_ef(ef_imsi, sizeof ef_imsi, imsi));
    const uint8_t imsi_bcd[8] = { 0x00, 0x10, 0x10, 0x12, 0x34, 0x56, 0x78, 0x9f };
    CHECK(!memcmp(imsi, imsi_bcd, sizeof imsi_bcd));
    const uint32_t ts = cbd_activation_state(tlvs, sizeof tlvs, iccid, imsi);
    CHECK(ts == CBD_ACTIVATION_SIM_TLVS && tlvs[20] == 0x42 && tlvs[21] == 10 && !memcmp(tlvs + 23, iccid, 10) &&
          tlvs[33] == 0x43 && tlvs[34] == 8 && !memcmp(tlvs + 36, imsi, 8));
    CHECK(cbd_activation_state(tlvs, sizeof tlvs, iccid, NULL) == CBD_ACTIVATION_TLVS + 13);
    CHECK(!cbd_activation_state(tlvs, CBD_ACTIVATION_SIM_TLVS - 1, iccid, imsi));
    /* Carrier bundle file push (BSP 0xa000): segment 3 of 9 of file type 9 is answered with success and the next segment, 4. */
    const uint8_t seg_req[] = { 0, 4, 0, 0x00, 0xa0, 13, 0, 1, 10, 0, 9, 0, 3, 0, 9, 0, 0xde, 0xad, 0xbe, 0xef };
    struct cbd_file_segment fs;
    CHECK(cbd_bsp_file_segment(seg_req, sizeof seg_req, &fs) && fs.type == 9 && fs.segment == 3 && fs.count == 9 && fs.bytes == 4);
    uint8_t seg_tlv[8];
    const uint32_t segt = cbd_bsp_file_answer(&fs, seg_tlv, sizeof seg_tlv);
    n = cbq_private_answer(seg_req, CBD_SVC_BSP, 1, 0, seg_tlv, segt, out, sizeof out);
    const uint8_t seg_ok[] = { 1, 24, 0, 0x80, 0xe2, 1, 2, 4, 0, 0x00, 0xa0, 12, 0, 2, 4, 0, 0, 0, 0, 0, 0x40, 2, 0, 4, 0 };
    CHECK(n == sizeof seg_ok && !memcmp(out, seg_ok, sizeof seg_ok));
    /* After the last segment: FileTransferStatus 0xa001 with {type 9, success} and subscription 1. */
    uint8_t done_tlv[12];
    const uint32_t dt = cbd_bsp_file_done(&fs, done_tlv, sizeof done_tlv);
    n = cbq_private_indication(CBD_BSP_FILE_TRANSFER_STATUS, CBD_SVC_BSP, 1, done_tlv, dt, out, sizeof out);
    const uint8_t done_ind[] = { 1, 21, 0, 0x80, 0xe2, 1, 4, 0, 0, 0x01, 0xa0, 9, 0, 1, 2, 0, 9, 0, 0x10, 1, 0, 1 };
    CHECK(dt == 9 && n == sizeof done_ind && !memcmp(out, done_ind, sizeof done_ind));
    /* BSP 0xb001: chip id and serial, little endian, chip id first. */
    uint8_t chip_tlv[12];
    CHECK(cbd_bsp_chip_info(0x009540e1u, 0x0c0ac42bu, chip_tlv, sizeof chip_tlv) == 11 && chip_tlv[0] == 1 && chip_tlv[1] == 8 &&
          chip_tlv[3] == 0xe1 && chip_tlv[5] == 0x95 && chip_tlv[7] == 0x2b && chip_tlv[10] == 0x0c);
    uint8_t seg_bad[sizeof seg_req]; memcpy(seg_bad, seg_req, sizeof seg_req); seg_bad[12] = 9;   /* segment past the count */
    CHECK(!cbd_bsp_file_segment(seg_bad, sizeof seg_bad, &fs));
    n = cbq_private_unsupported(act, 0xe5, 2, out, sizeof out);
    CHECK(n == 20 && out[4] == 0xe5 && cbq_rd16(out + 16) == 1 && cbq_rd16(out + 18) == CBQ_ERR_NOT_SUPPORTED);
    /* Apple's report configuration 0x555b on NAS and WDS is answered here; other messages, services and responses still go to the modem. */
    const uint8_t freq_req[] = { 0, 7, 0, 0x5b, 0x55, 4, 0, 0xb0, 1, 0, 1 };
    const uint8_t stats_req[] = { 0, 9, 0, 0x5b, 0x55, 24, 0, 1, 9, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 9, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(cbd_vendor_reporting(CBD_SVC_NAS, freq_req, sizeof freq_req) && cbd_vendor_reporting(CBD_SVC_WDS, stats_req, sizeof stats_req));
    CHECK(!cbd_vendor_reporting(CBD_SVC_BSP, freq_req, sizeof freq_req) && !cbd_vendor_reporting(CBD_SVC_NAS, freq_req, sizeof freq_req - 1));
    uint8_t cell_req[sizeof freq_req]; memcpy(cell_req, freq_req, sizeof freq_req); cell_req[3] = 0x56;   /* 0x5556 is not a setting */
    uint8_t freq_rsp[sizeof freq_req]; memcpy(freq_rsp, freq_req, sizeof freq_req); freq_rsp[0] = 2;
    CHECK(!cbd_vendor_reporting(CBD_SVC_NAS, cell_req, sizeof cell_req) && !cbd_vendor_reporting(CBD_SVC_NAS, freq_rsp, sizeof freq_rsp));
    n = cbq_private_answer(freq_req, CBD_SVC_NAS, 3, 0, NULL, 0, out, sizeof out);
    CHECK(n == 20 && out[4] == CBD_SVC_NAS && out[5] == 3 && out[6] == 2 && cbq_rd16(out + 7) == 7 && cbq_rd16(out + 9) == 0x555b &&
          cbq_rd16(out + 16) == 0 && cbq_rd16(out + 18) == 0);
    /* Two WDS clients on one mux: only modem success activates each family, and stopping IPv4 preserves IPv6. */
    struct cbp_client calls[2] = { { 0 }, { 0 } };
    uint8_t wire[64], reply[64], v;
    for (uint32_t i = 0; i < 2; i++) {
        v = 1;
        uint32_t wn = cbd_request(wire, 1, CBD_WDS_BIND_MUX_DATA_PORT, cbd_tlv(wire + 7, 57, 0x11, &v, 1));
        CHECK(cbp_observe(&calls[i], wire, wn) && !calls[i].mux);
        uint32_t rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR) && calls[i].mux == 1);
        v = i ? 6 : 4;
        wn = cbd_request(wire, 2, CBP_FAMILY, cbd_tlv(wire + 7, 57, 1, &v, 1));
        CHECK(cbp_observe(&calls[i], wire, wn));
        rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR));
        wn = cbd_request(wire, 3, CBP_START, 0);
        CHECK(cbp_observe(&calls[i], wire, wn) && !calls[i].active);
        uint8_t handle[7];
        uint32_t extra = cbd_tlv_u32(handle, sizeof handle, 1, 0x1234 + i);
        rn = cbq_private_answer(wire, 1, 1, 0, handle, extra, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR) && calls[i].active);
    }
    CHECK(cbp_active_families(calls, 2, 1) == 3 && !cbp_active_families(calls, 2, 2));
    cbd_request(wire, 4, CBP_STOP, 0);
    CHECK(cbp_observe(&calls[0], wire, 7));
    uint32_t rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
    CHECK(cbp_observe(&calls[0], reply + CBQ_HDR, rn - CBQ_HDR));
    CHECK(cbp_active_families(calls, 2, 1) == 2);
    const uint8_t disconnected[] = { 4, 0, 0, CBP_STATUS, 0, 5, 0, 1, 2, 0, 1, 0 };
    CHECK(cbp_observe(&calls[1], disconnected, sizeof disconnected));
    CHECK(!cbp_active_families(calls, 2, 1));
    if (!failures) printf("PASS data port and activation\n");
}

int main(void) {
  test_qmux();test_sahara();test_sahara_command();test_registers();
  test_control();test_channels();test_data();return failures?1:0;
}
