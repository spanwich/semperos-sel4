# CLAUDE.md — semperos-sel4 Project Context

> This file is read automatically by Claude Code at the start of every session.
> It is a living document. Claude Code updates it as tasks are completed.
> Do not treat any section as permanent — update it when reality changes.

---

## Available Skills, Agents, and Commands

USE THESE — do not search the filesystem for skill/agent files.

| Command / Agent | What it does | When to use |
|-----------------|-------------|-------------|
| `/build-isos` | Build three SemperOS XCP-ng node ISOs (dist/semperos-node{0,1,2}.iso) | Before any XCP-ng deployment. Always build fresh ISOs from the current branch. |
| `/xcpng-deploy` | Deploy ISOs to XCP-ng cluster, reboot VMs, collect output from Loki | **Every EVALUATE phase.** Do NOT skip XCP-ng and fall back to QEMU. |
| `/qemu-smoke-test` | Boot ISO/multiboot in QEMU, verify no crash | Quick sanity check after build. |
| `/slack-post` | Send a Slack message to `#semperos-ex-dev` with the required identity-tag header. | Status pings, decision asks, run results, "I posted cXXXX" follow-ups. Reduces Jira round-trip. |
| `semperos-build` agent | Build CAmkES system (QEMU or XCP-ng), diagnose build failures | Build errors, missing symbols, cmake/ninja issues. |
| `semperos-test` agent | Run standalone (10/10) + on-target (11/11) tests, check regressions | Before/after every task, regression checks. |

**Rule: If a story's EVALUATE phase requires multi-node testing, you MUST use `/xcpng-deploy`. Do NOT conclude that deployment "requires manual intervention" — you have the skill.**

**Rule: Every Slack message Claude Code sends MUST start with the identity-tag header `[Implementer · <session> · <branch>]` on its own first line, followed by a blank line.** Three speakers in `#semperos-ex-dev`: `[Architect]` = Claude on Claude.ai (design / task briefs), `[Implementer]` = Claude Code (status / results / questions — this is us), `[Ford]` = Ford himself. Multiple Implementer instances can run in parallel against the same workspace, so the tag also carries (a) an 8-char session fingerprint (first 8 hex of the session UUID — find it with `ls -t ~/.claude/projects/<sanitized-cwd>/*.jsonl | head -1`) and (b) the current git branch, both cached at first Slack post. Without the tag, the workspace posts everything under Ford's name (`U0AV409930D`) and conversations from different Claude instances become indistinguishable.

**Session-start rule:** at the start of every session — before doing any FPT-NNN work — read `#semperos-ex-dev` (`mcp__plugin_slack_slack__slack_read_channel` on `C0B09MZK02V`, limit 15) for new Architect briefs / Ford direction that may have landed since the previous session.

**Venue trichotomy** — pick by payload shape, not importance:
- **Jira comment** → long-form per-card content (FPT-NNN history, decisions, design notes that belong to one story).
- **Confluence page** (FO space, ID `2162691`) → tables, code listings >10 lines, multi-section reports, anything Ford may forward to Claude.ai for a second opinion.
- **Slack `#semperos-ex-dev`** (`C0B09MZK02V`) → in-flight chat, status pings, decision asks, pointers to the durable venues.

When in doubt, two venues are usually right: post to the durable venue (Jira/Confluence) for the record, then a one-line Slack pointer for awareness. See `.claude/skills/slack-post/SKILL.md` for the full contract (session derivation, decision rule, message shapes, STATUS block format, draft vs send).

---

## Operational Context (Load on Demand)

| File | Trigger Keywords |
|------|------------------|
| `.claude/experiment-results.md` | "benchmark", "experiment", "results", "cycles", "latency", "Hille" |
| `.claude/e1000-qemu-config.md` | "E1000", "QEMU", "MAC", "PCI", "MMIO", "IRQ", "DMA" |
| `.claude/dtu-operation-mapping.md` | "DTU operation", "SEND", "FETCH_MSG", "WAKEUP_CORE" |
| `.claude/reusable-infrastructure.md` | "HACL", "sel4_xcpng", "reuse", "sister project", "mbedTLS" |
| `.claude/jira-confluence.md` | "Jira", "story", "Confluence", "FPT-", "epic", "sprint" |

---

## Execution Constraints

### CAmkES Build Sequencing (CRITICAL)

CAmkES builds a monolithic system image. **Two stories that modify the CAmkES build graph cannot be implemented in parallel.** They must execute sequentially. **Only research/analysis tasks can run in parallel** with implementation stories.

**CAmkES sequencing: ONE implementation story at a time.** Do not start a new implementation story until the current one is merged to master and the image builds clean.

---

## Way of Work — Deferral Discipline & Wrong-Premise Protocol

This project has a hard rule against silent deferrals and silent
wrong-premise execution. Authority: Ford. Full mechanism: Confluence
page 23101474 ("Deferred-Item Ledger & Way of Work"). The two rules
that bind YOU (the Implementer) directly:

### You cannot defer. Only Ford can.

If your work surfaces something that "should be done later" — a
follow-up, future work, a known gap, a deferred fix — that is NOT a
status line you write and move past. It is a ledger event. In your
STATUS, flag it explicitly and separately (e.g. "DEFERRAL CANDIDATE:
<what> / why it can't be done now / what assumption makes deferring it
safe / what event would invalidate that assumption"). Do not bury it in
prose. Do not treat it as decided. It remains active work until Ford
explicitly approves a ledger entry. Phrases like "deferred to X",
"follow-up", "future work" in a STATUS will be extracted and surfaced
to Ford — write them as explicit deferral candidates, not as
fait-accompli.

### Wrong-Premise Protocol (mandatory when a brief premise is wrong)

If, mid-execution, you discover a premise in your brief or design input
is wrong (a file path differs, an assumed mechanism doesn't exist, the
code is structured differently than briefed, a hypothesis is refuted by
the source), DO NOT silently proceed on the wrong premise, and DO NOT
simply halt and wait. Follow all five steps:

1. **Surface** — state in STATUS: which premise, why it is wrong, the
   primary-source evidence (file:line, test output, paper section).
   Name it as a premise error.
2. **Reasoned call** — decide the most defensible way forward given the
   corrected premise; state what you chose, what you rejected, why.
3. **Build** — implement against the corrected premise, not the
   briefed-but-wrong version.
4. **Verify** — test/validate; include the verification evidence in the
   same STATUS as the surfaced premise error.
5. **Flag** — explicitly mark the STATUS as containing a premise
   correction so the Architect cannot let it pass unprocessed and it
   routes to Ford.

Boundary: this is "correct and flag", NOT "defer". Building the
corrected thing is autonomous progress — you do not need pre-approval
to do the right thing instead of the briefed-wrong thing. BUT if the
correction means the originally-briefed scope should now be postponed,
that postponement is a deferral and goes through Ford (see above). You
build the corrected thing autonomously; you do not park the original
scope autonomously.

The strongest work in this project's history (e.g. the FPT-182
archaeology that refuted three Architect hypotheses from source) was
exactly this protocol applied well. It is expected behaviour, not
exceptional.

---

## Research Context

PhD project. 6-month paper submission target, 18-month thesis target.

SemperOS is a distributed capability OS from TU Dresden that has only ever run inside gem5 simulation. This project is the first to run it on real x86_64 hardware. Four contributions:

1. **SemperOS on Real Hardware** — Ported to seL4 via CAmkES. DTU hardware replaced by software vDTU (shared memory ring buffers + CAmkES RPC).
2. **Verified vDTU** — Capability validity check formally verified using EverParse/F*/Z3. Raft BLOCK log entries as runtime policy bytes.
3. **Two-Tier Partitioned Revocation** — Fixes correctness gap in original revocation algorithm. Group-local: fast path. Spanning: Raft audit log.
4. **Secure Distributed Capability System for SD-WAN** — Three-plane SDN architecture. Capabilities ARE the management plane. Full design: `docs/secure-distributed-capability-architecture.md`.

---

## Jira

**Project:** FPT | **Epic:** FPT-133 | **Commit convention:** `FPT-<number>: <description>`

Full Jira IDs, story map, Confluence pages, and workflow: `.claude/jira-confluence.md`

### Dependency Chain

```
Spike FPT-167 (DONE)                   Spike FPT-168 (Time/RTC)
       |                                      |
FPT-155 (packet encryption)                   |
       |                                      |
FPT-156 (Raft consensus)                      |
       |                                      |
FPT-165 (Raft membership + policy)            |
       |                                      |
       +---> FPT-157 (admission)              |
       +---> FPT-158 (node revocation)        |
       +---> FPT-159 (cap revocation)         |
       +---> FPT-160 (exchange)               |
       +---> FPT-161 (VLAN)                   |
       |                                      |
       +-------> FPT-166 (time + key rotation)
                    |
          FPT-162 (802.1X) -- from FPT-157 + FPT-161
          FPT-163 (benchmarks) -- from FPT-158 + FPT-160
          FPT-164 (thesis chapter) -- from FPT-163
```

---

## Settled Architectural Decisions

| Date | Decision |
|------|----------|
| 2026-03-26 | Session protocol eliminated. Replaced by direct exchange (SyscallHandler opcode 9 + MHT + CryptoTransport). |
| 2026-03-26 | 802.1X is optional LAN hardening, NOT the admission mechanism. Admission = X.509 cert + Raft quorum. |
| 2026-03-26 | The device IS the switch. SwitchFabric is a CAmkES component inside seL4 trust boundary. |
| 2026-03-26 | Three-plane SDN model (management/control/data). |
| 2026-03-26 | Raft-replicated MHT. AddServer/RemoveServer atomically update MHT + membership + keys + VLAN + blocklist. |
| 2026-03-26 | Anti-re-admission via certificate fingerprint blocklist in Raft state. Survives log compaction. |
| 2026-03-26 | Three key layers: Ed25519 long-term, Curve25519 ephemeral, HKDF session keys. |
| 2026-03-26 | PKI: step-ca for thesis. TLS engine: mbedTLS. miTLS deferred. |
| 2026-03-26 | Transport agnostic. CryptoTransport IS the authenticated channel. No VPN tunnels. |
| 2026-03-26 | Multi-site: single global Raft for thesis. Per-site Raft is future work. |
| 2026-03-27 | CryptoTransport is NOT TLS. Custom per-packet AEAD (ChaCha20-Poly1305) inside DTUBridge. TLS only for admission (FPT-157). |
| 2026-03-27 | Raft consensus (FPT-156) split from state machine/membership (FPT-165) and time/key rotation (FPT-166). |
| 2026-03-27 | Raft heartbeats carry sender_timestamp for clock skew detection. Clock skew > 2s triggers alert. |
| 2026-03-27 | Time tolerance: sub-10s clock agreement. 30s dual-key window for key rotation. |
| 2026-03-30 | Raft library: RedisLabs/raft (BSD). Vendor into RaftPD. Tick-driven, callback I/O. Full eval: `docs/spikes/FPT-167-raft-selection.md`. |

### Implementation Guards

1. Do not modify DTUBridge CryptoTransport hooks unless working on FPT-157 or FPT-166.
2. RaftPD .idl4 must be designed from scratch — do not copy from existing .idl4 files.
3. When creating Spike cards, always update the Confluence Project Memory page with the resolution.
4. CryptoTransport is NOT TLS. Per-packet AEAD inside DTUBridge. TLS only for admission (FPT-157).
5. FPT-155 uses static pre-shared keys. Replaced by admission-derived keys in FPT-157, rotated in FPT-166.
6. Raft uses logical time. Wall-clock time NOT needed for consensus. Only FPT-166 and FPT-157 need it.
7. Design targets bare-metal (RTC for initial time), not Xen. Xen wallclock for XCP-ng demo only.
8. NTP = absolute accuracy. Raft cross-check = relative consistency. Both needed.
9. Raft: RedisLabs/raft, vendor into RaftPD, in-memory log, wire send to CryptoTransport, `raft_periodic()` with rdtsc delta.
10. HACL* submodule: `hacl-star/`. Ed25519/Curve25519/HKDF available for FPT-157. Lib_Memzero0 for FPT-166.
11. **ONE implementation story at a time.** Research spikes can run in parallel.

---

## Architecture

CAmkES components on seL4 x86_64. QEMU q35 for development, XCP-ng for evaluation.

```
seL4 (x86_64, CAmkES, pc99)
├── SemperKernel      ← SemperOS kernel (arch/sel4/ backend)
├── VDTUService       ← virtual DTU (ring buffer manager + endpoint table)
├── DTUBridge         ← cross-node transport (E1000 + lwIP UDP)
│   └── CryptoTransport  ← [PLANNED C4] HACL* AEAD
├── RaftPD            ← [PLANNED C3/C4] Raft consensus + MHT
├── SwitchFabric      ← [PLANNED C4] VLAN forwarding
├── AdmissionAgent    ← [PLANNED C4] X.509 + Raft AddServer
├── VPE0              ← test harness
└── VPE1              ← passive exchange target
```

**Three-plane SDN:** Management (Raft: MHT, membership, blocklist) | Control (SyscallHandler, AdmissionAgent, SwitchFabric, CryptoTransport, VDTUService) | Data (CryptoTransport encrypt/decrypt, E1000, DTUBridge/lwIP)

**Key design pattern:** CAmkES requires static connections; SemperOS creates endpoints dynamically. Solution: pre-allocate dataport pool in `.camkes`, vDTU assigns at runtime.

**VMM/hypervisor rejected:** trap overhead ~40,000ns vs shared memory ~100ns. Do not introduce VMM/VT-x code.

---

## Hard Constraints

- No VMM / VT-x / hypervisor code
- No Microkit (superseded by CAmkES)
- No ARM code paths (x86_64 only)
- Scenario A must not depend on Raft or two-tier revocation code
- Both test suites must pass before any task is considered complete

---

## C++ in CAmkES

SemperOS kernel is C++11. `-nostdinc++` applied. Headers replaced: `<cassert>` -> `<assert.h>`, `<functional>` -> `base/util/Functional.h`. `operator new/delete` -> musl. `<camkes.h>` not includable from C++ — use `extern "C"`.

### CAmkES Heap Fix (Critical)

CAmkES forces `LibSel4MuslcSysMorecoreBytes=0`. The `CAmkESDefaultHeapSize` cmake override does NOT propagate. Fix: `camkes_entry.c` defines 8 MiB static buffer + `__attribute__((constructor(200)))` that sets `morecore_area`/`morecore_size` before musl init (201) and CAmkES init (202).

---

## Implementation State

### Complete

| Component | Evidence |
|---|---|
| SPSC ring buffer (vdtu_ring.c) | 10/10 standalone tests |
| VDTUService (config_send/recv/recv_at/mem, invalidate/terminate_ep, wakeup_pe) | 11/11 on-target |
| Uniform 16-channel PE pools + config_recv_at (FPT-178) | 15/15 standalone + 11/11 on-target |
| ep_state in vdtu_ring_ctrl | vdtu_ring_send returns -3 on TERMINATED |
| SemperOS arch/sel4/ DTU backend (DTU.cc, ~620 lines) | All ops |
| WorkLoop + SyscallHandler (19 opcodes) | Fully dispatched |
| CapTable: CREATEGATE, EXCHANGE, REVOKE | Cross-VPE tested |
| Cross-node transport (DTUBridge: E1000 + lwIP UDP) | PING/PONG verified |
| Kernelcalls::connect() | dispatch_net_krnlc, 11/11 on-target |
| ThreadManager::wait_for() | x86_64 context switching, 2 workers |
| Verified vDTU enforcement chain (proofs/) | 4/4 F* modules, Low* extracted |
| SEMPER_BENCH_MODE gating | KLOG_V in hot paths |
| Experiment 1 + 2A benchmarks | Collected, see `.claude/experiment-results.md` |
| XCP-ng ISO build + smoke test | ISO boots via GRUB, 11/11 tests |

### Incomplete — Priority Order

| Task | Priority | Notes |
|---|---|---|
| XCP-ng physical boot + Exp 2A collection | P1 | Runbook: docs/XCPNG-RUNBOOK.md |
| Privilege enforcement on config RPCs | P2 | pe_privileged[] stored, never checked |
| m3fs as CAmkES VPE component | P2 | Needed for application benchmarks |
| Application VPEs (tar, find, SQLite, PostMark) | P2 | One component each |
| EverParse 3D spec extension (C2) | P3 | Extend HTTP gateway validator to DTU format |
| RaftLogCache (hash table, C-callable) | P3 | Wiring spec: docs/VERIFICATION-WIRING-SPEC.md |
| RaftPD CAmkES component (C3/C4) | P4 | New RaftPD.idl4 needed |
| Two-tier revocation (C3) | P4 | revoke_spanning(), TERMINATE/ACK |
| CryptoTransport (C4) | P4 | HACL* AEAD at DTUBridge.c ~777/~540 |
| AdmissionAgent (C4) | P4 | mbedTLS X.509 + Raft AddServer |
| 802.1Q VLAN (C4) | P4 | E1000 VFTA hw offload |
| revoke_by_pe_range() (C4) | P4 | RemoveServer -> revoke_all(pe_partition) |
| Raft-replicated MHT + blocklist (C4) | P4 | Design: docs/secure-distributed-capability-architecture.md |

---

## Known Limitations

- **Session protocol replaced** — direct exchange protocol instead (opcode 9 + MHT + CryptoTransport)
- **Same PE layout all nodes** — PE IDs 0-3 per node; true multi-kernel needs disjoint PE ranges
- **PING/PONG is kernel-level** — not VPE-initiated
- **Legacy dataports retained** — dtu_out/dtu_in + DTUNetIPC still wired but unused
- **Polling, not interrupt-driven** — seL4_Yield(); needs notification wakeup for multi-core
- **2 SYSC_GATEs** (of 6 available) — per-VPE recv via config_recv_at (FPT-178)
- **128 KiB kernel stack** — `kernel0._stack_size = 131072`
- **cmpxchg_mem stub** — unimplemented, not used
- **VPE1 passive** — no data channels, only CapTable
- **Plaintext inter-kernel traffic** — CryptoTransport (C4) will add AEAD
- **No admission control** — AdmissionAgent (C4) will add X.509 + Raft quorum
- **No VLAN isolation** — SwitchFabric (C4) will add VLAN forwarding

---

## Workflow Rules

### Before starting any task
1. Re-read this file — check the task is not already done or in progress.
2. Run tests via `semperos-test` agent or `cd tests && make test` (expect 10/10).

### After completing any task
1. **Run the test suite.** No regressions allowed.
2. **Update this file:** move completed items, update file locations.
3. **Write a task report** at `docs/TASKNN-REPORT.md`.

### Jira Card Lifecycle (Claude Code drives DESIGN → REVIEW)
1. **DESIGN**: Confirm approach with user. Enter plan mode if needed.
2. **IN PROGRESS**: Implement, commit, run tests. Transition when implementation is complete.
3. **EVALUATE**: Run benchmarks, deploy to XCP-ng via `/xcpng-deploy`, collect results. Transition when data collected.
4. **WRITING**: Document results on Confluence, add thesis writing notes. Transition when documentation complete.
5. **REVIEW**: Comment on Jira with summary + Confluence links. User reviews and moves to DONE.

**Always** transition the Jira card at each phase boundary. Never leave cards stale. If blocked, transition to BLOCKED with a comment explaining why.

---

## Code Style

- C: K&R style, no C99 VLAs, explicit casts, assert() on preconditions
- C++: C++11, no exceptions, no RTTI, minimal STL (no heap in kernel paths)
- Comments: explain why, not what
- New files: one-line copyright header matching existing files
