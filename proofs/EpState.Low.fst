(*
 * EpState.Low.fst -- Low* extractable ep_state transition function
 *
 * This module provides a Low* (machine-integer) implementation of the
 * ep_state transition function that KaRaMeL can extract to a C header.
 *
 * The extracted C code replaces the direct ep_state assignment in
 * VDTUService.c with a verified transition function.
 *
 * Target output: proofs/extracted/vdtu_ep_state.h
 *)
module EpState.Low

open FStar.UInt32

module U32 = FStar.UInt32

(* --------------------------------------------------------------------------
 *  Constants — must match vdtu_ring.h
 *    #define VDTU_EP_UNCONFIGURED 0
 *    #define VDTU_EP_CONFIGURED   1
 *    #define VDTU_EP_ACTIVE       2
 *    #define VDTU_EP_TERMINATED   3
 * -------------------------------------------------------------------------- *)

inline_for_extraction
let vdtu_ep_unconfigured : U32.t = 0ul

inline_for_extraction
let vdtu_ep_configured : U32.t = 1ul

inline_for_extraction
let vdtu_ep_active : U32.t = 2ul

inline_for_extraction
let vdtu_ep_terminated : U32.t = 3ul

(* --------------------------------------------------------------------------
 *  Type alias — the ep_state is a uint32_t
 * -------------------------------------------------------------------------- *)

type vdtu_ep_state_t = U32.t

(* --------------------------------------------------------------------------
 *  valid_transition_low — machine-integer version
 *
 *  Returns true iff (current, next) is a valid state transition.
 * -------------------------------------------------------------------------- *)

inline_for_extraction
let valid_transition_low (current: vdtu_ep_state_t) (next: vdtu_ep_state_t) : bool =
  (current = vdtu_ep_unconfigured && next = vdtu_ep_configured) ||
  (current = vdtu_ep_configured   && next = vdtu_ep_active)     ||
  (current = vdtu_ep_active       && next = vdtu_ep_terminated) ||
  (current = next)

(* --------------------------------------------------------------------------
 *  vdtu_ep_state_transition — verified state transition function
 *
 *  Parameters:
 *    current  : current ep_state value
 *    next     : requested next state
 *    blocked  : true if Raft cache check returned Blocked (validator gate)
 *
 *  Returns: (success: bool, new_state: vdtu_ep_state_t)
 *    success = true  => new_state = next (transition applied)
 *    success = false => new_state = current (transition rejected)
 *
 *  The key invariant: transition to TERMINATED requires blocked = true.
 * -------------------------------------------------------------------------- *)

let vdtu_ep_state_transition
  (current: vdtu_ep_state_t)
  (next: vdtu_ep_state_t)
  (blocked: bool)
  : Tot (bool & vdtu_ep_state_t)
  =
  if current = next then
    (* Identity transition: always succeeds, no state change *)
    (true, current)
  else if next = vdtu_ep_terminated then
    (* Termination gate: must have blocked = true AND valid transition *)
    if blocked && valid_transition_low current next then
      (true, next)
    else
      (false, current)
  else
    (* Non-terminating forward transition *)
    if valid_transition_low current next then
      (true, next)
    else
      (false, current)

(* --------------------------------------------------------------------------
 *  Verification lemmas for the Low* implementation
 * -------------------------------------------------------------------------- *)

(* Terminated is absorbing: from Terminated, only identity is valid *)
let low_terminated_absorbing (next: vdtu_ep_state_t) (blocked: bool)
  : Lemma (requires (fst (vdtu_ep_state_transition vdtu_ep_terminated next blocked) = true))
          (ensures  (next = vdtu_ep_terminated))
  = ()

(* Termination requires blocked flag *)
let low_termination_requires_blocked (current: vdtu_ep_state_t)
  : Lemma (requires (current <> vdtu_ep_terminated))
          (ensures  (fst (vdtu_ep_state_transition current vdtu_ep_terminated false) = false))
  = ()

(* Successful transition to terminated implies blocked *)
let low_termination_gated (current: vdtu_ep_state_t) (blocked: bool)
  : Lemma (requires (current <> vdtu_ep_terminated /\
                      fst (vdtu_ep_state_transition current vdtu_ep_terminated blocked) = true))
          (ensures  (blocked = true))
  = ()

(* Forward transitions succeed *)
let low_unconfigured_to_configured ()
  : Lemma (ensures (vdtu_ep_state_transition vdtu_ep_unconfigured vdtu_ep_configured false
                     = (true, vdtu_ep_configured)))
  = ()

let low_configured_to_active ()
  : Lemma (ensures (vdtu_ep_state_transition vdtu_ep_configured vdtu_ep_active false
                     = (true, vdtu_ep_active)))
  = ()

let low_active_to_terminated ()
  : Lemma (ensures (vdtu_ep_state_transition vdtu_ep_active vdtu_ep_terminated true
                     = (true, vdtu_ep_terminated)))
  = ()

(* Backward transitions are rejected *)
let low_no_backward (s1 s2: vdtu_ep_state_t) (blocked: bool)
  : Lemma (requires (U32.v s2 < U32.v s1 /\ U32.v s1 <= 3 /\ U32.v s2 <= 3))
          (ensures  (fst (vdtu_ep_state_transition s1 s2 blocked) = false))
  = ()
