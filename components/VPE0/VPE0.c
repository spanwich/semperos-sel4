/*
 * VPE0.c -- Application VPE test stub (CAmkES component)
 *
 * Handles messages from the kernel in a loop for tests 1-3:
 *   - "HELLO_VPE"       -> reply "ACK"
 *   - "TEST2_MSG"       -> reply "ACK"
 *   - "MEM_CHECK:<chan>" -> read memory dataport, verify, reply "MEM_OK"/"MEM_FAIL"
 *
 * After 3 message exchanges, VPE0 exits. Tests 4 and 5 are pure control
 * plane (vDTU config/invalidate RPCs) that don't involve VPE0 data path.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

#define MY_PE   1

/* Channel table */
static struct vdtu_channel_table channels;

/* Number of message exchanges to handle (tests 1, 2, 3) */
#define NUM_EXCHANGES 3

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
 * Handle a single message exchange.
 * The kernel signals us, we read from the appropriate channel, process, reply.
 */
static int handle_message(int exchange_num)
{
    const struct vdtu_message *msg;
    struct vdtu_ring *recv_ring;
    char buf[256];
    int len;

    /* Try each possible receive channel to find the message.
     * In a real system, we'd know which channel to check. For the test,
     * we scan attached channels. */
    int recv_chan = -1;
    for (int ch = 0; ch < VDTU_MSG_CHANNELS; ch++) {
        /* Try to attach (idempotent if already attached) */
        if (!channels.msg[ch]) continue;
        if (!channels.msg_rings[ch].ctrl) {
            /* Try attaching */
            vdtu_channels_attach_ring(&channels, ch);
        }
        struct vdtu_ring *r = vdtu_channels_get_ring(&channels, ch);
        if (r && !vdtu_ring_is_empty(r)) {
            recv_chan = ch;
            break;
        }
    }

    if (recv_chan < 0) {
        printf("[VPE0] ERROR: no message found on any channel\n");
        return -1;
    }

    recv_ring = vdtu_channels_get_ring(&channels, recv_chan);
    msg = vdtu_ring_fetch(recv_ring);
    if (!msg) {
        printf("[VPE0] ERROR: fetch returned NULL on channel %d\n", recv_chan);
        return -1;
    }

    len = msg->hdr.length;
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    memcpy(buf, msg->data, len);
    buf[len] = '\0';

    printf("[VPE0] [exchange %d] Received: \"%s\" on channel %d "
           "(from PE %d EP %d)\n",
           exchange_num, buf, recv_chan,
           msg->hdr.sender_core_id, msg->hdr.sender_ep_id);

    /* Remember reply channel from replylabel */
    int reply_chan = (int)msg->hdr.replylabel;
    vdtu_ring_ack(recv_ring);

    /* Determine reply based on message content */
    const char *reply_text = NULL;

    if (strncmp(buf, "MEM_CHECK:", 10) == 0) {
        /* Memory endpoint test: read from the specified memory channel */
        int mem_chan = buf[10] - '0';
        volatile void *mem_ptr = vdtu_channels_get_mem(&channels, mem_chan);
        if (!mem_ptr) {
            printf("[VPE0] ERROR: cannot access mem channel %d\n", mem_chan);
            reply_text = "MEM_FAIL";
        } else {
            char mem_buf[64];
            memcpy(mem_buf, (const void *)mem_ptr, sizeof(mem_buf) - 1);
            mem_buf[sizeof(mem_buf) - 1] = '\0';
            printf("[VPE0] Memory channel %d contains: \"%s\"\n", mem_chan, mem_buf);
            if (strncmp(mem_buf, "MEMORY_TEST_DATA_12345678", 25) == 0) {
                reply_text = "MEM_OK";
            } else {
                reply_text = "MEM_FAIL";
            }
        }
    } else {
        /* For HELLO_VPE, TEST2_MSG, or anything else: reply "ACK" */
        reply_text = "ACK";
    }

    /* Send reply on the reply channel */
    /* Attach to reply channel if not already attached */
    if (!channels.msg_rings[reply_chan].ctrl) {
        vdtu_channels_attach_ring(&channels, reply_chan);
    }
    struct vdtu_ring *reply_ring = vdtu_channels_get_ring(&channels, reply_chan);
    if (!reply_ring) {
        printf("[VPE0] ERROR: cannot get reply ring for channel %d\n", reply_chan);
        return -1;
    }

    printf("[VPE0] Sending reply: \"%s\" on channel %d\n", reply_text, reply_chan);
    int rc = vdtu_ring_send(reply_ring,
                            MY_PE, 0, 0, 0,
                            0, 0, VDTU_FLAG_REPLY,
                            reply_text, (uint16_t)strlen(reply_text));
    if (rc != 0) {
        printf("[VPE0] ERROR: reply send failed: %d\n", rc);
        return -1;
    }

    signal_kernel_emit();
    return 0;
}

int run(void)
{
    printf("[VPE0] Starting (PE %d)\n", MY_PE);

    init_channel_table();

    for (int i = 0; i < NUM_EXCHANGES; i++) {
        printf("[VPE0] Waiting for signal from kernel...\n");
        signal_from_kernel_wait();

        if (handle_message(i + 1) != 0) {
            printf("[VPE0] ERROR: handle_message failed at exchange %d\n", i + 1);
            break;
        }
    }

    printf("[VPE0] Done (handled %d exchanges)\n", NUM_EXCHANGES);
    return 0;
}
