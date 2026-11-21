/* One firmware owner per OUT request, including the diagnostic side of a
 * native USB transition. On a submission error, wait for a real reconnect. */
static void
usb_poll_command(void)
{
    if (cn_active || cn_deferred) { cn_pump(); return; }
    if (USB==NULL) return;
    uint32_t msg=UsbDeviceEventNoEvent;
    uint64_t size=0;
    usb_device_event_data data;
    efi_status status=USB->handle_event(&msg,&size,&data);
    if (EFI_ERROR(status)) {
        cd_event_errors++; cd_last_status=status;
        (void)USB->abort_xfer(USB_EP_OUT); usb_rx_queued=0; usb_debug_fault=1; return;
    }
    if (msg==UsbDeviceEventDeviceStateChange) {
        unsigned connected=data.device_state==UsbDeviceStateConnected;
        if (connected && usb_connected) cd_duplicate_attach++;
        if (!connected) {
            (void)USB->abort_xfer(USB_EP_OUT); usb_rx_queued=0;
            usb_debug_fault=0;
        } else if (!usb_connected) usb_debug_fault=0;
        usb_connected=connected; return;
    }
    if (msg==UsbDeviceEventNoEvent) {
        if (!usb_rx_queued && usb_connected && !usb_debug_fault) {
            status=USB->send(USB_EP_OUT,511,usb_rx_buf);
            if (EFI_ERROR(status)) {
                cd_send_errors++; cd_last_status=status;
                (void)USB->abort_xfer(USB_EP_OUT); usb_debug_fault=1;
            } else usb_rx_queued=1;
        }
        return;
    }
    if (msg!=UsbDeviceEventTransferNotification || data.transfer.endpoint_index!=USB_EP_OUT) return;
    if (!usb_rx_queued) return;
    if (data.transfer.status==UsbDeviceTransferStatusActive) { cd_active_events++; return; }
    usb_rx_queued=0;
    if (data.transfer.status!=UsbDeviceTransferStatusCompleteOK) {
        (void)USB->abort_xfer(USB_EP_OUT); return;
    }
    if (!data.transfer.bytes_completed) return;
    if (data.transfer.bytes_completed>511) { usb_debug_fault=1; return; }
    char cmd[128];
    uint64_t n=data.transfer.bytes_completed<sizeof(cmd)-1 ? data.transfer.bytes_completed:sizeof(cmd)-1;
    memcpy(cmd,usb_rx_buf,(size_t)n); cmd[n]=0;
    fb_command(cmd);
}
