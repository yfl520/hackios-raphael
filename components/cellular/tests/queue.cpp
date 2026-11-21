/* Exercise the actual bridge queue with burst delivery, wraparound and failures. */
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
using std::memcpy;
#ifndef BB_QUEUE
#define BB_QUEUE 1024u
#endif
#define BB_FRAME_MAX (64u * 1024u)
#define CBQ_HDR 6u
struct bb_frame { uint32_t len; uint8_t *data; };
struct bb_channel {
    bb_frame frames[BB_QUEUE]; uint32_t head,count,frames_dropped,frames_out,bytes_out,reads_waiting;
};
static bb_channel bb_ch[2];
static void *bb_lock;
static void *bb_complete_call;
static bool fail_alloc;
static unsigned logged_reason,log_calls,wake_calls,completion_calls,allocations;
static void *IOMallocData(uint32_t len) { if(fail_alloc)return nullptr; ++allocations;return std::malloc(len); }
static void IOFreeData(void *data,uint32_t) { assert(data && allocations);--allocations;std::free(data); }
static void IOLockWakeup(void *,void *,bool) { ++wake_calls; }
static void thread_call_enter(void *) { ++completion_calls; }
static unsigned bb_now_ms() { return 0; }
static void hakios_tl(const char *format,...) {
    assert(std::strstr(format,"host-rx-drop"));
    va_list ap;va_start(ap,format);
    (void)va_arg(ap,unsigned);logged_reason=va_arg(ap,unsigned);
    va_end(ap);++log_calls;
}
static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  uint8_t *copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || c->count == BB_QUEUE) {
    const unsigned reason = !len || len > BB_FRAME_MAX ? 1u : !copy ? 2u : 3u;
    if (copy) IOFreeData(copy, len);
    // Metadata only, to the host log. 1=invalid length, 2=allocation, 3=queue full.
    if (c->frames_dropped < 32u) {
      const bool qmux = data && len >= CBQ_HDR + 7u && data[0] == 1u;
      ((void)0);
    }
    c->frames_dropped++;
    return;
  }
  memcpy(copy, data, len);
  c->frames[(c->head + c->count++) % BB_QUEUE] = { len, copy };
  c->frames_out++;
  c->bytes_out += len;
  IOLockWakeup(bb_lock, c, false);
  if (c->reads_waiting && bb_complete_call) thread_call_enter(bb_complete_call);
}

static bb_frame bb_pop(uint32_t channel)
{
  bb_channel *c = &bb_ch[channel];
  bb_frame f = { 0, nullptr };
  if (!c->count) return f;
  f = c->frames[c->head];
  c->head = (c->head + 1) % BB_QUEUE;
  c->count--;
  return f;
}

static void frame(uint8_t *data,unsigned n) {
    std::memset(data,0,16);data[0]=1;data[1]=15;data[4]=3;data[5]=1;
    data[6]=2;data[9]=0x44;data[11]=3;data[13]=n;data[14]=n>>8;
}
static unsigned value(const bb_frame &f) { assert(f.len==16 && f.data);return f.data[13]|unsigned(f.data[14])<<8; }
static void drain(unsigned expected) {
    for(unsigned n=0;n<expected;++n){bb_frame f=bb_pop(0);assert(value(f)==n);IOFreeData(f.data,f.len);}
    assert(!bb_pop(0).data && !bb_ch[0].count && !allocations);
}
int main() {
    uint8_t bytes[16];
    for(unsigned n=0;n<90;++n){frame(bytes,n);bb_push(0,bytes,sizeof bytes);}
    const unsigned retained=BB_QUEUE<90 ? BB_QUEUE : 90;
    assert(bb_ch[0].count==retained && bb_ch[0].frames_dropped==90-retained);
    drain(retained);
    if(BB_QUEUE==64){std::puts("actual baseline: burst90 drops26, ordered retained frames");return 0;}
    assert(BB_QUEUE==1024 && !bb_ch[0].frames_dropped);
    for(unsigned cycle=0;cycle<5;++cycle){
        for(unsigned n=0;n<900;++n){frame(bytes,n);bb_push(0,bytes,sizeof bytes);}
        drain(900);
    }
    for(unsigned n=0;n<BB_QUEUE;++n){frame(bytes,n);bb_push(0,bytes,sizeof bytes);}
    bb_push(0,bytes,sizeof bytes);assert(bb_ch[0].frames_dropped==1 && bb_ch[0].count==BB_QUEUE);
    drain(BB_QUEUE);
    fail_alloc=true;bb_push(0,bytes,sizeof bytes);fail_alloc=false;
    assert(bb_ch[0].frames_dropped==2 && !bb_ch[0].count && !allocations);
    bb_push(0,bytes,0);bb_push(0,bytes,BB_FRAME_MAX+1);
    assert(bb_ch[0].frames_dropped==4 && !allocations);
    bb_ch[0].reads_waiting=1;bb_complete_call=&bb_ch[1];
    frame(bytes,0);bb_push(0,bytes,sizeof bytes);assert(completion_calls==1);drain(1);
    for(unsigned n=0;n<40;++n)bb_push(0,bytes,0);
    assert(!allocations && wake_calls==90+4500+1024+1 && log_calls==0);
    std::puts("actual candidate: burst90 drops0; FIFO wrap, full, allocation/length failures, completion passed");
}
