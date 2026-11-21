/* USB multiplex framing. Written from the wire format documented by usbmuxd;
 * no pairing, certificates, identity generation or activation lives here.
 * The same bounded core is compiled in the firmware and the host-side tests. */
#ifndef HAKIOS_LINK_MUX_CORE_H
#define HAKIOS_LINK_MUX_CORE_H
#define CM_CONNECTIONS 8
#define CM_DATA 16384u
#define CM_PACKET (CM_DATA + 64u)
#define CM_QUEUE 32u
#define CM_INPUT 65536u
#define CM_SYN 2u
#define CM_RST 4u
#define CM_ACK 16u
struct cm_connection {
    unsigned state; /* 0 free, 1 open, 2 connecting, 3 SYNACK, 4 established, 5 close */
    uint32_t stream, receive_next, transmit_next, transmit_acked, window;
    uint16_t host_port, device_port;
    uint32_t input_length;
    uint8_t input[CM_DATA];
    uint64_t next_poll, deadline;
    unsigned ack_pending;
};
struct cm_packet { uint32_t length; uint8_t bytes[CM_PACKET]; };
struct cm_mux {
    unsigned version, head, count, cursor, failed;
    uint16_t transmit_sequence, receive_sequence;
    uint32_t input_length;
    uint8_t input[CM_INPUT];
    struct cm_connection connections[CM_CONNECTIONS];
    struct cm_packet queue[CM_QUEUE];
    uint64_t frames_in, frames_out, connections_opened, rejected, bytes_in, bytes_out;
};
static uint16_t cm_get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t cm_get32(const uint8_t *p) { return ((uint32_t)cm_get16(p) << 16) | cm_get16(p+2); }
static void cm_put16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static void cm_put32(uint8_t *p, uint32_t n) { cm_put16(p,(uint16_t)(n>>16)); cm_put16(p+2,(uint16_t)n); }
static void cm_shift(uint8_t *p, unsigned total, unsigned used) {
    for (unsigned i=used; i<total; i++) p[i-used]=p[i];
}
static struct cm_packet *cm_begin(struct cm_mux *m, unsigned protocol, unsigned length) {
    unsigned header=m->version>=2 ? 16u:8u;
    if (m->count==CM_QUEUE || length+header>CM_PACKET) return NULL;
    struct cm_packet *p=&m->queue[(m->head+m->count)%CM_QUEUE];
    memset(p->bytes,0,header+length); p->length=header+length;
    cm_put32(p->bytes,protocol); cm_put32(p->bytes+4,p->length);
    if (m->version>=2) {
        cm_put32(p->bytes+8,0xfeedfaceu);
        cm_put16(p->bytes+12,m->transmit_sequence++);
        cm_put16(p->bytes+14,m->receive_sequence);
    }
    m->count++; m->frames_out++; return p;
}
static int cm_tcp(struct cm_mux *m, struct cm_connection *c, unsigned flags,
    const void *data, unsigned length) {
    struct cm_packet *p=cm_begin(m,6,20+length); if (!p) return 0;
    uint8_t *tcp=p->bytes+(m->version>=2 ? 16:8);
    cm_put16(tcp,c->device_port); cm_put16(tcp+2,c->host_port);
    cm_put32(tcp+4,c->transmit_next); cm_put32(tcp+8,c->receive_next);
    tcp[12]=0x50; tcp[13]=(uint8_t)flags;
    cm_put16(tcp+14,(uint16_t)((CM_DATA-c->input_length)>>8));
    if (length) memcpy(tcp+20,data,length);
    c->ack_pending=0;
    return 1;
}
static void cm_reject(struct cm_mux *m, uint16_t host, uint16_t device, uint32_t ack) {
    static struct cm_connection c; memset(&c,0,sizeof(c));
    c.host_port=host; c.device_port=device; c.receive_next=ack;
    (void)cm_tcp(m,&c,CM_RST,NULL,0); m->rejected++;
}
static void cm_reset_connection(struct cm_mux *m, struct cm_connection *c) {
    (void)cm_tcp(m,c,CM_RST,NULL,0);
    c->state=5;
}
static void cm_frame(struct cm_mux *m, const uint8_t *p, unsigned n, uint64_t now) {
    unsigned protocol=cm_get32(p), header=m->version>=2 ? 16u:8u;
    m->frames_in++;
    if (protocol==0 && !m->version) {
        if (n!=20 || (cm_get32(p+8)!=1 && cm_get32(p+8)!=2)) { m->failed=1; return; }
        unsigned version=cm_get32(p+8);
        struct cm_packet *reply=cm_begin(m,0,12);
        if (!reply) { m->failed=1; return; }
        cm_put32(reply->bytes+8,version);
        m->version=version; m->receive_sequence=0xffff; return;
    }
    if (!m->version || n<header || (m->version>=2 && cm_get32(p+8)!=0xfeedfaceu)) {
        m->failed=1; return;
    }
    if (m->version>=2) m->receive_sequence=cm_get16(p+12);
    if (protocol==2) { if (n!=header+1) m->failed=1; return; }
    if (protocol==1) return; /* host control has no stream payload */
    if (protocol!=6 || n<header+20) { m->failed=1; return; }
    const uint8_t *tcp=p+header;
    unsigned tcp_length=(tcp[12]>>4)*4u;
    if (tcp_length<20 || tcp_length>n-header) { m->failed=1; return; }
    unsigned length=n-header-tcp_length, flags=tcp[13];
    uint16_t host=cm_get16(tcp), device=cm_get16(tcp+2);
    uint32_t seq=cm_get32(tcp+4), ack=cm_get32(tcp+8);
    struct cm_connection *c=NULL;
    for (unsigned i=0;i<CM_CONNECTIONS;i++) {
        struct cm_connection *found=&m->connections[i];
        if (found->state && found->host_port==host && found->device_port==device) { c=found; break; }
    }
    if (flags & CM_RST) { if (c) c->state=5; return; }
    if (flags==CM_SYN && !length) {
        if (c) { cm_reset_connection(m,c); return; }
        /* Native lockdown publishes service ports after its own authorization.
         * This layer transports opaque bytes; it does not inspect TLS or grant
         * trust. Port zero is never a connectable service. */
        if (!device) { cm_reject(m,host,device,seq+1); return; }
        for (unsigned i=0;i<CM_CONNECTIONS;i++) if (!m->connections[i].state) { c=&m->connections[i]; break; }
        if (!c) { cm_reject(m,host,device,seq+1); return; }
        memset(c,0,sizeof(*c)); c->state=1; c->host_port=host; c->device_port=device;
        c->receive_next=seq+1; c->window=(uint32_t)cm_get16(tcp+14)<<8;
        c->deadline=now+5000; return;
    }
    if (!c) { cm_reject(m,host,device,seq); return; }
    if (flags!=CM_ACK && flags!=(CM_ACK|8u)) { cm_reset_connection(m,c); return; }
    if (c->state!=3 && c->state!=4) return;
    /* ACKs cannot acknowledge bytes never emitted; subtraction also handles wrap. */
    if ((uint32_t)(ack-c->transmit_acked)>(uint32_t)(c->transmit_next-c->transmit_acked)) {
        cm_reset_connection(m,c); return;
    }
    c->transmit_acked=ack; c->window=(uint32_t)cm_get16(tcp+14)<<8;
    if (c->state==3) {
        if (ack!=1 || seq!=c->receive_next) { cm_reset_connection(m,c); return; }
        c->state=4; m->connections_opened++;
    }
    if (seq!=c->receive_next) { c->ack_pending=1; return; }
    if (length>CM_DATA-c->input_length) { cm_reset_connection(m,c); return; }
    if (length) {
        memcpy(c->input+c->input_length,tcp+tcp_length,length);
        c->input_length+=length; c->receive_next+=length; c->ack_pending=1;
        m->bytes_in+=length;
    }
}
static void cm_feed(struct cm_mux *m, const uint8_t *data, unsigned length, uint64_t now) {
    if (m->failed) return;
    if (length>CM_INPUT-m->input_length) { m->failed=1; return; }
    memcpy(m->input+m->input_length,data,length); m->input_length+=length;
    while (m->input_length>=8) {
        unsigned n=cm_get32(m->input+4), minimum=m->version>=2 ? 16u:8u;
        if (n<minimum || n>CM_INPUT) { m->failed=1; return; }
        if (n>m->input_length) break;
        cm_frame(m,m->input,n,now); cm_shift(m->input,m->input_length,n); m->input_length-=n;
        if (m->failed) return;
    }
}
static void cm_connected(struct cm_mux *m, struct cm_connection *c) {
    if (cm_tcp(m,c,CM_SYN|CM_ACK,NULL,0)) { c->transmit_next=1; c->state=3; }
}
static unsigned cm_receive_capacity(const struct cm_connection *c) {
    uint32_t flight=c->transmit_next-c->transmit_acked;
    if (flight>=c->window) return 0;
    uint32_t available=c->window-flight; return available>CM_DATA ? CM_DATA:available;
}
#endif
