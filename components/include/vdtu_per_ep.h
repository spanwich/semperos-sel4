/*
 * vdtu_per_ep.h -- Per-EP partitioned ring buffers for the n-vDTU model
 *
 * FPT-183 — Rework vDTU transport: per-PE vDTU instances + DTUBridge as
 * virtual NoC + reliable delivery.
 *
 * Each PE owns a single CAmkES dataport per direction (outbound, inbound).
 * The dataport is partitioned into N independent SPSC rings — one per
 * endpoint. EP N being full does not block EP M.
 *
 * Design page: https://moleculestudio.atlassian.net/wiki/spaces/FO/pages/14876698
 *
 * Layout in a partitioned dataport (option ii from the design):
 *   [EP 0] [vdtu_ring_ctrl 64B][slot_count × slot_size]
 *   [EP 1] [vdtu_ring_ctrl 64B][slot_count × slot_size]
 *   ...
 *   [EP N-1]
 *
 * Per-EP stride is rounded up to a power of 2 so `ep × stride` is a
 * shift-and-add. Producer (sender side) calls vdtu_per_ep_init() once per
 * dataport to lay out and zero all N rings. Consumer (receiver side, e.g.
 * DTUBridge) calls vdtu_per_ep_attach() to map the same layout without
 * re-initializing — head/tail visible to both sides via shared memory.
 *
 * Per-EP isolation: each ring has its own (head, tail, ep_state, slot
 * buffer). PINGs filling EP 0's ring cannot block CREATESESSREP arriving
 * on EP 5.
 */

#ifndef VDTU_PER_EP_H
#define VDTU_PER_EP_H

#include "vdtu_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of EPs per PE — matches the gem5 DTU per-PE EP count. */
#define VDTU_PER_EP_COUNT VDTU_EP_COUNT  /* 32 */

/*
 * --------------------------------------------------------------------------
 *  Per-EP route tag (FPT-183 Phase 3b)
 *
 *  When messages flow through DTUBridge as virtual NoC, the bridge needs
 *  to know which destination PE + EP to deliver to. The DTU message
 *  header is locked to gem5's 25-byte format and only carries SENDER
 *  info (sender_pe, sender_ep), not destination. We prepend a 4-byte
 *  route tag to each slot in the per-EP rings so DTUBridge can route
 *  blindly without consulting the (pe, ep) channel table on every poll.
 *
 *  Slot layout when written via vdtu_per_ep_send_to / read via
 *  vdtu_per_ep_fetch_routed:
 *      [vdtu_per_ep_route 4B][vdtu_msg_header 25B][payload]
 *  Effective payload capacity = slot_size - 4 - 25.
 *
 *  Slot layout when written via the older vdtu_per_ep_send / read via
 *  vdtu_per_ep_fetch (no route tag, gem5-format only):
 *      [vdtu_msg_header 25B][payload]
 *  These older APIs remain for the standalone tests and any code that
 *  doesn't need bridge-mediated routing. A given EP must be used with
 *  ONE format consistently — mixing produces garbage.
 * --------------------------------------------------------------------------
 */

#define VDTU_PER_EP_ROUTE_SIZE 4

/* Magic byte stored in vdtu_per_ep_route.reserved when the route was
 * written by vdtu_per_ep_send_to. DTUBridge's UDP-recv path uses it to
 * unambiguously distinguish FPT-183 routed messages from the legacy
 * vdtu_msg_header-only wire format that the g_net_out_ring path emits.
 * 0xA5 was chosen so it would never collide with the legacy format's
 * first 4 bytes — flags + sender_core_id (lo) + sender_core_id (hi) +
 * sender_ep_id — which all carry small-integer values in practice. */
#define VDTU_PER_EP_ROUTE_MAGIC 0xA5

struct __attribute__((packed)) vdtu_per_ep_route {
    uint16_t dest_pe;       /* global PE ID of destination          */
    uint8_t  dest_ep;       /* endpoint ID on destination PE        */
    uint8_t  magic;         /* always VDTU_PER_EP_ROUTE_MAGIC after  *
                             * vdtu_per_ep_send_to has populated it. */
};

struct __attribute__((packed)) vdtu_per_ep_routed_msg {
    struct vdtu_per_ep_route route;
    struct vdtu_msg_header   hdr;
    unsigned char            data[];   /* payload, length in hdr.length */
};

/*
 * --------------------------------------------------------------------------
 *  Per-EP set handle
 *
 *  Wraps a single dataport (or test-allocated memory region) and presents
 *  N_EPS independent SPSC rings. Caller chooses ep_count, slot_count, and
 *  slot_size at init time; both sides must agree on these so the partition
 *  layout matches.
 * --------------------------------------------------------------------------
 */
struct vdtu_per_ep_set {
    void    *base;                                  /* dataport base ptr     */
    uint32_t ep_count;                              /* number of EPs         */
    uint32_t slot_count;                            /* per-EP slot count     */
    uint32_t slot_size;                             /* per-EP slot size      */
    uint32_t ep_stride;                             /* bytes per EP region   */
    struct vdtu_ring rings[VDTU_PER_EP_COUNT];      /* per-EP ring handles   */
};

/*
 * --------------------------------------------------------------------------
 *  API
 * --------------------------------------------------------------------------
 */

/**
 * Compute the dataport size needed for a per-EP set.
 *
 * Internally this rounds the per-EP slot region (ctrl + slot_count*slot_size)
 * up to a power of 2 to keep `ep × stride` cheap. ep_count entries × stride.
 */
size_t vdtu_per_ep_total_size(uint32_t ep_count,
                              uint32_t slot_count,
                              uint32_t slot_size);

/**
 * Compute the per-EP stride (rounded-up power-of-2 of ctrl + slots).
 * Useful for the consumer side which needs to mirror the producer's choice.
 */
uint32_t vdtu_per_ep_compute_stride(uint32_t slot_count, uint32_t slot_size);

/**
 * Initialize all ep_count rings within mem. Producer side.
 *
 * Zeros the entire dataport and writes initial head/tail/slot_count/slot_size
 * for every EP's ring_ctrl. Caller must ensure mem is at least
 * vdtu_per_ep_total_size(...) bytes.
 *
 * @param set        Output handle.
 * @param mem        Pointer to the dataport (or test memory).
 * @param ep_count   Number of EPs to lay out (≤ VDTU_PER_EP_COUNT).
 * @param slot_count Per-EP slot count (power of 2, ≥ 2).
 * @param slot_size  Per-EP slot size (power of 2, ≥ VDTU_HEADER_SIZE,
 *                   ≤ 4 KiB per the existing vdtu_ring sanity check).
 * @return 0 on success, -1 on bad params.
 */
int vdtu_per_ep_init(struct vdtu_per_ep_set *set, void *mem,
                     uint32_t ep_count,
                     uint32_t slot_count, uint32_t slot_size);

/**
 * Attach to an already-initialized per-EP set. Consumer side.
 *
 * Must be called with the same ep_count/slot_count/slot_size the producer
 * used. Does not modify any ring state.
 */
int vdtu_per_ep_attach(struct vdtu_per_ep_set *set, void *mem,
                       uint32_t ep_count,
                       uint32_t slot_count, uint32_t slot_size);

/**
 * Get the ring handle for a specific EP.
 * @return NULL if ep ≥ ep_count or set is uninitialized.
 */
struct vdtu_ring *vdtu_per_ep_get_ring(struct vdtu_per_ep_set *set, uint32_t ep);

/**
 * Send via the specified EP's ring (thin wrapper over vdtu_ring_send).
 */
int vdtu_per_ep_send(struct vdtu_per_ep_set *set, uint32_t ep,
                     uint16_t sender_pe, uint8_t sender_ep,
                     uint16_t sender_vpe, uint8_t reply_ep,
                     uint64_t label, uint64_t replylabel, uint8_t flags,
                     const void *payload, uint16_t payload_len);

/**
 * Fetch the next message on the specified EP's ring (does not advance tail).
 */
const struct vdtu_message *vdtu_per_ep_fetch(const struct vdtu_per_ep_set *set,
                                             uint32_t ep);

/**
 * Acknowledge / consume the current message on the specified EP's ring.
 */
void vdtu_per_ep_ack(struct vdtu_per_ep_set *set, uint32_t ep);

/*
 * --------------------------------------------------------------------------
 *  Routed API (FPT-183 Phase 3b)
 *
 *  Used by code that flows through DTUBridge as virtual NoC. Each message
 *  carries a vdtu_per_ep_route prefix so the bridge can route by dest_pe
 *  / dest_ep without a (pe, ep) → channel table lookup.
 * --------------------------------------------------------------------------
 */

/**
 * Send via the specified outbound EP's ring with a routing tag prepended
 * to the slot. The bridge will read the tag and deliver to dest_pe's
 * inbound[dest_ep] ring (local) or UDP-send to the remote DTUBridge.
 *
 * @return 0 on success; -1 if ring is full / bad params; -2 payload too
 *         large (slot_size must hold route 4B + hdr 25B + payload);
 *         -3 EP terminated; -4 corrupt ring state.
 */
int vdtu_per_ep_send_to(struct vdtu_per_ep_set *set, uint32_t ep,
                        uint16_t dest_pe, uint8_t dest_ep,
                        uint16_t sender_pe, uint8_t sender_ep,
                        uint16_t sender_vpe, uint8_t reply_ep,
                        uint64_t label, uint64_t replylabel, uint8_t flags,
                        const void *payload, uint16_t payload_len);

/**
 * Fetch the next routed message on the specified inbound EP's ring. The
 * returned pointer covers route + hdr + payload (does not advance tail).
 *
 * @return Pointer to vdtu_per_ep_routed_msg, or NULL if ring empty.
 */
const struct vdtu_per_ep_routed_msg *
vdtu_per_ep_fetch_routed(const struct vdtu_per_ep_set *set, uint32_t ep);

#ifdef __cplusplus
}
#endif

#endif /* VDTU_PER_EP_H */
