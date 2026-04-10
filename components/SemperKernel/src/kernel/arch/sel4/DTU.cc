/*
 * arch/sel4/DTU.cc -- DTU operations via vDTU shared memory
 *
 * Replaces arch/gem5/DTU.cc. DTU hardware register accesses are replaced
 * with vDTU RPC calls (control plane) and shared memory ring buffer
 * operations (data plane).
 *
 * Design:
 *   - ep_channel[ep] maps SemperOS endpoint IDs → vDTU channel indices
 *   - config_recv_local() → vDTU RPC + ring init
 *   - config_send_local() → vDTU RPC + ring attach
 *   - fetch_msg(ep) → vdtu_ring_fetch() on mapped channel
 *   - reply(ep, data, len, off) → extract reply target from original msg header,
 *     find/allocate reply channel, vdtu_ring_send()
 *   - mark_read(ep, off) → vdtu_ring_ack()
 *
 * Thread safety note (re: cooperative threading):
 *   The single-threaded stub ThreadManager means revocation blocking
 *   (wait_for/notify) is a no-op. This is safe for single-kernel Task 04
 *   since revocation callbacks never actually block. For Task 05+
 *   (multi-kernel), the ThreadManager must be made functional to avoid
 *   deadlocks in the revocation algorithm (Section 4.3.3).
 */

extern "C" {
#include <stdio.h>
#include <string.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

/* CAmkES-generated symbols — dataports and RPC stubs.
 * We declare them manually to avoid including <camkes.h> from C++
 * (camkes.h pulls in seL4 utility headers that use C-only constructs). */
/* 32 uniform vDTU channel dataports (16 per PE pair).
 * Channels 0-15: kernel ↔ VPE0.  Channels 16-31: kernel ↔ VPE1. */
extern volatile void *dtu_ch_0, *dtu_ch_1, *dtu_ch_2, *dtu_ch_3;
extern volatile void *dtu_ch_4, *dtu_ch_5, *dtu_ch_6, *dtu_ch_7;
extern volatile void *dtu_ch_8, *dtu_ch_9, *dtu_ch_10, *dtu_ch_11;
extern volatile void *dtu_ch_12, *dtu_ch_13, *dtu_ch_14, *dtu_ch_15;
extern volatile void *dtu_ch_16, *dtu_ch_17, *dtu_ch_18, *dtu_ch_19;
extern volatile void *dtu_ch_20, *dtu_ch_21, *dtu_ch_22, *dtu_ch_23;
extern volatile void *dtu_ch_24, *dtu_ch_25, *dtu_ch_26, *dtu_ch_27;
extern volatile void *dtu_ch_28, *dtu_ch_29, *dtu_ch_30, *dtu_ch_31;

/* vDTU config RPC stubs (from VDTUConfig interface) */
int vdtu_config_recv(int target_pe, int ep_id, int buf_order, int msg_order, int flags);
int vdtu_config_recv_at(int target_pe, int ep_id, int channel, int buf_order, int msg_order, int flags);
int vdtu_config_send(int target_pe, int ep_id, int dest_pe, int dest_ep,
                     int dest_vpe, int msg_size, uint64_t label, int credits);
int vdtu_config_mem(int target_pe, int ep_id, int dest_pe, uint64_t addr,
                    uint64_t size, int dest_vpe, int perm);
int vdtu_invalidate_ep(int target_pe, int ep_id);
int vdtu_invalidate_eps(int target_pe, int first_ep);
int vdtu_terminate_ep(int target_pe, int ep_id);

/* Notifications */
void signal_vpe0_emit(void);
}

#include <base/log/Kernel.h>
#include <base/Panic.h>

#include "DTU.h"
#include "pes/VPE.h"
#include "mem/MainMemory.h"
#include "Platform.h"

/* ================================================================
 * Endpoint → Channel mapping
 *
 * SemperOS endpoints (0–15 per PE) are mapped to vDTU channels via
 * the ep_channel[] table. The channel is assigned by the vDTU service
 * during config_recv/config_send RPC.
 * ================================================================ */

#define MY_PE  0  /* Kernel is always PE 0 */

/* Per-endpoint state */
enum ep_state {
    EP_NONE,
    EP_RECV,       /* Receive endpoint — we own the ring as consumer */
    EP_SEND,       /* Send endpoint — we own the ring as producer */
    EP_MEM,        /* Memory endpoint — raw shared memory */
};

static int           ep_channel[EP_COUNT];  /* EP → channel index (-1 = unconfigured) */
static enum ep_state ep_type[EP_COUNT];
static struct vdtu_channel_table channels;
static bool channels_initialized = false;

/* Send endpoint config cache (for reply routing) */
struct send_ep_config {
    int dest_pe;
    int dest_ep;
    int dest_vpe;
    label_t label;
};
static struct send_ep_config ep_send_config[EP_COUNT];

/* Memory endpoint config cache (for write_mem/read_mem bounds checking) */
struct mem_ep_config {
    int dest_pe;
    uintptr_t base_addr;
    size_t size;
};
static struct mem_ep_config ep_mem_config[EP_COUNT];

/*
 * Initialize the channel table from CAmkES-generated dataport symbols.
 * Called once on first DTU operation.
 */
static void ensure_channels_init(void)
{
    if (channels_initialized) return;

    /* 32 uniform channels: 0-15 = kernel↔VPE0, 16-31 = kernel↔VPE1 */
    volatile void *dataports[VDTU_TOTAL_CHANNELS] = {
        (volatile void *)dtu_ch_0,  (volatile void *)dtu_ch_1,
        (volatile void *)dtu_ch_2,  (volatile void *)dtu_ch_3,
        (volatile void *)dtu_ch_4,  (volatile void *)dtu_ch_5,
        (volatile void *)dtu_ch_6,  (volatile void *)dtu_ch_7,
        (volatile void *)dtu_ch_8,  (volatile void *)dtu_ch_9,
        (volatile void *)dtu_ch_10, (volatile void *)dtu_ch_11,
        (volatile void *)dtu_ch_12, (volatile void *)dtu_ch_13,
        (volatile void *)dtu_ch_14, (volatile void *)dtu_ch_15,
        (volatile void *)dtu_ch_16, (volatile void *)dtu_ch_17,
        (volatile void *)dtu_ch_18, (volatile void *)dtu_ch_19,
        (volatile void *)dtu_ch_20, (volatile void *)dtu_ch_21,
        (volatile void *)dtu_ch_22, (volatile void *)dtu_ch_23,
        (volatile void *)dtu_ch_24, (volatile void *)dtu_ch_25,
        (volatile void *)dtu_ch_26, (volatile void *)dtu_ch_27,
        (volatile void *)dtu_ch_28, (volatile void *)dtu_ch_29,
        (volatile void *)dtu_ch_30, (volatile void *)dtu_ch_31,
    };
    vdtu_channels_init(&channels, dataports, VDTU_TOTAL_CHANNELS);

    for (int i = 0; i < EP_COUNT; i++) {
        ep_channel[i] = -1;
        ep_type[i] = EP_NONE;
        memset(&ep_send_config[i], 0, sizeof(ep_send_config[i]));
        memset(&ep_mem_config[i], 0, sizeof(ep_mem_config[i]));
    }

    channels_initialized = true;
}

/* Find a send channel to a given PE's recv EP */
static int find_send_channel_for(int dest_pe, int dest_ep)
{
    for (int i = 0; i < EP_COUNT; i++) {
        if (ep_type[i] == EP_SEND &&
            ep_send_config[i].dest_pe == dest_pe &&
            ep_send_config[i].dest_ep == dest_ep)
            return ep_channel[i];
    }
    return -1;
}

/* ================================================================
 * kernel::DTU — Control plane (endpoint configuration)
 * ================================================================ */

namespace kernel {

/*
 * PE routing helpers.
 * In multi-node mode, PE IDs are globally unique (node 0: 0-3, node 1: 4-7).
 * VDTUService and shared memory channels always use local PE indices (0-3).
 * NUM_LOCAL_PES is defined in Platform.h.
 */
static inline bool is_local_pe(int pe) {
    int base = (int)Platform::pe_base();
    return pe >= base && pe < base + NUM_LOCAL_PES;
}
static inline int to_local_pe(int pe) {
    return pe - (int)Platform::pe_base();
}

/* Note: DTU::_inst is defined in top-level DTU.cc with INIT_PRIO_USER(1) */

void DTU::init() {
    /* Called during static init (INIT_PRIO_USER(1)).
     * Don't use KLOG — ThreadManager not yet initialized. */
    ensure_channels_init();
}

int DTU::log_to_phys(int pe) {
    return pe;
}

void DTU::deprivilege(int) { }
void DTU::privilege(int) { }
void DTU::set_vpeid(const VPEDesc &) { }
void DTU::unset_vpeid(const VPEDesc &) { }

void DTU::wakeup(const VPEDesc &) {
    /* On sel4 VPEs are CAmkES components, always running */
}

void DTU::suspend(const VPEDesc &) { }
void DTU::injectIRQ(const VPEDesc &) { }
void DTU::set_rw_barrier(const VPEDesc &, uintptr_t) { }
void DTU::config_pf_remote(const VPEDesc &, uint64_t, int) { }
void DTU::config_pt_remote(const VPEDesc &, uint64_t) { }
void DTU::map_page(const VPEDesc &, uintptr_t, uintptr_t, int) { }
void DTU::map_kernel_page(const VPEDesc &, uintptr_t, uintptr_t, int, KernelAllocation &) { }
void DTU::unmap_page(const VPEDesc &, uintptr_t) { }

void DTU::invalidate_ep(const VPEDesc &vpe, int ep) {
    ensure_channels_init();
    int local_pe = to_local_pe(vpe.core);
    int rc = vdtu_invalidate_ep(local_pe, ep);
    if (rc != 0) {
        KLOG_V(EPS, "invalidate_ep(pe=" << vpe.core << " ep=" << ep << ") failed: " << rc);
    }
    /* Clear local mapping if this is our own EP */
    if (local_pe == MY_PE && ep >= 0 && ep < EP_COUNT) {
        ep_channel[ep] = -1;
        ep_type[ep] = EP_NONE;
    }
}

void DTU::invalidate_eps(const VPEDesc &vpe, int first) {
    ensure_channels_init();
    int local_pe = to_local_pe(vpe.core);
    vdtu_invalidate_eps(local_pe, first);
    /* Clear local mappings */
    if (local_pe == MY_PE) {
        for (int i = first; i < EP_COUNT; i++) {
            ep_channel[i] = -1;
            ep_type[i] = EP_NONE;
        }
    }
}

void DTU::config_recv_local(int ep, uintptr_t buf, uint order, uint msgorder, int flags) {
    ensure_channels_init();

    /* RPC to vDTU: allocate a channel for this recv endpoint */
    int ch = vdtu_config_recv(MY_PE, ep, order, msgorder, flags);
    if (ch < 0) {
        KLOG(ERR, "config_recv_local(ep=" << ep << ") failed: no free channels");
        return;
    }

    /* Initialize the ring buffer in the channel's shared memory */
    uint32_t slot_count = 1u << (order - msgorder);
    uint32_t slot_size  = 1u << msgorder;

    /* Cap to dataport limits (4 KiB dataport = 64B ctrl + slots) */
    size_t avail = 4096 - VDTU_RING_CTRL_SIZE;
    while (slot_count * slot_size > avail && slot_count > 2)
        slot_count >>= 1;

    vdtu_channels_init_ring(&channels, ch, slot_count, slot_size);

    /* Store the mapping */
    ep_channel[ep] = ch;
    ep_type[ep] = EP_RECV;

    printf("[DTU] config_recv_local(MY_PE=%d, ep=%d) -> ch=%d, ring_ptr=%p\n",
           MY_PE, ep, ch, (void *)vdtu_channels_get_ring(&channels, ch));
}

void DTU::config_recv_remote(const VPEDesc &vpe, int ep, uintptr_t buf,
    uint order, uint msgorder, int flags, bool valid)
{
    ensure_channels_init();
    int target_pe = to_local_pe(vpe.core);

    /* RPC to vDTU for remote PE (using local PE index) */
    int ch = vdtu_config_recv(target_pe, ep, order, msgorder, flags);
    if (ch < 0) {
        KLOG(ERR, "config_recv_remote(pe=" << target_pe << " ep=" << ep << ") failed");
        return;
    }

    /* For remote PEs, we don't init the ring locally — the remote component does.
     * But we init it here since kernel runs first and both sides share the memory. */
    uint32_t slot_count = 1u << (order - msgorder);
    uint32_t slot_size  = 1u << msgorder;
    size_t avail = 4096 - VDTU_RING_CTRL_SIZE;
    while (slot_count * slot_size > avail && slot_count > 2)
        slot_count >>= 1;

    vdtu_channels_init_ring(&channels, ch, slot_count, slot_size);

    printf("[DTU] config_recv_remote(local_pe=%d, ep=%d) -> ch=%d, ring_ptr=%p\n",
           target_pe, ep, ch, (void *)vdtu_channels_get_ring(&channels, ch));
}

void DTU::init_recv_channel(int ep, int ch, uint32_t slot_count, uint32_t slot_size) {
    ensure_channels_init();
    vdtu_channels_init_ring(&channels, ch, slot_count, slot_size);
    ep_channel[ep] = ch;
    ep_type[ep] = EP_RECV;
    volatile uint8_t *page = (volatile uint8_t *)channels.ch[ch];
    /* Verify the ring init actually wrote to the page */
    struct vdtu_ring_ctrl *verify = (struct vdtu_ring_ctrl *)page;
    printf("[DTU] init_recv_channel(ep=%d, ch=%d, sc=%u, ss=%u) page=%p verify: sc=%u ss=%u\n",
           ep, ch, slot_count, slot_size, (void *)page,
           verify ? verify->slot_count : 0,
           verify ? verify->slot_size : 0);
}

void DTU::config_send_local(int ep, label_t label, int dstcore, int dstvpe,
    int dstep, size_t msgsize, word_t credits)
{
    ensure_channels_init();

    /* Translate global→local for VDTUService RPC */
    int local_dst = to_local_pe(dstcore);
    int ch = vdtu_config_send(MY_PE, ep, local_dst, dstep, dstvpe,
                              (int)msgsize, (uint64_t)label, (int)credits);
    if (ch < 0) {
        KLOG(ERR, "config_send_local(ep=" << ep << " -> pe=" << dstcore
             << " ep=" << dstep << ") failed: dest recv EP not configured");
        return;
    }

    /* Attach to the ring (the recv side already initialized it) */
    vdtu_channels_attach_ring(&channels, ch);

    ep_channel[ep] = ch;
    ep_type[ep] = EP_SEND;
    ep_send_config[ep].dest_pe = local_dst;
    ep_send_config[ep].dest_ep = dstep;
    ep_send_config[ep].dest_vpe = dstvpe;
    ep_send_config[ep].label = label;

    KLOG_V(EPS, "config_send_local(ep=" << ep << " -> pe=" << dstcore
         << " ep=" << dstep << ") -> channel " << ch);
}

void DTU::config_send_remote(const VPEDesc &vpe, int ep, label_t label,
    int dstcore, int dstvpe, int dstep, size_t msgsize, word_t credits)
{
    ensure_channels_init();
    int target_pe = to_local_pe(vpe.core);

    /* Translate both target and destination to local PE indices */
    int local_dst = to_local_pe(dstcore);
    int ch = vdtu_config_send(target_pe, ep, local_dst, dstep, dstvpe,
                              (int)msgsize, (uint64_t)label, (int)credits);
    if (ch < 0) {
        KLOG(ERR, "config_send_remote(pe=" << target_pe << " ep=" << ep << ") failed");
        return;
    }

    /* Attach to ring */
    vdtu_channels_attach_ring(&channels, ch);

    printf("[DTU] config_send_remote(vpe.core=%d->local_pe=%d, ep=%d, dst_pe=%d->%d, dst_ep=%d) -> ch=%d, ring_ptr=%p\n",
           vpe.core, target_pe, ep, dstcore, local_dst, dstep, ch,
           (void *)vdtu_channels_get_ring(&channels, ch));
}

void DTU::config_mem_local(int ep, int dstcore, int dstvpe, uintptr_t addr, size_t size) {
    ensure_channels_init();

    int ch = vdtu_config_mem(MY_PE, ep, to_local_pe(dstcore), addr, size, dstvpe, 3 /* RW */);
    if (ch < 0) {
        KLOG(ERR, "config_mem_local(ep=" << ep << ") failed");
        return;
    }
    ep_channel[ep] = ch;
    ep_type[ep] = EP_MEM;
    ep_mem_config[ep].dest_pe = dstcore;
    ep_mem_config[ep].base_addr = addr;
    ep_mem_config[ep].size = size;
}

void DTU::config_mem_remote(const VPEDesc &vpe, int ep, int dstcore,
    int dstvpe, uintptr_t addr, size_t size, int perm)
{
    ensure_channels_init();
    int target_pe = to_local_pe(vpe.core);

    int ch = vdtu_config_mem(target_pe, ep, to_local_pe(dstcore), addr, size, dstvpe, perm);
    if (ch < 0) {
        KLOG(ERR, "config_mem_remote(pe=" << target_pe << " ep=" << ep << ") failed");
        return;
    }
    /* For remote PEs, no local mapping needed */
}

/* Network ring buffer send (07e) — defined in camkes_entry.c */
extern "C" {
    int net_ring_send(uint16_t sender_pe, uint8_t sender_ep,
                      uint16_t sender_vpe, uint8_t reply_ep,
                      uint64_t label, uint64_t replylabel, uint8_t flags,
                      const void *payload, uint16_t payload_len);
}

void DTU::send_to(const VPEDesc &vpe, int ep, label_t label,
    const void *msg, size_t size, label_t replylbl, int replyep)
{
    ensure_channels_init();

    /* Route remote PEs via DTUBridge ring buffer → UDP */
    if (!is_local_pe(vpe.core)) {
        KLOG(KRNLC, "Routing PE " << vpe.core << " to remote via ring (" << size << "B)");

        int rc = net_ring_send(MY_PE, (uint8_t)ep,
                               Platform::kernelId(), (uint8_t)replyep,
                               label, replylbl, 0,
                               msg, (uint16_t)size);
        if (rc != 0) {
            KLOG(ERR, "net_ring_send failed: " << rc);
        }
        return;
    }

    /* Local PE: use shared memory channel (translate global→local) */
    int ch = find_send_channel_for(to_local_pe(vpe.core), ep);
    if (ch < 0) {
        KLOG(ERR, "send_to(pe=" << vpe.core << " ep=" << ep << ") no send channel");
        return;
    }

    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ch);
    if (!ring) return;

    vdtu_ring_send(ring, MY_PE, (uint8_t)ep, Platform::kernelId(),
                   (uint8_t)replyep, label, replylbl, 0,
                   msg, (uint16_t)size);
}

void DTU::reply_to(const VPEDesc &vpe, int ep, int crdep, word_t credits,
    label_t label, const void *msg, size_t size)
{
    /* This is used for kernel→VPE replies (e.g., kernelcall replies).
     * Find a send channel to the VPE's reply endpoint. */
    ensure_channels_init();

    int local_pe = to_local_pe(vpe.core);
    int ch = find_send_channel_for(local_pe, ep);
    if (ch < 0) {
        KLOG(ERR, "reply_to(pe=" << vpe.core << " ep=" << ep << ") no reply channel");
        return;
    }

    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ch);
    if (!ring) return;

    vdtu_ring_send(ring, MY_PE, (uint8_t)crdep, Platform::kernelId(),
                   (uint8_t)ep, label, 0, VDTU_FLAG_REPLY,
                   msg, (uint16_t)size);
}

void DTU::write_mem(const VPEDesc &vpe, uintptr_t addr, const void *data, size_t size) {
    ensure_channels_init();
    int local_pe = to_local_pe(vpe.core);

    for (int i = 0; i < EP_COUNT; i++) {
        if (ep_type[i] != EP_MEM || ep_mem_config[i].dest_pe != local_pe)
            continue;
        uintptr_t base = ep_mem_config[i].base_addr;
        size_t region = ep_mem_config[i].size;
        if (addr < base || addr + size > base + region)
            continue;

        volatile void *mem = vdtu_channels_get_mem(&channels, ep_channel[i]);
        if (!mem) continue;
        memcpy((char *)mem + (addr - base), data, size);
        return;
    }
    KLOG(ERR, "write_mem: no mem EP for pe=" << vpe.core << " addr=0x" << m3::fmt(addr, "x"));
}

void DTU::read_mem(const VPEDesc &vpe, uintptr_t addr, void *data, size_t size) {
    ensure_channels_init();
    int local_pe = to_local_pe(vpe.core);

    for (int i = 0; i < EP_COUNT; i++) {
        if (ep_type[i] != EP_MEM || ep_mem_config[i].dest_pe != local_pe)
            continue;
        uintptr_t base = ep_mem_config[i].base_addr;
        size_t region = ep_mem_config[i].size;
        if (addr < base || addr + size > base + region)
            continue;

        volatile void *mem = vdtu_channels_get_mem(&channels, ep_channel[i]);
        if (!mem) continue;
        memcpy(data, (const char *)mem + (addr - base), size);
        return;
    }
    KLOG(ERR, "read_mem: no mem EP for pe=" << vpe.core << " addr=0x" << m3::fmt(addr, "x"));
}

void DTU::cmpxchg_mem(const VPEDesc &, uintptr_t, const void *, size_t, size_t, size_t) {
    /* stub — not used in current prototype */
}

/* Private helpers — not needed on sel4 (gem5 register manipulation) */
void DTU::config_recv(void *, uintptr_t, uint, uint, int) { }
void DTU::config_send(void *, label_t, int, int, int, size_t, word_t) { }
void DTU::config_mem(void *, int, int, uintptr_t, size_t, int) { }

} // namespace kernel


/* ================================================================
 * m3::DTU — Data path (message send/recv/reply)
 *
 * These are called directly by the WorkLoop and GateIStream to
 * exchange messages over the vDTU ring buffers.
 * ================================================================ */

namespace m3 {

DTU DTU::inst;

Errors::Code DTU::send(int ep, const void *msg, size_t size, label_t replylbl, int reply_ep) {
    ensure_channels_init();

    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0) {
        return Errors::INV_ARGS;
    }

    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ep_channel[ep]);
    if (!ring) return Errors::INV_ARGS;

    int rc = vdtu_ring_send(ring, MY_PE, (uint8_t)ep,
                            kernel::Platform::kernelId(),
                            (uint8_t)reply_ep,
                            ep_send_config[ep].label, replylbl, 0,
                            msg, (uint16_t)size);
    return (rc == 0) ? Errors::NO_ERROR : Errors::NO_SPACE;
}

Errors::Code DTU::reply(int ep, const void *data, size_t size, size_t msgoff) {
    /*
     * reply() sends a response on the same receive endpoint.
     *
     * msgoff == reinterpret_cast<uintptr_t>(original_msg) from get_msgoff().
     * We can cast it back to extract the original message's sender info,
     * which tells us WHERE to send the reply.
     *
     * On gem5, the DTU hardware reads the reply EP from the original message
     * header and sends the reply there. We do the same in software.
     */
    ensure_channels_init();

    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0)
        return Errors::INV_ARGS;

    /* Cast msgoff back to the original message to read reply routing info.
     * sender_pe may be a global PE ID — translate to local for VDTUService. */
    const DTU::Message *orig = reinterpret_cast<const DTU::Message*>(msgoff);
    int sender_pe = orig->senderCoreId - (int)kernel::Platform::pe_base();
    int reply_ep_id = orig->replyEpId;
    uint64_t replylabel = orig->replylabel;

    /* Find a send channel to the sender's reply EP.
     * If none exists, configure one on-the-fly. */
    int reply_ch = find_send_channel_for(sender_pe, reply_ep_id);
    if (reply_ch < 0) {
        /* Auto-configure: ask vDTU for send access to the sender's reply EP.
         * Find a free EP slot (scan for EP_NONE, don't use alloc_ep()
         * which may be exhausted by INIT_PRIO allocations). */
        /* Find a free EP slot OUTSIDE the SYSC/KRNLC gate range.
         * The WorkLoop polls EPs 0-5 (SYSC) and 6-13 (KRNLC).
         * Using an EP in those ranges for auto-reply would cause
         * the kernel to fetch its own reply messages. Start scanning
         * from FIRST_FREE_EP (14). */
        int auto_ep = -1;
        for (int i = kernel::DTU::FIRST_FREE_EP; i < EP_COUNT; i++) {
            if (ep_type[i] == EP_NONE) {
                auto_ep = i;
                break;
            }
        }
        if (auto_ep < 0) {
            printf("[DTU] reply: no free EP slot for auto-send to pe=%d ep=%d\n",
                   sender_pe, reply_ep_id);
            return Errors::NO_SPACE;
        }

        reply_ch = vdtu_config_send(MY_PE, auto_ep, sender_pe, reply_ep_id,
                                    orig->senderVpeId, 512,
                                    replylabel, VDTU_CREDITS_UNLIM);
        if (reply_ch < 0) {
            printf("[DTU] reply: config_send failed for pe=%d ep=%d\n",
                   sender_pe, reply_ep_id);
            return Errors::INV_ARGS;
        }

        vdtu_channels_attach_ring(&channels, reply_ch);
        ep_channel[auto_ep] = reply_ch;
        ep_type[auto_ep] = EP_SEND;
        ep_send_config[auto_ep].dest_pe = sender_pe;
        ep_send_config[auto_ep].dest_ep = reply_ep_id;
        ep_send_config[auto_ep].dest_vpe = orig->senderVpeId;
        ep_send_config[auto_ep].label = replylabel;
    }

    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, reply_ch);
    if (!ring) return Errors::INV_ARGS;

    int rc = vdtu_ring_send(ring, MY_PE, (uint8_t)ep,
                            kernel::Platform::kernelId(),
                            (uint8_t)reply_ep_id,
                            replylabel, 0,
                            VDTU_FLAG_REPLY,
                            data, (uint16_t)size);

    /* Don't ack here — GateIStream::finish() will call mark_read() to
     * consume the original message. Acking here caused a double-ack fault
     * because finish() would advance the tail past valid data. */

    return (rc == 0) ? Errors::NO_ERROR : Errors::NO_SPACE;
}

Errors::Code DTU::read(int ep, void *data, size_t size, size_t off) {
    ensure_channels_init();
    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0)
        return Errors::INV_ARGS;

    volatile void *mem = vdtu_channels_get_mem(&channels, ep_channel[ep]);
    if (!mem) return Errors::INV_ARGS;

    memcpy(data, (const char *)mem + off, size);
    return Errors::NO_ERROR;
}

Errors::Code DTU::write(int ep, const void *data, size_t size, size_t off) {
    ensure_channels_init();
    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0)
        return Errors::INV_ARGS;

    volatile void *mem = vdtu_channels_get_mem(&channels, ep_channel[ep]);
    if (!mem) return Errors::INV_ARGS;

    memcpy((char *)mem + off, data, size);
    return Errors::NO_ERROR;
}

bool DTU::is_valid(int ep) const {
    if (ep < 0 || ep >= EP_COUNT) return false;
    return ep_type[ep] != EP_NONE;
}

DTU::Message *DTU::fetch_msg(int ep) const {
    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0)
        return nullptr;

    struct vdtu_ring *ring = vdtu_channels_get_ring(
        const_cast<struct vdtu_channel_table *>(&channels), ep_channel[ep]);
    if (!ring) return nullptr;

    const struct vdtu_message *vmsg = vdtu_ring_fetch(ring);
    if (!vmsg) return nullptr;

    /* The vdtu_message and m3::DTU::Message have the same packed header layout.
     * Both are 25-byte headers followed by data[]. Cast directly. */
    return const_cast<DTU::Message *>(
        reinterpret_cast<const DTU::Message *>(vmsg));
}

void DTU::mark_read(int ep, size_t off) {
    if (ep < 0 || ep >= EP_COUNT || ep_channel[ep] < 0)
        return;

    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ep_channel[ep]);
    if (!ring) return;

    vdtu_ring_ack(ring);
}

} // namespace m3
