/*
 * test_vdtu_config.c -- Unit tests for vDTU channel routing and config_recv_at
 *
 * Tests VPE/Kernel/vDTU communication:
 *   Group A: VDTUService endpoint table & routing logic (10 tests)
 *   Group B: Channel ring communication via PE pools (3 tests)
 *   Group C: End-to-end SYSC_GATE[1] simulation (1 test)
 *
 * Compile: gcc -Wall -Wextra -I../components/include -o test_vdtu_config \
 *          test_vdtu_config.c ../src/vdtu_ring.c ../src/vdtu_channels.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  TEST: %-55s ", name); } while(0)

#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* =========================================================================
 * Group A: VDTUService Config Routing Logic
 *
 * Extracted from VDTUService.c — same data structures and allocation
 * functions, plus the new config_recv_at. Tests verify routing correctness
 * without needing CAmkES.
 * ========================================================================= */

#define MAX_PES             8
#define EP_PER_PE           VDTU_EP_COUNT   /* 32 */
#define PE_KERNEL           0
#define PE_VPE0             2
#define PE_VPE1             3
#define CHANNELS_PER_PE     16
#define TOTAL_CHANNELS      32

enum ep_type {
    EP_INVALID = 0,
    EP_SEND    = 1,
    EP_RECEIVE = 2,
    EP_MEMORY  = 3,
};

struct ep_desc {
    enum ep_type type;
    int channel_idx;
    int dest_pe;
    int dest_ep;
    int dest_vpe;
    int msg_size;
    uint64_t label;
    int credits;
    int buf_order;
    int msg_order;
    int slot_count;
    int slot_size;
    int flags;
    uint64_t mem_addr;
    uint64_t mem_size;
    int mem_perm;
};

struct channel_pool {
    int in_use[TOTAL_CHANNELS];
    int assigned_pe[TOTAL_CHANNELS];
    int assigned_ep[TOTAL_CHANNELS];
};

static struct ep_desc g_endpoints[MAX_PES][EP_PER_PE];
static struct channel_pool g_pool;

static void reset_config_state(void)
{
    memset(g_endpoints, 0, sizeof(g_endpoints));
    memset(&g_pool, 0, sizeof(g_pool));
    for (int pe = 0; pe < MAX_PES; pe++)
        for (int ep = 0; ep < EP_PER_PE; ep++)
            g_endpoints[pe][ep].channel_idx = -1;
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        g_pool.assigned_pe[i] = -1;
        g_pool.assigned_ep[i] = -1;
    }
}

static int pe_channel_base(int pe)
{
    return (pe == PE_VPE1) ? CHANNELS_PER_PE : 0;
}

static int t_alloc_channel(int pe, int ep)
{
    int base = pe_channel_base(pe);
    int end = base + CHANNELS_PER_PE;
    for (int i = base; i < end; i++) {
        if (!g_pool.in_use[i]) {
            g_pool.in_use[i] = 1;
            g_pool.assigned_pe[i] = pe;
            g_pool.assigned_ep[i] = ep;
            return i;
        }
    }
    return -1;
}

static void t_free_channel(int ch)
{
    if (ch >= 0 && ch < TOTAL_CHANNELS) {
        g_pool.in_use[ch] = 0;
        g_pool.assigned_pe[ch] = -1;
        g_pool.assigned_ep[ch] = -1;
    }
}

static int t_config_recv(int target_pe, int ep_id,
                         int buf_order, int msg_order, int flags)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE)
        return -1;

    struct ep_desc *ep = &g_endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID)
        return -1;

    int ch = t_alloc_channel(target_pe, ep_id);
    if (ch < 0)
        return -1;

    uint32_t slot_size  = 1u << msg_order;
    uint32_t slot_count = 1u << (buf_order - msg_order);
    size_t needed = vdtu_ring_total_size(slot_count, slot_size);
    if (needed > 4096) {
        slot_count = (4096 - VDTU_RING_CTRL_SIZE) / slot_size;
        uint32_t p = 1;
        while (p * 2 <= slot_count) p *= 2;
        slot_count = p;
        if (slot_count < 2) slot_count = 2;
    }

    ep->type        = EP_RECEIVE;
    ep->channel_idx = ch;
    ep->buf_order   = buf_order;
    ep->msg_order   = msg_order;
    ep->slot_count  = (int)slot_count;
    ep->slot_size   = (int)slot_size;
    ep->flags       = flags;

    return ch;
}

/* config_recv_at: caller specifies the channel explicitly */
static int t_config_recv_at(int target_pe, int ep_id, int channel,
                            int buf_order, int msg_order, int flags)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE)
        return -1;

    if (channel < 0 || channel >= TOTAL_CHANNELS)
        return -1;

    if (g_pool.in_use[channel])
        return -1;

    struct ep_desc *ep = &g_endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID)
        return -1;

    g_pool.in_use[channel] = 1;
    g_pool.assigned_pe[channel] = target_pe;
    g_pool.assigned_ep[channel] = ep_id;

    uint32_t slot_size  = 1u << msg_order;
    uint32_t slot_count = 1u << (buf_order - msg_order);
    size_t needed = vdtu_ring_total_size(slot_count, slot_size);
    if (needed > 4096) {
        slot_count = (4096 - VDTU_RING_CTRL_SIZE) / slot_size;
        uint32_t p = 1;
        while (p * 2 <= slot_count) p *= 2;
        slot_count = p;
        if (slot_count < 2) slot_count = 2;
    }

    ep->type        = EP_RECEIVE;
    ep->channel_idx = channel;
    ep->buf_order   = buf_order;
    ep->msg_order   = msg_order;
    ep->slot_count  = (int)slot_count;
    ep->slot_size   = (int)slot_size;
    ep->flags       = flags;

    return channel;
}

static int t_config_send(int target_pe, int ep_id,
                         int dest_pe, int dest_ep, int dest_vpe,
                         int msg_size, uint64_t label, int credits)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE)
        return -1;

    if (dest_pe < 0 || dest_pe >= MAX_PES ||
        dest_ep < 0 || dest_ep >= EP_PER_PE)
        return -1;

    struct ep_desc *dest = &g_endpoints[dest_pe][dest_ep];
    if (dest->type != EP_RECEIVE)
        return -1;

    struct ep_desc *ep = &g_endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID) {
        ep->type = EP_INVALID;
        ep->channel_idx = -1;
    }

    ep->type        = EP_SEND;
    ep->channel_idx = dest->channel_idx;
    ep->dest_pe     = dest_pe;
    ep->dest_ep     = dest_ep;
    ep->dest_vpe    = dest_vpe;
    ep->msg_size    = msg_size;
    ep->label       = label;
    ep->credits     = credits;

    return ep->channel_idx;
}

static int t_invalidate_ep(int target_pe, int ep_id)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE)
        return -1;

    struct ep_desc *ep = &g_endpoints[target_pe][ep_id];
    if (ep->type == EP_RECEIVE || ep->type == EP_MEMORY)
        t_free_channel(ep->channel_idx);

    memset(ep, 0, sizeof(*ep));
    ep->channel_idx = -1;
    return 0;
}

/* ---- Group A Tests ---- */

static void test_alloc_channel_pe0(void)
{
    TEST("alloc_channel assigns from PE0 pool (0-15)");
    reset_config_state();

    int ch = t_alloc_channel(PE_KERNEL, 0);
    CHECK(ch >= 0 && ch < CHANNELS_PER_PE,
          "kernel channel should be in 0-15");

    int ch2 = t_alloc_channel(PE_VPE0, 1);
    CHECK(ch2 >= 0 && ch2 < CHANNELS_PER_PE,
          "VPE0 channel should be in 0-15");
    CHECK(ch2 != ch, "should allocate different channels");

    PASS();
}

static void test_alloc_channel_pe1(void)
{
    TEST("alloc_channel assigns from VPE1 pool (16-31)");
    reset_config_state();

    int ch = t_alloc_channel(PE_VPE1, 0);
    CHECK(ch >= CHANNELS_PER_PE && ch < TOTAL_CHANNELS,
          "VPE1 channel should be in 16-31");

    PASS();
}

static void test_config_recv_auto(void)
{
    TEST("config_recv auto-allocates channel");
    reset_config_state();

    int ch = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch >= 0, "config_recv should succeed");
    CHECK(ch < CHANNELS_PER_PE, "kernel recv should use PE0 pool");
    CHECK(g_endpoints[PE_KERNEL][0].type == EP_RECEIVE,
          "EP should be RECEIVE type");
    CHECK(g_endpoints[PE_KERNEL][0].channel_idx == ch,
          "EP should record assigned channel");
    CHECK(g_pool.in_use[ch] == 1, "channel should be marked in-use");

    PASS();
}

static void test_config_recv_at_explicit(void)
{
    TEST("config_recv_at assigns specified channel");
    reset_config_state();

    int ch = t_config_recv_at(PE_KERNEL, 1, 16, 14, 9, 0);
    CHECK(ch == 16, "should return the requested channel 16");
    CHECK(g_endpoints[PE_KERNEL][1].type == EP_RECEIVE,
          "EP should be RECEIVE type");
    CHECK(g_endpoints[PE_KERNEL][1].channel_idx == 16,
          "EP should record channel 16");
    CHECK(g_pool.in_use[16] == 1, "channel 16 should be marked in-use");

    PASS();
}

static void test_config_send_routes_to_recv(void)
{
    TEST("config_send returns recv EP's channel (auto-alloc)");
    reset_config_state();

    /* Kernel configures recv EP 0 */
    int recv_ch = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(recv_ch >= 0, "recv should succeed");

    /* VPE0 configures send EP targeting kernel EP 0 */
    int send_ch = t_config_send(PE_VPE0, 0, PE_KERNEL, 0, 0, 512, 0, 1);
    CHECK(send_ch == recv_ch,
          "send channel should match recv EP's channel");

    PASS();
}

static void test_config_send_routes_to_recv_at(void)
{
    TEST("config_send returns recv EP's channel (config_recv_at)");
    reset_config_state();

    /* Kernel configures recv EP 5 at explicit channel 7 */
    int recv_ch = t_config_recv_at(PE_KERNEL, 5, 7, 14, 9, 0);
    CHECK(recv_ch == 7, "recv_at should return channel 7");

    /* VPE0 configures send EP targeting kernel EP 5 */
    int send_ch = t_config_send(PE_VPE0, 0, PE_KERNEL, 5, 0, 512, 0, 1);
    CHECK(send_ch == 7,
          "send channel should be 7 (from recv_at)");

    PASS();
}

static void test_cross_pool_sysc_gate(void)
{
    TEST("cross-pool SYSC_GATE[1]: kernel EP 1 on VPE1 ch 16");
    reset_config_state();

    /* Kernel SYSC_GATE[0]: normal auto-alloc for VPE0 */
    int ch0 = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch0 >= 0 && ch0 < CHANNELS_PER_PE,
          "SYSC_GATE[0] should use PE0 pool");

    /* Kernel SYSC_GATE[1]: config_recv_at on VPE1's channel 16 */
    int ch1 = t_config_recv_at(PE_KERNEL, 1, CHANNELS_PER_PE, 14, 9, 0);
    CHECK(ch1 == (int)CHANNELS_PER_PE,
          "SYSC_GATE[1] should use channel 16");

    /* VPE1 configures send EP targeting kernel EP 1 */
    int send_ch = t_config_send(PE_VPE1, 0, PE_KERNEL, 1, 1, 512, 0, 1);
    CHECK(send_ch == (int)CHANNELS_PER_PE,
          "VPE1 send should route to channel 16 (not PE0 pool)");

    /* VPE0 configures send EP targeting kernel EP 0 */
    int send_ch0 = t_config_send(PE_VPE0, 0, PE_KERNEL, 0, 0, 512, 0, 1);
    CHECK(send_ch0 == ch0,
          "VPE0 send should route to SYSC_GATE[0]'s channel");
    CHECK(send_ch0 != send_ch,
          "VPE0 and VPE1 should use different channels");

    PASS();
}

static void test_invalidate_frees_channel(void)
{
    TEST("invalidate_ep frees recv/mem channels");
    reset_config_state();

    /* Configure recv EP */
    int ch = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch >= 0, "recv should succeed");
    CHECK(g_pool.in_use[ch] == 1, "channel should be in-use");

    /* Invalidate it */
    t_invalidate_ep(PE_KERNEL, 0);
    CHECK(g_pool.in_use[ch] == 0, "channel should be freed");
    CHECK(g_endpoints[PE_KERNEL][0].type == EP_INVALID,
          "EP should be invalid after invalidation");

    /* Can re-allocate the same channel */
    int ch2 = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch2 == ch, "should re-use freed channel");

    PASS();
}

static void test_error_channel_in_use(void)
{
    TEST("config_recv_at rejects already-used channel");
    reset_config_state();

    /* Allocate channel 5 via auto */
    int ch = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch == 0, "first auto-alloc should get channel 0");

    /* Try to config_recv_at on channel 0 (in-use) */
    int ch2 = t_config_recv_at(PE_KERNEL, 1, 0, 14, 9, 0);
    CHECK(ch2 == -1, "should reject in-use channel");

    PASS();
}

static void test_error_pool_exhausted(void)
{
    TEST("config_recv returns -1 when pool exhausted");
    reset_config_state();

    /* Fill all 16 channels in PE0 pool */
    for (int i = 0; i < CHANNELS_PER_PE; i++) {
        int ch = t_config_recv(PE_KERNEL, i, 14, 9, 0);
        CHECK(ch >= 0, "should succeed until pool full");
    }

    /* Next allocation should fail */
    int ch = t_config_recv(PE_KERNEL, CHANNELS_PER_PE, 14, 9, 0);
    CHECK(ch == -1, "should fail when pool exhausted");

    /* VPE1 pool should still work */
    int ch_vpe1 = t_config_recv(PE_VPE1, 0, 14, 9, 0);
    CHECK(ch_vpe1 >= CHANNELS_PER_PE,
          "VPE1 pool should be unaffected");

    PASS();
}

static void test_error_send_no_recv(void)
{
    TEST("config_send fails when dest recv EP missing");
    reset_config_state();

    /* No recv EP configured — config_send should fail */
    int ch = t_config_send(PE_VPE0, 0, PE_KERNEL, 0, 0, 512, 0, 1);
    CHECK(ch == -1, "should fail when dest recv EP not configured");

    PASS();
}

/* =========================================================================
 * Group B: Channel Ring Communication
 *
 * Uses vdtu_channels + vdtu_ring APIs with heap-allocated mock dataports
 * simulating CAmkES shared memory pages.
 * ========================================================================= */

#define DATAPORT_SIZE   4096
#define RING_SLOTS      4
#define RING_SLOT_SIZE  512

/* Allocate mock dataports for a full kernel channel table (32 channels) */
static void *mock_pages[TOTAL_CHANNELS];

static void alloc_mock_pages(void)
{
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        mock_pages[i] = calloc(1, DATAPORT_SIZE);
        assert(mock_pages[i] != NULL);
    }
}

static void free_mock_pages(void)
{
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        free(mock_pages[i]);
        mock_pages[i] = NULL;
    }
}

static void test_ring_pe0_path(void)
{
    TEST("ring PE0 path: init ch 0, attach, send/recv");
    alloc_mock_pages();

    /* Kernel side: init channel table and ring on ch 0 */
    struct vdtu_channel_table kern_ct;
    volatile void *kern_dp[TOTAL_CHANNELS];
    for (int i = 0; i < TOTAL_CHANNELS; i++)
        kern_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&kern_ct, kern_dp, TOTAL_CHANNELS);
    int rc = vdtu_channels_init_ring(&kern_ct, 0, RING_SLOTS, RING_SLOT_SIZE);
    CHECK(rc == 0, "kernel ring init should succeed");

    /* VPE0 side: init channel table with same pages, attach to ch 0 */
    struct vdtu_channel_table vpe0_ct;
    volatile void *vpe0_dp[VDTU_CHANNELS_PER_PE];
    for (int i = 0; i < VDTU_CHANNELS_PER_PE; i++)
        vpe0_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&vpe0_ct, vpe0_dp, VDTU_CHANNELS_PER_PE);
    rc = vdtu_channels_attach_ring(&vpe0_ct, 0);
    CHECK(rc == 0, "VPE0 ring attach should succeed");

    /* VPE0 sends a syscall */
    struct vdtu_ring *send_ring = vdtu_channels_get_ring(&vpe0_ct, 0);
    CHECK(send_ring != NULL, "VPE0 send ring should exist");
    uint64_t payload = 18;  /* NOOP opcode */
    rc = vdtu_ring_send(send_ring, 2, 0, 0, 1, 0, 0, 0,
                        &payload, sizeof(payload));
    CHECK(rc == 0, "VPE0 send should succeed");

    /* Kernel fetches */
    struct vdtu_ring *recv_ring = vdtu_channels_get_ring(&kern_ct, 0);
    CHECK(recv_ring != NULL, "kernel recv ring should exist");
    const struct vdtu_message *msg = vdtu_ring_fetch(recv_ring);
    CHECK(msg != NULL, "kernel should receive message");
    CHECK(msg->hdr.sender_core_id == 2, "sender PE should be 2 (VPE0)");
    CHECK(msg->hdr.length == sizeof(uint64_t), "payload length mismatch");

    uint64_t opcode;
    memcpy(&opcode, msg->data, sizeof(opcode));
    CHECK(opcode == 18, "opcode should be NOOP (18)");
    vdtu_ring_ack(recv_ring);

    free_mock_pages();
    PASS();
}

static void test_ring_pe1_cross_pool(void)
{
    TEST("ring PE1 cross-pool: init ch 16, attach, send/recv");
    alloc_mock_pages();

    /* Kernel side: full 32-channel table, init ring on ch 16 */
    struct vdtu_channel_table kern_ct;
    volatile void *kern_dp[TOTAL_CHANNELS];
    for (int i = 0; i < TOTAL_CHANNELS; i++)
        kern_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&kern_ct, kern_dp, TOTAL_CHANNELS);
    int rc = vdtu_channels_init_ring(&kern_ct, 16, RING_SLOTS, RING_SLOT_SIZE);
    CHECK(rc == 0, "kernel ring init ch 16 should succeed");

    /* VPE1 side: 16-channel table, ch 0 maps to mock_pages[16] (same physical page) */
    struct vdtu_channel_table vpe1_ct;
    volatile void *vpe1_dp[VDTU_CHANNELS_PER_PE];
    for (int i = 0; i < VDTU_CHANNELS_PER_PE; i++)
        vpe1_dp[i] = (volatile void *)mock_pages[VDTU_CHANNELS_PER_PE + i];
    vdtu_channels_init(&vpe1_ct, vpe1_dp, VDTU_CHANNELS_PER_PE);
    rc = vdtu_channels_attach_ring(&vpe1_ct, 0);
    CHECK(rc == 0, "VPE1 ring attach ch 0 should succeed");

    /* VPE1 sends */
    struct vdtu_ring *send_ring = vdtu_channels_get_ring(&vpe1_ct, 0);
    CHECK(send_ring != NULL, "VPE1 send ring should exist");
    uint64_t payload = 18;
    rc = vdtu_ring_send(send_ring, 3, 0, 1, 1, 0, 0, 0,
                        &payload, sizeof(payload));
    CHECK(rc == 0, "VPE1 send should succeed");

    /* Kernel fetches from ch 16 */
    struct vdtu_ring *recv_ring = vdtu_channels_get_ring(&kern_ct, 16);
    CHECK(recv_ring != NULL, "kernel recv ring ch 16 should exist");
    const struct vdtu_message *msg = vdtu_ring_fetch(recv_ring);
    CHECK(msg != NULL, "kernel should receive VPE1 message on ch 16");
    CHECK(msg->hdr.sender_core_id == 3, "sender PE should be 3 (VPE1)");

    uint64_t opcode;
    memcpy(&opcode, msg->data, sizeof(opcode));
    CHECK(opcode == 18, "opcode should be NOOP (18)");
    vdtu_ring_ack(recv_ring);

    free_mock_pages();
    PASS();
}

static void test_ring_both_independent(void)
{
    TEST("ring both PE pools: no crosstalk");
    alloc_mock_pages();

    /* Kernel: 32 channels, init rings on ch 0 (VPE0) and ch 16 (VPE1) */
    struct vdtu_channel_table kern_ct;
    volatile void *kern_dp[TOTAL_CHANNELS];
    for (int i = 0; i < TOTAL_CHANNELS; i++)
        kern_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&kern_ct, kern_dp, TOTAL_CHANNELS);
    vdtu_channels_init_ring(&kern_ct, 0, RING_SLOTS, RING_SLOT_SIZE);
    vdtu_channels_init_ring(&kern_ct, 16, RING_SLOTS, RING_SLOT_SIZE);

    /* VPE0: ch 0 */
    struct vdtu_channel_table vpe0_ct;
    volatile void *vpe0_dp[VDTU_CHANNELS_PER_PE];
    for (int i = 0; i < VDTU_CHANNELS_PER_PE; i++)
        vpe0_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&vpe0_ct, vpe0_dp, VDTU_CHANNELS_PER_PE);
    vdtu_channels_attach_ring(&vpe0_ct, 0);

    /* VPE1: ch 0 = mock_pages[16] */
    struct vdtu_channel_table vpe1_ct;
    volatile void *vpe1_dp[VDTU_CHANNELS_PER_PE];
    for (int i = 0; i < VDTU_CHANNELS_PER_PE; i++)
        vpe1_dp[i] = (volatile void *)mock_pages[VDTU_CHANNELS_PER_PE + i];
    vdtu_channels_init(&vpe1_ct, vpe1_dp, VDTU_CHANNELS_PER_PE);
    vdtu_channels_attach_ring(&vpe1_ct, 0);

    /* VPE0 sends "ALPHA" */
    struct vdtu_ring *r0 = vdtu_channels_get_ring(&vpe0_ct, 0);
    vdtu_ring_send(r0, 2, 0, 0, 1, 0xAAAA, 0, 0, "ALPHA", 5);

    /* VPE1 sends "BRAVO" */
    struct vdtu_ring *r1 = vdtu_channels_get_ring(&vpe1_ct, 0);
    vdtu_ring_send(r1, 3, 0, 1, 1, 0xBBBB, 0, 0, "BRAVO", 5);

    /* Kernel reads ch 0 — should get VPE0's "ALPHA" */
    struct vdtu_ring *kr0 = vdtu_channels_get_ring(&kern_ct, 0);
    const struct vdtu_message *m0 = vdtu_ring_fetch(kr0);
    CHECK(m0 != NULL, "should receive on ch 0");
    CHECK(m0->hdr.sender_core_id == 2, "ch 0 sender should be PE 2");
    CHECK(m0->hdr.label == 0xAAAA, "ch 0 label should be 0xAAAA");
    CHECK(memcmp(m0->data, "ALPHA", 5) == 0, "ch 0 payload should be ALPHA");
    vdtu_ring_ack(kr0);

    /* Kernel reads ch 16 — should get VPE1's "BRAVO" */
    struct vdtu_ring *kr1 = vdtu_channels_get_ring(&kern_ct, 16);
    const struct vdtu_message *m1 = vdtu_ring_fetch(kr1);
    CHECK(m1 != NULL, "should receive on ch 16");
    CHECK(m1->hdr.sender_core_id == 3, "ch 16 sender should be PE 3");
    CHECK(m1->hdr.label == 0xBBBB, "ch 16 label should be 0xBBBB");
    CHECK(memcmp(m1->data, "BRAVO", 5) == 0, "ch 16 payload should be BRAVO");
    vdtu_ring_ack(kr1);

    /* Verify no crosstalk: ch 0 should be empty now */
    CHECK(vdtu_ring_fetch(kr0) == NULL, "ch 0 should be empty after ack");
    CHECK(vdtu_ring_fetch(kr1) == NULL, "ch 16 should be empty after ack");

    free_mock_pages();
    PASS();
}

/* =========================================================================
 * Group C: End-to-end SYSC_GATE[1] Simulation
 *
 * Combines config routing + ring communication to simulate the complete
 * VPE1 → kernel syscall path that FPT-178 fixes.
 * ========================================================================= */

static void test_e2e_vpe1_syscall(void)
{
    TEST("e2e: VPE1 syscall via config_recv_at + ring");
    reset_config_state();
    alloc_mock_pages();

    /* --- Config phase (VDTUService logic) --- */

    /* Kernel SYSC_GATE[0]: auto-alloc for VPE0 */
    int ch0 = t_config_recv(PE_KERNEL, 0, 14, 9, 0);
    CHECK(ch0 >= 0, "SYSC_GATE[0] config_recv should succeed");

    /* Kernel SYSC_GATE[1]: config_recv_at on VPE1 channel 16 */
    int ch1 = t_config_recv_at(PE_KERNEL, 1, CHANNELS_PER_PE, 14, 9, 0);
    CHECK(ch1 == (int)CHANNELS_PER_PE, "SYSC_GATE[1] should be channel 16");

    /* VPE1 config_send targeting kernel EP 1 */
    int vpe1_send_ch = t_config_send(PE_VPE1, 0, PE_KERNEL, 1, 1, 512, 0, 1);
    CHECK(vpe1_send_ch == (int)CHANNELS_PER_PE,
          "VPE1 send should route to channel 16");

    /* --- Ring phase (data plane) --- */

    /* Kernel: init ring on ch 16 */
    struct vdtu_channel_table kern_ct;
    volatile void *kern_dp[TOTAL_CHANNELS];
    for (int i = 0; i < TOTAL_CHANNELS; i++)
        kern_dp[i] = (volatile void *)mock_pages[i];
    vdtu_channels_init(&kern_ct, kern_dp, TOTAL_CHANNELS);
    int rc = vdtu_channels_init_ring(&kern_ct, ch1, RING_SLOTS, RING_SLOT_SIZE);
    CHECK(rc == 0, "kernel ring init ch 16 should succeed");

    /* VPE1: attach to ch 0 (= mock_pages[16], same physical page) */
    struct vdtu_channel_table vpe1_ct;
    volatile void *vpe1_dp[VDTU_CHANNELS_PER_PE];
    for (int i = 0; i < VDTU_CHANNELS_PER_PE; i++)
        vpe1_dp[i] = (volatile void *)mock_pages[VDTU_CHANNELS_PER_PE + i];
    vdtu_channels_init(&vpe1_ct, vpe1_dp, VDTU_CHANNELS_PER_PE);
    rc = vdtu_channels_attach_ring(&vpe1_ct, 0);
    CHECK(rc == 0, "VPE1 ring attach should succeed");

    /* VPE1 sends NOOP syscall (opcode 18) */
    struct vdtu_ring *vpe1_ring = vdtu_channels_get_ring(&vpe1_ct, 0);
    CHECK(vpe1_ring != NULL, "VPE1 ring should exist");
    uint64_t noop_opcode = 18;
    rc = vdtu_ring_send(vpe1_ring, 3, 0, 1, 1, 0, 0, 0,
                        &noop_opcode, sizeof(noop_opcode));
    CHECK(rc == 0, "VPE1 NOOP send should succeed");

    /* Kernel fetches from SYSC_GATE[1] (ch 16) */
    struct vdtu_ring *kern_ring = vdtu_channels_get_ring(&kern_ct, ch1);
    CHECK(kern_ring != NULL, "kernel SYSC_GATE[1] ring should exist");
    const struct vdtu_message *msg = vdtu_ring_fetch(kern_ring);
    CHECK(msg != NULL, "kernel should receive VPE1 syscall");
    CHECK(msg->hdr.sender_core_id == 3, "sender PE should be 3 (VPE1)");
    CHECK(msg->hdr.sender_ep_id == 0, "sender EP should be SYSC_EP (0)");
    CHECK(msg->hdr.sender_vpe_id == 1, "sender VPE should be 1");

    uint64_t opcode;
    memcpy(&opcode, msg->data, sizeof(opcode));
    CHECK(opcode == 18, "opcode should be NOOP (18)");
    vdtu_ring_ack(kern_ring);

    /* Verify SYSC_GATE[0] (ch 0) is unaffected */
    vdtu_channels_init_ring(&kern_ct, ch0, RING_SLOTS, RING_SLOT_SIZE);
    struct vdtu_ring *kr0 = vdtu_channels_get_ring(&kern_ct, ch0);
    CHECK(vdtu_ring_fetch(kr0) == NULL, "SYSC_GATE[0] should be empty");

    free_mock_pages();
    PASS();
}

/* ========================================================================= */

int main(void)
{
    printf("=== vDTU Config & Communication Tests ===\n\n");

    printf("--- Group A: VDTUService Config Routing ---\n");
    test_alloc_channel_pe0();
    test_alloc_channel_pe1();
    test_config_recv_auto();
    test_config_recv_at_explicit();
    test_config_send_routes_to_recv();
    test_config_send_routes_to_recv_at();
    test_cross_pool_sysc_gate();
    test_invalidate_frees_channel();
    test_error_channel_in_use();
    test_error_pool_exhausted();
    test_error_send_no_recv();

    printf("\n--- Group B: Channel Ring Communication ---\n");
    test_ring_pe0_path();
    test_ring_pe1_cross_pool();
    test_ring_both_independent();

    printf("\n--- Group C: End-to-end SYSC_GATE[1] Simulation ---\n");
    test_e2e_vpe1_syscall();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed ? 1 : 0;
}
