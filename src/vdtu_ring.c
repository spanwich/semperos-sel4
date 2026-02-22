/*
 * vdtu_ring.c -- SPSC ring buffer implementation matching DTU message format
 */

#include "vdtu_ring.h"

/* Check that n is a power of 2 and >= 2 */
static int is_power_of_2(uint32_t n) {
    return n >= 2 && (n & (n - 1)) == 0;
}

int vdtu_ring_init(struct vdtu_ring *ring, void *mem,
                   uint32_t slot_count, uint32_t slot_size)
{
    if (!ring || !mem)
        return -1;
    if (!is_power_of_2(slot_count))
        return -1;
    if (slot_size < VDTU_HEADER_SIZE || !is_power_of_2(slot_size))
        return -1;

    struct vdtu_ring_ctrl *ctrl = (struct vdtu_ring_ctrl *)mem;
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->head       = 0;
    ctrl->tail       = 0;
    ctrl->slot_count = slot_count;
    ctrl->slot_size  = slot_size;
    ctrl->slot_mask  = slot_count - 1;

    ring->ctrl = ctrl;
    ring->slots = (uint8_t *)mem + VDTU_RING_CTRL_SIZE;

    return 0;
}

int vdtu_ring_attach(struct vdtu_ring *ring, void *mem)
{
    if (!ring || !mem)
        return -1;

    ring->ctrl = (struct vdtu_ring_ctrl *)mem;
    ring->slots = (uint8_t *)mem + VDTU_RING_CTRL_SIZE;

    return 0;
}

int vdtu_ring_send(struct vdtu_ring *ring,
                   uint16_t sender_pe, uint8_t sender_ep,
                   uint16_t sender_vpe, uint8_t reply_ep,
                   uint64_t label, uint64_t replylabel, uint8_t flags,
                   const void *payload, uint16_t payload_len)
{
    if (!ring || !ring->ctrl)
        return -1;

    /* Check if payload fits in slot (slot must hold header + payload) */
    if ((size_t)VDTU_HEADER_SIZE + payload_len > ring->ctrl->slot_size)
        return -2;

    /* Check if ring is full */
    uint32_t head = ring->ctrl->head;
    uint32_t next_head = (head + 1) & ring->ctrl->slot_mask;
    if (next_head == ring->ctrl->tail)
        return -1;  /* full */

    /* Get pointer to the slot */
    uint8_t *slot = ring->slots + (size_t)head * ring->ctrl->slot_size;

    /* Zero the slot first to avoid leaking stale data */
    memset(slot, 0, ring->ctrl->slot_size);

    /* Fill in the DTU message header (simulating HW auto-fill) */
    struct vdtu_msg_header *hdr = (struct vdtu_msg_header *)slot;
    hdr->flags          = flags;
    hdr->sender_core_id = sender_pe;
    hdr->sender_ep_id   = sender_ep;
    hdr->reply_ep_id    = reply_ep;
    hdr->length          = payload_len;
    hdr->sender_vpe_id  = sender_vpe;
    hdr->label           = label;
    hdr->replylabel      = replylabel;

    /* Copy payload after header */
    if (payload && payload_len > 0) {
        memcpy(slot + VDTU_HEADER_SIZE, payload, payload_len);
    }

    /* Memory barrier: ensure all writes are visible before advancing head.
     * On x86, stores are not reordered with other stores, but the compiler
     * might reorder. Use a compiler barrier. */
    __asm__ volatile("" ::: "memory");

    /* Advance head */
    ring->ctrl->head = next_head;

    return 0;
}

const struct vdtu_message *vdtu_ring_fetch(const struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl)
        return NULL;

    uint32_t tail = ring->ctrl->tail;
    uint32_t head = ring->ctrl->head;

    /* Empty? */
    if (tail == head)
        return NULL;

    /* Memory barrier: ensure we read head before reading slot data */
    __asm__ volatile("" ::: "memory");

    /* Return pointer to current slot */
    const uint8_t *slot = ring->slots + (size_t)tail * ring->ctrl->slot_size;
    return (const struct vdtu_message *)slot;
}

void vdtu_ring_ack(struct vdtu_ring *ring)
{
    if (!ring || !ring->ctrl)
        return;

    /* Memory barrier: ensure we are done reading the slot before advancing */
    __asm__ volatile("" ::: "memory");

    ring->ctrl->tail = (ring->ctrl->tail + 1) & ring->ctrl->slot_mask;
}
