/* Shared by the real synchronous Fastboot path and the controller regression
 * test. ACTIVE and repeated CONFIGURED notifications do not complete DMA. */
static int64_t
usb_wait(uint8_t endpoint)
{
    static uint64_t last_pet;
    uint64_t freq=READ_SYSREG(CNTFRQ_EL0), began=READ_SYSREG(CNTVCT_EL0);
    for (;;) {
        uint32_t msg=UsbDeviceEventNoEvent;
        uint64_t size=0;
        usb_device_event_data data;
        efi_status status=USB->handle_event(&msg,&size,&data);
        if (EFI_ERROR(status)) {
            cd_event_errors++; cd_last_status=status;
            if (endpoint!=0xff) (void)USB->abort_xfer(endpoint);
            usb_debug_fault=1; return -1;
        }
        if (msg==UsbDeviceEventDeviceStateChange) {
            unsigned connected=data.device_state==UsbDeviceStateConnected;
            if (!connected || !usb_connected) usb_debug_fault=0;
            if (connected && usb_connected) cd_duplicate_attach++;
            usb_connected=connected;
            /* 0xff is the command loop's state-change sentinel, not an
             * endpoint. Return on initial connect so OUT can be submitted. */
            if (endpoint==0xff) return -1;
            if (!connected) { (void)USB->abort_xfer(endpoint); return -1; }
        }
        if (msg==UsbDeviceEventTransferNotification && data.transfer.endpoint_index==endpoint) {
            if (data.transfer.status==UsbDeviceTransferStatusActive) { cd_active_events++; continue; }
            if (data.transfer.status==UsbDeviceTransferStatusCompleteOK)
                return (int64_t)data.transfer.bytes_completed;
            (void)USB->abort_xfer(endpoint); return -1;
        }
        if (running_live!=NULL && READ_SYSREG(CNTVCT_EL0)-began>3*freq) {
            cd_timeouts++;
            if (endpoint==0xff) return -1;
            status=USB->abort_xfer(endpoint);
            if (EFI_ERROR(status)) { cd_last_status=status; usb_debug_fault=1; }
            return -1;
        }
        if (READ_SYSREG(CNTVCT_EL0)-last_pet>freq) { last_pet=READ_SYSREG(CNTVCT_EL0); wdt_pet(); }
    }
}
