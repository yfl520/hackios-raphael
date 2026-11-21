/* Experimental Fastboot transport, kept separate from native pairing.
 * Only the shared, bounded socket mailbox is writable, never arbitrary RAM. */
static uint32_t computer_link_download_stream;

static void cl_loader_flush(uint64_t start, uint64_t end) {
    __asm__ volatile("" : : : "memory");
    flush_dcache(start, end);
    __asm__ volatile("" : : : "memory");
}
static void cl_loader_clean(uint64_t start, uint64_t end) {
    __asm__ volatile("" : : : "memory");
    clean_dcache(start, end);
    __asm__ volatile("" : : : "memory");
}

static struct cl_shared *cl_loader_shared(void) {
    if (!running_live || running_live->magic4 != CL_LIVE_MAGIC || !running_live->computer_link)
        return NULL;
    struct cl_shared *s = (struct cl_shared *)(uintptr_t)(running_live->computer_link + running_slide);
    cl_loader_flush((uint64_t)s, (uint64_t)s + 128);
    if (s->magic != CL_MAGIC || s->version != CL_VERSION || s->bytes != sizeof(*s)) return NULL;
    return s;
}

static void cl_loader_stage(struct cl_shared *s) {
    /* Only RECEIVE returns payload; SEND returns the byte count in its header. */
    staged_data = (const char *)&s->response;
    staged_len = 128 + (s->request.operation == CL_RECEIVE ? s->response.length : 0);
}

static int cl_loader_submit(uint32_t op, uint32_t stream, uint32_t port,
    const void *data, uint32_t length) {
    struct cl_shared *s = cl_loader_shared();
    if (!s) { fb_reply("FAIL", "computer link unavailable"); return 0; }
    if (length > CL_MAX_DATA) { fb_reply("FAIL", "computer link message too large"); return 0; }
    cl_loader_flush((uint64_t)&s->response, (uint64_t)&s->response + 128);
    if (s->request.sequence != s->response.sequence) {
        fb_reply("FAIL", "computer link pending; query link-status"); return 0;
    }
    uint64_t next = s->request.sequence + 1;
    if (!next) next = 1;
    s->request.operation = op; s->request.stream = stream;
    s->request.port = port; s->request.length = length;
    if (data && length) memcpy(s->request.data, data, length);
    cl_loader_clean((uint64_t)&s->request, (uint64_t)&s->request + sizeof(s->request));
    s->request.sequence = next;
    cl_loader_clean((uint64_t)&s->request, (uint64_t)&s->request + 128);
    uint64_t began = READ_SYSREG(CNTVCT_EL0), freq = READ_SYSREG(CNTFRQ_EL0);
    for (;;) {
        cl_loader_flush((uint64_t)&s->response, (uint64_t)&s->response + 128);
        if (s->response.sequence == next) break;
        if (READ_SYSREG(CNTVCT_EL0) - began > freq) {
            fb_reply("FAIL", "computer link pending; query link-status"); return 0;
        }
        BS->stall(200);
    }
    if (s->response.length > CL_MAX_DATA) {
        fb_reply("FAIL", "computer link invalid response size"); return 0;
    }
    cl_loader_flush((uint64_t)&s->response, (uint64_t)&s->response + sizeof(s->response));
    cl_loader_stage(s);
    return 1;
}

static int cl_loader_command(const char *cmd) {
    if (!starts_with(cmd, "oem link-")) return 0;
    if (starts_with(cmd, "oem link-open ")) {
        uint64_t port = parse_number(cmd + 14, 10);
        if (port != 62078) fb_reply("FAIL", "only native lockdown is enabled");
        else if (cl_loader_submit(CL_OPEN, 0, (uint32_t)port, NULL, 0)) fb_reply("OKAY", "");
    } else if (starts_with(cmd, "oem link-send ")) {
        uint64_t stream = parse_number(cmd + 14, 10);
        if (!cl_loader_shared() || !stream || stream > 0xffffffffu)
            fb_reply("FAIL", "invalid computer link stream");
        else { computer_link_download_stream = (uint32_t)stream; fb_reply("OKAY", ""); }
    } else if (starts_with(cmd, "oem link-recv ")) {
        uint64_t stream = parse_number(cmd + 14, 10);
        if (!stream || stream > 0xffffffffu) fb_reply("FAIL", "invalid computer link stream");
        else if (cl_loader_submit(CL_RECEIVE, (uint32_t)stream, 0, NULL, CL_MAX_DATA)) fb_reply("OKAY", "");
    } else if (starts_with(cmd, "oem link-close ")) {
        uint64_t stream = parse_number(cmd + 15, 10);
        if (!stream || stream > 0xffffffffu) fb_reply("FAIL", "invalid computer link stream");
        else if (cl_loader_submit(CL_CLOSE, (uint32_t)stream, 0, NULL, 0)) fb_reply("OKAY", "");
    } else if (starts_with(cmd, "oem link-poll ")) {
        uint64_t stream = parse_number(cmd + 14, 10);
        if (!stream || stream > 0xffffffffu) fb_reply("FAIL", "invalid computer link stream");
        else if (cl_loader_submit(CL_POLL, (uint32_t)stream, 0, NULL, 0)) fb_reply("OKAY", "");
    } else if (str_eq(cmd, "oem link-status")) {
        struct cl_shared *s = cl_loader_shared();
        if (!s) fb_reply("FAIL", "computer link unavailable");
        else {
            cl_loader_flush((uint64_t)&s->response, (uint64_t)&s->response + sizeof(s->response));
            if (s->request.sequence != s->response.sequence) fb_reply("FAIL", "computer link pending");
            else if (s->response.length > CL_MAX_DATA) fb_reply("FAIL", "invalid computer link response");
            else { cl_loader_stage(s); fb_reply("OKAY", ""); }
        }
    } else fb_reply("FAIL", "unknown computer link command");
    return 1;
}

static void cl_loader_download(const char *size_text) {
    uint32_t stream = computer_link_download_stream;
    computer_link_download_stream = 0;
    uint64_t total = parse_number(size_text, 16), got = 0;
    if (!total || total > CL_MAX_DATA) { fb_reply("FAIL", "invalid computer link download size"); return; }
    fb_reply_data(total);
    while (got < total) {
        int64_t n = usb_rx(usb_rx_buf + got, total - got);
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    if (got != total) fb_reply("FAIL", "short computer link download");
    else if (cl_loader_submit(CL_SEND, stream, 0, usb_rx_buf, (uint32_t)total)) fb_reply("OKAY", "");
}
