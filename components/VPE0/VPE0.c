/*
 * VPE0.c -- Application VPE test stub (CAmkES component)
 *
 * Simple echo server: polls its receive ring buffer for messages,
 * prints the payload, and sends a reply "ACK" back.
 *
 * In the real system, this component would run a SemperOS application
 * binary with the m3 user-space library.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"

#define MY_PE   1   /* VPE0 is PE 1 */

/* Ring buffer handles */
static struct vdtu_ring recv_ring;  /* messages from kernel */
static struct vdtu_ring reply_ring; /* replies back to kernel */

/*
 * Get a pointer to the dataport for a given channel index.
 */
static void *get_msgchan_dataport(int channel_idx)
{
    switch (channel_idx) {
        case 0: return (void *)msgchan_kv_0;
        case 1: return (void *)msgchan_kv_1;
        case 2: return (void *)msgchan_kv_2;
        case 3: return (void *)msgchan_kv_3;
        case 4: return (void *)msgchan_kv_4;
        case 5: return (void *)msgchan_kv_5;
        case 6: return (void *)msgchan_kv_6;
        case 7: return (void *)msgchan_kv_7;
        default: return NULL;
    }
}

/*
 * CAmkES component entry point.
 *
 * VPE0 is started after SemperKernel has configured the channels.
 * It attaches to the ring buffers that the kernel initialized in the
 * shared dataports, then enters a poll loop.
 *
 * Channel assignment is coordinated through the vDTU:
 *   - Channel 0: kernel -> VPE0 messages (VPE0 is consumer)
 *   - Channel 1: VPE0 -> kernel replies (VPE0 is producer)
 *
 * These indices match what config_recv returns to the kernel in
 * setup_channels(). In a real implementation, the vDTU would
 * communicate channel assignments to VPE0 as part of VPE setup.
 */
int run(void)
{
    printf("[VPE0] Starting (PE %d)\n", MY_PE);

    /*
     * Attach to channel 0 (kernel -> VPE0 messages).
     * The kernel already initialized this ring buffer.
     */
    void *recv_mem = get_msgchan_dataport(0);
    if (!recv_mem) {
        printf("[VPE0] ERROR: cannot access receive dataport\n");
        return 1;
    }
    vdtu_ring_attach(&recv_ring, recv_mem);
    printf("[VPE0] Attached to receive ring (channel 0, "
           "slot_count=%u, slot_size=%u)\n",
           recv_ring.ctrl->slot_count, recv_ring.ctrl->slot_size);

    /*
     * Attach to channel 1 (VPE0 -> kernel replies).
     * The kernel already initialized this ring buffer too.
     */
    void *reply_mem = get_msgchan_dataport(1);
    if (!reply_mem) {
        printf("[VPE0] ERROR: cannot access reply dataport\n");
        return 1;
    }
    vdtu_ring_attach(&reply_ring, reply_mem);
    printf("[VPE0] Attached to reply ring (channel 1)\n");

    /*
     * Poll for messages.
     */
    printf("[VPE0] Waiting for messages...\n");

    int handled = 0;
    int attempts = 0;
    while (handled < 1) {  /* Handle 1 message for the test */
        const struct vdtu_message *msg = vdtu_ring_fetch(&recv_ring);
        if (!msg) {
            /* No message available, poll again */
            attempts++;
            if (attempts > 1000000) {
                printf("[VPE0] Timeout waiting for messages\n");
                return 1;
            }
            continue;
        }

        /* Got a message */
        char buf[256];
        int len = msg->hdr.length;
        if (len > (int)sizeof(buf) - 1)
            len = (int)sizeof(buf) - 1;
        memcpy(buf, msg->data, len);
        buf[len] = '\0';

        printf("[VPE0] Received message: \"%s\" (len=%d, label=0x%lx, "
               "from PE %d EP %d)\n",
               buf, msg->hdr.length, (unsigned long)msg->hdr.label,
               msg->hdr.sender_core_id, msg->hdr.sender_ep_id);

        /* Remember reply info before acking */
        uint8_t  reply_ep   = msg->hdr.reply_ep_id;
        uint64_t replylabel = msg->hdr.replylabel;

        /* Acknowledge (consume) the message */
        vdtu_ring_ack(&recv_ring);

        /* Send reply "ACK" */
        const char *reply_text = "ACK";
        printf("[VPE0] Sending reply: \"%s\"\n", reply_text);

        int rc = vdtu_ring_send(&reply_ring,
                                MY_PE, 0,      /* sender PE, EP */
                                0,             /* sender VPE */
                                reply_ep,      /* reply EP (from original msg) */
                                replylabel,    /* label */
                                0,             /* replylabel */
                                VDTU_FLAG_REPLY,
                                reply_text,
                                (uint16_t)strlen(reply_text));
        if (rc != 0) {
            printf("[VPE0] ERROR: reply send failed: %d\n", rc);
            return 1;
        }

        handled++;
    }

    printf("[VPE0] Done (handled %d messages)\n", handled);
    return 0;
}
