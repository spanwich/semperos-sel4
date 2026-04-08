# FPT-167: Spike — Raft Implementation Selection for seL4/CAmkES

**Status:** Resolved
**Date:** 2026-03-30
**Timebox:** 3 days (completed in 1)
**Blocks:** FPT-156 (Raft consensus across 3 XCP-ng nodes)

---

## Decision

**Use RedisLabs/raft** — the actively maintained BSD-licensed fork of willemt/raft
(527 commits, production-tested via RedisRaft).

Repository: https://github.com/RedisLabs/raft
License: BSD 3-Clause
Core size: ~2,500 lines of C (4 source files + 3 headers)

---

## Hard Constraints

| # | Constraint | Required |
|---|-----------|----------|
| 1 | Pure C — no C++, no Rust FFI, no exceptions | Mandatory |
| 2 | No threads — CAmkES single-threaded event loops | Mandatory |
| 3 | No filesystem — state in dataports or custom serialization | Mandatory |
| 4 | No libc beyond seL4 muslc — no glibc, no POSIX signals | Mandatory |
| 5 | No blocking I/O — async via DTUBridge lwIP UDP | Mandatory |
| 6 | License: BSD/MIT/Apache — no GPL, no LGPL | Mandatory |
| 7 | Small scale: 3–64 nodes, ~64 MHT entries | Mandatory |

---

## Evaluation Matrix

| Criterion | RedisLabs/raft | willemt/raft | canonical/raft | From Scratch |
|-----------|---------------|-------------|----------------|-------------|
| **Pure C** | Yes | Yes | Yes | Yes |
| **No threads** | Single-threaded by design | Single-threaded | Core is single-threaded; libuv backend uses threads | Yes |
| **No filesystem** | Callback-based persistence | Callback-based persistence | `raft_io` vtable — core is clean | Yes |
| **No glibc** | stdlib.h, string.h, assert.h only | Same | Same (core only) | Yes |
| **No blocking I/O** | Callback-based networking | Callback-based networking | `raft_io` vtable | Yes |
| **License** | **BSD 3-Clause** | **BSD 3-Clause** | LGPL-3.0 + linking exception | N/A |
| **Small scale** | Designed for small clusters | Same | Same | Yes |
| **Pre-vote** | **Yes** | No | Yes | Must implement |
| **Membership changes** | AddServer/RemoveServer | AddServer/RemoveServer | add/assign/remove | Must implement |
| **Pluggable log** | **Yes** (`raft_log_impl_t`) | No (internal only) | No (libuv backend) | Yes |
| **Custom allocator** | **Yes** (`raft_set_heap_functions`) | **Yes** | Yes (`raft_heap_set`) | Yes |
| **Snapshot support** | Yes (chunked) | Yes (known bugs) | Yes | Must implement |
| **Leadership transfer** | Yes | No | Yes | Must implement |
| **Code size** | ~2,500 lines | ~2,200 lines | ~9,850 lines (core) | Est. 2,500–3,500 |
| **Maintenance** | Last commit 2023-06, 527 commits | Last commit 2023-06, 363 commits | Archived 2024-03 | N/A |
| **Production use** | RedisRaft (production) | ticketd (demo) | dqlite/LXD (production) | None |
| **Verdict** | **RECOMMENDED** | Viable backup | Too heavy, license risk | Too slow for timeline |

---

## Detailed Candidate Audits

### 1. RedisLabs/raft (RECOMMENDED)

**URL:** https://github.com/RedisLabs/raft
**License:** BSD 3-Clause
**Stars:** 82 | **Commits:** 527 | **Last push:** 2023-09-08

This is the actively maintained evolution of willemt/raft, used in production
by RedisRaft (840 stars). It is 164 commits ahead of the original.

#### Networking Model — Callback-Based, Zero Socket Code

The library does zero networking. Two send callbacks in `raft_cbs_t`:

```c
typedef int (*func_send_requestvote_f)(
    raft_server_t* raft, void *user_data,
    raft_node_t* node, msg_requestvote_t* msg);

typedef int (*func_send_appendentries_f)(
    raft_server_t* raft, void *user_data,
    raft_node_t* node, msg_appendentries_t* msg);

typedef int (*func_send_snapshot_f)(
    raft_server_t* raft, void *user_data,
    raft_node_t* node, msg_snapshot_t* msg);
```

On receive, the user deserializes and calls `raft_recv_appendentries()`,
`raft_recv_requestvote()`, etc. Each `raft_node_t` carries a `void* udata`
for the user to attach addressing info.

**seL4 fit:** Wire send callbacks to DTUBridge UDP via CryptoTransport.

#### Persistence Model — Callback-Based, No File I/O

```c
typedef int (*func_persist_vote_f)(raft_server_t*, void*, raft_node_id_t);
typedef int (*func_persist_term_f)(raft_server_t*, void*, raft_term_t, raft_node_id_t);
```

Plus the `raft_log_impl_t` pluggable log vtable (12 function pointers):
`init`, `free`, `reset`, `append`, `poll`, `pop`, `get`, `get_batch`,
`first_idx`, `current_idx`, `count`, `sync`.

A built-in in-memory implementation (`raft_log_internal_impl`) is provided.
For the thesis, this is sufficient — RAM-backed log with no disk persistence.

**seL4 fit:** Use the built-in in-memory log. Implement `persist_vote` and
`persist_term` as writes to a static BSS variable.

#### Timer Model — Tick-Driven, No System Clocks

```c
int raft_periodic(raft_server_t* me, int msec_elapsed);
```

The user calls this periodically, passing milliseconds elapsed. Internally it
manages election timeouts (randomized) and heartbeat intervals. No calls to
`gettimeofday()`, `clock_gettime()`, or any OS timer API. Randomization uses
`rand()` (available in musl).

**seL4 fit:** Call from RaftPD's polling loop with rdtsc-derived delta.

#### Threading — Completely Single-Threaded

Zero mutexes, pthreads, atomics, or TLS. README states: "this library is not
thread safe." All state in a single `raft_server_private_t` struct.

**seL4 fit:** Perfect for CAmkES single-threaded component model.

#### Dependencies — Minimal Standard C

```
<stdlib.h>   — malloc/calloc/realloc/free (overridable)
<string.h>   — memcpy, memset
<stdio.h>    — vsprintf (debug log only)
<assert.h>   — assert
<stdarg.h>   — va_list (debug log only)
```

No platform-specific code, no POSIX, no networking headers, no threading.

**seL4 fit:** All available in seL4's musl. Custom allocator via
`raft_set_heap_functions()` can point to the 8 MiB BSS morecore.

#### Pre-Vote — Yes

`RAFT_STATE_PRECANDIDATE` state, `prevote` field in RequestVote messages,
`raft_is_precandidate()` API. Prevents partitioned nodes from disrupting the
cluster on rejoin.

#### Membership Changes — Single-Server, Two-Phase Add

```
AddServer:    RAFT_LOGTYPE_ADD_NONVOTING_NODE → catch-up → RAFT_LOGTYPE_ADD_NODE
RemoveServer: RAFT_LOGTYPE_REMOVE_NODE
```

Safety guard: `RAFT_ERR_ONE_VOTING_CHANGE_ONLY` — only one voting change
in-flight at a time. Maps directly to FPT-165's AddServer/RemoveServer.

#### Snapshots — Supported, Chunked

Multi-step API: `raft_begin_snapshot()` → serialize FSM → `raft_end_snapshot()`.
Chunked transfer for large state. Follower loading via
`raft_begin_load_snapshot()` → add nodes → `raft_end_load_snapshot()`.

#### Known Issues (Inherited from willemt/raft)

Issue #118 in willemt/raft documents snapshot-related bugs:
- Log inconsistency when AE has `prev_log_idx==0` after compaction
- Non-monotonic term on snapshot load (potential two-leaders-in-one-term)
- Memory leak on snapshot loading

**Mitigation:** For the thesis 3-node cluster, the MHT + membership +
blocklist state is small (< 64 entries). We can avoid log compaction entirely
during the evaluation period. Snapshots are needed only for new node catch-up
(FPT-157), which is a later story. The snapshot bugs can be audited and fixed
as part of FPT-157 implementation.

Other fixed issues in RedisLabs fork vs willemt/raft:
- `raft_send_appendentries_all` early-return bug (#79) — likely fixed
- Reference-counted entries — cleaner memory management
- Pluggable log backend — avoids the circular buffer wrap bug (#95)

---

### 2. willemt/raft (Viable Backup)

**URL:** https://github.com/willemt/raft
**License:** BSD 3-Clause
**Stars:** 1,163 | **Commits:** 363 | **Last push:** 2023-06-26

The original library. Same architecture as RedisLabs/raft but 164 commits
behind. Key differences:

| Feature | willemt/raft | RedisLabs/raft |
|---------|-------------|---------------|
| Pre-vote | No | Yes |
| Pluggable log | No | Yes (`raft_log_impl_t`) |
| Leadership transfer | No | Yes |
| Snapshot chunking | No | Yes |
| Reference-counted entries | No | Yes |
| Production deployment | ticketd (demo) | RedisRaft (production) |

**Verdict:** No reason to use this over the RedisLabs fork. Same license,
same architecture, fewer features, less testing. Viable as a fallback if the
RedisLabs fork has unexpected issues.

---

### 3. canonical/raft (Not Recommended)

**URL:** https://github.com/canonical/raft (archived), https://github.com/cowsql/raft (maintained fork)
**License:** LGPL-3.0 with static linking exception
**Stars:** 952 | **Core size:** ~9,850 lines | **Last push:** 2024-03-04

#### Architecture

Clean two-layer design: core logic (~9,850 lines, 31 files) + libuv I/O
backend (~8,050 lines, 20 files). The core is behind a `struct raft_io`
vtable with 14 function pointers (init, close, load, start, set_term,
set_vote, send, append, truncate, snapshot_put, snapshot_get, time, random,
async_work).

#### Why Not Recommended

1. **License complexity.** LGPL-3.0 with linking exception is legally viable
   for static linking but adds compliance burden. BSD is cleaner.

2. **4x larger codebase.** 9,850 lines of core vs 2,500 for RedisLabs/raft.
   More code to audit, more surface area for bugs, harder to understand fully.

3. **Archived.** The canonical repo is archived; cowsql/raft fork is maintained
   but also LGPL-3.0.

4. **Higher integration effort.** Despite the clean `raft_io` vtable, the
   implementation assumes an event-driven model (callbacks fire asynchronously)
   that maps less naturally to CAmkES's synchronous polling loop than
   RedisLabs/raft's simple `raft_periodic()` tick model.

#### What It Does Better

- Pre-vote (also in RedisLabs/raft)
- Leadership transfer (also in RedisLabs/raft)
- Parallel disk writes, optimistic pipelining (not needed for RAM-backed log)
- More mature snapshotting (relevant later for FPT-157)

---

### 4. From Scratch (Not Recommended)

#### Estimated Effort

| Component | Lines of C | Time |
|-----------|-----------|------|
| Leader election | 500–700 | 1 week |
| Log replication (in-memory) | 800–1,000 | 1–2 weeks |
| Single-server membership changes | 300–500 | 1 week |
| Snapshotting | 400–600 | 1 week |
| Message serialization | 300–400 | 3 days |
| State machine interface | 200–300 | 2 days |
| Timer/event integration | 200–300 | 2 days |
| **Total** | **2,500–3,500** | **5–8 weeks** |
| + Correctness testing | — | **4–8 weeks** |

**Total: 3–4 months** to reach confidence, consuming half the paper timeline.

#### Why Not

Raft is deceptively simple. The paper's clarity hides 50+ edge cases:

- **Figure 8 scenario:** A leader must never commit entries from previous terms
  by counting replicas. Only current-term entries can be committed directly.
  Previous-term entries are committed indirectly.

- **Configuration change timing:** Each server uses the latest configuration in
  its log, whether committed or not. Getting this wrong allows two leaders.

- **Log matching after compaction:** When a follower receives AppendEntries with
  `prevLogIndex` pointing into compacted entries, the response is different from
  a simple mismatch.

- **Leader step-down on RemoveServer:** The leader must replicate the config
  change that removes itself, then step down after commitment.

These bugs are already handled in RedisLabs/raft. Building from scratch means
rediscovering and fixing each one.

---

### 5. Other Candidates Evaluated

| Candidate | Language | License | Why Not |
|-----------|---------|---------|---------|
| postgrespro/raft | C | BSD-2 | Bakes in POSIX UDP sockets directly; not callback-based |
| etcd/raft | Go | Apache 2.0 | Go runtime cannot run on bare-metal seL4 |
| tikv/raft-rs | Rust | Apache 2.0 | Rust FFI into CAmkES cmake is a multi-week effort with no precedent |
| hashicorp/raft | Go | MPL 2.0 | Go runtime; MPL license complications |
| Verdi-Raft | Coq→OCaml | BSD-2 | Extracts to OCaml, not C; academically valuable for related work |
| IronFleet | Dafny→C# | MIT | Implements Paxos not Raft; extracts to C# |
| F*/Low* Raft | — | — | Does not exist. No F* Raft implementation found. |
| Zephyr/FreeRTOS/NuttX | — | — | No RTOS ships a Raft implementation |

**Note on Verdi-Raft:** ~50,000 lines of Coq prove linearizability of ~530
lines of Raft. Valuable as related work citation — demonstrates Raft
correctness is mechanically provable. The combination of F*-verified vDTU
enforcement (Contribution 2) with a Raft state machine (Contribution 3/4) is
novel even though the Raft implementation itself is not formally verified.

---

## Integration Plan

### RaftPD CAmkES Component

RedisLabs/raft becomes the core of `RaftPD`, a new CAmkES component.

```
RaftPD
├── raft/              ← vendored RedisLabs/raft source (BSD, ~2,500 lines)
│   ├── raft_server.c
│   ├── raft_log.c
│   ├── raft_node.c
│   ├── raft_server_properties.c
│   └── include/raft.h, raft_types.h, raft_private.h
├── RaftPD.c           ← CAmkES component: callbacks, polling loop, state machine
├── raft_transport.c   ← serialize/deserialize Raft messages for DTUBridge UDP
└── raft_state.c       ← FSM apply callback: MHT, membership, blocklist (FPT-165)
```

### Callback Implementation Map

| RedisLabs/raft Callback | seL4/CAmkES Implementation |
|------------------------|---------------------------|
| `send_requestvote` | Serialize → DTUBridge net_outbound ring → CryptoTransport → UDP |
| `send_appendentries` | Same transport path |
| `send_snapshot` | Same, chunked |
| `persist_vote` | Write to BSS variable |
| `persist_term` | Write to BSS variable |
| `applylog` | Dispatch to MHT/membership/blocklist FSM (FPT-165) |
| `log` (debug) | `ZF_LOGD()` or `KLOG()` |
| `node_has_sufficient_logs` | Trigger RAFT_LOGTYPE_ADD_NODE for pending admission |
| `raft_periodic()` caller | RaftPD polling loop, rdtsc delta for msec_elapsed |

### Transport Message Format

Raft messages serialized to a fixed binary format over DTUBridge UDP:

```
[msg_type: 1B][term: 4B][payload: variable]
```

Message types: RequestVote, RequestVoteResponse, AppendEntries,
AppendEntriesResponse, InstallSnapshot, InstallSnapshotResponse.

The CryptoTransport AEAD envelope (FPT-155) wraps the serialized Raft message
— Raft itself is unaware of encryption.

### Timer Integration

```c
// In RaftPD polling loop:
uint64_t now = rdtsc();
uint64_t elapsed_ms = (now - last_tick) / (TSC_FREQ_KHZ);
if (elapsed_ms > 0) {
    raft_periodic(raft, (int)elapsed_ms);
    last_tick = now;
}
```

Election timeout: 1000ms (default, randomized to [1000, 2000) ms).
Heartbeat interval: 200ms (default). Both configurable via
`raft_set_election_timeout()` / `raft_set_request_timeout()`.

---

## Estimated Effort

| Task | Days | Story |
|------|------|-------|
| Vendor RedisLabs/raft into RaftPD component | 1 | FPT-156 |
| Implement RaftPD CAmkES wiring + polling loop | 2 | FPT-156 |
| Implement send/recv callbacks (DTUBridge transport) | 2 | FPT-156 |
| Implement persist callbacks (RAM-backed) | 0.5 | FPT-156 |
| Message serialization/deserialization | 1 | FPT-156 |
| 3-node leader election + log replication test | 2 | FPT-156 |
| **Subtotal: FPT-156** | **8.5** | |
| MHT + membership FSM (apply callback) | 3 | FPT-165 |
| AddServer/RemoveServer integration | 2 | FPT-165 |
| Blocklist persistence across snapshots | 1 | FPT-165 |
| **Subtotal: FPT-165** | **6** | |
| **Total** | **~15 days** | |

Compare: from-scratch Raft would be ~60–80 days (3–4 months).

---

## Identified Risks

| Risk | Severity | Mitigation |
|------|----------|-----------|
| Snapshot bugs (inherited from willemt/raft #118) | Medium | Defer snapshots; small state fits in log. Audit+fix for FPT-157. |
| Library unmaintained since 2023-09 | Low | Code is stable/mature; 527 commits; BSD allows forking. |
| `rand()` seeding on seL4 | Low | Seed from rdrand instruction (available on x86_64). |
| `vsprintf` in debug log (unbounded buffer) | Low | Stub the log callback or use a bounded `vsnprintf`. |
| Reserved identifier `__raft_malloc` etc. | Low | Cosmetic; works in practice with gcc/musl. |
| CryptoTransport not yet ready (FPT-155) | Medium | Test with plaintext UDP first; add AEAD wrapper later. |
| Election timeout tuning for CAmkES scheduling | Medium | Profile on XCP-ng; adjust timeouts based on observed RTT. |

---

## References

- Ongaro, D. & Ousterhout, J. (2014). In Search of an Understandable Consensus Algorithm. USENIX ATC.
- Ongaro, D. (2014). Consensus: Bridging Theory and Practice. PhD thesis, Stanford. §4.3 (single-server membership changes), §9.6 (pre-vote).
- Ongaro, D. Raft TLA+ specification. https://github.com/ongardie/raft.tla
- Howard, H. et al. (2020). Raft Refloated: Do We Have Consensus? ACM SIGOPS.
- Verdi-Raft. https://github.com/uwplse/verdi-raft (Coq-verified Raft, BSD-2).
