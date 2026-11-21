/* Included only in the isolated experimental kernel's BSD compilation unit.
 * The existing firmware remains the sole USB-controller owner.
 * Socket connections originate in a real kernel worker, as native lockdownd
 * requires. Its trust dialog, TLS and pairing record checks are untouched. */
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/proc_internal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include "computer-link-abi.h"

extern void loader_log_line(const char *line);
struct cl_shared computer_link_shared = {
    .magic = CL_MAGIC, .version = CL_VERSION, .bytes = sizeof(struct cl_shared)
};
static socket_t computer_link_sockets[CL_MAX_STREAMS];
static uint32_t computer_link_ids[CL_MAX_STREAMS];
static uint32_t computer_link_generation;
static thread_call_t computer_link_call;
static unsigned computer_link_active_ticks;
extern void computer_link_usb_state_update(int attached);
static uint64_t computer_link_usb_last_heartbeat, computer_link_usb_deadline;

static void cl_kernel_sync(void *pointer, size_t length) {
    uintptr_t start = (uintptr_t)pointer & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)pointer + length;
    for (; start < end; start += 64)
        __asm__ volatile("dc civac, %0" : : "r"(start) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

static socket_t cl_kernel_socket(uint32_t id) {
    uint32_t slot = id & 15u;
    if (!slot || slot > CL_MAX_STREAMS) return NULL;
    return computer_link_ids[slot - 1] == id ? computer_link_sockets[slot - 1] : NULL;
}

static void cl_kernel_execute(const struct cl_request *q, struct cl_response *r) {
    r->error = 0; r->stream = q->stream; r->length = 0; r->connected = 0;
    /* The transport must not accidentally originate a userspace peer. */
    if (current_proc() != kernproc) { r->error = EPERM; return; }
    if (q->length > CL_MAX_DATA) { r->error = EMSGSIZE; return; }
    if (q->operation == CL_OPEN) {
        /* Connect only to local native services. Their original pairing and TLS
         * authorization remains responsible for access; transport stays opaque. */
        if (!q->port || q->port > 65535u || q->length) { r->error = EACCES; return; }
        unsigned slot = 0;
        while (slot < CL_MAX_STREAMS && computer_link_sockets[slot]) slot++;
        if (slot == CL_MAX_STREAMS) { r->error = EMFILE; return; }
        socket_t socket = NULL;
        r->error = sock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &socket);
        if (r->error) return;
        int one = 1;
        (void)sock_setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        struct sockaddr_in address = {0};
        address.sin_len = sizeof(address); address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)q->port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        r->error = sock_connect(socket, (const struct sockaddr *)&address, MSG_DONTWAIT);
        if (r->error && r->error != EINPROGRESS) { sock_close(socket); return; }
        computer_link_generation = (computer_link_generation + 1) & 0x0fffffffu;
        if (!computer_link_generation) computer_link_generation = 1;
        r->stream = (computer_link_generation << 4) | (slot + 1);
        computer_link_sockets[slot] = socket;
        computer_link_ids[slot] = r->stream;
        r->connected = sock_isconnected(socket) ? 1u : 0u;
        r->error = 0;
        return;
    }
    socket_t socket = cl_kernel_socket(q->stream);
    if (!socket) { r->error = EBADF; return; }
    r->connected = sock_isconnected(socket) ? 1u : 0u;
    if (q->operation == CL_CLOSE) {
        unsigned slot = (q->stream & 15u) - 1;
        computer_link_sockets[slot] = NULL; computer_link_ids[slot] = 0;
        sock_close(socket); r->connected = 0;
    } else if (q->operation == CL_POLL) {
        socklen_t length = sizeof(int); int error = 0;
        r->error = sock_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length);
        if (!r->error) r->error = (uint32_t)error;
    } else if (q->operation == CL_SEND || q->operation == CL_RECEIVE) {
        if (!q->length) { r->error = EINVAL; return; }
        struct iovec iov = {
            .iov_base = q->operation == CL_SEND ? (void *)q->data : r->data,
            .iov_len = q->length
        };
        struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
        size_t transferred = 0;
        r->error = q->operation == CL_SEND
            ? sock_send(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL, &transferred)
            : sock_receive(socket, &message, MSG_DONTWAIT, &transferred);
        r->length = (uint32_t)transferred;
        /* A partial nonblocking transfer is valid and must not be repeated. */
        if (transferred && r->error == EWOULDBLOCK) r->error = 0;
    } else r->error = EINVAL;
}

static void cl_kernel_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1) {
    struct cl_shared *s = &computer_link_shared;
    cl_kernel_sync(s, 128);
    if (s->usb_heartbeat != computer_link_usb_last_heartbeat) {
        computer_link_usb_last_heartbeat = s->usb_heartbeat;
        clock_interval_to_deadline(3, NSEC_PER_SEC, &computer_link_usb_deadline);
    }
    computer_link_usb_state_update(s->usb_native && s->usb_connected &&
        computer_link_usb_deadline && mach_absolute_time() < computer_link_usb_deadline);
    cl_kernel_sync(&s->request, 128);
    uint64_t sequence = s->request.sequence;
    if (sequence && sequence != s->response.sequence) {
        cl_kernel_sync(&s->request, sizeof(s->request));
        cl_kernel_execute(&s->request, &s->response);
        /* Send acknowledgements report a count without echoing client data. */
        size_t bytes = s->request.operation == CL_RECEIVE ? s->response.length : 0;
        cl_kernel_sync(&s->response, 128 + bytes);
        s->response.sequence = sequence;
        cl_kernel_sync(&s->response, 128);
        computer_link_active_ticks = 250;
    }
    uint64_t deadline;
    unsigned ms = computer_link_active_ticks ? 2 : 50;
    if (computer_link_active_ticks) computer_link_active_ticks--;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    thread_call_enter_delayed(computer_link_call, deadline);
}

__startup_func static void cl_kernel_init(void) {
    cl_kernel_sync(&computer_link_shared, sizeof(computer_link_shared));
    computer_link_call = thread_call_allocate(cl_kernel_poll, NULL);
    if (!computer_link_call) return;
    uint64_t deadline;
    clock_interval_to_deadline(15, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(computer_link_call, deadline);
    loader_log_line("computer-link: bounded kernel socket transport initialized; native trust unchanged\n");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cl_kernel_init);
