/* XNU owns the display while running. Firmware USB diagnostics must not draw
 * into that framebuffer through the resident UEFI text console. Count calls
 * without retaining text (which may contain identifiers). Restore both original
 * protocol functions when XNU returns. This does not mask USB status codes. */
typedef efi_status (EFIAPI *cl_console_fn)(struct efi_text_output *,const char16 *);
static struct { efi_text_output *protocol; cl_console_fn original; } cl_console[2];
static uint64_t cl_console_calls,cl_console_chars,cl_console_usb_messages;
static int cl_console_has(const char16 *text,const char *word) {
    for (unsigned i=0;i<1024 && text[i];i++) {
        unsigned j=0;
        while (word[j] && i+j<1024 && text[i+j]==(uint8_t)word[j]) j++;
        if (!word[j]) return 1;
    }
    return 0;
}
static efi_status EFIAPI cl_console_capture(efi_text_output *self,const char16 *text) {
    (void)self;cl_console_calls++;
    if (text) {
        for (unsigned i=0;i<1024 && text[i];i++) cl_console_chars++;
        if (cl_console_has(text,"Dwc") || cl_console_has(text,"URB") || cl_console_has(text,"UsbDevice"))
            cl_console_usb_messages++;
    }
    return 0;
}
static void cl_console_acquire(void) {
    efi_text_output *protocols[2]={ST->con_out,ST->std_err};
    for (unsigned i=0;i<2;i++) {
        efi_text_output *p=protocols[i];
        if (!p || !p->output_string || (i && p==protocols[0]) || cl_console[i].protocol) continue;
        cl_console[i].protocol=p;cl_console[i].original=p->output_string;
        p->output_string=cl_console_capture;
    }
}
static void cl_console_release(void) {
    for (unsigned i=0;i<2;i++) {
        if (!cl_console[i].protocol) continue;
        if (cl_console[i].protocol->output_string==cl_console_capture)
            cl_console[i].protocol->output_string=cl_console[i].original;
        cl_console[i].protocol=NULL;cl_console[i].original=NULL;
    }
}
