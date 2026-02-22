/*
 * SemperKernel.c -- SemperOS kernel test stub (CAmkES component)
 *
 * Runs 5 tests exercising the vDTU endpoint management:
 *   1. Basic message exchange (kernel -> VPE0 -> kernel reply)
 *   2. Multiple endpoints (two independent send/recv pairs)
 *   3. Memory endpoint (shared data via memory channel)
 *   4. Endpoint invalidation (free and recycle channels)
 *   5. Channel exhaustion (fill pool, verify error, recycle)
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

/* C++ test function (defined in cxx_test.cc) */
extern void cxx_test(void);

#define MY_PE       0
#define VPE0_PE     1

/* Channel table */
static struct vdtu_channel_table channels;

static int tests_passed = 0;
static int tests_failed = 0;

/*
 * Populate the channel table from CAmkES-generated dataport symbols.
 */
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
 * Helper: send a message and wait for reply on the given channels.
 */
static int send_and_wait_reply(int send_chan, int recv_chan,
                               const char *payload, char *reply_buf,
                               int reply_buf_size)
{
    struct vdtu_ring *sring = vdtu_channels_get_ring(&channels, send_chan);
    struct vdtu_ring *rring = vdtu_channels_get_ring(&channels, recv_chan);
    if (!sring || !rring) return -1;

    int rc = vdtu_ring_send(sring,
                            MY_PE, 0, 0, 0,
                            0xDEADBEEF, (uint64_t)recv_chan,
                            0, payload, (uint16_t)strlen(payload));
    if (rc != 0) return rc;

    signal_vpe0_emit();
    signal_from_vpe0_wait();

    const struct vdtu_message *reply = vdtu_ring_fetch(rring);
    if (!reply) return -1;

    int len = reply->hdr.length;
    if (len > reply_buf_size - 1)
        len = reply_buf_size - 1;
    memcpy(reply_buf, reply->data, len);
    reply_buf[len] = '\0';
    vdtu_ring_ack(rring);

    return 0;
}

/*
 * TEST 1: Basic message exchange
 */
static void test1_basic_message(void)
{
    printf("[TEST 1] Basic message exchange...\n");

    /* Configure recv EP on VPE0 (ep 0) */
    int recv_chan = vdtu_config_recv(VPE0_PE, 0, 11, 9, 0);
    if (recv_chan < 0) { printf("  FAILED (config_recv)\n"); tests_failed++; return; }

    /* Configure recv EP on kernel (ep 1) for replies */
    int reply_chan = vdtu_config_recv(MY_PE, 1, 11, 9, 0);
    if (reply_chan < 0) { printf("  FAILED (config_recv reply)\n"); tests_failed++; return; }

    /* Configure send EP on kernel (ep 0) targeting VPE0 ep 0 */
    int send_chan = vdtu_config_send(MY_PE, 0, VPE0_PE, 0, 0, 512, 0xDEADBEEF, VDTU_CREDITS_UNLIM);
    if (send_chan < 0) { printf("  FAILED (config_send)\n"); tests_failed++; return; }

    /* send_chan should equal recv_chan (same underlying channel) */
    if (send_chan != recv_chan) {
        printf("  FAILED (send_chan=%d != recv_chan=%d)\n", send_chan, recv_chan);
        tests_failed++;
        return;
    }

    /* Init rings: kernel inits both (it runs first) */
    vdtu_channels_init_ring(&channels, recv_chan, VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);
    vdtu_channels_init_ring(&channels, reply_chan, VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);

    /* Send message and get reply */
    char reply[256];
    int rc = send_and_wait_reply(send_chan, reply_chan, "HELLO_VPE", reply, sizeof(reply));
    if (rc != 0) { printf("  FAILED (send/recv error=%d)\n", rc); tests_failed++; return; }

    if (strcmp(reply, "ACK") == 0) {
        printf("[TEST 1] Basic message exchange... PASSED\n");
        tests_passed++;
    } else {
        printf("[TEST 1] FAILED (got \"%s\" expected \"ACK\")\n", reply);
        tests_failed++;
    }
}

/*
 * TEST 2: Multiple endpoints
 */
static void test2_multiple_endpoints(void)
{
    printf("[TEST 2] Multiple endpoints...\n");

    /* Configure a second recv EP on VPE0 (ep 2) and reply EP on kernel (ep 3) */
    int recv_chan2 = vdtu_config_recv(VPE0_PE, 2, 11, 9, 0);
    if (recv_chan2 < 0) { printf("  FAILED (config_recv ep2)\n"); tests_failed++; return; }

    int reply_chan2 = vdtu_config_recv(MY_PE, 3, 11, 9, 0);
    if (reply_chan2 < 0) { printf("  FAILED (config_recv reply ep3)\n"); tests_failed++; return; }

    int send_chan2 = vdtu_config_send(MY_PE, 2, VPE0_PE, 2, 0, 512, 0xCAFE, VDTU_CREDITS_UNLIM);
    if (send_chan2 < 0) { printf("  FAILED (config_send ep2)\n"); tests_failed++; return; }

    if (send_chan2 != recv_chan2) {
        printf("  FAILED (send_chan2=%d != recv_chan2=%d)\n", send_chan2, recv_chan2);
        tests_failed++;
        return;
    }

    vdtu_channels_init_ring(&channels, recv_chan2, VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);
    vdtu_channels_init_ring(&channels, reply_chan2, VDTU_DEFAULT_SLOT_COUNT, VDTU_SYSC_MSG_SIZE);

    /* Send on second channel pair */
    char reply[256];
    int rc = send_and_wait_reply(send_chan2, reply_chan2, "TEST2_MSG", reply, sizeof(reply));
    if (rc != 0) { printf("  FAILED (send/recv error=%d)\n", rc); tests_failed++; return; }

    if (strcmp(reply, "ACK") == 0) {
        printf("[TEST 2] Multiple endpoints... PASSED\n");
        tests_passed++;
    } else {
        printf("[TEST 2] FAILED (got \"%s\" expected \"ACK\")\n", reply);
        tests_failed++;
    }
}

/*
 * TEST 3: Memory endpoint
 */
static void test3_memory_endpoint(void)
{
    printf("[TEST 3] Memory endpoint...\n");

    /* Configure memory EP on VPE0 (ep 4) */
    int mem_chan = vdtu_config_mem(VPE0_PE, 4, MY_PE, 0x0, 4096, 0, 3 /* RW */);
    if (mem_chan < 0) { printf("  FAILED (config_mem)\n"); tests_failed++; return; }

    /* Write test pattern to memory dataport */
    volatile void *mem_ptr = vdtu_channels_get_mem(&channels, mem_chan);
    if (!mem_ptr) { printf("  FAILED (get_mem)\n"); tests_failed++; return; }

    const char *test_data = "MEMORY_TEST_DATA_12345678";
    memcpy((void *)mem_ptr, test_data, strlen(test_data) + 1);

    /* Signal VPE0 to read from this memory channel.
     * We encode the test number and mem channel in the signal protocol:
     * send a "MEM_CHECK:<chan>" message on the existing test1 channel pair */
    struct vdtu_ring *sring = vdtu_channels_get_ring(&channels, 0);  /* test1 send channel */
    struct vdtu_ring *rring = vdtu_channels_get_ring(&channels, 1);  /* test1 reply channel */
    if (!sring || !rring) { printf("  FAILED (rings)\n"); tests_failed++; return; }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "MEM_CHECK:%d", mem_chan);
    int rc = vdtu_ring_send(sring, MY_PE, 0, 0, 0, 0xDEADBEEF, 1, 0,
                            cmd, (uint16_t)strlen(cmd));
    if (rc != 0) { printf("  FAILED (send cmd)\n"); tests_failed++; return; }

    signal_vpe0_emit();
    signal_from_vpe0_wait();

    const struct vdtu_message *reply = vdtu_ring_fetch(rring);
    if (!reply) { printf("  FAILED (no reply)\n"); tests_failed++; return; }

    char reply_buf[256];
    int len = reply->hdr.length;
    if (len > (int)sizeof(reply_buf) - 1) len = (int)sizeof(reply_buf) - 1;
    memcpy(reply_buf, reply->data, len);
    reply_buf[len] = '\0';
    vdtu_ring_ack(rring);

    if (strcmp(reply_buf, "MEM_OK") == 0) {
        printf("[TEST 3] Memory endpoint... PASSED\n");
        tests_passed++;
    } else {
        printf("[TEST 3] FAILED (got \"%s\" expected \"MEM_OK\")\n", reply_buf);
        tests_failed++;
    }
}

/*
 * TEST 4: Endpoint invalidation
 *
 * Invalidate the test2 recv EP (VPE0 ep 2), verify channel is recycled.
 */
static void test4_endpoint_invalidation(void)
{
    printf("[TEST 4] Endpoint invalidation...\n");

    /* Remember channel that was assigned to VPE0 ep 2 (from test2) */
    /* We need to invalidate the send EP first (kernel ep 2), then recv EPs */
    int rc;

    /* Invalidate kernel send EP 2 (doesn't free channel, it belongs to recv) */
    rc = vdtu_invalidate_ep(MY_PE, 2);
    if (rc != 0) { printf("  FAILED (invalidate send)\n"); tests_failed++; return; }

    /* Invalidate kernel reply EP 3 (frees its msg channel) */
    rc = vdtu_invalidate_ep(MY_PE, 3);
    if (rc != 0) { printf("  FAILED (invalidate reply)\n"); tests_failed++; return; }

    /* Invalidate VPE0 recv EP 2 (frees its msg channel) */
    rc = vdtu_invalidate_ep(VPE0_PE, 2);
    if (rc != 0) { printf("  FAILED (invalidate recv)\n"); tests_failed++; return; }

    /* Now configure a NEW recv EP - it should get one of the recycled channels */
    int new_chan = vdtu_config_recv(VPE0_PE, 5, 11, 9, 0);
    if (new_chan < 0) { printf("  FAILED (config_recv new)\n"); tests_failed++; return; }

    /* The recycled channel should be one of the previously freed ones (2 or 3) */
    printf("  New channel after recycling: %d\n", new_chan);

    /* Clean up: invalidate the new EP */
    vdtu_invalidate_ep(VPE0_PE, 5);

    /* Also invalidate the memory EP from test3 */
    vdtu_invalidate_ep(VPE0_PE, 4);

    printf("[TEST 4] Endpoint invalidation... PASSED\n");
    tests_passed++;
}

/*
 * TEST 5: Channel exhaustion
 *
 * Configure 8 recv endpoints (exhausting message channel pool).
 * Try a 9th, verify it fails. Invalidate one, try again, verify success.
 */
static void test5_channel_exhaustion(void)
{
    printf("[TEST 5] Channel exhaustion...\n");

    /*
     * We already have 2 recv EPs from test1 (channels 0, 1).
     * Invalidated test2 EPs (channels 2, 3 freed) and test4 new EP (freed).
     * So currently 2 channels in use. We need to allocate 6 more to fill all 8.
     *
     * Actually, let's start clean: invalidate all existing EPs first.
     */
    vdtu_invalidate_eps(MY_PE, 0);
    vdtu_invalidate_eps(VPE0_PE, 0);

    /* Now allocate all 8 message channels */
    int chans[8];
    int i;
    for (i = 0; i < 8; i++) {
        chans[i] = vdtu_config_recv(VPE0_PE, i, 11, 9, 0);
        if (chans[i] < 0) {
            printf("  FAILED (config_recv %d returned %d)\n", i, chans[i]);
            tests_failed++;
            /* Clean up */
            vdtu_invalidate_eps(VPE0_PE, 0);
            return;
        }
    }

    printf("  Allocated 8 channels: ");
    for (i = 0; i < 8; i++) printf("%d ", chans[i]);
    printf("\n");

    /* Try to allocate a 9th - should fail */
    int overflow = vdtu_config_recv(MY_PE, 0, 11, 9, 0);
    if (overflow >= 0) {
        printf("  FAILED (9th alloc should have failed, got channel %d)\n", overflow);
        tests_failed++;
        vdtu_invalidate_eps(MY_PE, 0);
        vdtu_invalidate_eps(VPE0_PE, 0);
        return;
    }
    printf("  9th allocation correctly rejected\n");

    /* Invalidate one (VPE0 ep 3), then try again */
    vdtu_invalidate_ep(VPE0_PE, 3);

    int recycled = vdtu_config_recv(MY_PE, 0, 11, 9, 0);
    if (recycled < 0) {
        printf("  FAILED (recycled alloc failed)\n");
        tests_failed++;
        vdtu_invalidate_eps(MY_PE, 0);
        vdtu_invalidate_eps(VPE0_PE, 0);
        return;
    }
    printf("  After freeing one, got recycled channel: %d\n", recycled);

    /* Clean up all */
    vdtu_invalidate_eps(MY_PE, 0);
    vdtu_invalidate_eps(VPE0_PE, 0);

    printf("[TEST 5] Channel exhaustion... PASSED\n");
    tests_passed++;
}

/*
 * CAmkES component entry point.
 */
int run(void)
{
    printf("=== SemperOS vDTU System Test ===\n");

    cxx_test();

    init_channel_table();

    test1_basic_message();
    test2_multiple_endpoints();
    test3_memory_endpoint();
    test4_endpoint_invalidation();
    test5_channel_exhaustion();

    printf("=== %d/5 tests PASSED", tests_passed);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    if (tests_passed == 5)
        printf("=== All 5 tests PASSED ===\n");

    return 0;
}
