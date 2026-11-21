/* SPDX-License-Identifier: BSD-2-Clause */
/* Raphael IPA4.1 QMI control, on the existing MPSS/QRTR owner thread. */
static struct {
    struct hakios_qrtr_port *host,*client;
    uint32_t node,remote,peer_node,peer_port;
    uint16_t transaction;
    bool started,published,init_sent,init_accepted,registered,uc_ready;
    bool prepared,prepare_tried,complete_sent,data_started,data_failed;
    uint32_t next_retry;
    /* Native INSTALL_FILTER_RULE accepts and caches; installation has its
     * own later FILTER_INSTALLED notification. Never fabricate that event. */
    uint8_t ul_rules[35][173],ul_count;
    uint64_t ul_xlat;
    bool ul_cached,config_accepted;
} ripa_qmi;
static bool ripa_data_clock_available(void)
{
    return ripa.hardware && ripa_data_opted_in();
}
static bool ripa_data_opted_in(void)
{
    uint32_t value=0;
    return PE_parse_boot_argn("ipa_preboot",&value,sizeof value) && value==6;
}
static bool ripa_qmi_valid(const uint8_t *p,size_t length)
{
    if(!p || length<7 || raphael_ipa_u16(p+5)!=length-7)return false;
    bool seen[256]={0};size_t at=7;
    while(at<length) {
        if(length-at<3)return false;
        uint8_t type=p[at];size_t n=raphael_ipa_u16(p+at+1);at+=3;
        if(n>length-at || seen[type])return false;
        seen[type]=true;at+=n;
    }
    return true;
}
static int ripa_qmi_reply(uint32_t node,uint32_t port,uint16_t transaction,uint16_t message,uint16_t error)
{
    uint8_t p[]={2,(uint8_t)transaction,(uint8_t)(transaction>>8),(uint8_t)message,(uint8_t)(message>>8),
        7,0,2,4,0,(uint8_t)(error!=0),0,(uint8_t)error,(uint8_t)(error>>8)};
    return qrtr_send(ripa_qmi.host,node,port,p,sizeof p);
}
/* Packed sizes/offsets from pinned Xiaomi QMI encoder; see qmi-wire-layout.json.
 * Raw rule bytes remain private in RAM, not printed to screen or host log. */
static bool ripa_ul_rules_valid(const uint8_t *p,size_t length,
    const uint8_t **rules,uint8_t *count,uint64_t *xlat)
{
    size_t n=0;const uint8_t *v=qmi_field(p,length,0x15,&n);
    if(!v || !n || v[0]>35 || n!=1+(size_t)v[0]*173)return false;
    *count=v[0];*rules=v+1;*xlat=0;
    for(unsigned i=0;i<*count;i++) {
        const uint8_t *r=v+1+i*173,*eq=r+4;
        if(eq[6]>2 || eq[17]>2 || eq[53]>2 || eq[72]>2)return false;
        if(r[158]>1 || r[163]>1 || r[172]>1)return false;
    }
    v=qmi_field(p,length,0x14,&n);
    if(v) {
        if(!n || v[0]>*count || n!=1+(size_t)v[0]*4)return false;
        for(unsigned i=0;i<v[0];i++) {
            uint32_t index=raphael_ipa_u32(v+1+4*i);
            if(index>=*count)return false;
            *xlat|=1ull<<index;
        }
    }
    for(unsigned type=0x11;type<=0x13;type++) {
        v=qmi_field(p,length,(uint8_t)type,&n);
        if(v && n!=4)return false;
    }
    return true;
}
static void ripa_qmi_filter_request(uint32_t node,uint32_t port,uint16_t transaction,
    const uint8_t *p,size_t length)
{
    const uint8_t *rules;uint8_t count;uint64_t xlat;
    if(!ripa_ul_rules_valid(p,length,&rules,&count,&xlat)) {
        /* Native copy failure also returns without sending success. */
        ((void)0);
        return;
    }
    /* Validate fully before replacing the previous request (single QRTR owner). */
    bzero(ripa_qmi.ul_rules,sizeof ripa_qmi.ul_rules);
    if(count)memcpy(ripa_qmi.ul_rules,rules,(size_t)count*173);
    ripa_qmi.ul_count=count;ripa_qmi.ul_xlat=xlat;ripa_qmi.ul_cached=true;
    uint8_t reply[14+4+35*4]={2,(uint8_t)transaction,(uint8_t)(transaction>>8),0x23,0,0,0,
        2,4,0,0,0,0,0,0x11,(uint8_t)(1+count*4),0,count};
    for(unsigned i=0;i<count;i++)memcpy(reply+18+4*i,rules+173*i+168,4);
    size_t size=18+(size_t)count*4;
    reply[5]=(uint8_t)(size-7);reply[6]=(uint8_t)((size-7)>>8);
    int rc=qrtr_send(ripa_qmi.host,node,port,reply,size);
    ((void)0);
}
static bool ripa_config_valid(const uint8_t *p,size_t length)
{
    /* Every scalar in pinned ipa3_config_req_msg_data_v01_ei is u32/enum,
     * except hw_deaggr_supported (TLV 0x11, u8). Unknown optional TLVs
     * are ignored as in the native QMI decoder. */
    for(size_t at=7;at<length;) {
        uint8_t type=p[at];size_t n=raphael_ipa_u16(p+at+1);
        if(type>=0x10 && type<=0x1e && n!=(type==0x11?1:4))return false;
        at+=3+n;
    }
    return true;
}
static void ripa_host_receive(__unused void *context,uint32_t node,uint32_t port,const void *data,size_t length)
{
    const uint8_t *p=data;
    if(node!=qrtr_peer || !ripa_qmi_valid(p,length) || p[0]!=0)return;
    uint16_t transaction=raphael_ipa_u16(p+1),message=raphael_ipa_u16(p+3);
    if(message==0x20 || message==0x35) {
        if(ripa_qmi_reply(node,port,transaction,message,0))return;
        ripa_qmi.peer_node=node;ripa_qmi.peer_port=port;
        if(message==0x20)ripa_qmi.registered=true;
        else ripa_qmi.uc_ready=true;
        ((void)0);
    } else if(message==0x27) {
        /* Xiaomi ipa_clients/ipa_mhi_client.c:2818: GSI transport skips
         * MHI sync programming and returns 0. This is the native no-op. */
        if(!ripa_config_valid(p,length))return;
        if(!ripa_qmi_reply(node,port,transaction,message,0))ripa_qmi.config_accepted=true;
        ((void)0);
    } else if(message==0x23) {
        ripa_qmi_filter_request(node,port,transaction,p,length);
    } else if(message==0x24) {
        /* Peer notification, not a claim that AP filters were installed. */
        ripa_qmi_reply(node,port,transaction,message,0);
    } else {
        ripa_qmi_reply(node,port,transaction,message,94);
        ((void)0);
    }
}
static void ripa_client_receive(__unused void *context,uint32_t node,uint32_t port,const void *data,size_t length)
{
    const uint8_t *p=data;
    if(node!=ripa_qmi.node || port!=ripa_qmi.remote || !ripa_qmi.init_sent ||
       !ripa_qmi_valid(p,length) || p[0]!=2 || raphael_ipa_u16(p+1)!=ripa_qmi.transaction ||
       raphael_ipa_u16(p+3)!=0x21)return;
    size_t size=0;const uint8_t *v=qmi_field(p,length,2,&size);
    if(!v || size!=4)return;
    uint16_t result=raphael_ipa_u16(v),error=raphael_ipa_u16(v+2);
    ripa_qmi.init_accepted=!result && !error;
    if(result || error)ripa_qmi.data_failed=true;
    ((void)0);
}
static void ripa_client_server(__unused void *context,bool arrived,uint32_t service,uint32_t version,uint32_t instance,uint32_t node,uint32_t port)
{
    if(service!=0x31 || version!=1 || instance!=2 || !ripa_qmi.started)return;
    if(!arrived) {
        if(node==ripa_qmi.node && port==ripa_qmi.remote) {
            raphael_ipa_data_stop();ripa_qmi.data_failed=true;
            ((void)0);
        }
        return;
    }
    ripa_qmi.node=node;ripa_qmi.remote=port;
}
static void ripa_qmi_stop(void)
{
    raphael_ipa_data_stop();
    ripa_qmi.started=false;
    if(ripa_qmi.published && ripa_qmi.host) {
        struct qrtr_control gone={.command=QRTR_DEL_SERVER,.words={0x31,0x101,QRTR_US,ripa_qmi.host->number}};
        qrtr_out_packet(QRTR_DEL_SERVER,QRTR_CONTROL,qrtr_peer,QRTR_CONTROL,&gone,sizeof gone);
    }
    if(ripa_qmi.host)qrtr_close(ripa_qmi.host);
    if(ripa_qmi.client)qrtr_close(ripa_qmi.client);
    bzero(&ripa_qmi,sizeof ripa_qmi);
}
static bool ripa_qmi_start(void)
{
    ((void)0);
    if(!ripa.hardware)return true;
    if(ripa_qmi.started)return true;
    ripa_qmi.host=qrtr_open(ripa_host_receive,NULL,NULL);
    ripa_qmi.client=qrtr_open(ripa_client_receive,ripa_client_server,NULL);
    if(!ripa_qmi.host || !ripa_qmi.client)goto fail;
    ripa_qmi.started=true;
    struct qrtr_control publish={.command=QRTR_NEW_SERVER,.words={0x31,0x101,QRTR_US,ripa_qmi.host->number}};
    if(qrtr_out_packet(QRTR_NEW_SERVER,QRTR_CONTROL,qrtr_peer,QRTR_CONTROL,&publish,sizeof publish))goto fail;
    ripa_qmi.published=true;
    if(qrtr_lookup(ripa_qmi.client,0x31,1,2))goto fail;
    return true;
fail:
    ripa_qmi_stop();return false;
}
static void ripa_data_tick(void)
{
    static uint64_t next_report;
    uint64_t now=now_us();
    if(ripa_data_opted_in() && now>=next_report) {
        struct smp2p status;bool valid=dsp_words(&status);
        int entry=valid?smp2p_entry(&status,"ipa"):-1;
        ((void)0);
        next_report=now+10000000;
    }
    if(!ripa.hardware || !ripa_data_opted_in() || ripa_qmi.data_failed)return;
    if(!ripa_qmi.prepare_tried) {
        /* Raphael's vendor SMP2P IPA input is a clock query, not the
         * SC7180 setup-ready protocol. Require the real EE0 enabled bit. */
        if(!ripa_qmi.started || !ripa_qmi.remote || !ripa_data_gsi_ready())return;
        ripa_qmi.prepare_tried=true;
        ripa_qmi.prepared=hakios_ipa_data_setup();
        if(!ripa_qmi.prepared) {ripa_qmi.data_failed=true;return;}
        ((void)0);
    }
    if(!ripa_qmi.started || !ripa_qmi.remote)return;
    if(!ripa_qmi.init_sent) {
        uint8_t p[RAPHAEL_IPA_INIT_BYTES+4];
        if(!ripa_qmi.transaction)ripa_qmi.transaction=1;
        size_t n=raphael_ipa_init_packet(p,sizeof p,ripa_qmi.transaction,ripa.restricted);
        if(!n) {ripa_qmi.data_failed=true;return;}
        /* Explicit fresh/uC state, matching the board's QMI optional TLV. */
        p[n++]=0x18;p[n++]=1;p[n++]=0;p[n++]=ripa_qmi.uc_ready?1:0;
        p[5]=(uint8_t)(n-7);p[6]=(uint8_t)((n-7)>>8);
        ripa_qmi.init_sent=qrtr_send(ripa_qmi.client,ripa_qmi.node,ripa_qmi.remote,p,n)==0;
        if(ripa_qmi.init_sent)((void)0);
        return;
    }
    if(ripa_qmi.init_accepted && ripa_qmi.uc_ready && ripa_qmi.registered && !ripa_qmi.complete_sent) {
        uint8_t p[]={4,2,0,0x22,0,7,0,2,4,0,0,0,0,0};
        ripa_qmi.complete_sent=qrtr_send(ripa_qmi.host,ripa_qmi.peer_node,ripa_qmi.peer_port,p,sizeof p)==0;
        if(ripa_qmi.complete_sent)((void)0);
    }
    if(ripa_qmi.complete_sent && !ripa_qmi.data_started) {
        ripa_qmi.data_started=raphael_ipa_data_start();
        if(!ripa_qmi.data_started)ripa_qmi.data_failed=true;
    }
    raphael_modem_data_poll();
}
