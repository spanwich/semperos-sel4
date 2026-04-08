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

#define NET_LABEL_PING  0x50494E47ULL  /* "PING" in ASCII */
#define NET_LABEL_PONG  0x504F4E47ULL  /* "PONG" in ASCII */

/* Dispatch inbound KRNLC messages to C++ KernelcallHandler (Task 08).
 * Defined in WorkLoop.cc. */
extern void dispatch_net_krnlc(const void *raw_msg, uint16_t len);
/* Layer 2: kernel-to-kernel ping. Returns 0 on success, -1 on no peer. */
extern int krnlc_ping_peer(void);

static volatile int net_ping_sent = 0;
static volatile int net_pong_sent = 0;
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

/* Called from kernel_start() to attach to ring buffers */
void net_init_rings(void)
{
    vdtu_ring_attach(&g_net_out_ring, (void *)net_outbound);
    vdtu_ring_attach(&g_net_in_ring, (void *)net_inbound);
    net_rings_attached = 1;
    g_net_state = NET_DISCONNECTED;
    printf("[SemperKernel] Net rings attached (outbound + inbound)\n");
}

/* C wrapper for DTU.cc to write to outbound ring.
 * Implements vDTU delivery guarantee: never returns failure to kernel.
 * In DISCONNECTED/RETRYING state, messages are cached and drained later. */
int net_ring_send(uint16_t sender_pe, uint8_t sender_ep,
                  uint16_t sender_vpe, uint8_t reply_ep,
                  uint64_t label, uint64_t replylabel, uint8_t flags,
                  const void *payload, uint16_t payload_len)
{
    if (!net_rings_attached) return -1;

    if (g_net_state == NET_CONNECTED) {
        int rc = vdtu_ring_send(&g_net_out_ring,
                                sender_pe, sender_ep, sender_vpe, reply_ep,
                                label, replylabel, flags,
                                payload, payload_len);
        if (rc == 0) return 0;
        /* Ring full — cache and retry later */
        g_net_state = NET_RETRYING;
    }

    /* DISCONNECTED or RETRYING: cache the message */
    return net_cache_push(sender_pe, sender_ep, sender_vpe, reply_ep,
                          label, replylabel, flags, payload, payload_len);
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
     * Retry PING periodically until PONG received. */
    if (!net_pong_received && (net_poll_count % 500000) == 100000) {
        const char *payload = "PING from kernel";
        int rc = vdtu_ring_send(&g_net_out_ring,
                                0, 0, 0, 0,
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

    /* Poll inbound ring for messages from remote node */
    const struct vdtu_message *msg = vdtu_ring_fetch(&g_net_in_ring);
    if (msg) {
        /* First inbound message proves bidirectional link — transition to CONNECTED */
        if (g_net_state != NET_CONNECTED) {
            net_link_connected();
        }

        if (msg->hdr.label == NET_LABEL_PING && !net_pong_sent) {
            /* Received PING -> send PONG back (through state machine) */
            const char *pong = "PONG from kernel";
            net_ring_send(0, 0, 0, 0,
                          NET_LABEL_PONG, 0, 0,
                          pong, (uint16_t)strlen(pong));
            net_pong_sent = 1;
            printf("[SemperKernel] NET: Sent PONG reply\n");
        } else if (msg->hdr.label == NET_LABEL_PONG) {
            net_pong_received = 1;
            printf("[SemperKernel] NET: === PONG RECEIVED -- round trip complete! ===\n");
        } else {
            /* Inter-kernel message (Task 08): dispatch to KernelcallHandler.
             * The raw vdtu_message has the same layout as m3::DTU::Message. */
            uint16_t total = VDTU_HEADER_SIZE + msg->hdr.length;
            dispatch_net_krnlc((const void *)msg, total);
        }

        vdtu_ring_ack(&g_net_in_ring);
    }

    /* Layer 2: kernel-to-kernel ping after PONG proves link works */
    static int krnlc_ping_done = 0;
    if (net_pong_received && !krnlc_ping_done && net_poll_count > 2000000) {
        printf("[SemperKernel] Layer 2: sending KRNLC_PING to peer kernel...\n");
        int rc = krnlc_ping_peer();
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
