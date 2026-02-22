/*
 * SemperKernel.c -- SemperOS kernel test stub (CAmkES component)
 *
 * This is a pure C test stub that validates the CAmkES plumbing:
 *   1. Calls vDTU config RPC to set up endpoints
 *   2. Initializes ring buffers in shared dataports
 *   3. Sends a test message through the ring buffer
 *   4. Signals VPE0 via notification, then waits for reply notification
 *   5. Reads reply from ring buffer
 *
 * This does NOT contain real SemperOS code. Integration with the actual
 * SemperOS kernel (C++) is Task 04.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"

/*
 * PE IDs matching the vDTU's assignment.
 * PE 0 = SemperKernel, PE 1 = VPE0.
 */
#define MY_PE       0
#define VPE0_PE     1

/* Endpoint assignments for this test:
 * EP 0 on kernel: send endpoint -> VPE0 EP 0 (recv)
 * EP 1 on kernel: receive endpoint (for replies from VPE0)
 *
 * This mirrors the real SemperOS layout where EP 0 on user VPEs
 * is the syscall send EP, and the kernel has recv EPs for syscalls.
 * But for this test we invert: kernel sends to VPE0, VPE0 replies.
 */
#define SEND_EP     0
#define RECV_EP     1

/* Channel indices (returned by vDTU config_recv) */
static int send_channel = -1;  /* channel for kernel->VPE0 messages */
static int recv_channel = -1;  /* channel for VPE0->kernel replies  */

/* Ring buffer handles */
static struct vdtu_ring send_ring;
static struct vdtu_ring recv_ring;

/*
 * Get a pointer to the dataport for a given channel index.
 * In the real CAmkES build, these map to msgchan_kv_N dataport symbols.
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
 * Configure endpoints and set up ring buffers for the test.
 */
static int setup_channels(void)
{
    int rc;

    printf("[SemperKernel] Configuring VPE0 receive endpoint (EP 0)\n");

    /* Step 1: Configure a receive endpoint on VPE0 (EP 0).
     * This is where our messages will arrive.
     * buf_order = 11 (2 KiB), msg_order = 9 (512 B) -> 4 slots
     * Fits in a 4 KiB dataport. */
    rc = vdtu_config_recv(VPE0_PE, 0, /* ep */
                          11, /* buf_order: 1<<11 = 2048 */
                          9,  /* msg_order: 1<<9 = 512 */
                          0   /* flags */);
    if (rc < 0) {
        printf("[SemperKernel] ERROR: config_recv failed: %d\n", rc);
        return -1;
    }
    send_channel = rc;
    printf("[SemperKernel] VPE0 recv EP 0 -> channel %d\n", send_channel);

    /* Step 2: Configure a receive endpoint on kernel (EP 1) for replies. */
    printf("[SemperKernel] Configuring kernel receive endpoint (EP 1)\n");
    rc = vdtu_config_recv(MY_PE, RECV_EP,
                          11, /* buf_order */
                          9,  /* msg_order */
                          0   /* flags */);
    if (rc < 0) {
        printf("[SemperKernel] ERROR: config_recv for replies failed: %d\n", rc);
        return -1;
    }
    recv_channel = rc;
    printf("[SemperKernel] Kernel recv EP 1 -> channel %d\n", recv_channel);

    /* Step 3: Configure a send endpoint on kernel (EP 0) targeting VPE0 EP 0. */
    printf("[SemperKernel] Configuring kernel send endpoint (EP 0)\n");
    rc = vdtu_config_send(MY_PE, SEND_EP,
                          VPE0_PE, 0, /* dest PE, dest EP */
                          0,          /* dest VPE */
                          512,        /* msg_size */
                          0xDEADBEEF, /* label */
                          VDTU_CREDITS_UNLIM);
    if (rc < 0) {
        printf("[SemperKernel] ERROR: config_send failed: %d\n", rc);
        return -1;
    }

    /* Step 4: Initialize ring buffers in the assigned dataports */
    void *send_mem = get_msgchan_dataport(send_channel);
    void *recv_mem = get_msgchan_dataport(recv_channel);

    if (!send_mem || !recv_mem) {
        printf("[SemperKernel] ERROR: could not get dataport for channels\n");
        return -1;
    }

    /* Initialize the send ring (kernel is the producer) */
    rc = vdtu_ring_init(&send_ring, send_mem,
                        VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);
    if (rc != 0) {
        printf("[SemperKernel] ERROR: ring_init for send failed\n");
        return -1;
    }

    /* Initialize the recv ring (VPE0 will produce replies here).
     * Kernel inits both rings since it starts first (higher priority).
     * VPE0 will attach to these after receiving our signal. */
    rc = vdtu_ring_init(&recv_ring, recv_mem,
                        VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);
    if (rc != 0) {
        printf("[SemperKernel] ERROR: ring_init for recv failed\n");
        return -1;
    }

    printf("[SemperKernel] Channels configured successfully\n");
    return 0;
}

/*
 * Run the basic message exchange test.
 */
static void run_test(void)
{
    const char *test_msg = "HELLO_VPE";
    int rc;

    /* Send test message */
    printf("[SemperKernel] Sending test message: \"%s\"\n", test_msg);
    rc = vdtu_ring_send(&send_ring,
                        MY_PE, SEND_EP,    /* sender PE, EP */
                        0,                 /* sender VPE */
                        RECV_EP,           /* reply EP */
                        0xDEADBEEF,        /* label */
                        (uint64_t)(uintptr_t)recv_channel,  /* replylabel: encode reply channel */
                        0,                 /* flags */
                        test_msg,
                        (uint16_t)strlen(test_msg));
    if (rc != 0) {
        printf("[SemperKernel] ERROR: send failed: %d\n", rc);
        return;
    }

    /* Signal VPE0 that a message is available.
     * This uses a direct kernel -> VPE0 notification (data path). */
    signal_vpe0_emit();

    /* Wait for reply notification from VPE0.
     * This blocks until VPE0 signals us after writing the reply.
     * Blocking drops our priority, allowing VPE0 (priority 150) to run. */
    printf("[SemperKernel] Waiting for reply...\n");
    signal_from_vpe0_wait();

    /* VPE0 has signaled -- reply should be in the recv ring */
    const struct vdtu_message *reply = vdtu_ring_fetch(&recv_ring);
    if (!reply) {
        printf("[SemperKernel] ERROR: notification received but no message in ring\n");
        return;
    }

    /* Read reply */
    char buf[256];
    int len = reply->hdr.length;
    if (len > (int)sizeof(buf) - 1)
        len = (int)sizeof(buf) - 1;
    memcpy(buf, reply->data, len);
    buf[len] = '\0';

    printf("[SemperKernel] Received reply: \"%s\"\n", buf);
    vdtu_ring_ack(&recv_ring);

    if (strcmp(buf, "ACK") == 0) {
        printf("[SemperKernel] Basic DTU channel test PASSED\n");
    } else {
        printf("[SemperKernel] Basic DTU channel test FAILED "
               "(unexpected reply: \"%s\")\n", buf);
    }
}

/*
 * CAmkES component entry point.
 */
int run(void)
{
    printf("[SemperKernel] Starting (PE %d)\n", MY_PE);

    if (setup_channels() != 0) {
        printf("[SemperKernel] Channel setup failed, aborting\n");
        return 1;
    }

    run_test();

    printf("[SemperKernel] Done\n");
    return 0;
}
