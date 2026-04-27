/*
 * camkes_entry.c -- CAmkES run() bridge to SemperOS C++ kernel
 *
 * CAmkES generates a C entry point (run). This file bridges to the
 * C++ kernel_start() function, and also calls the old test suite if
 * SEMPER_RUN_VDTU_TESTS is defined.
 */

#include <stdio.h>
#include <camkes.h>
#include <sel4/sel4.h>
#include <stddef.h>

/* C++ functions */
extern void cxx_test(void);
extern void kernel_start(void);

/*
 * Heap for the SemperOS kernel (4 MiB).
 *
 * CAmkES forces LibSel4MuslcSysMorecoreBytes=0 and provides a 1 MiB heap
 * via its component.common.c template (CONFIG_CAMKES_DEFAULT_HEAP_SIZE).
 * The SemperOS kernel needs ~4 MiB for capability tables, VPE objects,
 * receive buffers, and the DDL hash table.
 *
 * We override the heap via a constructor at priority 200, which runs BEFORE:
 *   - musl libc init (priority 201)
 *   - CAmkES init (priority 202)
 *   - SemperOS INIT_PRIO_USER constructors (priority 201+)
 *
 * This ensures that the first call to malloc/brk finds our 4 MiB buffer
 * via morecore_area/morecore_size (musl's init_morecore_region is lazy).
 */
#define KERNEL_HEAP_SIZE  (8 * 1024 * 1024)  /* 8 MiB — chain_revoke_100 needs headroom */

static char __attribute__((aligned(4096)))
    kernel_heap[KERNEL_HEAP_SIZE];

/* Defined in libsel4muslcsys/src/sys_morecore.c */
extern char *morecore_area;
extern size_t morecore_size;

static void __attribute__((constructor(200))) init_kernel_heap(void)
{
    morecore_area = kernel_heap;
    morecore_size = KERNEL_HEAP_SIZE;
}

/* Yield wrapper callable from C++ (sel4.h can't be included in C++) */
void sel4_yield_wrapper(void)
{
    seL4_Yield();
}

#ifndef SEMPEROS_NO_NETWORK
/*
 * Incoming network DTU message buffer.
 * DTUBridge deposits a message in the dtu_in dataport and signals us.
 * We copy it to this static buffer so the WorkLoop can poll it.
 *
 * Simple single-slot buffer for Tier 1 prototype.
 * The net_msg_arrived notification handler sets net_msg_pending=1.
 * The WorkLoop (or kernel polling loop) checks this flag.
 */
#include <string.h>
#include "vdtu_ring.h"
#include "vdtu_per_ep.h"   /* FPT-183: per-PE per-EP partitioned rings */

/* Must match Platform.h's NUM_LOCAL_PES. PEs are allocated per kernel in
 * contiguous blocks of this size; sender_kernel = sender_pe / NUM_LOCAL_PES. */
#ifndef NUM_LOCAL_PES
#define NUM_LOCAL_PES 4
#endif

static volatile int net_msg_pending = 0;
static uint8_t net_msg_buf[2048];
static uint16_t net_msg_len = 0;

/* CAmkES notification handler: DTUBridge -> SemperKernel */
void net_msg_arrived_handle(void)
{
    volatile uint8_t *src = (volatile uint8_t *)dtu_in;

    /* Read length header (first 2 bytes of dtu_in) */
    uint16_t len = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));

    if (len > 0 && len <= sizeof(net_msg_buf)) {
        memcpy(net_msg_buf, (const void *)(src + 2), len);
        net_msg_len = len;
        __sync_synchronize();
        net_msg_pending = 1;

        /* Parse and log the DTU header */
        if (len >= VDTU_HEADER_SIZE) {
            const struct vdtu_msg_header *hdr = (const struct vdtu_msg_header *)net_msg_buf;
            printf("[SemperKernel] NET RX: from PE %u EP %u, label=0x%lx, payload=%u bytes\n",
                   hdr->sender_core_id, hdr->sender_ep_id,
                   (unsigned long)hdr->label, hdr->length);
        }
    }
}

/* Accessors for C++ code */
int net_msg_is_pending(void)
{
    return net_msg_pending;
}

const uint8_t *net_msg_get_buf(void)
{
    return net_msg_buf;
}

uint16_t net_msg_get_len(void)
{
    return net_msg_len;
}

void net_msg_clear(void)
{
    net_msg_pending = 0;
    net_msg_len = 0;
}

/*
 * ================================================================
 *  Network ring buffer transport (07e)
 *
 *  net_outbound: kernel (producer) → DTUBridge (consumer) → UDP
 *  net_inbound:  UDP → DTUBridge (producer) → kernel (consumer)
 *
 *  DTUBridge initializes both rings in post_init().
 *  Kernel attaches in net_init_rings() called from kernel_start().
 *  WorkLoop calls net_poll() every iteration for PING/PONG demo.
 * ================================================================
 */

static struct vdtu_ring g_net_out_ring;  /* producer: kernel writes outbound msgs */
static struct vdtu_ring g_net_in_ring;   /* consumer: kernel reads inbound msgs */
static volatile int net_rings_attached = 0;

/* FPT-183: per-PE vDTU instance — kernel0 = PE 0 in the n-vDTU PE map.
 * DTUBridge initializes both dataports on its side (post_init); kernel
 * attaches in net_init_rings(). Phase 3a-step-2 stands the infrastructure
 * up; Phase 3b switches the DTU::send_to() routing to use these. */
static struct vdtu_per_ep_set g_vdtu_local_out;  /* kernel → DTUBridge */
static struct vdtu_per_ep_set g_vdtu_local_in;   /* DTUBridge → kernel */

/* slot_count must be ≥ 4: SPSC rings have effective capacity slot_count − 1.
 * SyscallHandler::broadcastCreateSess writes one createSessFwd per peer
 * to the same outbound EP back-to-back; a 2-slot ring (capacity 1)
 * silently drops the 2nd message with ring-full. With 4 slots, a 3-peer
 * cluster broadcasts both peer messages without bouncing. The next
 * larger pow2 (8) is reserved for D8 retransmission headroom in 3c. */
#define FPT183_SLOT_COUNT  4
#define FPT183_SLOT_SIZE   VDTU_KRNLC_MSG_SIZE  /* 2048 — covers KRNLC max */

#define NET_LABEL_PING  0x50494E47ULL  /* "PING" in ASCII */
#define NET_LABEL_PONG  0x504F4E47ULL  /* "PONG" in ASCII */

/* Dispatch inbound KRNLC messages to C++ KernelcallHandler (Task 08).
 * Defined in WorkLoop.cc. */
extern void dispatch_net_krnlc(const void *raw_msg, uint16_t len);
/* Layer 2: kernel-to-kernel ping. Returns 0 on success, -1 on no peer. */
extern int krnlc_ping_peer(void);

static volatile int net_ping_sent = 0;
static volatile int net_pong_sent = 0;

/* Per-sender KRNLC serialisation gate — mimics gem5 DTU hardware
 * backpressure. Set true before dispatching an inbound KRNLC msg from peer N.
 * Sync handlers: cleared immediately after dispatch_net_krnlc returns (the
 *   work was completed inline, no further async chain).
 * Async handlers (rgate subscribe in createSessFwd / exchangeOverSessionFwd):
 *   call krnlc_mark_async_pending(kid) so the post-dispatch auto-clear is
 *   skipped; the lambda then calls krnlc_clear_inflight(kid) at its
 *   completion point (lambda exit / unsubscribe).
 * While the flag for peer N is true, net_poll() defers the next msg from
 * peer N (leaves it in the ring; no ack). Cross-peer traffic is unaffected.
 * Indexed by sender_kernel_id. (FPT-176 comment 10540.) */
#ifndef MAX_SRC_KID
#define MAX_SRC_KID 8
#endif
static volatile int krnlc_inflight[MAX_SRC_KID];
static volatile int g_krnlc_async_pending;  /* set by handler if it subscribed
                                              * to an rgate before returning */

/* C++-callable helpers (see KernelcallHandler.cc).
 * Called from inside a FWD-style handler that subscribes to an rgate, to
 * tell net_poll "the work isn't done yet; don't clear inflight at dispatch
 * return — the lambda will clear via krnlc_clear_inflight when it finishes." */
void krnlc_mark_async_pending(void) {
    g_krnlc_async_pending = 1;
}

void krnlc_clear_inflight(unsigned char src_kid) {
    __sync_synchronize();
    if (src_kid < MAX_SRC_KID)
        krnlc_inflight[src_kid] = 0;
}
static volatile int net_pong_received = 0;
static uint32_t net_poll_count = 0;

/*
 * ================================================================
 *  vDTU Outbound State Machine (FPT-176, Design Doc Gap 7)
 *
 *  Preserves the gem5 DTU delivery contract: DTU::send_to() never
 *  fails. Messages sent before the network link is established are
 *  cached in an SPSC buffer and drained when the link comes up.
 *  Messages that fail after the link is up are retried.
 *
 *  State       | Event                  | Action                    | Next
 *  ------------|------------------------|---------------------------|------
 *  DISCONNECTED| net_ring_send()        | Push to cache             | DISC
 *  DISCONNECTED| inbound msg received   | Drain cache → ring        | CONN
 *  CONNECTED   | net_ring_send()        | Write to outbound ring    | CONN
 *  CONNECTED   | send fails (ring full) | Push to cache, retry      | RETRY
 *  RETRYING    | net_ring_send()        | Push to cache             | RETRY
 *  RETRYING    | net_poll() tick        | Try drain cache → ring    | RETRY/CONN
 * ================================================================
 */

enum net_link_state {
    NET_DISCONNECTED = 0,
    NET_CONNECTED    = 1,
    NET_RETRYING     = 2,
};

/* Cached outbound message (full DTU message with header) */
struct net_cache_msg {
    uint16_t sender_pe;
    uint8_t  sender_ep;
    uint16_t sender_vpe;
    uint8_t  reply_ep;
    uint64_t label;
    uint64_t replylabel;
    uint8_t  flags;
    uint16_t payload_len;
    uint8_t  payload[512];  /* max DTU message payload */
};

#define NET_CACHE_SIZE  16  /* max queued messages before link is up */

static enum net_link_state g_net_state = NET_DISCONNECTED;
static struct net_cache_msg g_net_cache[NET_CACHE_SIZE];
static int g_net_cache_head = 0;  /* next write position */
static int g_net_cache_tail = 0;  /* next read position */
static int g_net_cache_count = 0;

static int net_cache_push(uint16_t sender_pe, uint8_t sender_ep,
                          uint16_t sender_vpe, uint8_t reply_ep,
                          uint64_t label, uint64_t replylabel, uint8_t flags,
                          const void *payload, uint16_t payload_len)
{
    if (g_net_cache_count >= NET_CACHE_SIZE) {
        printf("[vDTU] WARNING: outbound cache full (%d msgs), dropping\n",
               NET_CACHE_SIZE);
        return -1;
    }
    struct net_cache_msg *slot = &g_net_cache[g_net_cache_head];
    slot->sender_pe   = sender_pe;
    slot->sender_ep   = sender_ep;
    slot->sender_vpe  = sender_vpe;
    slot->reply_ep    = reply_ep;
    slot->label       = label;
    slot->replylabel  = replylabel;
    slot->flags       = flags;
    slot->payload_len = (payload_len <= sizeof(slot->payload))
                        ? payload_len : sizeof(slot->payload);
    memcpy(slot->payload, payload, slot->payload_len);
    g_net_cache_head = (g_net_cache_head + 1) % NET_CACHE_SIZE;
    g_net_cache_count++;
    return 0;
}

static int net_cache_drain(void)
{
    int drained = 0;
    while (g_net_cache_count > 0) {
        struct net_cache_msg *slot = &g_net_cache[g_net_cache_tail];
        int rc = vdtu_ring_send(&g_net_out_ring,
                                slot->sender_pe, slot->sender_ep,
                                slot->sender_vpe, slot->reply_ep,
                                slot->label, slot->replylabel, slot->flags,
                                slot->payload, slot->payload_len);
        if (rc != 0) {
            /* Ring full — stop draining, retry next poll */
            return drained;
        }
        g_net_cache_tail = (g_net_cache_tail + 1) % NET_CACHE_SIZE;
        g_net_cache_count--;
        drained++;
    }
    return drained;
}

/* Transition to CONNECTED state — drain any cached messages */
static void net_link_connected(void)
{
    if (g_net_state == NET_CONNECTED) return;
    printf("[vDTU] Link CONNECTED (was %s, %d cached msgs)\n",
           g_net_state == NET_DISCONNECTED ? "DISCONNECTED" : "RETRYING",
           g_net_cache_count);
    g_net_state = NET_CONNECTED;
    if (g_net_cache_count > 0) {
        int n = net_cache_drain();
        printf("[vDTU] Drained %d cached messages\n", n);
        if (g_net_cache_count > 0)
            g_net_state = NET_RETRYING;
    }
}

/*
 * FPT-183 Phase 3b-step-5: unified send wrapper.
 *
 * DTU::send_to() and DTU::reply_to() in arch/sel4/DTU.cc call this for
 * EVERY destination — local-PE or remote-kernel alike. The kernel no
 * longer takes the is_local_pe() branch; DTUBridge inspects route.dest_pe
 * and routes accordingly (local → write to dest's vdtu_in[dest_ep],
 * remote → CryptoTransport+UDP to peer kernel's DTUBridge).
 *
 * src_ep is the kernel's outbound EP slot used for backpressure scoping
 * — convention: src_ep = dest_ep, so concurrent sends to different
 * dest EPs use different outbound rings, preserving per-EP isolation
 * between independent flows.
 *
 * Returns 0 on success, vdtu_per_ep_send_to error codes (-1..-4) on
 * failure. The Phase 3c D8 cache+retry will hide -1 (full) from callers
 * for forward messages; for now -1 propagates to KLOG.
 */
int net_route_send(uint32_t src_ep,
                   uint16_t dest_pe, uint8_t dest_ep,
                   uint16_t sender_pe, uint8_t sender_ep,
                   uint16_t sender_vpe, uint8_t reply_ep,
                   uint64_t label, uint64_t replylabel, uint8_t flags,
                   const void *payload, uint16_t payload_len)
{
    if (!net_rings_attached) return -1;
    return vdtu_per_ep_send_to(&g_vdtu_local_out, src_ep,
                               dest_pe, dest_ep,
                               sender_pe, sender_ep,
                               sender_vpe, reply_ep,
                               label, replylabel, flags,
                               payload, payload_len);
}

/* Called from kernel_start() to attach to ring buffers */
void net_init_rings(void)
{
    vdtu_ring_attach(&g_net_out_ring, (void *)net_outbound);
    vdtu_ring_attach(&g_net_in_ring, (void *)net_inbound);
    net_rings_attached = 1;
    g_net_state = NET_DISCONNECTED;
    printf("[SemperKernel] Net rings attached (outbound + inbound)\n");

    /* FPT-183: attach to the per-PE per-EP partitioned dataports owned by
     * DTUBridge. Used in Phase 3b when DTU::send_to() switches over. */
    int rc;
    rc = vdtu_per_ep_attach(&g_vdtu_local_out, (void *)vdtu_out,
                            VDTU_PER_EP_COUNT,
                            FPT183_SLOT_COUNT, FPT183_SLOT_SIZE);
    if (rc != 0)
        printf("[SemperKernel] vdtu_per_ep_attach(out) FAILED rc=%d\n", rc);

    rc = vdtu_per_ep_attach(&g_vdtu_local_in, (void *)vdtu_in,
                            VDTU_PER_EP_COUNT,
                            FPT183_SLOT_COUNT, FPT183_SLOT_SIZE);
    if (rc != 0)
        printf("[SemperKernel] vdtu_per_ep_attach(in) FAILED rc=%d\n", rc);
    printf("[SemperKernel] FPT-183: per-PE vDTU attached "
           "(%u EPs x %u slots x %u bytes)\n",
           VDTU_PER_EP_COUNT, FPT183_SLOT_COUNT, FPT183_SLOT_SIZE);
}

/* C wrapper for DTU.cc to write to outbound ring.
 * Implements vDTU delivery guarantee: never returns failure to kernel.
 * In DISCONNECTED/RETRYING state, messages are cached and drained later.
 *
 * FPT-179 Stage 4: dest_kernel_id tells DTUBridge which peer to route the
 * message to. Encoded in the high nibble of `flags` for wire compatibility. */
int net_ring_send_to(uint8_t dest_kernel_id,
                     uint16_t sender_pe, uint8_t sender_ep,
                     uint16_t sender_vpe, uint8_t reply_ep,
                     uint64_t label, uint64_t replylabel, uint8_t flags,
                     const void *payload, uint16_t payload_len)
{
    if (!net_rings_attached) return -1;

    uint8_t encoded_flags = VDTU_FLAGS_WITH_DEST(flags, dest_kernel_id);

    if (g_net_state == NET_CONNECTED) {
        int rc = vdtu_ring_send(&g_net_out_ring,
                                sender_pe, sender_ep, sender_vpe, reply_ep,
                                label, replylabel, encoded_flags,
                                payload, payload_len);
        if (rc == 0) return 0;
        /* Ring full — cache and retry later */
        g_net_state = NET_RETRYING;
    }

    /* DISCONNECTED or RETRYING: cache the message */
    return net_cache_push(sender_pe, sender_ep, sender_vpe, reply_ep,
                          label, replylabel, encoded_flags, payload, payload_len);
}

/* Backward-compat wrapper: default to peer 0 (for PING link probes). */
int net_ring_send(uint16_t sender_pe, uint8_t sender_ep,
                  uint16_t sender_vpe, uint8_t reply_ep,
                  uint64_t label, uint64_t replylabel, uint8_t flags,
                  const void *payload, uint16_t payload_len)
{
    /* dest_kernel_id=0xFF means "broadcast to peer 0" (link-layer fallback).
     * DTUBridge interprets 0xFF as "use peer 0 / first peer". */
    return net_ring_send_to(0xFF, sender_pe, sender_ep, sender_vpe, reply_ep,
                            label, replylabel, flags, payload, payload_len);
}

/*
 * FPT-183 Phase 3b-step-5: shared inbound dispatch.
 *
 * Both the legacy g_net_in_ring path and the new per-EP g_vdtu_local_in
 * path feed into this helper. Mirrors the pre-183 inline logic exactly:
 *   - PING: send PONG back (per-src-kid throttle).
 *   - PONG: mark net_pong_received.
 *   - Other: dispatch to KernelcallHandler with per-sender inflight gate
 *     mimicking gem5 DTU hardware backpressure (FPT-176 c10540).
 *
 * Returns 1 on consume, 0 on defer (caller should not ack and should
 * return from net_poll for retry on the next iteration).
 */
static int try_process_inbound_msg(const struct vdtu_message *msg)
{
    /* First inbound message proves bidirectional link. */
    if (g_net_state != NET_CONNECTED) {
        net_link_connected();
    }

    if (msg->hdr.label == NET_LABEL_PING) {
        /* FPT-179 Stage 4: reply once per sender-kernel, not globally. */
        static int pong_sent_to[MAX_SRC_KID];  /* indexed by src_kid */
        uint8_t src_kid =
            (uint8_t)(msg->hdr.sender_core_id / NUM_LOCAL_PES);
        uint16_t my_pe = (uint16_t)(KERNEL_ID * NUM_LOCAL_PES);
        printf("[SemperKernel] NET: PING RX from global_pe=%u → "
               "src_kid=%u (my kid=%u)\n",
               (unsigned)msg->hdr.sender_core_id, (unsigned)src_kid,
               (unsigned)KERNEL_ID);
        if (src_kid < MAX_SRC_KID && src_kid != KERNEL_ID
            && !pong_sent_to[src_kid]) {
            const char *pong = "PONG from kernel";
            net_ring_send_to(src_kid, my_pe, 0, 0, 0,
                             NET_LABEL_PONG, 0, 0,
                             pong, (uint16_t)strlen(pong));
            pong_sent_to[src_kid] = 1;
            net_pong_sent = 1;
            printf("[SemperKernel] NET: Sent PONG reply to kid=%u "
                   "(my kid=%u)\n",
                   (unsigned)src_kid, (unsigned)KERNEL_ID);
        }
        return 1;
    }
    if (msg->hdr.label == NET_LABEL_PONG) {
        net_pong_received = 1;
        printf("[SemperKernel] NET: === PONG RECEIVED -- "
               "round trip complete! ===\n");
        return 1;
    }

    /* Inter-kernel message (Task 08): dispatch to KernelcallHandler.
     * Per-sender KRNLC serialisation (FPT-176 c10540): mimic gem5's
     * DTU hardware backpressure. While the inflight flag for peer N is
     * set, defer subsequent messages from N (caller leaves the slot
     * unacked so we retry on the next net_poll). */
    uint8_t src_kid = (uint8_t)(msg->hdr.sender_core_id / NUM_LOCAL_PES);
    if (src_kid < MAX_SRC_KID && krnlc_inflight[src_kid]) {
        return 0;  /* defer */
    }
    if (src_kid < MAX_SRC_KID) {
        krnlc_inflight[src_kid] = 1;
        __sync_synchronize();
    }
    g_krnlc_async_pending = 0;
    uint16_t total = VDTU_HEADER_SIZE + msg->hdr.length;
    dispatch_net_krnlc((const void *)msg, total);
    __sync_synchronize();
    if (!g_krnlc_async_pending && src_kid < MAX_SRC_KID)
        krnlc_inflight[src_kid] = 0;
    return 1;
}

/* Called from WorkLoop every iteration to handle network I/O */
void net_poll(void)
{
    if (!net_rings_attached) return;

    net_poll_count++;

    /* Send PING directly to outbound ring (bypasses state machine).
     * PING is a link probe — it's OK if it fails before hello completes.
     * When the PONG comes back on the inbound ring, the state machine
     * transitions to CONNECTED and drains the SPSC cache.
     * Retry PING periodically until PONG received.
     *
     * FPT-179 Stage 4 fix-4: sender_pe MUST be this kernel's global PE
     * ID (KERNEL_ID * NUM_LOCAL_PES), NOT 0. The PONG responder derives
     * src_kid from sender_core_id/NUM_LOCAL_PES — if sender_pe=0, every
     * PING looks like it came from kid=0 and PONGs get misrouted. */
    if (!net_pong_received && (net_poll_count % 500000) == 100000) {
        const char *payload = "PING from kernel";
        uint16_t my_pe = (uint16_t)(KERNEL_ID * NUM_LOCAL_PES);
        int rc = vdtu_ring_send(&g_net_out_ring,
                                my_pe, 0, 0, 0,
                                NET_LABEL_PING, 0, 0,
                                payload, (uint16_t)strlen(payload));
        if (rc == 0 && !net_ping_sent) {
            net_ping_sent = 1;
            printf("[SemperKernel] NET: PING sent (state=%s)\n",
                   g_net_state == NET_CONNECTED ? "CONNECTED" :
                   g_net_state == NET_RETRYING ? "RETRYING" : "DISCONNECTED");
        }
    }

    /* Retry drain: if we have cached messages and the ring has space */
    if (g_net_state == NET_RETRYING && g_net_cache_count > 0) {
        int n = net_cache_drain();
        if (n > 0)
            printf("[vDTU] Retry drained %d msgs (%d remaining)\n",
                   n, g_net_cache_count);
        if (g_net_cache_count == 0)
            g_net_state = NET_CONNECTED;
    }

    /* Poll inbound ring(s) for messages from remote node.
     * Two sources active during the FPT-183 transition:
     *   - g_net_in_ring (legacy, single shared ring) — drained by older
     *     peer kernels still running pre-183 code.
     *   - g_vdtu_local_in (per-EP routed, FPT-183) — drained by peer
     *     kernels that have cut over.
     * Both feed the same dispatch helper. The legacy ring goes silent
     * once all peers run the new code; we keep the path until 5f cleanup.
     *
     * Helper return value: 1 = consumed, ack the source. 0 = defer (per-
     * sender KRNLC inflight gate active), don't ack and return from
     * net_poll so the next iteration retries.
     */

    {
        const struct vdtu_message *msg = vdtu_ring_fetch(&g_net_in_ring);
        if (msg) {
            int rc = try_process_inbound_msg(msg);
            if (rc == 0) return;
            vdtu_ring_ack(&g_net_in_ring);
        }
    }

    /* FPT-183: drain per-EP inbound rings. Each of 32 EPs is independent
     * — EP N being full does not block EP M (the property the legacy
     * single shared ring violated and FPT-183 restores). One slot per
     * EP per iteration; remaining slots come up on subsequent net_poll
     * calls. */
    if (net_rings_attached) {
        for (uint32_t ep = 0; ep < VDTU_PER_EP_COUNT; ep++) {
            const struct vdtu_per_ep_routed_msg *rm =
                vdtu_per_ep_fetch_routed(&g_vdtu_local_in, ep);
            if (!rm) continue;
            /* The slot layout is [route 4][hdr 25][payload]; vdtu_message
             * starts at &rm->hdr. */
            const struct vdtu_message *m =
                (const struct vdtu_message *)&rm->hdr;
            int rc = try_process_inbound_msg(m);
            if (rc == 0) return;  /* defer this EP, retry next iteration */
            vdtu_per_ep_ack(&g_vdtu_local_in, ep);
        }
    }

    /* Layer 2: kernel-to-kernel ping after PONG proves link works.
     * krnlc_ping_peer() blocks in ThreadManager::wait_for(). When a worker
     * thread takes over the WorkLoop, it must NOT re-enter ping. The
     * "in_progress" flag prevents re-entrancy. */
    static int krnlc_ping_done = 0;
    static volatile int krnlc_ping_in_progress = 0;
    if (net_pong_received && !krnlc_ping_done && !krnlc_ping_in_progress
        && net_poll_count > 2000000) {
        krnlc_ping_in_progress = 1;
        __sync_synchronize();
        printf("[SemperKernel] Layer 2: sending KRNLC_PING to peer kernel...\n");
        int rc = krnlc_ping_peer();
        krnlc_ping_in_progress = 0;
        if (rc == 0) {
            printf("[SemperKernel] Layer 2: === KRNLC_PING SUCCESS ===\n");
            krnlc_ping_done = 1;
        } else {
            printf("[SemperKernel] Layer 2: KRNLC_PING failed (rc=%d)\n", rc);
        }
    }

    /* Status report */
    if (net_poll_count == 3000000) {
        if (net_pong_received) {
            printf("[SemperKernel] NET: === PING-PONG SUCCESS ===\n");
        } else if (net_ping_sent && !net_pong_received) {
            printf("[SemperKernel] NET: PING sent, PONG not yet received (state=%s)\n",
                   g_net_state == NET_CONNECTED ? "CONNECTED" :
                   g_net_state == NET_RETRYING ? "RETRYING" : "DISCONNECTED");
        }
    }

    /* FPT-176 c10553 follow-up — periodic dump of FWD lifecycle counters
     * so we can correlate per-leg counts between the three nodes. Dumps
     * every ~10M poll iterations once the kernel has been up long enough
     * to have meaningful traffic. Read-only. */
    extern volatile unsigned long g_fwd_alloc_createsess;
    extern volatile unsigned long g_fwd_recvreply_createsess;
    extern volatile unsigned long g_fwd_unsubscribe_createsess;
    extern volatile unsigned long g_fwd_alloc_xchgsess;
    extern volatile unsigned long g_fwd_recvreply_xchgsess;
    extern volatile unsigned long g_fwd_unsubscribe_xchgsess;
    extern volatile unsigned long g_resp_send_createsess;
    extern volatile unsigned long g_resp_dispatch_createsess;
    extern volatile unsigned long g_resp_send_xchgsess;
    extern volatile unsigned long g_resp_dispatch_xchgsess;
    if (net_poll_count > 4000000 && (net_poll_count % 10000000) == 0) {
        printf("[SemperKernel] FWD-trace: createsess "
               "alloc=%lu recvreply=%lu unsub=%lu | resp send=%lu dispatch=%lu\n",
               g_fwd_alloc_createsess, g_fwd_recvreply_createsess,
               g_fwd_unsubscribe_createsess,
               g_resp_send_createsess, g_resp_dispatch_createsess);
        printf("[SemperKernel] FWD-trace: xchgsess   "
               "alloc=%lu recvreply=%lu unsub=%lu | resp send=%lu dispatch=%lu\n",
               g_fwd_alloc_xchgsess, g_fwd_recvreply_xchgsess,
               g_fwd_unsubscribe_xchgsess,
               g_resp_send_xchgsess, g_resp_dispatch_xchgsess);
    }
}
#else  /* SEMPEROS_NO_NETWORK */
/* Stubs for builds without DTUBridge (e.g. XCP-ng local-only benchmarks) */
void net_init_rings(void) {}
void net_poll(void) {}
int net_ring_send(uint16_t s_pe, uint8_t s_ep, uint16_t s_vpe, uint8_t r_ep,
                  uint64_t label, uint64_t rlabel, uint8_t flags,
                  const void *payload, uint16_t plen) { (void)s_pe; (void)s_ep; (void)s_vpe; (void)r_ep; (void)label; (void)rlabel; (void)flags; (void)payload; (void)plen; return -1; }
#endif /* SEMPEROS_NO_NETWORK */

int run(void)
{
    printf("=== SemperOS Kernel on seL4/CAmkES ===\n");

    /* Verify C++ runtime is working */
    cxx_test();

    /* Start the SemperOS kernel */
    kernel_start();

    return 0;
}
