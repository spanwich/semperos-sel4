/*
 * test_ring.c -- Standalone test for the SPSC ring buffer
 *
 * Compile: gcc -Wall -Wextra -I../components/include -o test_ring \
 *          test_ring.c ../src/vdtu_ring.c
 *
 * Or just: make (uses the provided Makefile)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "vdtu_ring.h"

#define SLOT_COUNT 4
#define SLOT_SIZE  VDTU_SYSC_MSG_SIZE  /* 512 bytes */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  TEST: %-50s ", name); } while(0)

#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Helper: send a simple text message */
static int send_text(struct vdtu_ring *ring, uint16_t pe, uint8_t ep,
                     uint64_t label, const char *text)
{
    return vdtu_ring_send(ring, pe, ep, 0, 1, label, 0, 0,
                          text, (uint16_t)strlen(text));
}

/* ========================================================================= */

static void test_init(void)
{
    TEST("ring_init with valid params");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;

    int rc = vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);
    CHECK(rc == 0, "init should succeed");
    CHECK(ring.ctrl->slot_count == SLOT_COUNT, "slot_count mismatch");
    CHECK(ring.ctrl->slot_size == SLOT_SIZE, "slot_size mismatch");
    CHECK(ring.ctrl->head == 0, "head should be 0");
    CHECK(ring.ctrl->tail == 0, "tail should be 0");
    CHECK(vdtu_ring_is_empty(&ring), "should be empty after init");
    CHECK(!vdtu_ring_is_full(&ring), "should not be full after init");

    free(mem);
    PASS();
}

static void test_init_bad_params(void)
{
    TEST("ring_init rejects bad params");

    void *mem = calloc(1, 4096);
    struct vdtu_ring ring;

    /* Non-power-of-2 slot count */
    CHECK(vdtu_ring_init(&ring, mem, 3, 512) == -1, "slot_count=3 should fail");
    /* Slot count too small */
    CHECK(vdtu_ring_init(&ring, mem, 1, 512) == -1, "slot_count=1 should fail");
    /* Slot size too small */
    CHECK(vdtu_ring_init(&ring, mem, 4, 8) == -1, "slot_size=8 should fail");
    /* Non-power-of-2 slot size */
    CHECK(vdtu_ring_init(&ring, mem, 4, 300) == -1, "slot_size=300 should fail");
    /* NULL mem */
    CHECK(vdtu_ring_init(&ring, NULL, 4, 512) == -1, "NULL mem should fail");

    free(mem);
    PASS();
}

static void test_send_and_fetch(void)
{
    TEST("send one message, fetch it back");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;
    vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);

    const char *payload = "HELLO_VPE";
    int rc = vdtu_ring_send(&ring, 0, 0, 0, 1,
                            0xDEADBEEF, 0xCAFE, 0,
                            payload, (uint16_t)strlen(payload));
    CHECK(rc == 0, "send should succeed");
    CHECK(!vdtu_ring_is_empty(&ring), "should not be empty after send");

    const struct vdtu_message *msg = vdtu_ring_fetch(&ring);
    CHECK(msg != NULL, "fetch should return message");
    CHECK(msg->hdr.sender_core_id == 0, "sender_core_id mismatch");
    CHECK(msg->hdr.sender_ep_id == 0, "sender_ep_id mismatch");
    CHECK(msg->hdr.reply_ep_id == 1, "reply_ep_id mismatch");
    CHECK(msg->hdr.length == strlen(payload), "length mismatch");
    CHECK(msg->hdr.label == 0xDEADBEEF, "label mismatch");
    CHECK(msg->hdr.replylabel == 0xCAFE, "replylabel mismatch");
    CHECK(memcmp(msg->data, payload, strlen(payload)) == 0, "payload mismatch");

    vdtu_ring_ack(&ring);
    CHECK(vdtu_ring_is_empty(&ring), "should be empty after ack");

    free(mem);
    PASS();
}

static void test_fill_all_slots(void)
{
    TEST("fill all slots, verify full detection");

    /* With SLOT_COUNT=4 and SPSC using head+1==tail for full detection,
     * we can store SLOT_COUNT-1 = 3 messages before full */
    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;
    vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);

    char buf[32];
    int i;
    /* Fill SLOT_COUNT - 1 slots (that's the max with one-slot-empty convention) */
    for (i = 0; i < (int)SLOT_COUNT - 1; i++) {
        snprintf(buf, sizeof(buf), "MSG_%d", i);
        int rc = send_text(&ring, (uint16_t)i, (uint8_t)i, (uint64_t)i, buf);
        CHECK(rc == 0, "send should succeed");
    }

    CHECK(vdtu_ring_is_full(&ring), "should be full after SLOT_COUNT-1 sends");
    CHECK(vdtu_ring_available(&ring) == SLOT_COUNT - 1, "available count wrong");

    /* Trying to send one more should fail */
    int rc = send_text(&ring, 99, 99, 99, "OVERFLOW");
    CHECK(rc == -1, "send to full ring should fail");

    /* Read all back in FIFO order */
    for (i = 0; i < (int)SLOT_COUNT - 1; i++) {
        snprintf(buf, sizeof(buf), "MSG_%d", i);
        const struct vdtu_message *msg = vdtu_ring_fetch(&ring);
        CHECK(msg != NULL, "fetch should succeed");
        CHECK(msg->hdr.sender_core_id == (uint16_t)i, "sender PE mismatch in FIFO");
        CHECK(msg->hdr.label == (uint64_t)i, "label mismatch in FIFO");
        CHECK(msg->hdr.length == strlen(buf), "length mismatch in FIFO");
        CHECK(memcmp(msg->data, buf, strlen(buf)) == 0, "payload mismatch in FIFO");
        vdtu_ring_ack(&ring);
    }

    CHECK(vdtu_ring_is_empty(&ring), "should be empty after reading all");

    free(mem);
    PASS();
}

static void test_wraparound(void)
{
    TEST("wraparound: write 2, read 2, write 3, read 3");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;
    vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);

    /* Write 2 */
    CHECK(send_text(&ring, 1, 0, 100, "WRAP_A") == 0, "send A");
    CHECK(send_text(&ring, 2, 0, 200, "WRAP_B") == 0, "send B");
    CHECK(vdtu_ring_available(&ring) == 2, "should have 2 available");

    /* Read 2 */
    const struct vdtu_message *msg;
    msg = vdtu_ring_fetch(&ring);
    CHECK(msg && msg->hdr.label == 100, "fetch A label");
    CHECK(memcmp(msg->data, "WRAP_A", 6) == 0, "fetch A payload");
    vdtu_ring_ack(&ring);

    msg = vdtu_ring_fetch(&ring);
    CHECK(msg && msg->hdr.label == 200, "fetch B label");
    CHECK(memcmp(msg->data, "WRAP_B", 6) == 0, "fetch B payload");
    vdtu_ring_ack(&ring);

    CHECK(vdtu_ring_is_empty(&ring), "should be empty");

    /* Now write 3 more (this wraps around the buffer) */
    CHECK(send_text(&ring, 3, 0, 300, "WRAP_C") == 0, "send C");
    CHECK(send_text(&ring, 4, 0, 400, "WRAP_D") == 0, "send D");
    CHECK(send_text(&ring, 5, 0, 500, "WRAP_E") == 0, "send E");
    CHECK(vdtu_ring_is_full(&ring), "should be full after 3 more sends");

    /* Read 3 */
    msg = vdtu_ring_fetch(&ring);
    CHECK(msg && msg->hdr.label == 300, "fetch C label");
    CHECK(memcmp(msg->data, "WRAP_C", 6) == 0, "fetch C payload");
    vdtu_ring_ack(&ring);

    msg = vdtu_ring_fetch(&ring);
    CHECK(msg && msg->hdr.label == 400, "fetch D label");
    vdtu_ring_ack(&ring);

    msg = vdtu_ring_fetch(&ring);
    CHECK(msg && msg->hdr.label == 500, "fetch E label");
    vdtu_ring_ack(&ring);

    CHECK(vdtu_ring_is_empty(&ring), "should be empty after reading all");

    free(mem);
    PASS();
}

static void test_empty_fetch(void)
{
    TEST("fetch from empty ring returns NULL");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;
    vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);

    CHECK(vdtu_ring_fetch(&ring) == NULL, "fetch from empty should be NULL");

    free(mem);
    PASS();
}

static void test_payload_too_large(void)
{
    TEST("send with payload exceeding slot size");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);
    struct vdtu_ring ring;
    vdtu_ring_init(&ring, mem, SLOT_COUNT, SLOT_SIZE);

    /* Payload that exceeds slot_size - HEADER_SIZE */
    char big[SLOT_SIZE];
    memset(big, 'X', sizeof(big));
    int rc = vdtu_ring_send(&ring, 0, 0, 0, 0, 0, 0, 0,
                            big, (uint16_t)sizeof(big));
    CHECK(rc == -2, "oversized payload should return -2");

    free(mem);
    PASS();
}

static void test_header_size(void)
{
    TEST("verify DTU header struct size");

    CHECK(sizeof(struct vdtu_msg_header) == VDTU_HEADER_SIZE,
          "header size must match VDTU_HEADER_SIZE");

    /* Verify field offsets match DTU.h packed layout */
    struct vdtu_msg_header h;
    memset(&h, 0, sizeof(h));
    uint8_t *p = (uint8_t *)&h;

    h.flags = 0xAA;
    CHECK(p[0] == 0xAA, "flags at offset 0");

    memset(&h, 0, sizeof(h));
    h.sender_core_id = 0x1234;
    CHECK(p[1] == 0x34 && p[2] == 0x12, "sender_core_id at offset 1 (LE)");

    memset(&h, 0, sizeof(h));
    h.sender_ep_id = 0xBB;
    CHECK(p[3] == 0xBB, "sender_ep_id at offset 3");

    memset(&h, 0, sizeof(h));
    h.reply_ep_id = 0xCC;
    CHECK(p[4] == 0xCC, "reply_ep_id at offset 4");

    memset(&h, 0, sizeof(h));
    h.length = 0x5678;
    CHECK(p[5] == 0x78 && p[6] == 0x56, "length at offset 5 (LE)");

    memset(&h, 0, sizeof(h));
    h.sender_vpe_id = 0x9ABC;
    CHECK(p[7] == 0xBC && p[8] == 0x9A, "sender_vpe_id at offset 7 (LE)");

    PASS();
}

static void test_attach(void)
{
    TEST("attach to existing ring buffer");

    size_t sz = vdtu_ring_total_size(SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, sz);

    /* Producer initializes */
    struct vdtu_ring producer;
    vdtu_ring_init(&producer, mem, SLOT_COUNT, SLOT_SIZE);

    /* Consumer attaches */
    struct vdtu_ring consumer;
    vdtu_ring_attach(&consumer, mem);

    CHECK(consumer.ctrl->slot_count == SLOT_COUNT, "consumer sees slot_count");
    CHECK(consumer.ctrl->slot_size == SLOT_SIZE, "consumer sees slot_size");

    /* Producer sends */
    send_text(&producer, 0, 0, 42, "HELLO");

    /* Consumer fetches */
    const struct vdtu_message *msg = vdtu_ring_fetch(&consumer);
    CHECK(msg != NULL, "consumer should see message");
    CHECK(msg->hdr.label == 42, "label matches");
    CHECK(memcmp(msg->data, "HELLO", 5) == 0, "payload matches");
    vdtu_ring_ack(&consumer);

    CHECK(vdtu_ring_is_empty(&consumer), "consumer sees empty");
    CHECK(vdtu_ring_is_empty(&producer), "producer also sees empty");

    free(mem);
    PASS();
}

static void test_total_size(void)
{
    TEST("total_size calculation");

    size_t sz = vdtu_ring_total_size(4, 512);
    CHECK(sz == 64 + 4 * 512, "4 slots of 512: 64 + 2048 = 2112");

    sz = vdtu_ring_total_size(32, 512);
    CHECK(sz == 64 + 32 * 512, "32 slots of 512: 64 + 16384 = 16448");

    sz = vdtu_ring_total_size(32, 2048);
    CHECK(sz == 64 + 32 * 2048, "32 slots of 2048: 64 + 65536 = 65600");

    PASS();
}

/* ========================================================================= */

int main(void)
{
    printf("=== vDTU Ring Buffer Tests ===\n\n");

    test_init();
    test_init_bad_params();
    test_header_size();
    test_total_size();
    test_send_and_fetch();
    test_fill_all_slots();
    test_empty_fetch();
    test_payload_too_large();
    test_wraparound();
    test_attach();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed ? 1 : 0;
}
