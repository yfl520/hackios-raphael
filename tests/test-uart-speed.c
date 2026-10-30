
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
static uint32_t g_oper_baud, g_baud;
static bool g_platform_prepare, g_rx_running;
enum {SE_UART_MANUAL_RFR=1,SE_UART_TX_TRANS_CFG=2};
struct bt_h4 {uint8_t data[512];uint32_t got,off,len;bool found;};
static unsigned seq, sent, ack_step, hci_step, unblocked;
static int fault;
static void bt_se_wr(unsigned reg,uint32_t value,const char *why) {
    (void)why;++seq;
    if(reg==SE_UART_MANUAL_RFR && value==0x80000002u) assert(!sent);
    if(reg==SE_UART_MANUAL_RFR && value==0) unblocked=seq;
}
static bool bt_tx_quiet(const uint8_t *p,uint32_t n) {
    ++seq;assert(n==5 && !memcmp(p,"\x01\x48\xfc\x01\x0e",5));sent=seq;
    return fault!=1;
}
static void bt_sleep_ms(unsigned ms){assert(sent && ms==10);++seq;}
static bool bt_cancel_s(void){assert(sent);++seq;return fault!=2;}
static bool bt_platform_oper_clock(void){assert(!g_rx_running);++seq;return fault!=3;}
static bool bt_set_baud(uint32_t b){assert(!g_rx_running && b==3000000);g_baud=b;++seq;return fault!=4;}
static bool bt_start_rx(void){assert(g_baud==3000000);g_rx_running=true;++seq;return fault!=5;}
static void bt_h4_collect(uint8_t ev,uint16_t op,uint32_t ms,struct bt_h4 *h) {
    assert(ev==0xff && op==0 && ms==100 && unblocked && g_baud==3000000);
    ack_step=++seq;memset(h,0,sizeof(*h));h->found=fault!=6;h->off=2;h->len=fault==7?4:5;
    memcpy(h->data+2,"\x04\xff\x02\x92\x01",5);
    if(fault==8)h->data[6]=0;
    if(fault==9)h->data[5]=0x19;
}
static void bt_dump_counted(const char *s,const uint8_t *p,uint32_t n){(void)s;(void)p;(void)n;}
static bool bt_h4_xfer(const uint8_t *cmd,uint32_t n,uint16_t op,uint32_t ms,struct bt_h4 *h,bool drain) {
    (void)h;assert(ack_step && n==4 && op==0x1001 && ms==500 && drain && cmd[1]==1);
    hci_step=++seq;return fault!=10;
}
static bool bt_cmd_complete(const struct bt_h4 *h,uint16_t op,const uint8_t **p,uint32_t *n) {
    (void)h;assert(op==0x1001);static uint8_t body[12];memset(body,0,sizeof(body));
    body[3]=fault==12?1:0;*p=body;*n=fault==13?4:12;return fault!=11;
}

/* Linux v6.12 hci_qca.c qca_set_speed/qca_set_baudrate: block controller
 * RX with RTS while sending fc48 at the old rate. WCN3990 replies at the
 * new rate, so do not synchronously wait for old-rate Command Complete.
 * Only this initialization thread owns the UART; no host packet is pending. */
static bool
bt_oper_speed(void)
{
    if (g_oper_baud == 115200u) return true;
    if (g_oper_baud != 3000000u || !g_platform_prepare || g_baud != 115200u) return false;
    static const uint8_t change[5] = { 0x01, 0x48, 0xfc, 0x01, 0x0e };
    static const uint8_t version[4] = { 0x01, 0x01, 0x10, 0x00 };
    struct bt_h4 h4;
    const uint8_t *payload;
    uint32_t plen;
    bool ok = false;
    printf("raphael-bt: operating UART switch 115200 -> 3000000 begins\n");
    bt_se_wr(SE_UART_MANUAL_RFR, 0x80000002u, "RTS not ready during baud transition");
    bt_se_wr(SE_UART_TX_TRANS_CFG, 0, "honour controller CTS");
    if (!bt_tx_quiet(change, sizeof(change))) goto out;
    bt_sleep_ms(10);
    if (!bt_cancel_s()) goto out;
    g_rx_running = false;
    if (!bt_platform_oper_clock() || !bt_set_baud(g_oper_baud) || !bt_start_rx()) goto out;
    bt_se_wr(SE_UART_MANUAL_RFR, 0, "hardware RTS at new rate");
    bt_h4_collect(0xff, 0, 100, &h4);
    if (!h4.found || h4.len < 5) {
        printf("raphael-bt: operating UART switch has no complete vendor acknowledgment\n");
        goto out;
    }
    const uint8_t *event = h4.data + h4.off;
    bt_dump_counted("operating baud acknowledgment", event, h4.len);
    /* QCA vendor BAUDRATE_CHANGE_SUCCESS is 1, unlike HCI status success 0.
     * Android qcom/libbt-vendor hw_rome.h and hw_rome.c define/check it. */
    if (event[1] != 0xff || event[2] != 2 || event[3] != 0x92 || event[4] != 1) {
        printf("raphael-bt: operating UART acknowledgment unexpected or refused\n");
        goto out;
    }
    if (!bt_h4_xfer(version, sizeof(version), 0x1001, 500, &h4, true) ||
        !bt_cmd_complete(&h4, 0x1001, &payload, &plen) || plen < 12 || payload[3] != 0) {
        printf("raphael-bt: operating UART failed real HCI verification\n");
        goto out;
    }
    printf("raphael-bt: operating UART 3000000 acknowledged; real HCI version status0\n");
    ok = true;
out:
    bt_se_wr(SE_UART_MANUAL_RFR, 0, "restore hardware RTS");
    return ok;
}

static void reset(int f) {g_oper_baud=3000000;g_baud=115200;g_platform_prepare=true;g_rx_running=true;seq=sent=ack_step=hci_step=unblocked=0;fault=f;}
int main(void) {
    reset(0);assert(bt_oper_speed() && sent && ack_step>sent && hci_step>ack_step && unblocked>hci_step);
    for(int i=1;i<=13;i++){reset(i);assert(!bt_oper_speed());assert(unblocked);}
    reset(0);g_oper_baud=115200;assert(bt_oper_speed() && !sent && !seq);
    reset(0);g_oper_baud=921600;assert(!bt_oper_speed() && !sent);
    reset(0);g_platform_prepare=false;assert(!bt_oper_speed() && !sent);
    reset(0);g_baud=2400;assert(!bt_oper_speed() && !sent);
    puts("UART speed transition order, real acknowledgment/HCI gates, 13 failure paths and legacy mode pass");
    return 0;
}
