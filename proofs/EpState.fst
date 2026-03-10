(*
 * EpState.fst -- Lemma 2: ep_state transition safety (specification level)
 *
 * Models the endpoint lifecycle state machine from vdtu_ring.h:
 *   UNCONFIGURED(0) -> CONFIGURED(1) -> ACTIVE(2) -> TERMINATED(3)
 *
 * Proves:
 *   (a) Terminated is absorbing — once Terminated, stays Terminated
 *   (b) Send safety — ring_send on Terminated returns Error (-3)
 *   (c) Termination gating — transition to Terminated requires Blocked verdict
 *)
module EpState

(* --------------------------------------------------------------------------
 *  Endpoint state — matches vdtu_ring.h constants
 *    VDTU_EP_UNCONFIGURED  0
 *    VDTU_EP_CONFIGURED    1
 *    VDTU_EP_ACTIVE        2
 *    VDTU_EP_TERMINATED    3
 * -------------------------------------------------------------------------- *)

type ep_state =
  | Unconfigured
  | Configured
  | Active
  | Terminated

(* Integer encoding (matches C #defines) *)
let ep_state_to_u32 (s: ep_state) : nat =
  match s with
  | Unconfigured -> 0
  | Configured   -> 1
  | Active       -> 2
  | Terminated   -> 3

(* --------------------------------------------------------------------------
 *  Valid transitions
 *
 *  The state machine is linear: U -> C -> A -> T
 *  Identity transitions are always valid (no-op).
 *  Backward transitions are invalid.
 * -------------------------------------------------------------------------- *)

let valid_transition (s: ep_state) (s': ep_state) : bool =
  match s, s' with
  | Unconfigured, Configured -> true
  | Configured, Active       -> true
  | Active, Terminated       -> true
  | _ -> s = s'    (* identity transitions always valid *)

(* --------------------------------------------------------------------------
 *  Validator result — models the EverParse/Raft cache verdict
 * -------------------------------------------------------------------------- *)

type validator_result =
  | Allowed   (* no blocked ancestor found *)
  | Blocked   (* ancestor found in Raft cache *)

(* --------------------------------------------------------------------------
 *  Ring send result — models vdtu_ring_send() return value
 * -------------------------------------------------------------------------- *)

type send_result =
  | SendOk
  | SendError of int    (* error code: -3 for terminated *)

(* Abstract payload type — we do not model payload contents *)
type payload = unit

(* --------------------------------------------------------------------------
 *  ring_send model
 *
 *  Matches vdtu_ring.c lines 59-60:
 *    if (ring->ctrl->ep_state == VDTU_EP_TERMINATED)
 *        return -3;
 * -------------------------------------------------------------------------- *)

let ring_send (state: ep_state) (_p: payload) : send_result =
  if state = Terminated then SendError (-3)
  else SendOk

(* --------------------------------------------------------------------------
 *  ep_state_transition — gated state transition
 *
 *  The transition to Terminated is only permitted when the validator
 *  has returned Blocked. All other valid transitions proceed unconditionally.
 *
 *  Returns true if the transition was applied, false if rejected.
 * -------------------------------------------------------------------------- *)

let ep_state_transition (s: ep_state) (next: ep_state) (v: validator_result) : (bool & ep_state) =
  if s = next then
    (* Identity — always valid, no change *)
    (true, s)
  else if next = Terminated then
    (* Termination gate: only proceed if validator says Blocked *)
    if v = Blocked then
      if valid_transition s next then (true, next)
      else (false, s)
    else (false, s)
  else
    (* Non-terminating forward transition — no validator gate *)
    if valid_transition s next then (true, next)
    else (false, s)

(* --------------------------------------------------------------------------
 *  Lemma (a): Terminated is absorbing
 *
 *  Once in Terminated state, the only valid transition is the identity
 *  (staying in Terminated). No other state is reachable.
 * -------------------------------------------------------------------------- *)

let terminated_absorbing (s': ep_state)
  : Lemma (requires (valid_transition Terminated s'))
          (ensures  (s' = Terminated))
  = ()

(* Stronger form: forall quantified *)
let terminated_absorbing_forall ()
  : Lemma (ensures (forall (s': ep_state).
                      valid_transition Terminated s' ==> s' = Terminated))
  = ()

(* --------------------------------------------------------------------------
 *  Lemma (b): Send safety
 *
 *  ring_send on a Terminated endpoint always returns Error (-3).
 *  Matches the C code: vdtu_ring_send returns -3 on VDTU_EP_TERMINATED.
 * -------------------------------------------------------------------------- *)

let send_terminated_returns_error (p: payload)
  : Lemma (ensures (ring_send Terminated p = SendError (-3)))
  = ()

(* --------------------------------------------------------------------------
 *  Lemma (c): Termination is gated
 *
 *  The ep_state_transition function only moves to Terminated when the
 *  validator verdict is Blocked. If the validator says Allowed, the
 *  transition is rejected.
 * -------------------------------------------------------------------------- *)

let termination_gated (s: ep_state) (v: validator_result)
  : Lemma (requires (s <> Terminated /\
                      fst (ep_state_transition s Terminated v) = true))
          (ensures  (v = Blocked))
  = ()

(* Contrapositive: if Allowed, cannot reach Terminated via transition *)
let termination_blocked_on_allowed (s: ep_state)
  : Lemma (requires (s <> Terminated))
          (ensures  (fst (ep_state_transition s Terminated Allowed) = false))
  = ()
