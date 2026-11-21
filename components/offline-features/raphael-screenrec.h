/* Local screen capture ABI, v1. No pointers; same layout on host and ARM64. */
#ifndef RAPHAEL_SCREENREC_H
#define RAPHAEL_SCREENREC_H
#include <stdint.h>
#include <sys/ioctl.h>
#define HSR_MAGIC 0x31525348u /* HSR1 */
#define HSR_VERSION 1u
#define HSR_MAX_W 540u
#define HSR_MAX_H 1280u
#define HSR_MAX_PIXELS (HSR_MAX_W * HSR_MAX_H * 3u)
struct hsr_config { uint32_t version, step, fps, reserved; };
struct hsr_frame {
 uint32_t magic, version, width, height, stride, bytes;
 uint64_t sequence, monotonic_ns;
 uint32_t active, fps, step, reserved;
};
#define HSR_START _IOW('R',0x61,struct hsr_config)
#define HSR_INFO _IOR('R',0x62,struct hsr_frame)
#define HSR_STOP _IO('R',0x63)
#endif
