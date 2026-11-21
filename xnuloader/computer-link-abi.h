/* Experimental bounded transport between the existing UEFI USB owner and XNU.
 * Neither side writes storage. This is transport, not pairing/activation logic.
 * Each cache line has a single writer until the matching sequence completes. */
#ifndef HAKIOS_COMPUTER_LINK_ABI_H
#define HAKIOS_COMPUTER_LINK_ABI_H
#define CL_MAGIC 0x214b4e494c435048ULL
#define CL_LIVE_MAGIC 0x344556494c554e58ULL
#define CL_VERSION 2u
#define CL_MAX_DATA 16384u
#define CL_MAX_STREAMS 8u
#define CL_OPEN 1u
#define CL_SEND 2u
#define CL_RECEIVE 3u
#define CL_CLOSE 4u
#define CL_POLL 5u

struct cl_request {
    uint64_t sequence;
    uint32_t operation, stream, port, length;
    uint8_t reserved[104];
    uint8_t data[CL_MAX_DATA];
} __attribute__((aligned(128)));
struct cl_response {
    uint64_t sequence;
    uint32_t error, stream, length, connected;
    uint8_t reserved[104];
    uint8_t data[CL_MAX_DATA];
} __attribute__((aligned(128)));
struct cl_shared {
    uint64_t magic;
    uint32_t version, bytes;
    /* Firmware owns these status fields; they describe a negotiated, physical
     * host connection, never a trust decision. Heartbeat makes detach fail closed. */
    uint32_t usb_connected, usb_native;
    uint64_t usb_epoch, usb_heartbeat;
    uint8_t reserved[88];
    struct cl_request request;
    struct cl_response response;
} __attribute__((aligned(128)));
_Static_assert(__builtin_offsetof(struct cl_request, data) == 128, "request layout");
_Static_assert(__builtin_offsetof(struct cl_response, data) == 128, "response layout");
_Static_assert(__builtin_offsetof(struct cl_shared, request) == 128, "shared layout");
_Static_assert(sizeof(struct cl_shared) == 33152, "ABI size");
#endif
