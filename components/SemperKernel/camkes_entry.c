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
#define KERNEL_HEAP_SIZE  (4 * 1024 * 1024)  /* 4 MiB */

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

static volatile int net_ping_sent = 0;
static volatile int net_pong_sent = 0;
static volatile int net_pong_received = 0;
static uint32_t net_poll_count = 0;

/* Called from kernel_start() to attach to ring buffers */
void net_init_rings(void)
{
    vdtu_ring_attach(&g_net_out_ring, (void *)net_outbound);
    vdtu_ring_attach(&g_net_in_ring, (void *)net_inbound);
    net_rings_attached = 1;
    printf("[SemperKernel] Net rings attached (outbound + inbound)\n");
}

/* C wrapper for DTU.cc to write to outbound ring */
int net_ring_send(uint16_t sender_pe, uint8_t sender_ep,
                  uint16_t sender_vpe, uint8_t reply_ep,
                  uint64_t label, uint64_t replylabel, uint8_t flags,
                  const void *payload, uint16_t payload_len)
{
    if (!net_rings_attached) return -1;
    return vdtu_ring_send(&g_net_out_ring,
                          sender_pe, sender_ep, sender_vpe, reply_ep,
                          label, replylabel, flags,
                          payload, payload_len);
}

/* Called from WorkLoop every iteration to handle network I/O */
void net_poll(void)
{
    if (!net_rings_attached) return;

    net_poll_count++;

    /* Send PING after delay (let both nodes boot + hello exchange complete) */
    if (!net_ping_sent && net_poll_count == 1000000) {
        const char *payload = "PING from kernel";
        int rc = vdtu_ring_send(&g_net_out_ring,
                                0, 0, 0, 0,
                                NET_LABEL_PING, 0, 0,
                                payload, (uint16_t)strlen(payload));
        if (rc == 0) {
            net_ping_sent = 1;
            printf("[SemperKernel] NET: Sent PING to outbound ring\n");
        }
    }

    /* Poll inbound ring for messages from remote node */
    const struct vdtu_message *msg = vdtu_ring_fetch(&g_net_in_ring);
    if (msg) {
        /* Extract payload as string */
        char payload_str[128];
        uint16_t plen = msg->hdr.length;
        if (plen >= sizeof(payload_str)) plen = sizeof(payload_str) - 1;
        memcpy(payload_str, msg->data, plen);
        payload_str[plen] = '\0';

        printf("[SemperKernel] NET RX: label=0x%lx len=%u \"%s\"\n",
               (unsigned long)msg->hdr.label, msg->hdr.length, payload_str);

        if (msg->hdr.label == NET_LABEL_PING && !net_pong_sent) {
            /* Received PING → send PONG back */
            const char *pong = "PONG from kernel";
            vdtu_ring_send(&g_net_out_ring,
                           0, 0, 0, 0,
                           NET_LABEL_PONG, 0, 0,
                           pong, (uint16_t)strlen(pong));
            net_pong_sent = 1;
            printf("[SemperKernel] NET: Sent PONG reply\n");
        } else if (msg->hdr.label == NET_LABEL_PONG) {
            net_pong_received = 1;
            printf("[SemperKernel] NET: === PONG RECEIVED — round trip complete! ===\n");
        }

        vdtu_ring_ack(&g_net_in_ring);
    }

    /* Status report */
    if (net_poll_count == 3000000) {
        if (net_pong_received) {
            printf("[SemperKernel] NET: === PING-PONG SUCCESS ===\n");
        } else if (net_ping_sent) {
            printf("[SemperKernel] NET: PING sent, PONG not yet received\n");
        }
    }
}

int run(void)
{
    printf("=== SemperOS Kernel on seL4/CAmkES ===\n");

    /* Verify C++ runtime is working */
    cxx_test();

    /* Start the SemperOS kernel */
    kernel_start();

    return 0;
}
