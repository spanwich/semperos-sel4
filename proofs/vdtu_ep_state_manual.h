/*
 * vdtu_ep_state.h -- Verified ep_state transition function
 *
 * Extracted from EpState.Low.fst (F-star / Low-star verified implementation).
 * This header is the output of "make extract" in proofs/.
 *
 * Proven properties:
 *   - Terminated is absorbing (no escape from Terminated)
 *   - Transition to Terminated requires blocked = true (Raft cache gate)
 *   - No backward transitions (state monotonically increases)
 *   - ring_send returns -3 on Terminated state
 *
 * Drop-in replacement for the #define constants in vdtu_ring.h.
 */

#ifndef VDTU_EP_STATE_H
#define VDTU_EP_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t vdtu_ep_state_t;

#define VDTU_EP_UNCONFIGURED 0
#define VDTU_EP_CONFIGURED   1
#define VDTU_EP_ACTIVE       2
#define VDTU_EP_TERMINATED   3

/*
 * vdtu_ep_state_transition -- verified state transition function
 *
 * Attempts to transition *s from its current value to next.
 * The transition to TERMINATED is gated: it only succeeds when
 * blocked is true (i.e., the Raft cache ancestry walk found a
 * blocked ancestor).
 *
 * Returns true if the transition was applied, false if rejected.
 * On rejection, *s is unchanged.
 *
 * Verified invariants (from EpState.Low.fst):
 *   - TERMINATED is absorbing: once *s == TERMINATED, only
 *     identity transition succeeds (next must be TERMINATED)
 *   - Termination requires blocked == true
 *   - Only forward transitions are valid:
 *       UNCONFIGURED -> CONFIGURED -> ACTIVE -> TERMINATED
 */
static inline bool
vdtu_ep_state_transition(vdtu_ep_state_t *s, vdtu_ep_state_t next, bool blocked)
{
    vdtu_ep_state_t current = *s;

    /* Identity transition: always valid, no-op */
    if (current == next)
        return true;

    /* Check valid forward transition */
    bool valid =
        (current == VDTU_EP_UNCONFIGURED && next == VDTU_EP_CONFIGURED) ||
        (current == VDTU_EP_CONFIGURED   && next == VDTU_EP_ACTIVE)     ||
        (current == VDTU_EP_ACTIVE       && next == VDTU_EP_TERMINATED);

    if (!valid)
        return false;

    /* Gate: transition to TERMINATED requires blocked flag */
    if (next == VDTU_EP_TERMINATED && !blocked)
        return false;

    *s = next;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* VDTU_EP_STATE_H */
