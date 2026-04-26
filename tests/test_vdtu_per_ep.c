/*
 * test_vdtu_per_ep.c -- Standalone tests for the per-EP partitioned ring
 *                       buffers (FPT-183 Phase 3a foundation).
 *
 * Compile: gcc -Wall -Wextra -I../components/include -o test_vdtu_per_ep \
 *          test_vdtu_per_ep.c ../src/vdtu_per_ep.c ../src/vdtu_ring.c
 * Or: make (uses the provided Makefile).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "vdtu_per_ep.h"

#define EP_COUNT    8           /* small N for the test                 */
#define SLOT_COUNT  4
#define SLOT_SIZE   VDTU_SYSC_MSG_SIZE   /* 512                          */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  TEST: %-58s ", name); } while (0)
#define PASS()     do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); tests_failed++; return; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while (0)

/* ------------------------------------------------------------------- */

static void test_total_size_and_stride(void)
{
    TEST("total_size and stride match expectations");

    uint32_t stride = vdtu_per_ep_compute_stride(SLOT_COUNT, SLOT_SIZE);
    /* Raw = 64 (ctrl) + 4 × 512 = 2112 → next pow2 = 4096. */
    CHECK(stride == 4096, "stride should be 4096");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    CHECK(total == (size_t)EP_COUNT * 4096, "total_size mismatch");
    PASS();
}

static void test_init_attach_roundtrip(void)
{
    TEST("init + attach see the same rings");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);

    struct vdtu_per_ep_set producer, consumer;
    CHECK(vdtu_per_ep_init(&producer, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0,
          "init should succeed");
    CHECK(vdtu_per_ep_attach(&consumer, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0,
          "attach should succeed");

    /* Both should see all ep_count rings. */
    for (uint32_t ep = 0; ep < EP_COUNT; ep++) {
        CHECK(vdtu_per_ep_get_ring(&producer, ep) != NULL,
              "producer ring missing");
        CHECK(vdtu_per_ep_get_ring(&consumer, ep) != NULL,
              "consumer ring missing");
    }

    /* Out-of-range returns NULL. */
    CHECK(vdtu_per_ep_get_ring(&producer, EP_COUNT) == NULL,
          "out-of-range should be NULL");

    free(mem);
    PASS();
}

static void test_per_ep_isolation(void)
{
    TEST("EP N being full does not block EP M");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);

    struct vdtu_per_ep_set s;
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0,
          "init failed");

    /* Fill EP 0 to capacity (slot_count - 1 messages, since ring loses
     * one slot to distinguish empty from full). */
    const char *p = "ep0";
    for (uint32_t i = 0; i < SLOT_COUNT - 1; i++) {
        int rc = vdtu_per_ep_send(&s, 0, 1, 0, 1, 1, 0, 0, 0,
                                  p, (uint16_t)strlen(p));
        CHECK(rc == 0, "EP 0 send should succeed before full");
    }
    /* Next send on EP 0 must fail (-1 = full). */
    CHECK(vdtu_per_ep_send(&s, 0, 1, 0, 1, 1, 0, 0, 0,
                           p, (uint16_t)strlen(p)) == -1,
          "EP 0 should be full");

    /* But EP 5 (different ring) MUST still accept sends — this is the
     * load-bearing property FPT-183 restores. */
    const char *q = "ep5";
    for (uint32_t i = 0; i < SLOT_COUNT - 1; i++) {
        int rc = vdtu_per_ep_send(&s, 5, 1, 0, 1, 1, 0, 0, 0,
                                  q, (uint16_t)strlen(q));
        CHECK(rc == 0, "EP 5 send must succeed despite EP 0 full");
    }

    /* Drain EP 5 and verify the messages came back, untouched by EP 0. */
    for (uint32_t i = 0; i < SLOT_COUNT - 1; i++) {
        const struct vdtu_message *m = vdtu_per_ep_fetch(&s, 5);
        CHECK(m != NULL, "EP 5 fetch should return a message");
        CHECK(m->hdr.length == strlen(q), "EP 5 length mismatch");
        CHECK(memcmp(m->data, q, strlen(q)) == 0, "EP 5 payload mismatch");
        vdtu_per_ep_ack(&s, 5);
    }
    /* EP 5 now empty; EP 0 still full. */
    CHECK(vdtu_per_ep_fetch(&s, 5) == NULL, "EP 5 should be empty after drain");
    CHECK(vdtu_per_ep_fetch(&s, 0) != NULL, "EP 0 should still have data");

    free(mem);
    PASS();
}

static void test_send_recv_roundtrip(void)
{
    TEST("producer→consumer send/recv across attach boundary");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);

    struct vdtu_per_ep_set tx, rx;
    CHECK(vdtu_per_ep_init(&tx, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "init");
    CHECK(vdtu_per_ep_attach(&rx, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "attach");

    const char *payload = "hello from ep 3";
    CHECK(vdtu_per_ep_send(&tx, 3,
                           42, 9, 7, 1,
                           0xDEADBEEFULL, 0xCAFEBABEULL,
                           0,
                           payload, (uint16_t)strlen(payload)) == 0,
          "send should succeed");

    /* Other side sees it via attach. */
    const struct vdtu_message *m = vdtu_per_ep_fetch(&rx, 3);
    CHECK(m != NULL, "fetch should return non-NULL");
    CHECK(m->hdr.sender_core_id == 42, "sender_core_id mismatch");
    CHECK(m->hdr.sender_ep_id == 9, "sender_ep_id mismatch");
    CHECK(m->hdr.sender_vpe_id == 7, "sender_vpe_id mismatch");
    CHECK(m->hdr.reply_ep_id == 1, "reply_ep_id mismatch");
    CHECK(m->hdr.label == 0xDEADBEEFULL, "label mismatch");
    CHECK(m->hdr.replylabel == 0xCAFEBABEULL, "replylabel mismatch");
    CHECK(m->hdr.length == strlen(payload), "length mismatch");
    CHECK(memcmp(m->data, payload, strlen(payload)) == 0, "payload mismatch");
    vdtu_per_ep_ack(&rx, 3);

    /* Drained: another fetch returns NULL. */
    CHECK(vdtu_per_ep_fetch(&rx, 3) == NULL, "EP 3 should be empty");

    /* Untouched EP returns NULL too. */
    CHECK(vdtu_per_ep_fetch(&rx, 4) == NULL, "EP 4 should be empty");

    free(mem);
    PASS();
}

static void test_init_bad_params(void)
{
    TEST("init rejects bad params");

    void *mem = calloc(1, 1024 * 1024);
    struct vdtu_per_ep_set s;

    /* ep_count = 0 */
    CHECK(vdtu_per_ep_init(&s, mem, 0, 4, 512) == -1, "ep_count=0 should fail");
    /* ep_count > VDTU_PER_EP_COUNT */
    CHECK(vdtu_per_ep_init(&s, mem, VDTU_PER_EP_COUNT + 1, 4, 512) == -1,
          "ep_count > max should fail");
    /* Non-power-of-2 slot_count */
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, 3, 512) == -1, "slot_count=3 fail");
    /* Non-power-of-2 slot_size */
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, 4, 300) == -1, "slot_size=300 fail");
    /* slot_size > 4 KiB */
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, 4, 8192) == -1,
          "slot_size > 4096 fail");
    /* NULL set */
    CHECK(vdtu_per_ep_init(NULL, mem, EP_COUNT, 4, 512) == -1, "NULL set fail");
    /* NULL mem */
    CHECK(vdtu_per_ep_init(&s, NULL, EP_COUNT, 4, 512) == -1, "NULL mem fail");

    free(mem);
    PASS();
}

static void test_max_ep_count(void)
{
    TEST("init with VDTU_PER_EP_COUNT EPs");

    size_t total = vdtu_per_ep_total_size(VDTU_PER_EP_COUNT,
                                          SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);
    struct vdtu_per_ep_set s;

    CHECK(vdtu_per_ep_init(&s, mem, VDTU_PER_EP_COUNT,
                           SLOT_COUNT, SLOT_SIZE) == 0,
          "max EP init should succeed");

    /* Send on every EP, drain in reverse to verify isolation across the
     * entire EP fan. */
    char buf[16];
    for (uint32_t ep = 0; ep < VDTU_PER_EP_COUNT; ep++) {
        snprintf(buf, sizeof(buf), "ep%u", ep);
        int rc = vdtu_per_ep_send(&s, ep, 1, (uint8_t)ep, 1, 1, 0, 0, 0,
                                  buf, (uint16_t)strlen(buf));
        CHECK(rc == 0, "max EP send failed");
    }
    for (int ep = VDTU_PER_EP_COUNT - 1; ep >= 0; ep--) {
        const struct vdtu_message *m = vdtu_per_ep_fetch(&s, (uint32_t)ep);
        CHECK(m != NULL, "max EP fetch returned NULL");
        snprintf(buf, sizeof(buf), "ep%d", ep);
        CHECK(m->hdr.length == strlen(buf), "max EP length");
        CHECK(memcmp(m->data, buf, strlen(buf)) == 0, "max EP payload");
        vdtu_per_ep_ack(&s, (uint32_t)ep);
    }

    free(mem);
    PASS();
}

static void test_kernelcalls_slot_size(void)
{
    TEST("KRNLC slot size (2048) — large messages partition cleanly");

    /* Verifies the layout works at the largest existing message size
     * (Kernelcalls::MSG_SIZE = 2048). With slot_count = 2 and slot_size =
     * 2048, raw = 64 + 4096 = 4160, next pow2 = 8192. Stride = 8192. */
    uint32_t stride = vdtu_per_ep_compute_stride(2, VDTU_KRNLC_MSG_SIZE);
    CHECK(stride == 8192, "stride for KRNLC should be 8192");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, 2, VDTU_KRNLC_MSG_SIZE);
    void *mem = calloc(1, total);
    struct vdtu_per_ep_set s;
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, 2, VDTU_KRNLC_MSG_SIZE) == 0,
          "KRNLC-sized init should succeed");

    /* Round-trip a max-size payload (slot_size − header = 2048 − 25 = 2023). */
    char *big = malloc(VDTU_KRNLC_MSG_SIZE - VDTU_HEADER_SIZE);
    memset(big, 0xAB, VDTU_KRNLC_MSG_SIZE - VDTU_HEADER_SIZE);
    int rc = vdtu_per_ep_send(&s, 7, 0, 0, 0, 0, 0, 0, 0,
                              big, (uint16_t)(VDTU_KRNLC_MSG_SIZE - VDTU_HEADER_SIZE));
    CHECK(rc == 0, "max-payload KRNLC send failed");

    const struct vdtu_message *m = vdtu_per_ep_fetch(&s, 7);
    CHECK(m != NULL, "KRNLC fetch returned NULL");
    CHECK(m->hdr.length == VDTU_KRNLC_MSG_SIZE - VDTU_HEADER_SIZE,
          "KRNLC length mismatch");
    CHECK(memcmp(m->data, big, VDTU_KRNLC_MSG_SIZE - VDTU_HEADER_SIZE) == 0,
          "KRNLC payload mismatch");

    free(big);
    free(mem);
    PASS();
}

/* ------------------------------------------------------------------- */
/*  Routed API tests (FPT-183 Phase 3b)
 *  Slot layout: [route 4B][hdr 25B][payload]
 *  Effective payload = slot_size - 29.
 */

static void test_routed_roundtrip(void)
{
    TEST("send_to + fetch_routed roundtrip with route tag");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);

    struct vdtu_per_ep_set tx, rx;
    CHECK(vdtu_per_ep_init(&tx, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "init");
    CHECK(vdtu_per_ep_attach(&rx, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "attach");

    const char *p = "routed payload";
    int rc = vdtu_per_ep_send_to(&tx, /*src_ep*/ 3,
                                 /*dest_pe*/ 0x0102, /*dest_ep*/ 7,
                                 /*sender_pe*/ 42, /*sender_ep*/ 9,
                                 /*sender_vpe*/ 5, /*reply_ep*/ 1,
                                 /*label*/ 0xDEADBEEFULL,
                                 /*replylabel*/ 0xCAFEBABEULL,
                                 /*flags*/ 0,
                                 p, (uint16_t)strlen(p));
    CHECK(rc == 0, "send_to should succeed");

    const struct vdtu_per_ep_routed_msg *m = vdtu_per_ep_fetch_routed(&rx, 3);
    CHECK(m != NULL, "fetch_routed returned NULL");
    CHECK(m->route.dest_pe == 0x0102, "dest_pe mismatch");
    CHECK(m->route.dest_ep == 7,      "dest_ep mismatch");
    CHECK(m->route.reserved == 0,     "reserved should be 0");
    CHECK(m->hdr.sender_core_id == 42, "sender_core_id mismatch");
    CHECK(m->hdr.sender_ep_id   == 9,  "sender_ep_id mismatch");
    CHECK(m->hdr.sender_vpe_id  == 5,  "sender_vpe_id mismatch");
    CHECK(m->hdr.reply_ep_id    == 1,  "reply_ep_id mismatch");
    CHECK(m->hdr.label          == 0xDEADBEEFULL, "label mismatch");
    CHECK(m->hdr.replylabel     == 0xCAFEBABEULL, "replylabel mismatch");
    CHECK(m->hdr.length == strlen(p), "length mismatch");
    CHECK(memcmp(m->data, p, strlen(p)) == 0, "payload mismatch");
    vdtu_per_ep_ack(&rx, 3);

    /* Drained: fetch returns NULL again. */
    CHECK(vdtu_per_ep_fetch_routed(&rx, 3) == NULL, "should be empty after ack");

    free(mem);
    PASS();
}

static void test_routed_payload_too_large(void)
{
    TEST("send_to rejects payload that pushes slot beyond slot_size");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);
    struct vdtu_per_ep_set s;
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "init");

    /* Slot size is 512 bytes. Route 4 + hdr 25 = 29. Max payload = 483. */
    char *p = malloc(SLOT_SIZE);
    memset(p, 0xAB, SLOT_SIZE);

    /* Exactly at max — should succeed. */
    int rc = vdtu_per_ep_send_to(&s, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 p, SLOT_SIZE - 29);
    CHECK(rc == 0, "max-fitting payload should succeed");

    /* One byte over — must fail with -2. */
    rc = vdtu_per_ep_send_to(&s, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             p, SLOT_SIZE - 29 + 1);
    CHECK(rc == -2, "oversize payload should return -2");

    free(p);
    free(mem);
    PASS();
}

static void test_routed_per_ep_isolation(void)
{
    TEST("routed: EP N full does not block EP M");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);
    struct vdtu_per_ep_set s;
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "init");

    /* Fill EP 2 to capacity. */
    const char *p = "fill";
    for (uint32_t i = 0; i < SLOT_COUNT - 1; i++) {
        CHECK(vdtu_per_ep_send_to(&s, 2, 99, 7, 1, 0, 0, 0, 0, 0, 0,
                                  p, (uint16_t)strlen(p)) == 0,
              "EP 2 send before full");
    }
    CHECK(vdtu_per_ep_send_to(&s, 2, 99, 7, 1, 0, 0, 0, 0, 0, 0,
                              p, (uint16_t)strlen(p)) == -1,
          "EP 2 must be full");

    /* EP 6 unaffected. */
    CHECK(vdtu_per_ep_send_to(&s, 6, 12, 3, 1, 0, 0, 0, 0, 0, 0,
                              p, (uint16_t)strlen(p)) == 0,
          "EP 6 must accept despite EP 2 full");

    const struct vdtu_per_ep_routed_msg *m = vdtu_per_ep_fetch_routed(&s, 6);
    CHECK(m != NULL, "EP 6 fetch should succeed");
    CHECK(m->route.dest_pe == 12, "EP 6 dest_pe mismatch");
    CHECK(m->route.dest_ep == 3,  "EP 6 dest_ep mismatch");

    free(mem);
    PASS();
}

static void test_routed_terminated_ep(void)
{
    TEST("send_to rejects writes to TERMINATED EP");

    size_t total = vdtu_per_ep_total_size(EP_COUNT, SLOT_COUNT, SLOT_SIZE);
    void *mem = calloc(1, total);
    struct vdtu_per_ep_set s;
    CHECK(vdtu_per_ep_init(&s, mem, EP_COUNT, SLOT_COUNT, SLOT_SIZE) == 0, "init");

    /* Manually mark EP 4 as TERMINATED. */
    struct vdtu_ring *r = vdtu_per_ep_get_ring(&s, 4);
    CHECK(r != NULL, "EP 4 ring exists");
    r->ctrl->ep_state = VDTU_EP_TERMINATED;

    int rc = vdtu_per_ep_send_to(&s, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 "x", 1);
    CHECK(rc == -3, "TERMINATED EP must return -3");

    /* Other EPs still work. */
    rc = vdtu_per_ep_send_to(&s, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, "y", 1);
    CHECK(rc == 0, "other EPs unaffected");

    free(mem);
    PASS();
}

/* ------------------------------------------------------------------- */

int main(void)
{
    printf("vdtu_per_ep tests (FPT-183 Phase 3a foundation + 3b routing)\n");
    printf("============================================================\n");
    test_total_size_and_stride();
    test_init_attach_roundtrip();
    test_per_ep_isolation();
    test_send_recv_roundtrip();
    test_init_bad_params();
    test_max_ep_count();
    test_kernelcalls_slot_size();
    /* Phase 3b additions — routed API */
    test_routed_roundtrip();
    test_routed_payload_too_large();
    test_routed_per_ep_isolation();
    test_routed_terminated_ep();
    printf("\nResult: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
