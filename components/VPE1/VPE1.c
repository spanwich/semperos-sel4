/*
 * VPE1.c -- Service VPE for SemperOS spanning exchange (FPT-176 Layer 3)
 *
 * VPE1 registers a service ("testsrv") via real CREATEGATE + CREATESRV
 * syscalls, then handles OPEN/OBTAIN/DELEGATE requests from the kernel.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"
#include "vdtu_per_ep.h"   /* FPT-183: per-PE per-EP partitioned rings */

#ifndef SEMPER_KERNEL_ID
#define SEMPER_KERNEL_ID 0
#endif
#define MY_PE       (SEMPER_KERNEL_ID * 4 + 3)
#define MY_VPE_ID   1

/* SemperOS endpoint IDs */
#define SYSC_EP     0
#define DEF_RECVEP  1
#define SRV_EP      2   /* service recv endpoint */

/* SemperOS syscall opcodes */
#define SYSCALL_CREATEGATE  4
#define SYSCALL_CREATESRV   1

/* Service protocol commands (m3::KIF::Service) */
#define SRV_CMD_OPEN     0
#define SRV_CMD_OBTAIN   1
#define SRV_CMD_DELEGATE 2
#define SRV_CMD_CLOSE    3

#define CAP_TYPE_OBJ 0

static struct vdtu_channel_table channels;
static int send_chan = -1;

/* FPT-183: per-PE vDTU instance — VPE1's outbound + inbound EP rings.
 * DTUBridge initializes both dataports on its side; we attach. Data path
 * wired in Phase 3b once DTUBridge becomes the virtual NoC. */
static struct vdtu_per_ep_set g_vdtu_local_out;  /* VPE1 → DTUBridge */
static struct vdtu_per_ep_set g_vdtu_local_in;   /* DTUBridge → VPE1 */

/* See camkes_entry.c FPT183_SLOT_COUNT comment for the slot_count rationale. */
#define FPT183_SLOT_COUNT  4
#define FPT183_SLOT_SIZE   VDTU_KRNLC_MSG_SIZE  /* 2048 */

static void init_vdtu_per_ep(void)
{
    int rc;
    rc = vdtu_per_ep_attach(&g_vdtu_local_out, (void *)vdtu_out,
                            VDTU_PER_EP_COUNT,
                            FPT183_SLOT_COUNT, FPT183_SLOT_SIZE);
    if (rc != 0) {
        printf("[VPE1] vdtu_per_ep_attach(out) FAILED rc=%d\n", rc);
    }
    rc = vdtu_per_ep_attach(&g_vdtu_local_in, (void *)vdtu_in,
                            VDTU_PER_EP_COUNT,
                            FPT183_SLOT_COUNT, FPT183_SLOT_SIZE);
    if (rc != 0) {
        printf("[VPE1] vdtu_per_ep_attach(in) FAILED rc=%d\n", rc);
    }
}

static void init_channels(void)
{
    /* 16 uniform channels per PE pair.
     * Maps to kernel channels 16-31 (kernel ↔ VPE1 links). */
    volatile void *dataports[VDTU_CHANNELS_PER_PE] = {
        (volatile void *)dtu_ch_0,  (volatile void *)dtu_ch_1,
        (volatile void *)dtu_ch_2,  (volatile void *)dtu_ch_3,
        (volatile void *)dtu_ch_4,  (volatile void *)dtu_ch_5,
        (volatile void *)dtu_ch_6,  (volatile void *)dtu_ch_7,
        (volatile void *)dtu_ch_8,  (volatile void *)dtu_ch_9,
        (volatile void *)dtu_ch_10, (volatile void *)dtu_ch_11,
        (volatile void *)dtu_ch_12, (volatile void *)dtu_ch_13,
        (volatile void *)dtu_ch_14, (volatile void *)dtu_ch_15,
    };
    vdtu_channels_init(&channels, dataports, VDTU_CHANNELS_PER_PE);
}

/* Wait for reply on any recv channel. Yields every iteration.
 * Scans channels 1-15, skipping ch 0 which is the send channel — otherwise
 * vdtu_ring_fetch() would read back our own sent message (head > tail on
 * the same ring) and interpret the opcode as a spurious error code. */
static int wait_for_reply(void)
{
    /* FPT-183 Phase 3b-step-5: kernel replies arrive at our vdtu_in
     * (per-EP) via DTUBridge as virtual NoC. Scan all 32 EPs; legacy
     * dtu_ch_<N> scan retained for transition until 5f cleanup. */
    int timeout = 50000;
    while (timeout-- > 0) {
        for (uint32_t ep = 0; ep < VDTU_PER_EP_COUNT; ep++) {
            const struct vdtu_per_ep_routed_msg *m =
                vdtu_per_ep_fetch_routed(&g_vdtu_local_in, ep);
            if (m) {
                int result = -1;
                if (m->hdr.length >= sizeof(uint64_t))
                    result = (int)(*(const uint64_t *)m->data);
                vdtu_per_ep_ack(&g_vdtu_local_in, ep);
                return result;
            }
        }
        for (int ch = 1; ch < VDTU_CHANNELS_PER_PE; ch++) {
            if (!channels.ch[ch]) continue;
            if (!channels.rings[ch].ctrl)
                vdtu_channels_attach_ring(&channels, ch);
            struct vdtu_ring *r = vdtu_channels_get_ring(&channels, ch);
            if (!r) continue;
            const struct vdtu_message *reply = vdtu_ring_fetch(r);
            if (reply) {
                int result = -1;
                if (reply->hdr.length >= sizeof(uint64_t))
                    result = (int)(*(const uint64_t *)reply->data);
                vdtu_ring_ack(r);
                return result;
            }
        }
        seL4_Yield();
    }
    return -1;
}

static int send_syscall(const void *payload, uint16_t len)
{
    if (send_chan < 0) return -1;
    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, send_chan);
    if (!ring) return -1;
    /* Re-attach ring in case kernel initialized it after our first attach.
     * Also dump first 16 bytes of the page to verify physical page content. */
    if (ring->ctrl && ring->ctrl->slot_size == 0) {
        volatile uint8_t *page = (volatile uint8_t *)channels.ch[send_chan];
        vdtu_channels_attach_ring(&channels, send_chan);
        printf("[VPE1] re-attach ch %d: slot_size=%u, page[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
               send_chan, ring->ctrl ? ring->ctrl->slot_size : 0,
               page[0], page[1], page[2], page[3],
               page[4], page[5], page[6], page[7]);
    }

    int rc = vdtu_ring_send(ring, MY_PE, SYSC_EP, MY_VPE_ID,
                            DEF_RECVEP, 0, 0, 0, payload, len);
    if (rc != 0) return -1;
    return wait_for_reply();
}

/* Register service: CREATEGATE then CREATESRV */
static int register_service(void)
{
    int err;

    /* Step 1: CREATEGATE — create a MsgCapability for the service recv EP.
     * opcode=4, tcap=0(self), dstcap=5, label=0x54455354, epid=SRV_EP, credits */
    {
        uint64_t payload[6];
        payload[0] = SYSCALL_CREATEGATE;
        payload[1] = 0;               /* tcap = self */
        payload[2] = 5;               /* dstcap = selector 5 */
        payload[3] = 0x54455354;       /* label = "TEST" */
        payload[4] = SRV_EP;          /* endpoint ID */
        payload[5] = 512;             /* credits */
        err = send_syscall(payload, sizeof(payload));
        if (err != 0) {
            printf("[VPE1] CREATEGATE failed: %d\n", err);
            return err;
        }
        printf("[VPE1] CREATEGATE(sel=5, ep=%d): OK\n", SRV_EP);
    }

    /* Step 2: CREATESRV — register per-kernel service using the gate at sel 5.
     * Service name is "testsrv-k<KERNEL_ID>" so each node's service is unique.
     * This lets VPE0 on node X call createsess("testsrv-k<peer>") and exercise
     * the remote (spanning) path. */
    {
        uint8_t payload[48];
        memset(payload, 0, sizeof(payload));
        int off = 0;

        uint64_t opcode = SYSCALL_CREATESRV;
        memcpy(payload + off, &opcode, 8); off += 8;

        /* gatesel = 5 */
        uint64_t gatesel = 5;
        memcpy(payload + off, &gatesel, 8); off += 8;

        /* srvsel = 6 */
        uint64_t srvsel = 6;
        memcpy(payload + off, &srvsel, 8); off += 8;

        /* name = "testsrv-k<KERNEL_ID>" (10 bytes, padded to 16) */
        char srvname[11];
        srvname[0] = 't'; srvname[1] = 'e'; srvname[2] = 's'; srvname[3] = 't';
        srvname[4] = 's'; srvname[5] = 'r'; srvname[6] = 'v';
        srvname[7] = '-'; srvname[8] = 'k';
        srvname[9] = '0' + (SEMPER_KERNEL_ID & 0xF);
        srvname[10] = '\0';

        uint64_t namelen = 10;
        memcpy(payload + off, &namelen, 8); off += 8;
        memcpy(payload + off, srvname, 10); off += 16;  /* pad to 8 */

        err = send_syscall(payload, (uint16_t)off);
        if (err != 0) {
            printf("[VPE1] CREATESRV failed: %d\n", err);
            return err;
        }
        printf("[VPE1] CREATESRV('%s', gate=5, srv=6): OK\n", srvname);
    }

    return 0;
}

/* Reply to service request via the SYSC send channel (ch 0).
 * On gem5, the DTU hardware routes replies via reply_ep_id. On sel4,
 * the SRV_EP ring (ch 2) is unidirectional kernel→VPE1. We can't write
 * back to it without violating the SPSC invariant. Instead, send the
 * reply through the SYSC channel (which the kernel already polls) with
 * VDTU_FLAG_REPLY set so the kernel dispatches to the service callback. */
static int send_reply(struct vdtu_ring *ring, const struct vdtu_message *orig,
                      const void *data, uint16_t len)
{
    (void)ring;  /* unused — reply goes through send_chan, not the recv ring */
    struct vdtu_ring *send_ring = vdtu_channels_get_ring(&channels, send_chan);
    if (!send_ring) {
        printf("[VPE1] send_reply: no send ring (send_chan=%d)\n", send_chan);
        return -1;
    }
    return vdtu_ring_send(send_ring,
                          MY_PE, SRV_EP, MY_VPE_ID,
                          orig->hdr.reply_ep_id,
                          orig->hdr.replylabel, 0,
                          VDTU_FLAG_REPLY,
                          data, len);
}

/* Dispatch one service-request message. Used by both the per-EP scan
 * and the legacy channel scan in service_poll(). */
static void dispatch_srv_msg(const struct vdtu_msg_header *hdr,
                             const void *data_ptr,
                             uint16_t length,
                             struct vdtu_ring *legacy_ring)
{
    if (length < sizeof(uint64_t)) return;

    uint64_t cmd;
    memcpy(&cmd, data_ptr, sizeof(cmd));

    /* Build a synthetic vdtu_message wrapper for send_reply (which only
     * reads orig->hdr.reply_ep_id and orig->hdr.replylabel). For per-EP
     * delivery we don't have a vdtu_message in legacy form — pass a
     * stack copy of the header alone, with no payload. */
    struct vdtu_message *fake = NULL;
    uint8_t fake_buf[VDTU_HEADER_SIZE];
    if (legacy_ring == NULL) {
        memcpy(fake_buf, hdr, VDTU_HEADER_SIZE);
        fake = (struct vdtu_message *)fake_buf;
    }

    switch (cmd) {
    case SRV_CMD_OPEN: {
        printf("[VPE1] OPEN (replylabel=0x%lx)\n",
               (unsigned long)hdr->replylabel);
        uint64_t reply[2] = { 0, 1 };
        send_reply(legacy_ring, fake ? fake :
                   (const struct vdtu_message *)hdr,
                   reply, sizeof(reply));
        break;
    }
    case SRV_CMD_OBTAIN:
    case SRV_CMD_DELEGATE: {
        static uint32_t next_obtain_slot   = 10;
        static uint32_t next_delegate_slot = 20;
        uint32_t start = (cmd == SRV_CMD_OBTAIN)
                             ? next_obtain_slot++
                             : next_delegate_slot++;
        printf("[VPE1] %s -> slot=%u\n",
               cmd == SRV_CMD_OBTAIN ? "OBTAIN" : "DELEGATE",
               (unsigned)start);
        struct __attribute__((packed)) {
            uint64_t error;
            uint32_t type, start, count, pad;
        } reply = { 0, CAP_TYPE_OBJ, start, 1, 0 };
        send_reply(legacy_ring, fake ? fake :
                   (const struct vdtu_message *)hdr,
                   &reply, sizeof(reply));
        break;
    }
    case SRV_CMD_CLOSE:
        printf("[VPE1] CLOSE\n");
        break;
    default:
        printf("[VPE1] Unknown cmd %lu\n", (unsigned long)cmd);
        break;
    }
}

/* Poll for service requests.
 *
 * FPT-183 Phase 3b-step-5 follow-up: post-cutover, the kernel delivers
 * OPEN/OBTAIN/DELEGATE messages through the per-EP path —
 * net_route_send → DTUBridge → vpe1.vdtu_in[ep]. Without scanning that
 * here, OPEN never reaches us, the service never replies, CREATESESS
 * times out, and Tests 12/13/14 fail on every node. (3-node validation
 * caught this; on-target single-node passed because the kernel and
 * VPE1 in single-node never exchange service messages.)
 *
 * Both paths active during transition; legacy `dtu_ch_<N>` scan stays
 * until 5f cleanup once we're confident no service request takes the
 * legacy path any more.
 */
static void service_poll(void)
{
    /* New per-EP path. */
    for (uint32_t ep = 0; ep < VDTU_PER_EP_COUNT; ep++) {
        const struct vdtu_per_ep_routed_msg *m =
            vdtu_per_ep_fetch_routed(&g_vdtu_local_in, ep);
        if (!m) continue;
        dispatch_srv_msg(&m->hdr, m->data, m->hdr.length, NULL);
        vdtu_per_ep_ack(&g_vdtu_local_in, ep);
    }

    /* Legacy dtu_ch_<N> scan (transition path; will go silent post-5f). */
    for (int ch = 1; ch < VDTU_CHANNELS_PER_PE; ch++) {
        if (!channels.ch[ch]) continue;
        if (!channels.rings[ch].ctrl)
            vdtu_channels_attach_ring(&channels, ch);

        struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ch);
        if (!ring) continue;

        const struct vdtu_message *msg = vdtu_ring_fetch(ring);
        if (!msg) continue;

        dispatch_srv_msg(&msg->hdr, msg->data, msg->hdr.length, ring);
        vdtu_ring_ack(ring);
    }
}

int run(void)
{
    printf("[VPE1] Starting (PE %d, VPE ID %d)\n", MY_PE, MY_VPE_ID);

    init_channels();
    init_vdtu_per_ep();

    /* Debug: show dataport pointers and ring state for all channels */
    for (int ch = 0; ch < 4; ch++) {
        printf("[VPE1] ch[%d]: dataport=%p ring_ctrl=%p\n",
               ch, (void *)channels.ch[ch],
               channels.ch[ch] ? (void *)channels.rings[ch].ctrl : NULL);
    }

    /* Find send channel (kernel initialized it via VPE::init) */
    for (int ch = 0; ch < 4; ch++) {
        if (!channels.ch[ch]) continue;
        if (!channels.rings[ch].ctrl)
            vdtu_channels_attach_ring(&channels, ch);
        if (channels.rings[ch].ctrl) {
            send_chan = ch;
            break;
        }
    }

    /* Debug: show state after attach */
    for (int ch = 0; ch < 4; ch++) {
        if (!channels.ch[ch]) continue;
        struct vdtu_ring *r = vdtu_channels_get_ring(&channels, ch);
        printf("[VPE1] ch[%d] after attach: ring=%p empty=%d\n",
               ch, (void *)r, r ? vdtu_ring_is_empty(r) : -1);
    }
    printf("[VPE1] Send channel: %d\n", send_chan);

    /* Wait for kernel readiness (NOOP retries with yields) */
    {
        int ready = 0;
        uint64_t noop = 18;  /* SYSCALL_NOOP */
        for (int attempt = 0; attempt < 500; attempt++) {
            if (send_syscall(&noop, sizeof(noop)) == 0) { ready = 1; break; }
            for (int y = 0; y < 100; y++) seL4_Yield();
        }
        if (!ready)
            printf("[VPE1] WARNING: kernel not ready\n");
    }

    /* Register service via real syscalls */
    int err = register_service();
    if (err != 0) {
        printf("[VPE1] Service registration failed, entering passive mode\n");
        for (;;) seL4_Yield();
    }

    printf("[VPE1] Service 'testsrv' registered, polling for requests\n");

    /* Service event loop */
    for (;;) {
        service_poll();
        seL4_Yield();
    }

    return 0;
}
