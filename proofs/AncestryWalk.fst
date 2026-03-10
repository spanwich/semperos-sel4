(*
 * AncestryWalk.fst -- Lemma 1: Ancestry walk correctness for Raft cache
 *
 * Models the SemperOS capability tree (DDL keys, parent pointers) and proves
 * that ancestry_walk correctly identifies whether any ancestor of a given key
 * is in the Raft blocked-keys cache.
 *
 * The capability tree is modelled as a finite forest of keys with a partial
 * parent function. Acyclicity is encoded via a depth function that strictly
 * decreases along parent edges (well-founded relation).
 *)
module AncestryWalk

open FStar.Classical
open FStar.Set

(* --------------------------------------------------------------------------
 *  Abstract capability key type — corresponds to mht_key_t in CapTable.cc
 * -------------------------------------------------------------------------- *)

(* Keys are 64-bit DDL hashes in the real system; we abstract them as nat
   for proof purposes. The specific hash structure is irrelevant to
   ancestry correctness. *)
type key = nat

(* --------------------------------------------------------------------------
 *  Capability tree structure
 *
 *  We model the tree as two total functions on keys:
 *    parent : key -> option key     (None for roots)
 *    depth  : key -> nat            (strictly decreasing along parent edges)
 *
 *  The depth function encodes finite DAG / well-foundedness: if parent k = Some p,
 *  then depth p < depth k. This guarantees termination of the ancestry walk.
 * -------------------------------------------------------------------------- *)

(* Tree structure bundled as a record for modularity *)
noeq type cap_tree = {
  parent : key -> option key;
  depth  : key -> nat;
}

(* Well-formedness: parent edges strictly decrease depth *)
let wf_tree (t: cap_tree) : prop =
  forall (k: key) (p: key).
    t.parent k = Some p ==> t.depth p < t.depth k

(* --------------------------------------------------------------------------
 *  Ancestor relation (reflexive-transitive closure of parent)
 * -------------------------------------------------------------------------- *)

(* ancestor t a k means: a is an ancestor of k (or a = k) in tree t *)
let rec ancestor (t: cap_tree) (a: key) (k: key) : Tot prop (decreases (t.depth k)) =
  a = k \/
  (match t.parent k with
   | None   -> False
   | Some p -> t.depth p < t.depth k /\ ancestor t a p)

(* --------------------------------------------------------------------------
 *  Ancestry walk — the function we are verifying
 *
 *  Walks up the parent chain from k, checking each node against the cache.
 *  Returns true iff some ancestor (including k itself) is in the cache.
 * -------------------------------------------------------------------------- *)

let rec ancestry_walk (t: cap_tree{wf_tree t}) (k: key) (cache: set key)
  : Tot bool (decreases (t.depth k))
  =
  if mem k cache then true
  else match t.parent k with
       | None   -> false
       | Some p -> ancestry_walk t p cache

(* --------------------------------------------------------------------------
 *  Lemma (a): Termination
 *
 *  ancestry_walk terminates on any well-formed tree.
 *  This is proven structurally by the (decreases (t.depth k)) clause above.
 *  F* verifies the decreases clause automatically: when parent k = Some p,
 *  wf_tree guarantees depth p < depth k.
 *
 *  We state this as a trivially-true lemma to make the proof artifact explicit.
 * -------------------------------------------------------------------------- *)

let ancestry_walk_terminates (t: cap_tree{wf_tree t}) (k: key) (cache: set key)
  : Lemma (ensures (ancestry_walk t k cache = true \/ ancestry_walk t k cache = false))
  = ()

(* --------------------------------------------------------------------------
 *  Lemma (b): Completeness
 *
 *  If any ancestor of k is in the cache, ancestry_walk returns true.
 * -------------------------------------------------------------------------- *)

let rec ancestry_walk_complete
  (t: cap_tree{wf_tree t})
  (k: key)
  (cache: set key)
  (a: key)
  : Lemma
      (requires (ancestor t a k /\ mem a cache))
      (ensures  (ancestry_walk t k cache = true))
      (decreases (t.depth k))
  =
  if mem k cache then ()
  else
    match t.parent k with
    | None   ->
      (* k has no parent, so ancestor t a k means a = k.
         But mem k cache is false and mem a cache is true, so a <> k.
         This is contradictory with ancestor t a k when parent k = None. *)
      assert (a = k)  (* from ancestor definition: only a=k is possible *)
    | Some p ->
      (* ancestor t a k and a <> k means ancestor t a p *)
      if a = k then ()
      else ancestry_walk_complete t p cache a

(* --------------------------------------------------------------------------
 *  Lemma (c): Soundness
 *
 *  If ancestry_walk returns false, no ancestor of k is in the cache.
 *  Proven by contraposition using completeness.
 * -------------------------------------------------------------------------- *)

let ancestry_walk_sound
  (t: cap_tree{wf_tree t})
  (k: key)
  (cache: set key)
  : Lemma
      (requires (ancestry_walk t k cache = false))
      (ensures  (forall (a: key). ancestor t a k ==> ~(mem a cache)))
  =
  let aux (a: key) : Lemma (requires (ancestor t a k))
                           (ensures  (~(mem a cache)))
                     = (* by contraposition: if mem a cache were true,
                          completeness would give ancestry_walk = true,
                          contradicting our precondition *)
    Classical.move_requires (ancestry_walk_complete t k cache) a
  in
  Classical.forall_intro (Classical.move_requires aux)
