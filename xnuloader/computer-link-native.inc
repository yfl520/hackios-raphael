/* Firmware USB endpoint owner. Native mode offers explicit continuous and bounded sessions, returning to
 * the original diagnostic interface without stopping XNU or touching storage. */
#include "computer-link-mux-core.h"
#include "computer-link-audit.h"
static struct ca_reader cn_audit[CM_CONNECTIONS][2];
static unsigned cn_audit_count;
static void cn_audit_event(void *ctx,unsigned kind,const char *value) {
    if (cn_audit_count++>=160) return;
    int quiet=screen_quiet; screen_quiet=1;
    print("computer-link: native plist slot=%d %s=%s\n",(uint64_t)(uintptr_t)ctx,
        kind==1 ? "Request":"Error",value);
    screen_quiet=quiet;
}
static struct cm_mux cn_mux;
static unsigned cn_active, cn_deferred, cn_rx, cn_tx, cn_zlp, cn_exit;
static uint64_t cn_began, cn_switch_at, cn_pending_sequence;
static uint64_t cn_status_at, cn_exit_at;
static uint64_t cn_timeout_ms=180000;
static unsigned cn_link_seen;
static unsigned cn_pending_slot, cn_pending_op;
static uint64_t cn_usb_events, cn_usb_errors, cn_disconnects, cn_mailbox_errors;
static uint64_t cn_send_errors, cn_event_errors, cn_duplicate_attach, cn_active_events;
static uint64_t cn_last_send_status, cn_last_event_status;

static void cn_abort_io(void) {
    /* Never discard ownership while the controller still owns a transfer. */
    (void)USB->abort_xfer(USB_EP_OUT);
    (void)USB->abort_xfer(USB_EP_IN);
    cn_rx=cn_tx=cn_zlp=0;
}

static void cn_fail_io(void) {
    cn_usb_errors++; cn_exit=1; cn_abort_io();
}
static uint8_t cn_serial[82];
static const uint8_t cn_dev[18]={18,1,0x10,0x02,0,0,0,64,0xac,0x05,0xa8,0x12,0,1,1,2,3,1};
static const uint8_t cn_ss_dev[18]={18,1,0,3,0,0,0,9,0xac,0x05,0xa8,0x12,0,1,1,2,3,1};
static const uint8_t cn_cfg[32]={9,2,32,0,1,1,0,0x80,0x50,
    9,4,0,0,2,0xff,0xfe,2,4,7,5,0x81,2,0,2,0,7,5,1,2,0,2,0};
static const uint8_t cn_ss_cfg[44]={9,2,44,0,1,1,0,0x80,0x10,
    9,4,0,0,2,0xff,0xfe,2,4,7,5,0x81,2,0,4,0,6,0x30,4,0,0,0,
    7,5,1,2,0,4,0,6,0x30,4,0,0,0};
static const uint8_t cn_mfr[]={16,3,'H',0,'a',0,'c',0,'k',0,'i',0,'O',0,'S',0};
static const uint8_t cn_product[]={16,3,'K',0,'2',0,'0',0,' ',0,'P',0,'r',0,'o',0};
static const uint8_t cn_interface[]={14,3,'u',0,'s',0,'b',0,'m',0,'u',0,'x',0};

static uint64_t cn_ms(void) {
    return READ_SYSREG(CNTVCT_EL0)/(READ_SYSREG(CNTFRQ_EL0)/1000);
}
static void cn_publish_status(uint64_t now) {
    static unsigned previous;
    unsigned active=cn_active && !cn_exit && usb_connected && cn_mux.version;
    if (now-cn_status_at<500 && active==previous) return;
    struct cl_shared *s=cl_loader_shared(); if (!s) return;
    if (active!=previous) s->usb_epoch++;
    s->usb_connected=active; s->usb_native=active; s->usb_heartbeat=now;
    cl_loader_clean((uint64_t)s,(uint64_t)s+128);
    cn_status_at=now; previous=active;
}
static int cn_start_usb(int native) {
    static const void *cfg, *ss_cfg, *strings[5];
    static usb_device_descriptor_set set;
    cfg=native ? cn_cfg:usb_cfg_desc; ss_cfg=native ? cn_ss_cfg:usb_ss_cfg_desc;
    strings[0]=usb_str0; strings[1]=native ? cn_mfr:usb_str_mfr;
    strings[2]=native ? cn_product:usb_str_prod; strings[3]=native ? cn_serial:usb_str_serial;
    strings[4]=native ? cn_interface:usb_str_iface;
    memset(&set,0,sizeof(set));
    set.device_descriptor=native ? cn_dev:usb_dev_desc;
    set.ss_device_descriptor=native ? cn_ss_dev:usb_ss_dev_desc;
    set.descriptors=&cfg; set.ss_descriptors=&ss_cfg;
    set.device_qualifier=usb_qualifier; set.bos=usb_bos;
    set.strings=strings; set.string_count=5;
    efi_status cancel_out=USB->abort_xfer(USB_EP_OUT),cancel_in=USB->abort_xfer(USB_EP_IN);
    unsigned drained=0; efi_status drain_status=0;
    for (;drained<128;drained++) {
        uint32_t kind=UsbDeviceEventNoEvent; uint64_t bytes=0; usb_device_event_data event;
        drain_status=USB->handle_event(&kind,&bytes,&event);
        if (EFI_ERROR(drain_status) || kind==UsbDeviceEventNoEvent) break;
    }
    efi_status stop_status=USB->stop(); usb_connected=0; usb_rx_queued=0; usb_debug_fault=0;
    BS->stall(250000);
    efi_status start_status=USB->start_ex(&set);
    int quiet=screen_quiet; screen_quiet=1;
    print("computer-link: USB transition native=%d cancel_out=%x cancel_in=%x drained=%d drain_status=%x stop_status=%x start_status=%x\n",
        (uint64_t)native,cancel_out,cancel_in,(uint64_t)drained,drain_status,stop_status,start_status);
    print("computer-link: diagnostic ownership send_errors=%d event_errors=%d duplicate_attach=%d active_events=%d timeouts=%d status=%x\n",
        cd_send_errors,cd_event_errors,cd_duplicate_attach,cd_active_events,cd_timeouts,cd_last_status);
    screen_quiet=quiet;
    return !EFI_ERROR(start_status);
}
static int cn_submit(unsigned slot, unsigned op, unsigned length) {
    struct cl_shared *s=cl_loader_shared(); if (!s || cn_pending_sequence) return 0;
    cl_loader_flush((uint64_t)&s->response,(uint64_t)&s->response+128);
    if (s->request.sequence!=s->response.sequence) return 0;
    struct cm_connection *c=&cn_mux.connections[slot];
    s->request.operation=op; s->request.stream=c->stream;
    s->request.port=op==CL_OPEN ? c->device_port:0; s->request.length=length;
    if (op==CL_SEND && length) memcpy(s->request.data,c->input,length);
    cl_loader_clean((uint64_t)&s->request,(uint64_t)&s->request+sizeof(s->request));
    uint64_t seq=s->request.sequence+1; if (!seq) seq=1;
    s->request.sequence=seq;
    cl_loader_clean((uint64_t)&s->request,(uint64_t)&s->request+128);
    cn_pending_slot=slot; cn_pending_op=op; cn_pending_sequence=seq; return 1;
}
static void cn_complete(uint64_t now) {
    if (!cn_pending_sequence) return;
    struct cl_shared *s=cl_loader_shared(); if (!s) { cn_exit=1; return; }
    cl_loader_flush((uint64_t)&s->response,(uint64_t)&s->response+128);
    if (s->response.sequence!=cn_pending_sequence) return;
    cl_loader_flush((uint64_t)&s->response,(uint64_t)&s->response+sizeof(s->response));
    struct cm_connection *c=&cn_mux.connections[cn_pending_slot];
    unsigned op=cn_pending_op, closing=c->state==5;
    cn_pending_sequence=0;
    if (s->response.length>CM_DATA) { cn_mailbox_errors++; cn_exit=1; return; }
    unsigned error=s->response.error, length=s->response.length;
    if (op==CL_CLOSE) { memset(c,0,sizeof(*c)); return; }
    if (op==CL_OPEN) {
        if (error || !s->response.stream) {
            cn_mailbox_errors++; if (!closing) cm_reset_connection(&cn_mux,c);
            c->state=5; return;
        }
        c->stream=s->response.stream;
        memset(cn_audit[cn_pending_slot],0,sizeof(cn_audit[cn_pending_slot]));
        if (closing) return;
        c->state=2;
        if (s->response.connected) cm_connected(&cn_mux,c);
    } else if (error && error!=35) {
        cn_mailbox_errors++; if (!closing) cm_reset_connection(&cn_mux,c); return;
    } else if (closing) return;
    else if (op==CL_POLL) {
        if (s->response.connected) cm_connected(&cn_mux,c);
        c->next_poll=now+10;
    } else if (op==CL_SEND) {
        if (length>c->input_length) { cn_exit=1; return; }
        if (c->device_port==62078) ca_feed(&cn_audit[cn_pending_slot][0],c->input,length,cn_audit_event,(void *)(uintptr_t)cn_pending_slot);
        cm_shift(c->input,c->input_length,length); c->input_length-=length;
        if (length) c->ack_pending=1;
        c->next_poll=now+(error==35 ? 2:0);
    } else if (op==CL_RECEIVE) {
        if (length) {
            if (c->device_port==62078) ca_feed(&cn_audit[cn_pending_slot][1],s->response.data,length,cn_audit_event,(void *)(uintptr_t)cn_pending_slot);
            /* Reserve queue headroom before submitting this asynchronous read. */
            if (!cm_tcp(&cn_mux,c,CM_ACK,s->response.data,length)) { cn_exit=1; return; }
            c->transmit_next+=length; cn_mux.bytes_out+=length;
        } else if (!error) cm_reset_connection(&cn_mux,c);
        c->next_poll=now+(error==35 ? 4:0);
    }
}
static void cn_schedule(uint64_t now) {
    cn_complete(now); if (cn_pending_sequence) return;
    for (unsigned k=0;k<CM_CONNECTIONS;k++) {
        unsigned i=(cn_mux.cursor+k)%CM_CONNECTIONS;
        struct cm_connection *c=&cn_mux.connections[i];
        if (!c->state) continue;
        if (cn_exit) c->state=5;
        if ((c->state==1 || c->state==2 || c->state==3) && now>c->deadline)
            cm_reset_connection(&cn_mux,c);
        unsigned op=0, length=0;
        if (c->state==5) {
            if (!c->stream) { memset(c,0,sizeof(*c)); continue; }
            op=CL_CLOSE;
        } else if (cn_mux.count>CM_QUEUE-CM_CONNECTIONS-2) continue;
        else if (c->state==1) op=CL_OPEN;
        else if (c->state==2 && now>=c->next_poll) op=CL_POLL;
        else if (c->state==4 && now>=c->next_poll) {
            if (c->ack_pending) (void)cm_tcp(&cn_mux,c,CM_ACK,NULL,0);
            if (c->input_length) { op=CL_SEND; length=c->input_length; }
            else if ((length=cm_receive_capacity(c))) op=CL_RECEIVE;
        }
        if (op && cn_submit(i,op,length)) { cn_mux.cursor=(i+1)%CM_CONNECTIONS; return; }
    }
}
static void cn_pump(void) {
    uint64_t now=cn_ms();
    if (cn_deferred) {
        if (now<cn_switch_at) return;
        cn_deferred=0; memset(&cn_mux,0,sizeof(cn_mux)); cn_mux.receive_sequence=0xffff; cn_audit_count=0;
        cn_usb_events=cn_usb_errors=cn_disconnects=cn_mailbox_errors=0;
        cn_send_errors=cn_event_errors=cn_duplicate_attach=cn_active_events=0;
        cn_last_send_status=cn_last_event_status=0;
        cn_rx=cn_tx=cn_zlp=cn_exit=0; cn_pending_sequence=0; cn_began=now; cn_exit_at=0;
        cn_active=1; cn_link_seen=0;
        if (!cn_start_usb(1)) { cn_usb_errors++; cn_exit=1; }
    }
    /* Only the legacy diagnostic command has a session duration limit.
     * An unestablished persistent request still returns to diagnostics after
     * 30 seconds, so missing Apple drivers cannot strand the debug interface. */
    if ((cn_timeout_ms && now-cn_began>=cn_timeout_ms) ||
        (!cn_timeout_ms && !cn_mux.version && now-cn_began>=30000) || cn_mux.failed) cn_exit=1;
    cn_publish_status(now);
    cn_schedule(now);
    if (cn_exit) {
        if (!cn_exit_at) cn_exit_at=now;
        unsigned busy=cn_pending_sequence!=0;
        for (unsigned i=0;i<CM_CONNECTIONS;i++) if (cn_mux.connections[i].state) busy=1;
        /* Retain diagnostics even if the kernel mailbox has stopped responding.
         * A pending operation is not repeated; its sequence remains outstanding. */
        if (!busy || now-cn_exit_at>3000) {
            cn_active=0; cn_abort_io();
            print("computer-link: native USB window ended; version=%d in=%d out=%d opened=%d rejected=%d rx=%d tx=%d usb_errors=%d mailbox_errors=%d\n",
                (uint64_t)cn_mux.version,cn_mux.frames_in,cn_mux.frames_out,cn_mux.connections_opened,
                cn_mux.rejected,cn_mux.bytes_in,cn_mux.bytes_out,cn_usb_errors,cn_mailbox_errors);
            print("computer-link: endpoint ownership send_errors=%d event_errors=%d duplicate_attach=%d active_events=%d send_status=%x event_status=%x\n",
                cn_send_errors,cn_event_errors,cn_duplicate_attach,cn_active_events,cn_last_send_status,cn_last_event_status);
            print("computer-link: firmware console calls=%d chars=%d usb_messages=%d\n",cl_console_calls,cl_console_chars,cl_console_usb_messages);
            print("display-serial: lookup=%d installed=%d calls=%d bytes=%d usb=%d power=%d\n",(uint64_t)cl_sio_lookup,(uint64_t)cl_sio_installed,cl_sio_calls,cl_sio_bytes,cl_sio_usb,cl_sio_power);
            if (!cn_start_usb(0)) print("computer-link: diagnostic USB restart failed\n");
        }
        return;
    }
    /* Drain earlier controller events before reusing either DMA buffer. */
    uint32_t event=UsbDeviceEventNoEvent; uint64_t size=0; usb_device_event_data data;
    efi_status event_status=USB->handle_event(&event,&size,&data);
    if (EFI_ERROR(event_status)) {
        cn_last_event_status=event_status; cn_event_errors++; cn_fail_io(); return;
    }
    if (event==UsbDeviceEventNoEvent) {
        /* At most one request per endpoint. A failed submission may leave a
         * partial URB in firmware: cancel and leave native mode, never hammer
         * send() again at polling speed and exhaust the hardware ring. */
        if (usb_connected && !cn_rx) {
            efi_status status=USB->send(USB_EP_OUT,CM_DATA,usb_rx_buf);
            if (EFI_ERROR(status)) {
                cn_last_send_status=status; cn_send_errors++; cn_fail_io(); return;
            }
            cn_rx=1;
        }
        if (usb_connected && !cn_tx && (cn_zlp || cn_mux.count)) {
            unsigned n=cn_zlp ? 0:cn_mux.queue[cn_mux.head].length;
            if (n) memcpy(usb_tx_buf,cn_mux.queue[cn_mux.head].bytes,n);
            efi_status status=USB->send(USB_EP_IN,n,usb_tx_buf);
            if (EFI_ERROR(status)) {
                cn_last_send_status=status; cn_send_errors++; cn_fail_io(); return;
            }
            cn_tx=cn_zlp ? 2:1;
        }
        return;
    }
    cn_usb_events++;
    if (event==UsbDeviceEventDeviceStateChange) {
        unsigned connected=data.device_state==UsbDeviceStateConnected;
        if (connected && usb_connected) cn_duplicate_attach++;
        usb_connected=connected;
        if (connected) cn_link_seen=1;
        /* Repeated CONNECTED is informational. Pending requests remain owned
         * by the controller until their completion or explicit cancellation. */
        if (!connected) {
            cn_disconnects++; cn_abort_io(); if (cn_link_seen || cn_mux.version) cn_exit=1;
        }
    } else if (event==UsbDeviceEventTransferNotification) {
        if (data.transfer.status==UsbDeviceTransferStatusActive) { cn_active_events++; return; }
        if (data.transfer.endpoint_index==USB_EP_OUT) {
            if (!cn_rx) return; /* no request from this generation owns it */
            cn_rx=0;
            if (data.transfer.status==UsbDeviceTransferStatusCompleteOK && data.transfer.bytes_completed<=CM_DATA)
                cm_feed(&cn_mux,usb_rx_buf,(unsigned)data.transfer.bytes_completed,now);
            else cn_fail_io();
        } else if (data.transfer.endpoint_index==USB_EP_IN && cn_tx) {
            if (data.transfer.status!=UsbDeviceTransferStatusCompleteOK) { cn_fail_io(); return; }
            else if (cn_tx==2) cn_zlp=0;
            else {
                struct cm_packet *p=&cn_mux.queue[cn_mux.head];
                if (data.transfer.bytes_completed!=p->length) { cn_fail_io(); return; }
                cn_zlp=p->length%512==0;
                cn_mux.head=(cn_mux.head+1)%CM_QUEUE; cn_mux.count--;
            }
            cn_tx=0;
        }
    }
}
static int cn_command(const char *cmd) {
    if (str_eq(cmd,"oem link-native-capabilities")) {
        static const uint64_t capabilities[4]={1,1,180000,30000};
        staged_data=(const char *)capabilities; staged_len=sizeof(capabilities);
        fb_reply("OKAY",""); return 1;
    }
    unsigned persistent=starts_with(cmd,"oem link-native-persistent ");
    if (persistent || starts_with(cmd,"oem link-native ")) {
        const char *serial=cmd+(persistent ? 27:16); unsigned length=0;
        while (serial[length] && length<41) length++;
        if (length!=40 || !cl_loader_shared()) { fb_reply("FAIL","native identity or kernel link unavailable"); return 1; }
        for (unsigned i=0;i<40;i++) if (!((serial[i]>='0' && serial[i]<='9') ||
            (serial[i]>='a' && serial[i]<='f') || (serial[i]>='A' && serial[i]<='F'))) {
            fb_reply("FAIL","invalid native identity format"); return 1;
        }
        cn_serial[0]=82; cn_serial[1]=3;
        for (unsigned i=0;i<40;i++) { cn_serial[2+2*i]=(uint8_t)serial[i]; cn_serial[3+2*i]=0; }
        cn_timeout_ms=persistent ? 0:180000;
        fb_reply("OKAY",""); cn_deferred=1; cn_switch_at=cn_ms()+250; return 1;
    }
    if (str_eq(cmd,"oem display-serial-status")) {
        static uint64_t report[8];
        report[0]=1;report[1]=cl_sio_lookup;report[2]=cl_sio_installed;
        report[3]=cl_sio_calls;report[4]=cl_sio_bytes;report[5]=cl_sio_usb;
        report[6]=cl_sio_power;report[7]=cl_console_calls;
        staged_data=(const char *)report;staged_len=sizeof(report);
        fb_reply("OKAY","");return 1;
    }
    if (str_eq(cmd,"oem link-native-status")) {
        static uint64_t report[16]; memset(report,0,sizeof(report));
        report[0]=3; report[1]=cn_active; report[2]=cn_mux.version;
        report[3]=cn_mux.frames_in; report[4]=cn_mux.frames_out;
        report[5]=cn_mux.connections_opened; report[6]=cn_mux.rejected;
        report[7]=cn_mux.bytes_in; report[8]=cn_mux.bytes_out;
        report[9]=cn_usb_events; report[10]=cn_usb_errors; report[11]=cn_disconnects;
        report[12]=cn_mailbox_errors; report[13]=cn_mux.failed; report[14]=cn_mux.count;
        staged_data=(const char *)report; staged_len=sizeof(report); fb_reply("OKAY",""); return 1;
    }
    return 0;
}
