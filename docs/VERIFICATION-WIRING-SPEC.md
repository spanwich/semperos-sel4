# Verification Wiring Specification

This document specifies the exact C code changes needed to integrate the
formally verified vDTU enforcement chain (proofs/) into the codebase.

These changes should be applied after Agent A and Agent B work is merged.

---

## Wiring Point 1: Replace ep_state constants with verified header

**File:** `components/include/vdtu_ring.h`
**Location:** Lines 91-95 (ep_state `#define` constants)
**Change:** Replace the four `#define` constants with an `#include` of the verified header.

Before:
```c
/* EP lifecycle states (stored in vdtu_ring_ctrl.ep_state) */
#define VDTU_EP_UNCONFIGURED    0
#define VDTU_EP_CONFIGURED      1
#define VDTU_EP_ACTIVE          2
#define VDTU_EP_TERMINATED      3
```

After:
```c
/* EP lifecycle states — verified implementation (from proofs/extracted/) */
#include "vdtu_ep_state.h"
```

**Reason:** The `vdtu_ep_state.h` header provides the same constants plus the
verified `vdtu_ep_state_transition()` function (from EpState.Low.fst, Lemma 2).
This ensures all state transitions go through the verified function.

**Build system change:** Add `proofs/extracted/` to the include path for
VDTUService and any component that includes `vdtu_ring.h`. In CMakeLists.txt,
add:
```cmake
target_include_directories(VDTUService.instance.vdtu0 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/proofs/extracted)
```

---

## Wiring Point 2: Add raft_cache_check_ancestry() to config handlers

**File:** `components/VDTUService/VDTUService.c`
**Location:** Functions `config_config_recv()` (line ~173), `config_config_send()` (line ~224), `config_config_mem()` (line ~275)
**Change:** Add a Raft cache ancestry check before channel allocation.

For each config handler, add after parameter validation and before channel allocation:

```c
/* Enforcement chain: check Raft cache for blocked ancestors (Lemma 1) */
if (raft_cache_check_ancestry(/* cap_id for this endpoint */)) {
    printf("[vDTU] EPERM: blocked ancestor found for pe=%d ep=%d\n",
           target_pe, ep_id);
    return -1;  /* EPERM */
}
```

**Reason:** This implements the first stage of the enforcement chain proven in
`EnforcementChain.fst`. The `raft_cache_check_ancestry()` function corresponds
to `ancestry_walk` in `AncestryWalk.fst` (Lemma 1).

**Dependency:** Requires the RaftLogCache (hash table, C-callable) to be
implemented. Until then, the function should be a stub returning false (no
blocked ancestors), which preserves the existing behavior.

**Stub for initial integration:**
```c
/* Stub until RaftLogCache is implemented — returns false (no blocked ancestors).
 * This is safe: it means the enforcement chain is permissive (allows all),
 * which matches current behavior. */
static inline bool raft_cache_check_ancestry(uint64_t cap_id) {
    (void)cap_id;
    return false;
}
```

---

## Wiring Point 3: Use vdtu_ep_state_transition() in terminate_ep

**File:** `components/VDTUService/VDTUService.c`
**Location:** Function `config_terminate_ep()` (line ~345)
**Change:** The terminate_ep handler currently returns the channel index
and lets the caller (SemperKernel) set ep_state directly on the ring ctrl.
Change this to use the verified transition function.

**Current behavior (VDTUService.c:345-366):**
```c
int config_terminate_ep(int target_pe, int ep_id) {
    // ... validation ...
    int ch = ep->channel_idx;
    // Returns channel index; caller sets ep_state = TERMINATED directly
    return ch;
}
```

**Caller (DTU.cc) currently does:**
```c
ring->ctrl->ep_state = VDTU_EP_TERMINATED;
```

**New behavior — two options:**

**Option A (preferred): VDTUService calls transition via RPC result**
Add a `blocked` parameter to the terminate_ep RPC and use the verified
transition in the kernel-side DTU code:

```c
/* In DTU.cc (or camkes_entry.c), after receiving terminate_ep channel index: */
#include "vdtu_ep_state.h"

bool ok = vdtu_ep_state_transition(
    &ring->ctrl->ep_state,
    VDTU_EP_TERMINATED,
    blocked  /* from Raft cache check */
);
if (!ok) {
    /* Transition rejected — ep_state was not Active, or not blocked */
    printf("[vDTU] WARNING: terminate transition rejected\n");
}
```

**Option B (simpler): Gate only at config time**
Keep the direct assignment in terminate_ep, but add the Raft cache check
at config_recv/send/mem time (Wiring Point 2). This means termination is
always permitted once requested, but new configurations are blocked.

**Reason:** This implements the ep_state transition safety proven in
`EpState.fst` (Lemma 2) and `EpState.Low.fst`. The verified function
ensures that:
- Only valid forward transitions are applied
- Terminated is absorbing (no escape once terminated)
- Termination is gated by the Raft cache blocked flag

---

## Wiring Point 4: VDTUConfig IDL extension (optional, for Option A)

**File:** `interfaces/VDTUConfig.idl4`
**Location:** `terminate_ep` procedure declaration
**Change:** Add a `blocked` parameter:

Before:
```idl4
int terminate_ep(in int target_pe, in int ep_id);
```

After:
```idl4
int terminate_ep(in int target_pe, in int ep_id, in int blocked);
```

**Reason:** Passes the Raft cache verdict through the RPC interface so the
verified transition function can gate on it.

---

## Wiring Point 5: ring_send ep_state check (already wired)

**File:** `src/vdtu_ring.c`
**Location:** `vdtu_ring_send()`, lines 59-60
**Change:** No change needed. The existing check:
```c
if (ring->ctrl->ep_state == VDTU_EP_TERMINATED)
    return -3;
```
is exactly the behavior proven in `EpState.send_terminated_returns_error`.
The formal proof confirms this check is correct and sufficient.

---

## Summary Table

| # | File | Function/Location | Change | Lemma |
|---|------|-------------------|--------|-------|
| 1 | `vdtu_ring.h` | Lines 91-95 | Replace `#define`s with `#include "vdtu_ep_state.h"` | Lemma 2 |
| 2 | `VDTUService.c` | `config_config_{recv,send,mem}` | Add `raft_cache_check_ancestry()` | Lemma 1 + Composition |
| 3 | `VDTUService.c` or `DTU.cc` | `terminate_ep` path | Use `vdtu_ep_state_transition()` | Lemma 2 |
| 4 | `VDTUConfig.idl4` | `terminate_ep` | Add `blocked` parameter (Option A) | Lemma 2 |
| 5 | `vdtu_ring.c` | `vdtu_ring_send` | No change (already correct) | Lemma 2b |

## Dependencies

The wiring depends on:
1. **RaftLogCache** (P3 backlog item) — provides `raft_cache_check_ancestry()`
2. **RaftPD CAmkES component** (P4) — provides the Raft log that feeds the cache
3. **Agent A completion** — Tasks 08/09 and Exp 2A must be merged first

Without the Raft infrastructure, Wiring Points 2 and 3 use stubs that
preserve current behavior (all operations permitted).
