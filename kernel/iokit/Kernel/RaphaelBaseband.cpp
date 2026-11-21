// SPDX-License-Identifier: BSD-2-Clause
// The cellular bridge: the IOKit side of Apple's PCIe baseband (AppleBaseband, AppleBasebandPCIControl and one
// AppleBasebandPCIInterface per channel, with the user-client classes the sandbox knows them by) over this SoC's modem. ABM and
// BBUpdater talk to these as to an MHI modem; the QMUX channels go to the modem's QMI services over QRTR (coachz_bb_qmux.h), the
// sahara channel is answered by a device end that reads and drops the firmware it is offered (coachz_bb_sahara.h), and the board
// methods are answered by coachz_bb_control.h. Raphael opt-in: raphael_native_bb=1.
//
// Threads: the existing Wi-Fi/MPSS thread owns QRTR (bsd/dev/hakios.wifi.c): it hands modem messages to raphael_baseband_qrtr and sends what
// raphael_baseband_poll gives it. User threads only queue. One lock covers the bridge state and the queues; user memory is copied
// outside it, and asynchronous reads complete on a thread call.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "raphael_bb_transport.h"
#include "raphael_bb_policy.h"

#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_data.h"
#include "coachz_bb_pdp.h"
extern "C" bool hakios_ipa_data_ready(void);
#include "raphael_pdc_readonly.h"
#include "coachz_bb_esim.h"

extern "C" void hakios_tl(const char *format, ...) __printflike(1, 2);

#define BB_FRAME_MAX (64u * 1024u)       // one read or write: a QMUX frame, a SAHARA packet or a READ_DATA answer
#define BB_QUEUE 1024u                     // frames waiting for ABM, per channel
#define BB_READS 8u                      // asynchronous reads waiting for a frame, per channel
#define BB_TX 64u                        // QMI messages waiting for the Wi-Fi thread
#define BB_SYNC_READ_SECONDS 30u

// Which SAHARA images the device end asks for. SBL1 (Qualcomm image id 0x15) is the first image a boot loader requests; the rest of
// a Mav16 boot (rpm, tz, apps, acdb, mba, qdsp6sw in the bbfw) follow once BBUpdater's log shows how it takes this one.
static const uint32_t bb_sahara_images[] = { 0x15 };

namespace {
struct bb_frame { uint32_t len; uint8_t *data; };
struct bb_read { OSAsyncReference64 reference; IOMemoryDescriptor *memory; uint32_t len; void *owner; };
struct bb_channel {
  bb_frame frames[BB_QUEUE];
  uint32_t head, count;
  bb_read reads[BB_READS];
  uint32_t reads_waiting;
  void *owner;
  uint32_t opens, frames_in, frames_out, frames_dropped, bytes_in, bytes_out;
};
struct bb_tx { uint32_t local_port, node, port, len; uint8_t *data; };
}

static IOLock *bb_lock;
static thread_call_t bb_complete_call;
static struct cbq_bridge bb_qmux;
static struct cbs_device bb_sahara;
static struct cbc_state bb_control;
static bb_channel bb_ch[CBH_PUBLISHED];
static bb_tx bb_txq[BB_TX];
static uint32_t bb_tx_head, bb_tx_count, bb_tx_dropped;
static int (*bb_send)(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length);
static int (*bb_del_client)(uint32_t local_port);
static int bb_published;
static uint32_t bb_private_seen[64], bb_private_count, bb_ctl_logged;
static uint8_t bb_out[BB_FRAME_MAX];     // answers built under the lock
static struct cbp_client bb_pdp_clients[CBQ_MAX_CLIENTS];
static uint32_t bb_pdp_owners[CBQ_MAX_CLIENTS];
static struct cbd_setup bb_data;         // the data port setup ModemManager runs for IPA (coachz_bb_data.h)
static int bb_data_slot[2] = { -1, -1 }; // the bridge's own DPM and WDA clients
static uint32_t bb_endpoint_fixes, bb_activations;
static uint8_t bb_fix[2048];
static struct cbe bb_esim;
static int bb_esim_slot = -1;
static bb_tx bb_uim_held[BB_TX];
static uint32_t bb_uim_head, bb_uim_count;
static uint8_t bb_uim_cid[BB_TX];
static uint8_t *bb_esim_in, *bb_esim_resp;
#define BB_ESIM_BYTES (1024u * 1024u)

static uint32_t bb_now_ms(void)
{
  uint64_t ticks, ns;
  clock_get_uptime(&ticks);
  absolutetime_to_nanoseconds(ticks, &ns);
  return (uint32_t)(ns / 1000000u);
}

extern "C" {
int raphael_baseband_enabled(void);
void raphael_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), int (*del_client)(uint32_t));
void raphael_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
void raphael_baseband_del_server(uint32_t service, uint32_t node, uint32_t port);
int raphael_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
void raphael_baseband_poll(void);
void raphael_baseband_report(unsigned second);
}

// ------------------------------------------------------------------ device tree
extern "C" int raphael_baseband_enabled(void) {
  static int cached=-1;
  if(cached<0) {
    int on=0; const bool found=PE_parse_boot_argn("raphael_native_bb",&on,sizeof on);
    cached=found && on==1;
    ((void)0);
  }
  return cached;
}
// Explicit experiment only; hardware readiness is checked by the IPA owner.
static bool bb_data_supported(void) {
  int on=0;return PE_parse_boot_argn("ipa_preboot",&on,sizeof on) && on==6;
}
static bool bb_activation_supported(void) {
  // Apple-facing BSP state translation, as in upstream. This Qualcomm modem
  // does not consume Apple activation tickets. Does not change Apple account
  // activation, IMEI, ICCID, or the actual network registration state.
  return raphael_baseband_enabled() != 0;
}
static bool bb_esim_operations_supported(void) {
  int on=0;return PE_parse_boot_argn("raphael_esim_ops",&on,sizeof on) && on==1;
}

// ------------------------------------------------------------------ queues (bb_lock held)
static void bb_complete(thread_call_param_t, thread_call_param_t);

static bool bb_init(void)
{
  if (bb_lock) return true;
  bb_lock = IOLockAlloc();
  if(!bb_lock)return false;
  bb_complete_call = thread_call_allocate(bb_complete, nullptr);
  if(!bb_complete_call){IOLockFree(bb_lock);bb_lock=nullptr;return false;}
  cbq_init(&bb_qmux);
  bb_esim_in = static_cast<uint8_t *>(IOMallocData(BB_ESIM_BYTES));
  bb_esim_resp = static_cast<uint8_t *>(IOMallocData(BB_ESIM_BYTES));
  cbe_init(&bb_esim, bb_esim_in, bb_esim_in ? BB_ESIM_BYTES : 0,
           bb_esim_resp, bb_esim_resp ? BB_ESIM_BYTES : 0);
  cbc_init(&bb_control);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  return true;
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

static void bb_flush(uint32_t channel)
{
  for (bb_frame f = bb_pop(channel); f.data; f = bb_pop(channel)) IOFreeData(f.data, f.len);
}

// Only metadata for the exact requests observed timing out; bb_lock held.
// Never print SIM identities, signal values, APNs or arbitrary TLV bytes.
static void bb_trace(const char *stage,uint32_t local,const uint8_t *sdu,uint32_t len,int error)
{
  if (local<CBQ_PORT_BASE || local-CBQ_PORT_BASE>=CBQ_MAX_CLIENTS || !sdu || len<7) return;
  const struct cbq_client &client=bb_qmux.client[local-CBQ_PORT_BASE];
  const uint32_t message=(uint32_t)sdu[3]|(uint32_t)sdu[4]<<8;
  if (!(client.service==3 || (client.service==2 && message==0x2e) ||
        (client.service==1 && message==0xc8) || (client.service==5 && message==0x47) ||
        (client.service==9 && message==0x44))) return;
  static unsigned trace_count;
  if (trace_count++>=384) return;
  uint32_t result_len=0;
  const uint8_t *result=sdu[0]==2 ? cbq_tlv(sdu+7,len-7,2,&result_len) : nullptr;
  const unsigned qmi_error=result && result_len==4 ? (unsigned)cbq_rd16(result+2) : 0xffffu;
  ((void)0);
}

// A QMI message for the modem, sent by the Wi-Fi thread from raphael_baseband_poll.
static void bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len)
{
  uint8_t *copy = len ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || bb_tx_count == BB_TX) {
    bb_trace("queue_reject",local_port,data,len,-1);
    if (copy) IOFreeData(copy, len);
    bb_tx_dropped++;
    return;
  }
  memcpy(copy, data, len);
  bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = { local_port, node, port, len, copy };
  bb_trace("queued",local_port,data,len,0);
}

// BBUEUR13Loader pushes the SBL with SendImage on pbl, frees that transport, opens sahara and waits 40 s for the loaded SBL's HELLO there. So the
// HELLO goes out when sahara opens after a SendImage.
static bool bb_boot_image_sent;

static void bb_sahara_restart(void)
{
  struct cbs_out o;
  bb_flush(CBH_SAHARA);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  cbs_start(&bb_sahara, &o);
  for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
  ((void)0);
}

static void bb_private_note(uint32_t service, uint32_t message)
{
  const uint32_t key = service << 16 | (message & 0xffff);
  for (uint32_t i = 0; i < bb_private_count; i++)
    if (bb_private_seen[i] == key) return;
  if (bb_private_count < 64) bb_private_seen[bb_private_count++] = key;
  ((void)0);
}

// ------------------------------------------------------------------ data port setup (bb_lock held)
static void bb_data_step(void)
{
  uint8_t sdu[128];
  uint32_t service = 0;
  const uint32_t n = cbd_next(&bb_data, sdu, sizeof sdu, &service);
  if (!n) {
    ((void)0);
    return;
  }
  int *slot = &bb_data_slot[service == CBD_SVC_DPM ? 0 : 1];
  if (*slot < 0) *slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, service);
  const struct cbq_server *server = cbq_server_find(&bb_qmux, service);
  if (*slot < 0 || !server) {
    ((void)0);
    bb_data.step = CBD_DONE;
    return;
  }
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)*slot, server->node, server->port, sdu, n);
}

static void bb_data_start(void)
{
  if (!bb_data_supported() || !hakios_ipa_data_ready() || bb_data.step != CBD_IDLE ||
      !cbq_server_find(&bb_qmux,CBD_SVC_DPM) || !cbq_server_find(&bb_qmux,CBD_SVC_WDA)) return;
  bb_data.has_dpm = cbq_server_find(&bb_qmux, CBD_SVC_DPM) != nullptr;
  bb_data.has_wda = cbq_server_find(&bb_qmux, CBD_SVC_WDA) != nullptr;
  bb_data_step();
}

// WDS is still entirely owned by CommCenter. Observe only wire lifecycle facts, never credentials or addresses.
static void bb_pdp_note(uint32_t local_port, uint32_t cid, uint32_t channel, const uint8_t *sdu, uint32_t n)
{
  const uint32_t slot = local_port - CBQ_PORT_BASE;
  if (slot >= CBQ_MAX_CLIENTS || !cbd_sdu_ok(sdu, n)) return;
  const uint32_t owner = 1u + cid + (channel << 8);
  if (bb_pdp_owners[slot] != owner) {
    bzero(&bb_pdp_clients[slot], sizeof bb_pdp_clients[slot]);
    bb_pdp_clients[slot].family = 4; // WDS default family, overridden by Set IP Family or Start Network
    bb_pdp_owners[slot] = owner;
  }
  cbp_client &c = bb_pdp_clients[slot];
  if (cbp_observe(&c, sdu, n))
    ((void)0);
  const bool start_failed = sdu[0] == 2 && cbd_rd16(sdu + 3) == CBP_START && cbd_error(sdu, n);
  if (start_failed || (sdu[0] == 4 && cbd_rd16(sdu + 3) == CBP_STATUS && !c.active)) {
    // libqmi WDS 0x20 responses and 0x22 indications use the same call-end TLVs.
    uint32_t reason_size = 0, verbose_size = 0;
    const uint8_t *reason = cbd_find(sdu, n, 0x10, &reason_size);
    const uint8_t *verbose = cbd_find(sdu, n, 0x11, &verbose_size);
    ((void)0);
  }
}

static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu);

// A request for the modem, with the data port named as this modem has it.
static void bb_to_modem(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service == CBD_SVC_WDS) bb_pdp_note(r.local_port,r.cid,r.channel,sdu,r.len);
  bb_sim_request(r,sdu);
  const bool valid = rbb_sdu_ok(sdu,r.len,0);
  const uint32_t message = valid ? cbd_rd16(sdu+3) : 0xffffu;
  const bool allowed = valid && (bb_data_supported() || rbb_control_request_allowed(r.service,message));
  static uint32_t attach_logs;
  if (valid && r.service==CBD_SVC_WDS && message==0xc8 && attach_logs++<8) {
    ((void)0);
    // Shape only: never record APNs, credentials, SIM identities or payload bytes.
    for (uint32_t at=7, count=0; at+3<=r.len && count<24; ++count) {
      const uint32_t n=cbd_rd16(sdu+at+1);
      if (n>r.len-at-3) { ((void)0); break; }
      ((void)0);
      at+=3+n;
    }
  }
  static uint32_t control_logs;
  if (control_logs < 96 && (r.service==2 || r.service==CBD_SVC_WDS)) {
    ++control_logs;
    ((void)0);
  }
  if (!allowed) {
    const uint32_t n=cbq_private_unsupported(sdu,r.service,r.cid,bb_out,sizeof bb_out);
    if(n)bb_push(r.channel?CBH_QMUX2:CBH_QMUX1,bb_out,n);
    return;
  }
  if (r.service == CBE_SVC_UIM && bb_esim.route_failed) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_INTERNAL, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(r.channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, n);
    return;
  }
  if (r.service == CBE_SVC_UIM && bb_esim_in && bb_esim_resp &&
      (bb_esim.freeze || (!bb_esim.slots.known && r.len >= 7 && cbd_rd16(sdu + 3) == CBE_UIM_GET_CARD_STATUS))) {
    uint8_t *copy = bb_uim_count < BB_TX ? static_cast<uint8_t *>(IOMallocData(r.len)) : nullptr;
    if (copy) {
      memcpy(copy, sdu, r.len);
      const uint32_t slot = (bb_uim_head + bb_uim_count++) % BB_TX;
      bb_uim_held[slot] = { r.local_port, r.node, r.port, r.len, copy };
      bb_uim_cid[slot] = r.cid;
    } else {
      const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_DEVICE_IN_USE, nullptr, 0, bb_out, sizeof bb_out);
      if (n) bb_push(r.channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, n);
    }
    return;
  }
  // Registration control traffic is byte-for-byte passthrough. The Coachz
  // IPA4.2 endpoint translation is never applied on this SIM-only path.
  if (!bb_data_supported()) {
    bb_queue_tx(r.local_port,r.node,r.port,sdu,r.len);
    return;
  }
  const struct cbd_fix f = cbd_endpoint_fix(r.service, sdu, r.len, bb_fix, sizeof bb_fix);
  if (!f.len) {
    bb_queue_tx(r.local_port, r.node, r.port, sdu, r.len);
    return;
  }
  if (bb_endpoint_fixes++ < 8)
    ((void)0);
  bb_queue_tx(r.local_port, r.node, r.port, bb_fix, f.len);
}

// eSIM control runs on the same QRTR owner as the other QMI traffic (bb_lock held).
// Only explicit VINYL commands may switch slots or send an APDU. Idle discovery reads slot status.
static void bb_esim_step(void)
{
  if (!bb_esim_in || !bb_esim_resp) return;
  const struct cbq_server *server = cbq_server_find(&bb_qmux, CBE_SVC_UIM);
  if (!server) return;
  if (bb_esim_slot < 0) bb_esim_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBE_SVC_UIM);
  if (bb_esim_slot < 0) return;
  const uint32_t now = bb_now_ms();
  while (!bb_esim.freeze && !bb_esim.route_failed && (bb_esim.slots.known || (bb_esim.step == CBE_S_IDLE && bb_esim.txn)) &&
         bb_uim_count && bb_tx_count < BB_TX) {
    bb_tx &held = bb_uim_held[bb_uim_head];
    const uint32_t index = held.local_port - CBQ_PORT_BASE;
    if (index < CBQ_MAX_CLIENTS && bb_qmux.client[index].used && bb_qmux.client[index].service == CBE_SVC_UIM &&
        bb_qmux.client[index].cid == bb_uim_cid[bb_uim_head]) {
      bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = held;
    } else IOFreeData(held.data, held.len);
    bb_uim_head = (bb_uim_head + 1) % BB_TX; bb_uim_count--;
  }
  for (uint32_t i = 0; i < CBE_JOBS; i++) {
    struct cbe_job *j = &bb_esim.job[i];
    if (j->state && cbq_client_find(&bb_qmux, j->channel, CBE_SVC_VINYL, j->cid) < 0) {
      if ((int)i == bb_esim.active) cbe_finish(&bb_esim, 1, CBE_R_INACTIVE, 0, now);
      else j->state = CBE_J_FREE;
    }
  }
  cbe_tick(&bb_esim, now);
  if (bb_esim.req_len) {
    const uint32_t queued = bb_tx_count;
    bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_esim_slot, server->node, server->port, bb_esim.req, bb_esim.req_len);
    if (bb_tx_count != queued) bb_esim.req_len = 0;
  }
  for (uint32_t i = 0; i < 4 && bb_esim.done_pending; i++) {
    const struct cbe_job *done = &bb_esim.done;
    if (cbq_client_find(&bb_qmux, done->channel, CBE_SVC_VINYL, done->cid) < 0) {
      if (bb_esim.step == CBE_S_IDLE) bb_esim.done_pending = 0;
      break;
    }
    const uint32_t channel = done->channel ? CBH_QMUX2 : CBH_QMUX1;
    if (bb_ch[channel].count == BB_QUEUE) break;
    const uint32_t chunk = bb_esim.done_chunk, pending = bb_esim.done_pending, queued = bb_ch[channel].count;
    uint8_t qmux = 0;
    const uint32_t n = cbe_output(&bb_esim, &qmux, bb_out, sizeof bb_out);
    if (!n) break;
    bb_push(channel, bb_out, n);
    if (bb_ch[channel].count == queued) { bb_esim.done_chunk = chunk; bb_esim.done_pending = (uint8_t)pending; break; }
  }
  struct cbe_event event;
  while (cbe_take_event(&bb_esim, &event))
    ((void)0);
}

// Two query-only PDC observations, default modem context, on the QRTR owner.
// Never hold, synthesize, or delay CommCenter's online requests.
static int bb_pdc_slot=-1;
static struct { bool sent,ack,indication,timed_out; uint32_t at; } bb_pdc[2];
static void __attribute__((noinline)) bb_pdc_step(void)
{
  const struct cbq_server *server=cbq_server_find(&bb_qmux,0x24u);
  if (!server) return;
  if (bb_pdc_slot<0) bb_pdc_slot=cbq_client_alloc(&bb_qmux,CBQ_INTERNAL,0x24u);
  if (bb_pdc_slot<0) return;
  const uint32_t now=bb_now_ms();
  for (uint32_t type=0;type<2;type++) {
    if (!bb_pdc[type].sent && bb_tx_count<BB_TX) {
      uint8_t request[21];
      const uint32_t n=rpd_request(request,sizeof request,type,(uint16_t)(0x7d01u+type),0x52425000u+type);
      const uint32_t queued=bb_tx_count;
      if (n) bb_queue_tx(CBQ_PORT_BASE+(uint32_t)bb_pdc_slot,server->node,server->port,request,n);
      if (bb_tx_count!=queued) {
        bb_pdc[type].sent=true;bb_pdc[type].at=now;
        ((void)0);
      }
    }
    if (bb_pdc[type].sent && !bb_pdc[type].indication && !bb_pdc[type].timed_out &&
        (uint32_t)(now-bb_pdc[type].at)>=5000u) {
      bb_pdc[type].timed_out=true;
      ((void)0);
    }
  }
}
static void bb_pdc_answer(const uint8_t *body,uint32_t size)
{
  for (uint32_t type=0;type<2;type++) {
    struct rpd_result r;
    if (!bb_pdc[type].sent || !rpd_parse(body,size,(uint16_t)(0x7d01u+type),0x52425000u+type,&r)) continue;
    if (r.indication && !bb_pdc[type].indication) {
      bb_pdc[type].indication=true;
      ((void)0);
    } else if (!r.indication && !bb_pdc[type].ack) {
      bb_pdc[type].ack=true;
      ((void)0);
    }
    return;
  }
}

// The activation indication (coachz_bb_data.h) to the BSP client that registered, with slot one's ICCID and IMSI once the card gave them.
static uint8_t bb_iccid[CBD_ICCID_BYTES], bb_imsi[CBD_IMSI_BYTES], bb_iccid_known, bb_imsi_known, bb_act_known, bb_act_channel, bb_act_cid;
static void bb_activation_indicate(void)
{
  if (!bb_act_known) return;
  uint8_t tlvs[CBD_ACTIVATION_SIM_TLVS];
  const uint32_t t = cbd_activation_state(tlvs, sizeof tlvs, bb_iccid_known ? bb_iccid : nullptr, bb_imsi_known ? bb_imsi : nullptr);
  const uint32_t n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, bb_act_cid, tlvs, t, bb_out, sizeof bb_out);
  if (n) bb_push(bb_act_channel, bb_out, n);
}

// CommCenter's own EF_ICCID and EF_IMSI reads: the request's client, transaction and file, so the answer can be recognized. Never logged.
struct bb_sim_read { uint8_t used, channel, cid; uint16_t txn, file; };
static bb_sim_read bb_sim_reads[8];
static uint32_t bb_sim_next;
static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service != CBD_SVC_UIM) return;
  const uint32_t file = cbd_uim_read_file(sdu, r.len);
  if (file != CBD_EF_ICCID && file != CBD_EF_IMSI) return;
  bb_sim_reads[bb_sim_next++ % 8] = { 1, r.channel, r.cid, cbd_rd16(sdu + 1), (uint16_t)file };
}
static void bb_sim_answer(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  if (r.service != CBD_SVC_UIM || n < 7u || sdu[0] != 2u || cbd_rd16(sdu + 3) != CBD_UIM_READ_TRANSPARENT) return;
  for (auto &q : bb_sim_reads) {
    if (!q.used || q.channel != r.channel || q.cid != r.cid || q.txn != cbd_rd16(sdu + 1)) continue;
    q.used = 0;
    uint8_t ef[16], value[CBD_ICCID_BYTES];
    const uint32_t len = cbd_uim_read_content(sdu, n, ef, sizeof ef);
    const bool iccid = q.file == CBD_EF_ICCID;
    if (iccid ? !cbd_iccid_from_ef(ef, len, value) : !cbd_imsi_from_ef(ef, len, value)) return;
    uint8_t *have = iccid ? bb_iccid : bb_imsi, *known = iccid ? &bb_iccid_known : &bb_imsi_known;
    const uint32_t size = iccid ? CBD_ICCID_BYTES : CBD_IMSI_BYTES;
    if (*known && !memcmp(value, have, size)) return;
    memcpy(have, value, size); *known = 1;
    ((void)0);
    bb_activation_indicate();
    return;
  }
}

// A private request the bridge answers itself: 1 when it did.
static int bb_private_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
  const uint32_t message = (uint32_t)(sdu[3] | sdu[4] << 8);
  if (r.service==CBE_SVC_VINYL && !bb_esim_operations_supported() && message==0x1002u) return 0;
  if (r.service == CBE_SVC_VINYL) {
    const uint32_t n = bb_esim_in && bb_esim_resp ?
      cbe_host(&bb_esim, r.channel, r.cid, sdu, r.len, bb_out, sizeof bb_out, bb_now_ms()) :
      cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_NO_MEMORY, nullptr, 0, bb_out, sizeof bb_out);
    if (!n) return 0;
    if (n != CBE_DEFERRED) bb_push(channel, bb_out, n);
    return 1;
  }
  // Carrier bundle files (coachz_bb_data.h): accepted and dropped, each segment acknowledged with the next segment number.
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_SEND_FILE) {
    struct cbd_file_segment f;
    if (!cbd_bsp_file_segment(sdu, r.len, &f)) return 0;
    uint8_t tlvs[8];
    const uint32_t t = cbd_bsp_file_answer(&f, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    if (f.segment + 1u == f.count) {
      // The file is complete: report it received as an Apple modem does (FileTransferStatus), or CommCenter times out after 22 s.
      uint8_t done[12];
      const uint32_t d = cbd_bsp_file_done(&f, done, sizeof done);
      const uint32_t m = cbq_private_indication(CBD_BSP_FILE_TRANSFER_STATUS, r.service, r.cid, done, d, bb_out, sizeof bb_out);
      if (m) bb_push(channel, bb_out, m);
      ((void)0);
    }
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_CHIP_INFO) {
    uint8_t tlvs[12];
    const uint32_t t = cbd_bsp_chip_info(CBS_CHIP_ID, CBS_SERIAL, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_SET_CONFIG) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    ((void)0);
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_ACTIVATION_REGISTER) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, 0, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    bb_act_known = 1; bb_act_channel = (uint8_t)channel; bb_act_cid = r.cid;
    bb_activation_indicate();
    if (bb_activations++ < 4)
      ((void)0);
    return 1;
  }
  return 0;
}

// One write from ABM on a channel, after its bytes were copied in.
static void bb_host_write(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  c->frames_in++;
  c->bytes_in += len;
  if (channel == CBH_SAHARA) {
    const enum cbs_state before = bb_sahara.state;
    for (uint32_t at = 0; at < len && bb_sahara.state != CBS_FAILED;) {
      struct cbs_out o;
      const uint32_t used = cbs_receive(&bb_sahara, data + at, len - at, &o);
      for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
      if (!used) break;
      at += used;
    }
    if (bb_sahara.state != before && bb_sahara.state == CBS_COMMAND)
      ((void)0);
    if (bb_sahara.state != before && (bb_sahara.state == CBS_FINISHED || bb_sahara.state == CBS_FAILED || bb_sahara.state == CBS_WAIT_DONE))
      ((void)0);
    return;
  }
  if (channel != CBH_QMUX1 && channel != CBH_QMUX2) return;   // counted above, nothing behind it yet
  const uint32_t qmux = channel == CBH_QMUX1 ? 0 : 1;
  const struct cbq_result r = cbq_from_host(&bb_qmux, qmux, data, len, bb_out, sizeof bb_out);
  switch (r.kind) {
  case CBQ_TO_HOST:
    bb_push(channel, bb_out, r.len);
    // The first WDS client ABM takes: set the data port up as ModemManager does before its first bind.
    if (r.service == CBD_SVC_WDS && r.local_port && cbq_rd16(data + CBQ_HDR + 2) == CBQ_CTL_GET_CLIENT_ID) bb_data_start();
    if (bb_qmux.ctl_other != bb_ctl_logged) {
      bb_ctl_logged = bb_qmux.ctl_other;
      ((void)0);
    }
    break;
  case CBQ_TO_MODEM:
    if (cbd_vendor_reporting(r.service,data+CBQ_HDR,r.len)) {
      const uint32_t n=cbq_private_answer(data+CBQ_HDR,r.service,r.cid,0,nullptr,0,bb_out,sizeof bb_out);
      if(n)bb_push(channel,bb_out,n);
      break;
    }
    bb_to_modem(r, data + CBQ_HDR);
    break;
  case CBQ_PRIVATE: {
    const uint8_t *sdu = data + CBQ_HDR;
    if (bb_private_answer(channel, r, sdu)) break;
    bb_private_note(r.service, (uint32_t)(sdu[3] | sdu[4] << 8));
    const uint32_t n = cbq_private_unsupported(sdu, r.service, r.cid, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    break;
  }
  case CBQ_DROP:
    break;
  }
}

// ------------------------------------------------------------------ asynchronous completions
// IOUserClient::sendAsyncResult64 is a protected static; this never-instantiated subclass only forwards to it.
class CoachzBasebandAsync : public IOUserClient {
public:
  static IOReturn send(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *args, UInt32 count)
  {
    return sendAsyncResult64(reference, result, args, count);
  }
};

static void bb_finish_read(bb_read *read, IOReturn result, const uint8_t *data, uint32_t len)
{
  io_user_reference_t args[1] = { 0 };
  if (data && read->memory) {
    const uint32_t n = len < read->len ? len : read->len;
    args[0] = read->memory->writeBytes(0, data, n);
    if (len > read->len) result = kIOReturnOverrun;
  }
  if (read->memory) {
    read->memory->complete();
    read->memory->release();
    read->memory = nullptr;
  }
  CoachzBasebandAsync::send(read->reference, result, args, 1);
}

static void bb_complete(thread_call_param_t, thread_call_param_t)
{
  for (;;) {
    bb_read read;
    bb_frame frame = { 0, nullptr };
    bool found = false;
    IOLockLock(bb_lock);
    for (uint32_t c = 0; c < CBH_PUBLISHED && !found; c++) {
      bb_channel *ch = &bb_ch[c];
      if (!ch->reads_waiting || !ch->count) continue;
      read = ch->reads[0];
      memmove(&ch->reads[0], &ch->reads[1], (--ch->reads_waiting) * sizeof ch->reads[0]);
      frame = bb_pop(c);
      found = true;
    }
    IOLockUnlock(bb_lock);
    if (!found) return;
    bb_finish_read(&read, kIOReturnSuccess, frame.data, frame.len);
    IOFreeData(frame.data, frame.len);
  }
}

// Every read a client has waiting ends with `result` (close, abort).
static void bb_abort_reads(uint32_t channel, void *owner, IOReturn result)
{
  bb_read ended[BB_READS];
  uint32_t n = 0;
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[channel];
  for (uint32_t i = 0; i < ch->reads_waiting;) {
    if (!owner || ch->reads[i].owner == owner) {
      ended[n++] = ch->reads[i];
      memmove(&ch->reads[i], &ch->reads[i + 1], (--ch->reads_waiting - i) * sizeof ch->reads[0]);
    } else {
      i++;
    }
  }
  IOLockWakeup(bb_lock, ch, false);
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) bb_finish_read(&ended[i], result, nullptr, 0);
}

// ------------------------------------------------------------------ user memory
static uint8_t *bb_copy_in(task_t task, mach_vm_address_t address, uint32_t len)
{
  if (!len || len > BB_FRAME_MAX) return nullptr;
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, len, kIODirectionOut, task);
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(len));
  bool ok = memory && copy && memory->prepare() == kIOReturnSuccess;
  if (ok) {
    ok = memory->readBytes(0, copy, len) == len;
    memory->complete();
  }
  OSSafeReleaseNULL(memory);
  if (!ok && copy) {
    IOFreeData(copy, len);
    copy = nullptr;
  }
  return copy;
}

static bool bb_async(IOExternalMethodArguments *args) { return args->asyncWakePort != MACH_PORT_NULL && args->asyncReference; }

static void bb_reply(IOExternalMethodArguments *args, IOReturn result, uint32_t count, uint64_t a = 0, uint64_t b = 0)
{
  io_user_reference_t values[2] = { a, b };
  OSAsyncReference64 reference;
  memset(reference, 0, sizeof reference);
  memcpy(reference, args->asyncReference, sizeof(io_user_reference_t) *
         (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
  CoachzBasebandAsync::send(reference, result, values, count);
}

// ------------------------------------------------------------------ AppleBasebandPCIInterface
class AppleBasebandPCIUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIUserClient)
  task_t fTask;
  uint32_t fChannel;
public:
  bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
  {
    fTask = task;
    return IOUserClient::initWithTask(task, security, type, properties);
  }
  void setChannel(uint32_t channel) { fChannel = channel; }
  IOReturn clientClose() override
  {
    closeChannel();
    terminate();
    return kIOReturnSuccess;
  }
  void closeChannel()
  {
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    IOLockLock(bb_lock);
    if (bb_ch[fChannel].owner == this) bb_ch[fChannel].owner = nullptr;
    IOLockUnlock(bb_lock);
  }
  IOReturn write(IOExternalMethodArguments *args);
  IOReturn read(IOExternalMethodArguments *args);
  IOReturn readRegister(IOExternalMethodArguments *args);
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIUserClient, IOUserClient)

IOReturn AppleBasebandPCIUserClient::write(IOExternalMethodArguments *args)
{
  uint8_t *copy = nullptr;
  uint32_t len = 0;
  if (bb_async(args)) {
    if (args->scalarInputCount < 2) return kIOReturnBadArgument;
    len = (uint32_t)args->scalarInput[1];
    copy = bb_copy_in(fTask, args->scalarInput[0], len);
  } else if (args->structureInputDescriptor) {
    IOMemoryDescriptor *memory = args->structureInputDescriptor;
    len = (uint32_t)memory->getLength();
    copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    bool ok = copy && memory->prepare() == kIOReturnSuccess;
    if (ok) {
      ok = memory->readBytes(0, copy, len) == len;
      memory->complete();
    }
    if (!ok && copy) {
      IOFreeData(copy, len);
      copy = nullptr;
    }
  } else if (args->structureInput && args->structureInputSize) {
    len = args->structureInputSize;
    copy = len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    if (copy) memcpy(copy, args->structureInput, len);
  }
  if (!copy) return kIOReturnBadArgument;
  IOLockLock(bb_lock);
  bb_host_write(fChannel, copy, len);
  IOLockUnlock(bb_lock);
  IOFreeData(copy, len);
  if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 1, len);
  return kIOReturnSuccess;
}

IOReturn AppleBasebandPCIUserClient::read(IOExternalMethodArguments *args)
{
  if (bb_async(args)) {
    if (args->scalarInputCount < 2 || !args->scalarInput[1]) return kIOReturnBadArgument;
    bb_read r;
    memset(&r, 0, sizeof r);
    memcpy(r.reference, args->asyncReference, sizeof(io_user_reference_t) *
           (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
    r.len = (uint32_t)args->scalarInput[1];
    r.owner = this;
    r.memory = IOMemoryDescriptor::withAddressRange(args->scalarInput[0], r.len, kIODirectionIn, fTask);
    if (!r.memory || r.memory->prepare() != kIOReturnSuccess) {
      OSSafeReleaseNULL(r.memory);
      return kIOReturnVMError;
    }
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool queued = ch->reads_waiting < BB_READS;
    if (queued) ch->reads[ch->reads_waiting++] = r;
    if (queued && ch->count && bb_complete_call) thread_call_enter(bb_complete_call);
    IOLockUnlock(bb_lock);
    if (!queued) {
      r.memory->complete();
      r.memory->release();
      return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
  }
  // synchronous: the next frame, waiting up to BB_SYNC_READ_SECONDS
  uint64_t deadline = 0;
  clock_interval_to_deadline(BB_SYNC_READ_SECONDS, kSecondScale, &deadline);
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[fChannel];
  int waited = THREAD_AWAKENED;
  while (!ch->count && ch->owner == this && waited == THREAD_AWAKENED)
    waited = IOLockSleepDeadline(bb_lock, ch, deadline, THREAD_INTERRUPTIBLE);
  bb_frame frame = bb_pop(fChannel);
  IOLockUnlock(bb_lock);
  if (!frame.data) return waited == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
  IOReturn result = kIOReturnSuccess;
  if (args->structureOutputDescriptor) {
    IOMemoryDescriptor *memory = args->structureOutputDescriptor;
    const uint32_t room = (uint32_t)memory->getLength(), n = frame.len < room ? frame.len : room;
    if (memory->prepare() == kIOReturnSuccess) {
      memory->writeBytes(0, frame.data, n);
      memory->complete();
      args->structureOutputDescriptorSize = n;
    } else {
      result = kIOReturnVMError;
    }
    if (frame.len > room) result = kIOReturnOverrun;
  } else if (args->structureOutput) {
    const uint32_t n = frame.len < args->structureOutputSize ? frame.len : args->structureOutputSize;
    memcpy(args->structureOutput, frame.data, n);
    if (frame.len > args->structureOutputSize) result = kIOReturnOverrun;
    args->structureOutputSize = n;
  } else {
    result = kIOReturnBadArgument;
  }
  IOFreeData(frame.data, frame.len);
  return result;
}

// IOABPClass::readRegister: one scalar, the register, and a structure output of the size asked for.
IOReturn AppleBasebandPCIUserClient::readRegister(IOExternalMethodArguments *args)
{
  if (args->scalarInputCount < 1) return kIOReturnBadArgument;
  const uint32_t reg = (uint32_t)args->scalarInput[0];   // pbl
  static uint32_t seen;
  if (reg < 32 && !(__atomic_fetch_or(&seen, 1u << reg, __ATOMIC_RELAXED) & (1u << reg)))
    ((void)0);
  if (args->structureOutput && args->structureOutputSize) {
    cbs_read_register(reg, static_cast<uint8_t *>(args->structureOutput), args->structureOutputSize);
    return kIOReturnSuccess;
  }
  if (IOMemoryDescriptor *memory = args->structureOutputDescriptor) {
    const uint32_t size = (uint32_t)memory->getLength();
    if (!size || size > BB_FRAME_MAX) return kIOReturnBadArgument;
    uint8_t *data = static_cast<uint8_t *>(IOMallocData(size));
    if (!data) return kIOReturnNoMemory;
    cbs_read_register(reg, data, size);
    IOReturn result = memory->prepare();
    if (result == kIOReturnSuccess) {
      memory->writeBytes(0, data, size);
      memory->complete();
      args->structureOutputDescriptorSize = size;
    }
    IOFreeData(data, size);
    return result;
  }
  return kIOReturnBadArgument;
}

// Selectors and shapes: coachz_bb_channels.h (libIOABP's IOConnectCall* sites).
IOReturn AppleBasebandPCIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                                    OSObject *, void *)
{
  if (!args) return kIOReturnBadArgument;
  switch (selector) {
  case 0: {   // open
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool free_channel = !ch->owner || ch->owner == this;
    if (free_channel) {
      ch->owner = this;
      if (!ch->opens++) ((void)0);
      if (fChannel == CBH_SAHARA && bb_boot_image_sent) {
        bb_boot_image_sent = false;
        bb_sahara_restart();
      }
    }
    IOLockUnlock(bb_lock);
    return free_channel ? kIOReturnSuccess : kIOReturnExclusiveAccess;
  }
  case 1:     // close
    closeChannel();
    return kIOReturnSuccess;
  case 2:
    return write(args);
  case 3:
    return read(args);
  case 4: {   // sendImage (BHI): accept metadata only; never execute Apple firmware on MPSS.
    const bool async = bb_async(args);
    uint64_t length = 0;
    if (async) {
      if (args->scalarInputCount < 2) return kIOReturnBadArgument;
      length = args->scalarInput[1];
    } else {
      if (!args->scalarOutput || args->scalarOutputCount < 2) return kIOReturnBadArgument;
      length = args->structureInputDescriptor ? args->structureInputDescriptor->getLength() :
               (args->structureInput ? args->structureInputSize : 0);
    }
    if (!length || length > 64u * 1024u * 1024u) return kIOReturnBadArgument;
    // iOS 16.5 IOABPClass::sendImage reads {bytesSent, result} from two scalar outputs.
    if (!async) {
      args->scalarOutput[0] = length;
      args->scalarOutput[1] = 0;
      args->scalarOutputCount = 2;
    }
    IOLockLock(bb_lock);
    if (fChannel == CBH_PBL) bb_boot_image_sent = true;
    IOLockUnlock(bb_lock);
    if (async) bb_reply(args, kIOReturnSuccess, 2, length, 0);
    return kIOReturnSuccess;
  }
  case 5:
    return readRegister(args);
  case 6:     // abort channel: pending reads end
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 7:     // start channel
  case 9:     // map shared memory: nothing here reads it
  case 10:    // unmap
  case 12:    // deregister time event
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 11:    // register time event: none is ever sent
    return kIOReturnSuccess;
  default:    // 8 time sync: no time base behind this channel
    ((void)0);
    return kIOReturnUnsupported;
  }
}

class AppleBasebandPCIInterface : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIInterface)
  uint32_t fChannel;
public:
  bool setup(uint32_t channel)
  {
    fChannel = channel;
    return setProperty(CBH_PROTOCOL_KEY, cbh_published[channel]);
  }
  // IOABPClass::start_nl puts ABPInterfaceProtocol straight in the IOServiceMatching dictionary. IOKit does not compare a key it does not know: the
  // family's matchPropertyTable does, and without it every client gets whichever channel comes first.
  bool matchPropertyTable(OSDictionary *table, SInt32 *score) override
  {
    return IOService::matchPropertyTable(table, score) && matchProtocol(table);
  }
  bool matchPropertyTable(OSDictionary *table) override
  {
    return IOService::matchPropertyTable(table) && matchProtocol(table);
  }
  bool matchProtocol(OSDictionary *table) const
  {
    OSString *wanted = OSDynamicCast(OSString, table->getObject(CBH_PROTOCOL_KEY));
    return !wanted || wanted->isEqualTo(cbh_published[fChannel]);
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) override
  {
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    static uint32_t seen;
    if (!(__atomic_fetch_or(&seen, 1u << fChannel, __ATOMIC_RELAXED) & (1u << fChannel)))
      ((void)0);
    AppleBasebandPCIUserClient *client = OSTypeAlloc(AppleBasebandPCIUserClient);
    if (!client) return kIOReturnNoMemory;
    client->setChannel(fChannel);
    if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
      client->release();
      return kIOReturnError;
    }
    if (!client->start(this)) {
      client->detach(this);
      client->release();
      return kIOReturnError;
    }
    *out = client;
    return kIOReturnSuccess;
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **out) override
  {
    return newUserClient(task, security, type, nullptr, out);
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, IOService)

// ------------------------------------------------------------------ AppleBasebandPCIControl
class AppleBasebandPCIControlUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIControlUserClient)
  OSAsyncReference64 fLogs;
  bool fLogsPending;
public:
  void endLogs()
  {
    if (!fLogsPending) return;
    fLogsPending = false;
    io_user_reference_t none[1] = { 0 };
    CoachzBasebandAsync::send(fLogs, kIOReturnAborted, none, 1);
  }
  IOReturn clientClose() override
  {
    endLogs();
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    switch (selector) {
    case 0:   // open
    case 3:   // logger tune
      return kIOReturnSuccess;
    case 1:   // close
      endLogs();
      return kIOReturnSuccess;
    case 2:   // read logs: there are none; the request waits until close
      if (!bb_async(args) || fLogsPending) return kIOReturnBadArgument;
      memset(fLogs, 0, sizeof fLogs);
      memcpy(fLogs, args->asyncReference, sizeof(io_user_reference_t) *
             (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
      fLogsPending = true;
      return kIOReturnSuccess;
    default:
      return kIOReturnUnsupported;
    }
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControlUserClient, IOUserClient)

template <class Client>
static IOReturn bb_open(IOService *service, task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out)
{
  if (!out) return kIOReturnBadArgument;
  *out = nullptr;
  Client *client = OSTypeAlloc(Client);
  if (!client) return kIOReturnNoMemory;
  if (!client->initWithTask(task, security, type, properties) || !client->attach(service)) {
    client->release();
    return kIOReturnError;
  }
  if (!client->start(service)) {
    client->detach(service);
    client->release();
    return kIOReturnError;
  }
  *out = client;
  return kIOReturnSuccess;
}

class AppleBasebandPCIControl : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIControl)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) ((void)0);
    return bb_open<AppleBasebandPCIControlUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControl, IOService)

// ------------------------------------------------------------------ AppleBaseband
class AppleBasebandUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    uint64_t out[4] = { 0 };
    uint32_t nout = args->scalarOutputCount < 4 ? args->scalarOutputCount : 4;
    IOLockLock(bb_lock);
    const int result = cbc_call(&bb_control, selector, args->scalarInput, args->scalarInputCount, out, &nout);
    // A reset or power-on goes back to before SendImage: nothing greets until the next one.
    if (!result && (selector == CBC_RESET_MODEM || selector == CBC_POWERCYCLE || (selector == CBC_POWER && args->scalarInputCount && args->scalarInput[0]))) {
      bb_boot_image_sent = false;
      bb_flush(CBH_SAHARA);
    }
    static uint32_t board_logs;
    const bool log_board = board_logs++ < 64;
    IOLockUnlock(bb_lock);
    if (log_board) ((void)0);
    if (result == CBC_UNSUPPORTED) ((void)0);
    for (uint32_t i = 0; i < nout; i++) args->scalarOutput[i] = out[i];
    args->scalarOutputCount = nout;
    return result;
  }
};
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient)

class AppleBaseband : public IOService {
  OSDeclareDefaultStructors(AppleBaseband)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    return bb_open<AppleBasebandUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBaseband, IOService)

#include "coachz_bb_pdp.cpp"

// ------------------------------------------------------------------ the data plane's kernel services
// ABM's data::TransportService calls ApplePDPHelperInterface::getInstance()->enableMetadataTrailer() without a null check, and getInstance
// is null without an AppleIPAppender service to open. ApplePDPHelper picks its variant by which PDP manager is registered; this is the Mav
// one. Nothing is behind them until pdp_ip exists: every AppleIPAppender method (0 addFlow ... 20 registerFilterRuleCallBack, libApplePDPHelper)
// succeeds with zeroed outputs, and the first use of each selector is logged.
class AppleIPAppenderUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleIPAppenderUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    // Capture the first call shape for each helper operation, including the traffic gate.
    // Payloads can contain network identities. Record only sizes and counts.
    static uint32_t shapes_seen;
    if (selector < 32 && !(__atomic_fetch_or(&shapes_seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      ((void)0);
    if (selector == 16) {
      if (args->scalarInputCount != 2) return kIOReturnBadArgument;
      return bb_pdp_block(static_cast<uint32_t>(args->scalarInput[0]), static_cast<uint32_t>(args->scalarInput[1]));
    }
    static uint32_t seen;
    if (selector < 32 && !(__atomic_fetch_or(&seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      ((void)0);
    for (uint32_t i = 0; i < args->scalarOutputCount; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    if (args->structureOutputDescriptor) args->structureOutputDescriptorSize = 0;
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, IOUserClient)

class AppleIPAppender : public IOService {
  OSDeclareDefaultStructors(AppleIPAppender)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    return bb_open<AppleIPAppenderUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleIPAppender, IOService)

// libPCITransport's pci::event::PDPListener matches the base class, ApplePDPHelper the leaf and AppleBasebandPCIPDPManager.
class AppleBasebandPCIPDPManagerBase : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManagerBase)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManagerBase, IOService)

class AppleBasebandPCIPDPManager : public AppleBasebandPCIPDPManagerBase {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManager, AppleBasebandPCIPDPManagerBase)

class AppleBasebandPCIMAVPDPManager : public AppleBasebandPCIPDPManager {
  OSDeclareDefaultStructors(AppleBasebandPCIMAVPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIMAVPDPManager, AppleBasebandPCIPDPManager)

// ------------------------------------------------------------------ publication and the Wi-Fi thread's entry points
template <class Service>
static Service *bb_publish(IOService *provider, uint32_t channel = ~0u)
{
  Service *service = OSTypeAlloc(Service);
  bool ok = service && service->init();
  if (ok && channel != ~0u) {
    AppleBasebandPCIInterface *interface = OSDynamicCast(AppleBasebandPCIInterface, service);
    ok = interface && interface->setup(channel);
  }
  ok = ok && service->attach(provider);
  if (ok && !service->start(provider)) {
    service->detach(provider);
    ok = false;
  }
  if (!ok) {
    OSSafeReleaseNULL(service);
    return nullptr;
  }
  service->registerService();
  return service;
}

// The modem's QRTR is up (GLINK started): publish the services ABM looks for. `send` and `del_client` run on the Wi-Fi thread only.
extern "C" void raphael_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), int (*del_client)(uint32_t))
{
  if (!raphael_baseband_enabled() || bb_published) return;
  if(!bb_init()) {
    static bool warned; if(!warned){warned=true;((void)0);}
    return;
  }
  if(bb_data_supported())bb_pdp_init();
  bb_send = send;
  bb_del_client = del_client;
  IOService *provider = IOService::getResourceService();
  if(!provider) {
    static bool warned; if(!warned){warned=true;((void)0);}
    return;
  }
  int made = 0;
  made += provider && bb_publish<AppleBaseband>(provider);
  made += provider && bb_publish<AppleBasebandPCIControl>(provider);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) made += provider && bb_publish<AppleBasebandPCIInterface>(provider, c);
  made += provider && bb_publish<AppleBasebandPCIMAVPDPManager>(provider);
  made += provider && bb_publish<AppleIPAppender>(provider);
  bb_published = 1;
  ((void)0);
}

extern "C" void raphael_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
  if (!raphael_baseband_enabled()) return;
  if(!bb_init())return;
  IOLockLock(bb_lock);
  cbq_server_new(&bb_qmux, service, version_instance, node, port);
  IOLockUnlock(bb_lock);
}

extern "C" void raphael_baseband_del_server(uint32_t service, uint32_t node, uint32_t port)
{
  if (!bb_lock) return;
  IOLockLock(bb_lock);
  cbq_server_del(&bb_qmux, service, node, port);
  if(service==CBD_SVC_WDS)bzero(bb_pdp_clients,sizeof bb_pdp_clients);
  IOLockUnlock(bb_lock);
}

// QRTR data for one of the bridge's client ports: 1 when the port is the bridge's.
extern "C" int raphael_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size)
{
  if (dst_port < CBQ_PORT_BASE || dst_port >= CBQ_PORT_BASE + CBQ_MAX_CLIENTS) return 0;
  if (!bb_lock) return 1;
  IOLockLock(bb_lock);
  const uint32_t index=dst_port-CBQ_PORT_BASE;
  const struct cbq_server *server=bb_qmux.client[index].used?
      cbq_server_find(&bb_qmux,bb_qmux.client[index].service):nullptr;
  const bool peer_ok=server && rbb_peer_ok(src_node,src_port,server->node,server->port);
  const bool sdu_ok=rbb_sdu_ok(static_cast<const uint8_t *>(body),size,1);
  bb_trace(peer_ok && sdu_ok ? "rx" : "rx_reject",dst_port,static_cast<const uint8_t *>(body),size,peer_ok && sdu_ok ? 0 : -1);
  if(!peer_ok || !sdu_ok) {
    static unsigned reject_logs;
    if(reject_logs++<24) ((void)0);
    bb_qmux.dropped++;IOLockUnlock(bb_lock);return 1;
  }
  static uint32_t reply_logs;
  const uint8_t *reply=static_cast<const uint8_t *>(body);
  const uint32_t service=bb_qmux.client[index].service;
  if (reply_logs<96 && reply[0]==CBQ_QMI_RESPONSE && (service==2 || service==CBD_SVC_WDS)) {
    ++reply_logs;
    ((void)0);
    if(service==2 && cbd_rd16(reply+3)==0x2du && !cbd_error(reply,size)) {
      uint32_t mode_len=0;const uint8_t *mode=cbd_find(reply,size,1,&mode_len);
      if(mode && mode_len==1) ((void)0);
    }
  }
  static uint32_t attach_reply_logs;
  if (service==CBD_SVC_WDS && reply[0]==CBQ_QMI_RESPONSE && cbd_rd16(reply+3)==0xc8 && attach_reply_logs++<16)
    ((void)0);
  const struct cbq_result r = cbq_from_modem(&bb_qmux, dst_port, src_node, static_cast<const uint8_t *>(body), size, bb_out, sizeof bb_out);
  if (r.kind == CBQ_TO_HOST && r.channel == CBQ_INTERNAL) {
    if (bb_pdc_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_pdc_slot) {
      bb_pdc_answer(static_cast<const uint8_t *>(body),size);
    } else if (bb_esim_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_esim_slot) {
      cbe_answer(&bb_esim, static_cast<const uint8_t *>(body), size, bb_now_ms());
    } else if (cbd_answer(&bb_data, r.service, static_cast<const uint8_t *>(body), size)) bb_data_step();
  } else if (r.kind == CBQ_TO_HOST) {
    if(r.service==CBD_SVC_WDS)bb_pdp_note(dst_port,r.cid,r.channel,static_cast<const uint8_t *>(body),size);
    bb_trace("host_queue",dst_port,static_cast<const uint8_t *>(body),size,0);
    // A temporary eUICC card-status indication is not the restored logical SIM.
    const uint8_t *sdu = static_cast<const uint8_t *>(body);
    if (bb_esim.freeze && r.service == CBE_SVC_UIM && size >= 7 &&
        sdu[0] == CBQ_QMI_INDICATION && cbd_rd16(sdu + 3) == 0x32u) {
      IOLockUnlock(bb_lock);
      return 1;
    }
    const uint32_t fixed = cbe_card_status_fix(&bb_esim, r.service,
      static_cast<const uint8_t *>(body), size, bb_fix, sizeof bb_fix);
    if (fixed) {
      cbq_frame_header(bb_out, fixed, r.service, r.cid);
      memcpy(bb_out + CBQ_HDR, bb_fix, fixed);
      bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, CBQ_HDR + fixed);
    } else bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, r.len);
    bb_sim_answer(r,sdu,size);
  }
  IOLockUnlock(bb_lock);
  return 1;
}

// On the Wi-Fi thread: QMI requests for the modem, then DEL_CLIENT for released clients.
extern "C" void raphael_baseband_poll(void)
{
  if (!bb_lock || !bb_send) return;
  IOLockLock(bb_lock);bb_data_start();IOLockUnlock(bb_lock);
  static uint32_t tx_retries, retire_retries, cancelled, error_logs;
  IOLockLock(bb_lock);
  bb_esim_step();
  /* Release: no PDC diagnostic transactions. */
  IOLockUnlock(bb_lock);
  // Only this QRTR owner removes queue entries. User threads may append while
  // callbacks run, so retain the front entry and its memory until accepted.
  for (uint32_t work=0; work<8; ++work) {
    IOLockLock(bb_lock);
    if (!bb_tx_count) { IOLockUnlock(bb_lock); break; }
    const bb_tx tx=bb_txq[bb_tx_head];
    const uint32_t slot=tx.local_port-CBQ_PORT_BASE;
    const bool closed=slot>=CBQ_MAX_CLIENTS || !bb_qmux.client[slot].used;
    if (closed) {
      bb_tx_head=(bb_tx_head+1)%BB_TX; --bb_tx_count; ++bb_tx_dropped; ++cancelled;
      IOLockUnlock(bb_lock); IOFreeData(tx.data,tx.len); continue;
    }
    IOLockUnlock(bb_lock);
    const int error=bb_send(tx.local_port,tx.node,tx.port,tx.data,tx.len);
    IOLockLock(bb_lock);
    bb_trace(error ? "deferred" : "sent",tx.local_port,tx.data,tx.len,error);
    if (!error) { bb_tx_head=(bb_tx_head+1)%BB_TX; --bb_tx_count; }
    else {
      ++tx_retries;
      if (error_logs++<32)
        ((void)0);
    }
    IOLockUnlock(bb_lock);
    if (error) break; // let the outer link loop dispatch received QRTR replies
    IOFreeData(tx.data,tx.len);
  }
  // Delete only after old writes have drained or their clients have closed.
  // Failed sends retain their reservation, rather than discarding the delete.
  for (uint32_t work=0; work<8 && bb_del_client; ++work) {
    IOLockLock(bb_lock);
    const uint32_t port=bb_tx_count ? 0 : cbq_next_release(&bb_qmux);
    IOLockUnlock(bb_lock);
    if (!port) break;
    const int error=bb_del_client(port);
    IOLockLock(bb_lock);
    const int completed=error ? cbq_release_retry(&bb_qmux,port) : cbq_release_complete(&bb_qmux,port);
    if (error) ++retire_retries;
    if (error_logs++<32)
      ((void)0);
    IOLockUnlock(bb_lock);
    if (error) break;
  }
  if(bb_data_supported())bb_pdp_poll();
  static uint32_t next_report_ms,report_count;
  const uint32_t now=bb_now_ms();
  if (report_count<18 && (!next_report_ms || (int32_t)(now-next_report_ms)>=0)) {
    next_report_ms=now+10000u; ++report_count;
    IOLockLock(bb_lock);
    ((void)0);
    IOLockUnlock(bb_lock);
    raphael_baseband_report(now/1000u);
  }
}

extern "C" void raphael_baseband_report(unsigned second)
{
  if (!bb_lock || !bb_published) return;
  if(bb_data_supported())bb_pdp_report(second);
  IOLockLock(bb_lock);
  uint32_t clients = 0, servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) clients += bb_qmux.client[i].used;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  ((void)0);
  ((void)0);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) {
    const bb_channel *ch = &bb_ch[c];
    if (ch->opens)
      ((void)0);
  }
  IOLockUnlock(bb_lock);
}
