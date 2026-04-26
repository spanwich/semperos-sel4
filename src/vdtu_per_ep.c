/*
 * vdtu_per_ep.c -- Per-EP partitioned ring buffers (FPT-183 Phase 3a)
 *
 * Layers on top of vdtu_ring to give each PE an N-EP fan of independent
 * SPSC rings within a single CAmkES dataport. Layout / API: see header.
 */

#include "vdtu_per_ep.h"
#include <string.h>

/* Round x up to the nearest power of 2 (x ≥ 1). */
static uint32_t round_up_pow2(uint32_t x)
{
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

uint32_t vdtu_per_ep_compute_stride(uint32_t slot_count, uint32_t slot_size)
{
    /* ctrl header (64B) + slot_count × slot_size, rounded up to a power of 2
     * so `ep × stride` is a shift. */
    size_t raw = vdtu_ring_total_size(slot_count, slot_size);
    if (raw > 0xFFFFFFFFu) return 0;
    return round_up_pow2((uint32_t)raw);
}

size_t vdtu_per_ep_total_size(uint32_t ep_count,
                              uint32_t slot_count,
                              uint32_t slot_size)
{
    uint32_t stride = vdtu_per_ep_compute_stride(slot_count, slot_size);
    if (stride == 0) return 0;
    return (size_t)ep_count * (size_t)stride;
}

static int validate_params(uint32_t ep_count,
                           uint32_t slot_count,
                           uint32_t slot_size)
{
    if (ep_count == 0 || ep_count > VDTU_PER_EP_COUNT) return -1;
    if (slot_count < 2 || (slot_count & (slot_count - 1)) != 0) return -1;
    if (slot_size < VDTU_HEADER_SIZE) return -1;
    if ((slot_size & (slot_size - 1)) != 0) return -1;
    /* vdtu_ring's runtime sanity check rejects slot_size > 4096. */
    if (slot_size > 4096) return -1;
    return 0;
}

int vdtu_per_ep_init(struct vdtu_per_ep_set *set, void *mem,
                     uint32_t ep_count,
                     uint32_t slot_count, uint32_t slot_size)
{
    if (!set || !mem) return -1;
    if (validate_params(ep_count, slot_count, slot_size) != 0) return -1;

    uint32_t stride = vdtu_per_ep_compute_stride(slot_count, slot_size);
    if (stride == 0) return -1;

    /* Zero the whole region — covers ctrl headers + slot buffers + any
     * padding inside each EP's stride window. */
    memset(mem, 0, (size_t)ep_count * stride);

    set->base       = mem;
    set->ep_count   = ep_count;
    set->slot_count = slot_count;
    set->slot_size  = slot_size;
    set->ep_stride  = stride;

    /* Initialize one vdtu_ring per EP at offset (ep × stride). */
    for (uint32_t ep = 0; ep < ep_count; ep++) {
        void *ep_mem = (uint8_t *)mem + (size_t)ep * stride;
        int rc = vdtu_ring_init(&set->rings[ep], ep_mem, slot_count, slot_size);
        if (rc != 0) return rc;
    }

    /* Zero out the rest of the rings[] array (EPs above ep_count) so a
     * later get_ring(out-of-range) returns a deterministic NULL-ctrl. */
    for (uint32_t ep = ep_count; ep < VDTU_PER_EP_COUNT; ep++) {
        memset(&set->rings[ep], 0, sizeof(set->rings[ep]));
    }

    return 0;
}

int vdtu_per_ep_attach(struct vdtu_per_ep_set *set, void *mem,
                       uint32_t ep_count,
                       uint32_t slot_count, uint32_t slot_size)
{
    if (!set || !mem) return -1;
    if (validate_params(ep_count, slot_count, slot_size) != 0) return -1;

    uint32_t stride = vdtu_per_ep_compute_stride(slot_count, slot_size);
    if (stride == 0) return -1;

    set->base       = mem;
    set->ep_count   = ep_count;
    set->slot_count = slot_count;
    set->slot_size  = slot_size;
    set->ep_stride  = stride;

    for (uint32_t ep = 0; ep < ep_count; ep++) {
        void *ep_mem = (uint8_t *)mem + (size_t)ep * stride;
        int rc = vdtu_ring_attach(&set->rings[ep], ep_mem);
        if (rc != 0) return rc;
    }
    for (uint32_t ep = ep_count; ep < VDTU_PER_EP_COUNT; ep++) {
        memset(&set->rings[ep], 0, sizeof(set->rings[ep]));
    }

    return 0;
}

struct vdtu_ring *vdtu_per_ep_get_ring(struct vdtu_per_ep_set *set, uint32_t ep)
{
    if (!set || ep >= set->ep_count) return NULL;
    if (!set->rings[ep].ctrl) return NULL;
    return &set->rings[ep];
}

int vdtu_per_ep_send(struct vdtu_per_ep_set *set, uint32_t ep,
                     uint16_t sender_pe, uint8_t sender_ep,
                     uint16_t sender_vpe, uint8_t reply_ep,
                     uint64_t label, uint64_t replylabel, uint8_t flags,
                     const void *payload, uint16_t payload_len)
{
    struct vdtu_ring *r = vdtu_per_ep_get_ring(set, ep);
    if (!r) return -1;
    return vdtu_ring_send(r,
                          sender_pe, sender_ep, sender_vpe, reply_ep,
                          label, replylabel, flags,
                          payload, payload_len);
}

const struct vdtu_message *vdtu_per_ep_fetch(const struct vdtu_per_ep_set *set,
                                             uint32_t ep)
{
    if (!set || ep >= set->ep_count) return NULL;
    if (!set->rings[ep].ctrl) return NULL;
    return vdtu_ring_fetch(&set->rings[ep]);
}

void vdtu_per_ep_ack(struct vdtu_per_ep_set *set, uint32_t ep)
{
    struct vdtu_ring *r = vdtu_per_ep_get_ring(set, ep);
    if (r) vdtu_ring_ack(r);
}
