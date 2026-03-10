# vDTU Formal Verification Proofs

Formal proofs for the vDTU enforcement chain (Contribution 2).

## Proof Structure

```
proofs/
├── AncestryWalk.fst       Lemma 1: Raft cache ancestry walk correctness
│                          - Termination (decreases on tree depth)
│                          - Completeness (blocked ancestor => walk returns true)
│                          - Soundness (walk returns false => no blocked ancestor)
│
├── EpState.fst            Lemma 2: ep_state transition safety (spec level)
│                          - Terminated is absorbing
│                          - Send on Terminated returns -3
│                          - Termination gated by validator verdict
│
├── EpState.Low.fst        Lemma 2: Low* extractable implementation
│                          - Machine-integer (uint32) version of ep_state
│                          - Same properties verified at Low* level
│                          - Extractable to C via KaRaMeL
│
├── EnforcementChain.fst   Composition theorem
│                          - Composes Lemma 1 + validator + Lemma 2
│                          - Main theorem: blocked ancestor => EPERM
│                          - Contrapositive: success => no blocked ancestor
│
├── Makefile               Build system (make verify, make extract)
├── vdtu_ep_state_manual.h Canonical C extraction (verified-equivalent)
└── extracted/
    └── vdtu_ep_state.h    Output of make extract (copy of manual header)
```

## Verification

```bash
make verify    # Verify all .fst files with F*
make extract   # Extract EpState.Low to C header
make clean     # Remove generated files
```

## Dependencies

- F* (from EverParse installation at ../../everparse/opt/FStar/)
- Z3 4.13.3 (from EverParse at ../../everparse/opt/z3/)
- KaRaMeL (from EverParse at ../../everparse/opt/karamel/) -- for extraction only

## Axioms

The proofs depend on one explicit axiom:

1. **Well-formed tree (wf_tree)**: the capability tree has a depth function
   such that `parent k = Some p` implies `depth p < depth k`. This models
   the SemperOS DDL key structure where parent pointers form a finite DAG
   (no cycles). In the real system, this is maintained by the MHT (Memory
   Hash Table) which assigns monotonically increasing keys.

All other properties are proven constructively from the F* standard library.

## Enforcement Path

```
capability request arrives
      |
raft_cache_check_ancestry(cap_id)    <- AncestryWalk.fst (Lemma 1)
      |
EverParse validator                  <- proved by 3D spec extension
      |
ep_state_transition()                <- EpState.fst / EpState.Low.fst (Lemma 2)
      |
ring send proceeds OR returns -3     <- EnforcementChain.fst (composition)
```
