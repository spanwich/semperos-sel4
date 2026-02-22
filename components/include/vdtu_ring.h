/*
 * vdtu_ring.h -- SPSC ring buffer matching the M3/SemperOS DTU message format
 *
 * This ring buffer implements the circular message buffer used by the DTU
 * hardware for receive endpoints. Each slot contains a DTU message header
 * followed by payload data. The ring is single-producer single-consumer:
 * the producer (sender side / vDTU) writes messages and advances head;
 * the consumer (receiver side) fetches messages and advances tail.
 *
 * The layout matches the gem5 DTU's receive buffer semantics:
 *   - Power-of-2 number of slots
 *   - Each slot is a fixed size (e.g., 512 bytes for syscalls)
 *   - The DTU header is filled by the producer (simulating HW auto-fill)
 *   - fetch_msg returns a pointer to the next unread slot
 *   - mark_read (ack) advances the consumer position
 *
 * Memory layout in a shared dataport:
 *   [0..63]   Ring control header (head, tail, counts, sizes)
 *   [64..]    Message slots (slot_count * slot_size bytes)
 */

#ifndef VDTU_RING_H
#define VDTU_RING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * --------------------------------------------------------------------------
 *  DTU Message Header (from src/include/base/arch/gem5/DTU.h:150-161)
 *
 *  This struct is PACKED and matches the gem5 DTU Header exactly.
 *  sizeof(vdtu_msg_header) == VDTU_HEADER_SIZE == 25 bytes
 *
 *  Verified against SemperOS gem5 DTU.h:150-161 (struct Header):
 *  flags(1) + senderCoreId(2) + senderEpId(1) + replyEpId(1) +
 *  length(2) + senderVpeId(2) + label(8) + replylabel(8) = 25 bytes
 * --------------------------------------------------------------------------
 */

#define VDTU_HEADER_SIZE    25

struct __attribute__((packed)) vdtu_msg_header {
    uint8_t  flags;            /* bit 0: is_reply, bit 1: grant_credits      */
    uint16_t sender_core_id;   /* PE ID of sender                            */
    uint8_t  sender_ep_id;     /* endpoint ID of sender's send EP            */
    uint8_t  reply_ep_id;      /* for msg: reply EP; for reply: credit EP    */
    uint16_t length;           /* payload length in bytes                     */
    uint16_t sender_vpe_id;    /* VPE ID of sender                           */
    uint64_t label;            /* routing label (set by send EP config)       */
    uint64_t replylabel;       /* reply label (set by REPLY_LABEL register)  */
};

/* A complete message: header + variable-length payload */
struct __attribute__((packed)) vdtu_message {
    struct vdtu_msg_header hdr;
    unsigned char data[];      /* flexible array member */
};

/*
 * --------------------------------------------------------------------------
 *  DTU Constants (from dtu-interface-spec.md)
 * --------------------------------------------------------------------------
 */

#define VDTU_EP_COUNT           16
#define VDTU_MAX_MSG_SLOTS      32
#define VDTU_DTU_PKG_SIZE       8

/* Common slot sizes (power of 2) */
#define VDTU_SYSC_MSG_SIZE      512     /* 1 << SYSC_CREDIT_ORD (9) */
#define VDTU_KRNLC_MSG_SIZE     2048    /* Kernelcalls::MSG_SIZE     */
#define VDTU_SRV_MSG_SIZE       256     /* Service::SRV_MSG_SIZE     */

/* Default slot count for this prototype */
#define VDTU_DEFAULT_SLOT_COUNT 4
#define VDTU_DEFAULT_SLOT_SIZE  VDTU_SYSC_MSG_SIZE

/* Message header flags */
#define VDTU_FLAG_REPLY         (1 << 0)
#define VDTU_FLAG_GRANT_CREDITS (1 << 1)

/* Credits */
#define VDTU_CREDITS_UNLIM      0xFFFF

/*
 * --------------------------------------------------------------------------
 *  Ring Buffer Control Structure
 *
 *  Sits at the start of each shared dataport. Padded to 64 bytes for
 *  cache-line alignment and to separate producer/consumer fields.
 * --------------------------------------------------------------------------
 */

#define VDTU_RING_CTRL_SIZE     64

struct vdtu_ring_ctrl {
    /* Written by producer */
    volatile uint32_t head;         /* next slot to write (producer)    */

    /* Written by consumer */
    volatile uint32_t tail;         /* next slot to read (consumer)     */

    /* Immutable after init */
    uint32_t slot_count;            /* number of slots (power of 2)     */
    uint32_t slot_size;             /* bytes per slot (power of 2)      */
    uint32_t slot_mask;             /* slot_count - 1 (for wrapping)    */

    uint8_t  _pad[VDTU_RING_CTRL_SIZE - 5 * sizeof(uint32_t)];
};

/* Compile-time check that control is exactly 64 bytes */
#ifdef __cplusplus
static_assert(sizeof(struct vdtu_ring_ctrl) == VDTU_RING_CTRL_SIZE,
              "ring ctrl must be 64 bytes");
#else
_Static_assert(sizeof(struct vdtu_ring_ctrl) == VDTU_RING_CTRL_SIZE,
               "ring ctrl must be 64 bytes");
#endif

/*
 * --------------------------------------------------------------------------
 *  Ring Buffer Handle
 *
 *  Wraps a pointer to the shared memory region. The caller provides the
 *  backing memory (either a CAmkES dataport or malloc'd for testing).
 * --------------------------------------------------------------------------
 */

struct vdtu_ring {
    struct vdtu_ring_ctrl *ctrl;    /* points to start of shared region */
    uint8_t *slots;                 /* points to ctrl + RING_CTRL_SIZE  */
};

/*
 * --------------------------------------------------------------------------
 *  API
 * --------------------------------------------------------------------------
 */

/**
 * Initialize a ring buffer in the given memory region.
 *
 * @param ring       Output handle
 * @param mem        Pointer to shared memory (must be >= ring_total_size)
 * @param slot_count Number of message slots (must be power of 2, >= 2)
 * @param slot_size  Bytes per slot (must be power of 2, >= VDTU_HEADER_SIZE)
 * @return 0 on success, -1 on invalid parameters
 */
int vdtu_ring_init(struct vdtu_ring *ring, void *mem,
                   uint32_t slot_count, uint32_t slot_size);

/**
 * Attach to an already-initialized ring buffer (other side called init).
 *
 * @param ring  Output handle
 * @param mem   Pointer to shared memory (already initialized by producer)
 * @return 0 on success
 */
int vdtu_ring_attach(struct vdtu_ring *ring, void *mem);

/**
 * Compute total bytes needed for a ring buffer.
 */
static inline size_t vdtu_ring_total_size(uint32_t slot_count, uint32_t slot_size) {
    return VDTU_RING_CTRL_SIZE + (size_t)slot_count * slot_size;
}

/**
 * Check if ring is full (no space for producer to write).
 */
static inline int vdtu_ring_is_full(const struct vdtu_ring *ring) {
    uint32_t next_head = (ring->ctrl->head + 1) & ring->ctrl->slot_mask;
    return next_head == ring->ctrl->tail;
}

/**
 * Check if ring is empty (no messages for consumer to read).
 */
static inline int vdtu_ring_is_empty(const struct vdtu_ring *ring) {
    return ring->ctrl->head == ring->ctrl->tail;
}

/**
 * Number of messages available for reading.
 */
static inline uint32_t vdtu_ring_available(const struct vdtu_ring *ring) {
    return (ring->ctrl->head - ring->ctrl->tail) & ring->ctrl->slot_mask;
}

/**
 * Send a message: write header + payload into the next slot, advance head.
 *
 * The header fields are filled in by this function (simulating the DTU HW
 * auto-fill behavior). The caller provides sender info + payload.
 *
 * @param ring          Ring buffer handle
 * @param sender_pe     Sender's PE ID
 * @param sender_ep     Sender's endpoint ID
 * @param sender_vpe    Sender's VPE ID
 * @param reply_ep      Reply endpoint ID
 * @param label         Message label (from send EP config)
 * @param replylabel    Reply label
 * @param flags         Header flags (VDTU_FLAG_REPLY, etc.)
 * @param payload       Payload data (may be NULL if payload_len == 0)
 * @param payload_len   Payload length in bytes
 * @return 0 on success, -1 if ring is full, -2 if payload too large
 */
int vdtu_ring_send(struct vdtu_ring *ring,
                   uint16_t sender_pe, uint8_t sender_ep,
                   uint16_t sender_vpe, uint8_t reply_ep,
                   uint64_t label, uint64_t replylabel, uint8_t flags,
                   const void *payload, uint16_t payload_len);

/**
 * Fetch the next unread message (consumer side).
 *
 * Returns a pointer to the message slot. Does NOT advance the tail pointer.
 * The returned pointer is valid until vdtu_ring_ack() is called.
 *
 * This matches the DTU fetch_msg() semantics: fetch returns a pointer,
 * and mark_read (ack) consumes the slot.
 *
 * @param ring  Ring buffer handle
 * @return Pointer to message (vdtu_message*), or NULL if ring is empty
 */
const struct vdtu_message *vdtu_ring_fetch(const struct vdtu_ring *ring);

/**
 * Acknowledge/consume the current message (advance tail).
 *
 * Must be called after vdtu_ring_fetch() to release the slot.
 * Matches DTU mark_read() / ACK_MSG semantics.
 *
 * @param ring  Ring buffer handle
 */
void vdtu_ring_ack(struct vdtu_ring *ring);

/**
 * Get the slot offset for a fetched message (for DTU get_msgoff compatibility).
 *
 * @param ring  Ring buffer handle
 * @param msg   Message pointer returned by vdtu_ring_fetch()
 * @return Byte offset of the message from the start of the slot area
 */
static inline size_t vdtu_ring_msg_offset(const struct vdtu_ring *ring,
                                          const struct vdtu_message *msg) {
    return (const uint8_t *)msg - ring->slots;
}

#ifdef __cplusplus
}
#endif

#endif /* VDTU_RING_H */
