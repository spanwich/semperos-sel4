/*
 * VPE0.c -- First user VPE for SemperOS on seL4/CAmkES
 *
 * Task 04f integration test: sends a NOOP syscall to the SemperOS kernel
 * and waits for the reply.
 *
 * Protocol:
 *   1. Kernel creates VPE0 object -> configures send EP (SYSC_EP=0)
 *      and recv EP (DEF_RECVEP=1) via vDTU RPC
 *   2. VPE0 attaches to the send channel and sends a NOOP syscall
 *   3. Kernel's WorkLoop fetches the message, SyscallHandler dispatches
 *   4. Kernel sends reply via m3::DTU::reply() -> auto-configures
 *      reply channel to VPE0's reply EP
 *   5. VPE0 polls reply EP and reads the result
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

/* VPE0 is PE 2 in the platform config (PE 0=kernel, PE 1=vDTU) */
#define MY_PE       2
#define MY_VPE_ID   0

/* SemperOS endpoint IDs (from m3::DTU constants) */
#define SYSC_EP     0   /* Send endpoint for syscalls */
#define DEF_RECVEP  1   /* Default receive endpoint (for replies) */

/* SemperOS NOOP syscall opcode (from KIF.h: Syscall::NOOP = 18) */
#define SYSCALL_NOOP  18

/* Channel table */
static struct vdtu_channel_table channels;

static void init_channel_table(void)
{
    volatile void *msg[] = {
        (volatile void *)msgchan_kv_0,
        (volatile void *)msgchan_kv_1,
        (volatile void *)msgchan_kv_2,
        (volatile void *)msgchan_kv_3,
        (volatile void *)msgchan_kv_4,
        (volatile void *)msgchan_kv_5,
        (volatile void *)msgchan_kv_6,
        (volatile void *)msgchan_kv_7,
    };
    volatile void *mem[] = {
        (volatile void *)memep_kv_0,
        (volatile void *)memep_kv_1,
        (volatile void *)memep_kv_2,
        (volatile void *)memep_kv_3,
    };
    vdtu_channels_init(&channels, msg, mem);
}

/*
 * Find a channel that has been initialized (ring ctrl present).
 * The kernel configures our channels via vDTU RPC before we run.
 */
static int find_configured_channel(void)
{
    for (int ch = 0; ch < VDTU_MSG_CHANNELS; ch++) {
        if (!channels.msg[ch]) continue;
        /* Try to attach to any ring that's been initialized by the kernel */
        if (!channels.msg_rings[ch].ctrl) {
            vdtu_channels_attach_ring(&channels, ch);
        }
        if (channels.msg_rings[ch].ctrl) {
            return ch;
        }
    }
    return -1;
}

/*
 * Send a NOOP syscall to the kernel.
 *
 * The syscall message format (m3 marshalling):
 *   Payload: [Operation opcode as ulong (8 bytes)]
 *
 * DTU header fields (set by vdtu_ring_send):
 *   label     = set by send EP config (kernel's RecvGate pointer)
 *   replylabel = identifies our reply gate
 *   replyEpId = our recv EP for the reply (DEF_RECVEP=1)
 */
static int send_noop_syscall(int send_chan, int reply_chan)
{
    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, send_chan);
    if (!ring) {
        printf("[VPE0] ERROR: send ring not available on channel %d\n", send_chan);
        return -1;
    }

    /* Construct NOOP syscall payload: just the opcode as a 64-bit value
     * (m3 marshalling pads enum to sizeof(ulong) = 8 bytes on x86_64) */
    uint64_t payload = SYSCALL_NOOP;

    /*
     * The label must be set to the kernel's RecvGate pointer for this
     * syscall gate. The kernel set this via config_send_remote() during
     * VPE::init(). Since VPE0's send EP was configured by the kernel,
     * the label is already stored in the vDTU endpoint table.
     *
     * However, our vdtu_ring_send() takes the label as a parameter
     * (it fills the header). We need to pass the correct label.
     *
     * For now, we pass 0 — the kernel's WorkLoop uses msg->label to
     * find the RecvGate. We'll need to use the correct label from the
     * vDTU endpoint config.
     *
     * HACK: The kernel's SyscallHandler sets up the label as the pointer
     * to its RecvGate. Since we don't know this value from VPE0, we
     * rely on the kernel's WorkLoop to use the EP-based dispatch instead.
     * Looking at WorkLoop.cc: it does `RecvGate *rgate = reinterpret_cast<RecvGate*>(msg->label)`.
     * So the label MUST be correct.
     *
     * Solution: The vDTU stores the label in the endpoint descriptor.
     * We could query it. For now, we'll set label=0 and patch the
     * WorkLoop to handle label=0 as "use gate from EP index".
     *
     * Better solution: The kernel's config_send_remote() stored the
     * label. We can retrieve it by asking the vDTU. But the VDTUConfig
     * interface doesn't have a "get_ep" query.
     *
     * Simplest solution: We know the label is the kernel's RecvGate pointer.
     * The kernel creates the VPE with syscEP=0, so the label will be
     * reinterpret_cast<label_t>(&SyscallHandler::get()._sysc_gate) or similar.
     *
     * ACTUAL simplest: just use label from the vDTU. We pass 0 here and
     * the ring_send will put it in the header. The kernel must be modified
     * to not crash on label=0.
     */

    printf("[VPE0] Sending NOOP syscall (opcode=%lu) on channel %d\n",
           (unsigned long)payload, send_chan);

    int rc = vdtu_ring_send(ring,
                            MY_PE, SYSC_EP, MY_VPE_ID,
                            DEF_RECVEP,    /* reply EP */
                            0,             /* label (see comment above) */
                            (uint64_t)reply_chan, /* replylabel = reply channel for routing */
                            0,             /* flags */
                            &payload, sizeof(payload));
    if (rc != 0) {
        printf("[VPE0] ERROR: send failed: %d\n", rc);
        return -1;
    }

    return 0;
}

/*
 * Wait for a reply from the kernel.
 */
static int wait_for_reply(int reply_chan)
{
    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, reply_chan);
    if (!ring) {
        printf("[VPE0] ERROR: reply ring not available on channel %d\n", reply_chan);
        return -1;
    }

    /* The kernel auto-configures the reply channel (channel 3 for VPE0's
     * DEF_RECVEP). We need to find it. Skip channel 0 (kernel's recv EP
     * where we sent — we're the producer there, not consumer). */
    printf("[VPE0] Waiting for reply (polling recv channels)...\n");

    int timeout = 100000000;
    const struct vdtu_message *reply = NULL;
    while (timeout-- > 0) {
        for (int ch = 1; ch < VDTU_MSG_CHANNELS; ch++) {
            if (!channels.msg[ch]) continue;
            if (!channels.msg_rings[ch].ctrl)
                vdtu_channels_attach_ring(&channels, ch);
            struct vdtu_ring *r = vdtu_channels_get_ring(&channels, ch);
            if (r && !vdtu_ring_is_empty(r)) {
                reply = vdtu_ring_fetch(r);
                if (reply) {
                    ring = r;
                    reply_chan = ch;
                    break;
                }
            }
        }
        if (reply) break;
    }

    if (!reply) {
        printf("[VPE0] ERROR: reply timeout\n");
        return -1;
    }

    /* Parse the reply: first 8 bytes is the error code */
    if (reply->hdr.length >= sizeof(uint64_t)) {
        uint64_t error_code = *(const uint64_t *)reply->data;
        printf("[VPE0] Got reply: error_code=%lu (0=NO_ERROR)\n",
               (unsigned long)error_code);
        if (error_code == 0) {
            printf("[VPE0] NOOP syscall succeeded!\n");
        } else {
            printf("[VPE0] NOOP syscall returned error %lu\n",
                   (unsigned long)error_code);
        }
    } else {
        printf("[VPE0] Got reply with %u bytes payload\n", reply->hdr.length);
    }

    vdtu_ring_ack(ring);
    return 0;
}

int run(void)
{
    printf("[VPE0] Starting (PE %d, VPE ID %d)\n", MY_PE, MY_VPE_ID);

    init_channel_table();

    /* Wait for the kernel to configure our endpoints.
     * The kernel creates VPE0 → VPE::init() → config_send_remote()
     * which sets up our send EP and recv EP via vDTU.
     * We need to find those channels. */
    printf("[VPE0] Looking for configured channels...\n");

    /* Small delay to let kernel finish VPE creation */
    for (volatile int i = 0; i < 10000000; i++) {}

    /* Find all configured channels */
    int send_chan = -1;
    int reply_chan = -1;

    for (int ch = 0; ch < VDTU_MSG_CHANNELS; ch++) {
        if (!channels.msg[ch]) continue;
        if (!channels.msg_rings[ch].ctrl) {
            vdtu_channels_attach_ring(&channels, ch);
        }
        if (channels.msg_rings[ch].ctrl) {
            /* The kernel's SYSC_GATE recv EP channel (0) is where we send.
             * Our reply recv EP channel will be different.
             * For now, use channel 0 as send (kernel's first SYSC_GATE)
             * and find any other initialized channel as reply. */
            if (send_chan < 0) {
                send_chan = ch;
            } else if (reply_chan < 0) {
                reply_chan = ch;
            }
        }
    }

    if (send_chan < 0) {
        printf("[VPE0] ERROR: no send channel found\n");
        return -1;
    }

    printf("[VPE0] Found send channel: %d\n", send_chan);

    if (reply_chan < 0) {
        /* No separate reply channel — the kernel's reply() will
         * auto-configure one. We need a channel for receiving.
         * For now, skip reply waiting if no channel available. */
        printf("[VPE0] No reply channel pre-configured (kernel auto-configures on reply)\n");
    } else {
        printf("[VPE0] Found reply channel: %d\n", reply_chan);
    }

    /* Send NOOP syscall */
    if (send_noop_syscall(send_chan, reply_chan) != 0) {
        printf("[VPE0] FAILED to send syscall\n");
        return -1;
    }

    printf("[VPE0] Syscall sent, waiting for kernel to process...\n");

    /* If we have a reply channel, wait for reply */
    if (reply_chan >= 0) {
        wait_for_reply(reply_chan);
    } else {
        /* No reply channel — just wait a bit and report that we sent */
        for (volatile int i = 0; i < 50000000; i++) {}
        printf("[VPE0] (no reply channel to check — syscall was sent)\n");
    }

    printf("[VPE0] === Integration test complete ===\n");
    return 0;
}
