# CLAUDE.md — semperos-sel4 Project Context

> This file is read automatically by Claude Code at the start of every session.
> It is a living document. Claude Code updates it as tasks are completed.
> Do not treat any section as permanent — update it when reality changes.

---

## Research Context

PhD project. 6-month paper submission target, 18-month thesis target.

SemperOS is a distributed capability OS from TU Dresden that has only ever
run inside gem5 simulation. This project is the first to run it on real
x86_64 hardware. Four contributions:

**Contribution 1 — SemperOS on Real Hardware**
Ported to seL4 via CAmkES component architecture. The DTU hardware is
replaced by a software vDTU (shared memory ring buffers + CAmkES RPC).
SemperOS's capability protocols, syscall handler, WorkLoop, and ThreadManager
run on real hardware for the first time.

**Contribution 2 — Verified vDTU**
The vDTU's capability validity check is formally verified using EverParse/
F*/Z3 — an extension of prior HTTP gateway work (same author, CCS '26) where
Raft BLOCK log entries play the role of runtime policy bytes. Z3 universal
quantification covers all capability states without re-verification.

**Contribution 3 — Two-Tier Partitioned Revocation**
Fixes a correctness gap in the original SemperOS revocation algorithm
(ThreadManager::wait_for() is a stub that allows early acknowledgement in
multi-kernel spanning revocations). Partitioned approach:
- Group-local revocations: original fast path (~7us), no change
- Spanning revocations: Raft audit log replaces ThreadManager blocking.
  Tier 1 return on Raft quorum commit. Tier 2 on TERMINATE ACK chain.

**Contribution 4 — Secure Distributed Capability System**
Provides the security properties that SemperOS's DTU hardware was intended
to provide, but on commodity Ethernet. Three new components:
- CryptoTransport: HACL* ChaCha20-Poly1305 AEAD on every inter-kernel packet
  (hooks into DTUBridge between ring buffer read and UDP send/recv)
- AdmissionAgent: 802.1X EAP-TLS node admission → Raft AddServer → capability minting
- Raft-replicated MHT: consensus-backed membership, PE partition assignment,
  node blocklist, and revoke_all() on RemoveServer commit (~300ms revocation)
Depends on Contributions 1-3. Full design: `docs/secure-distributed-capability-architecture.md`.
Reusable infrastructure: HACL* from `http_gateway_x86`, 802.1X/802.1Q from `sel4_xcpng`.

---

## Architecture

CAmkES components on seL4 x86_64. QEMU q35 for development, XCP-ng for
evaluation.

**Control plane + data plane separation.** The vDTU handles endpoint
configuration via RPC; after setup, messages flow directly through shared
memory ring buffers without vDTU involvement.

```
seL4 (x86_64, CAmkES, pc99)
├── VDTUService       ← virtual DTU (ring buffer manager + endpoint table)
├── SemperKernel      ← SemperOS kernel (arch/sel4/ backend)
├── DTUBridge         ← cross-node transport (E1000 + lwIP UDP)
│   └── CryptoTransport  ← [PLANNED C4] HACL* AEAD on every inter-kernel packet
├── VPE0              ← test harness
├── VPE1              ← passive exchange target
├── RaftPD            ← [PLANNED C3/C4] Raft consensus: log, MHT, blocklist
└── AdmissionAgent    ← [PLANNED C4] 802.1X EAP-TLS + VPN + Raft join
```

### Components

| Component | Role | Priority |
|-----------|------|----------|
| **VDTUService** | Control plane: endpoint table, channel assignment via RPC | 250 |
| **SemperKernel** | SemperOS kernel (PE 0): C++11, arch/sel4/ backend, vDTU data path | 200 |
| **VPE0** | First user VPE (PE 2): sends syscalls to kernel, receives replies | 200 |
| **VPE1** | Second user VPE (PE 3): passive EXCHANGE target, no dataports | 200 |
| **DTUBridge** | E1000 + lwIP UDP bridge: inter-node DTU message transport | 200 |

SemperOS talks to vDTU via CAmkES RPC (VDTUConfig.idl4, 9 procedures).
VPEs read/write shared memory ring buffer slots directly (zero-copy).
DTUBridge polls outbound ring -> UDP -> remote node for cross-kernel ops.

### Connections

- **RPC:** SemperKernel -> VDTUService (`VDTUConfig` interface in `interfaces/VDTUConfig.idl4`)
- **Shared Memory:** 8 message channels (4 KiB each) + 4 memory channels — pre-allocated in CAmkES, assigned dynamically by vDTU at runtime
- **Network Transport:** 2 SPSC ring dataports (net_outbound, net_inbound) between SemperKernel and DTUBridge for kernel-to-network message handoff
- **Hardware:** E1000 MMIO (128 KiB at 0xFEB80000), IOAPIC IRQ 11, PCI config ports (0xCF8-0xCFF)
- **Notifications:** vDTU -> kernel, vDTU -> VPE0, kernel -> vDTU (done signal)

### Ring Buffer (SPSC)

- 64-byte control header (cache-line aligned) + 4 x 512-byte slots per 4 KiB dataport
- DTU message header is exactly 25 bytes packed (`struct vdtu_msg_header`) — binary-compatible with gem5 DTU
- Full detection: `(head + 1) & mask == tail`
- x86 TSO: only compiler barriers needed, no hardware memory fences

### Key Design Pattern: Pre-allocation

CAmkES requires static connections, but SemperOS creates endpoints dynamically.
Solution: pre-allocate a pool of dataports in the `.camkes` assembly, then vDTU
assigns them to endpoints at runtime via `assign_msg_channel()` /
`assign_mem_channel()`.

**The VMM/hypervisor approach was evaluated and rejected.**
Reason: VMM trap overhead ~40,000ns vs CAmkES shared memory ~100ns —
400-1000x slower, incompatible with benchmarking. Do not introduce VMM,
VT-x, MMIO trap, or seL4_X86_VCPU code.

---

## Hard Constraints

- No VMM / VT-x / hypervisor code (evaluated and rejected — see above)
- No Microkit (superseded by CAmkES)
- No ARM code paths (x86_64 only)
- Scenario A must not depend on Raft or two-tier revocation code
- Both test suites must pass before any task is considered complete

---

## Build

### CAmkES System (QEMU x86_64)

```bash
# From camkes-vm-examples root:
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=<seL4-path>/kernel/gcc.cmake \
      -G Ninja -C ../projects/semperos-sel4/easy-settings.cmake \
      ../projects/semperos-sel4
ninja
./simulate  # Run on QEMU
```

Platform: pc99 (x86_64), simulation mode, debug build with kernel printing.

### XCP-ng ISO Build

```bash
# From camkes-vm-examples root:
rm -rf build-xcpng && mkdir build-xcpng && cd build-xcpng
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -DSEMPEROS_NO_NETWORK=ON \
    -DSEMPER_BENCH_MODE=ON \
    -C ../projects/semperos-sel4/settings.cmake \
    ../projects/semperos-sel4
ninja
../projects/semperos-sel4/scripts/make-iso.sh .
# Output: build-xcpng/semperos-sel4.iso
```

Strips DTUBridge + E1000 (not needed for local benchmarks on XCP-ng).
Log transport uses Xen's `hvm_serial=tcp:` serial redirect — no in-guest NIC.
See docs/XCPNG-RUNBOOK.md for full deployment instructions.

### Three-Node ISO Build (with DTUBridge)

Use `/build-isos` to build all three XCP-ng node ISOs with distinct identity.
Or pass a commit message to also commit+push: `/build-isos "rebuild with fix X"`.

### Standalone Ring Buffer Tests

```bash
cd tests && make test    # compiles + runs 10 host-side tests
```

---

## Key File Locations

```
CLAUDE.md                             <- this file (update it)
semperos-sel4.camkes                  <- CAmkES assembly (5 components)
CMakeLists.txt                        <- build entry point
settings.cmake                        <- seL4/CAmkES build settings (KernelX86HypervisorSupport OFF)
interfaces/VDTUConfig.idl4            <- vDTU RPC interface (9 procedures)
components/include/vdtu_ring.h        <- SPSC ring API + DTU header format
src/vdtu_ring.c                       <- ring buffer implementation
components/VDTUService/VDTUService.c  <- endpoint table + channel pool
components/SemperKernel/
  camkes_entry.c                      <- CAmkES lifecycle + net_poll()
  src/kernel/arch/sel4/DTU.cc         <- DTU backend (591 lines)
  src/kernel/arch/sel4/kernel.cc      <- seL4 kernel init
  src/kernel/cap/CapTable.cc          <- CREATEGATE / EXCHANGE / REVOKE
  src/kernel/WorkLoop.cc              <- main polling loop
  src/kernel/SyscallHandler.cc        <- 19 opcodes
  src/kernel/Kernelcalls.cc           <- inter-kernel protocol
  src/libs/thread/                    <- ThreadManager
components/DTUBridge/DTUBridge.c      <- E1000 + lwIP UDP (771 lines)
components/DTUBridge/e1000_hw.h       <- Intel 82540EM register definitions
components/DTUBridge/lwipopts.h       <- lwIP configuration (UDP-only, NO_SYS=1)
components/VPE0/VPE0.c               <- 11-test harness + Exp 1 + Exp 2A benchmarks
proofs/                               <- formal verification (F*/Low*)
proofs/AncestryWalk.fst               <- Lemma 1: ancestry walk correctness
proofs/EpState.fst                    <- Lemma 2: ep_state model
proofs/EpState.Low.fst                <- Lemma 2: Low* extraction
proofs/EnforcementChain.fst           <- Composition theorem
proofs/extracted/vdtu_ep_state.h      <- Verified C header (from Low*)
proofs/Makefile                       <- make verify / make extract
docs/VERIFICATION-WIRING-SPEC.md      <- Wiring spec for C integration
tests/                                <- standalone ring tests (host)
docker/                               <- dual-QEMU orchestration
docker/docker-compose.yml             <- dual-node services (node-a, node-b)
docker/run-qemu.sh                    <- QEMU launcher with $QEMU_EXTRA
docs/                                 <- task reports and design docs
DESIGN.md                             <- full architecture specification
TASK07-REPORT.md                      <- cross-node communication report
semperos-sel4-xcpng.camkes            <- stripped assembly for XCP-ng (no DTUBridge)
settings-xcpng.cmake                  <- XCP-ng build preset (SEMPEROS_NO_NETWORK + BENCH_MODE)
scripts/grub.cfg                      <- GRUB2 multiboot config for ISO boot
scripts/make-iso.sh                   <- grub-mkrescue wrapper -> semperos-sel4.iso
logserver/                            <- host-side TCP log receiver (socat + systemd)
docs/XCPNG-AUDIT.md                   <- sel4_xcpng reference project analysis
docs/XCPNG-RUNBOOK.md                 <- XCP-ng deployment + benchmark collection
docs/secure-distributed-capability-architecture.md <- Contribution 4 design: CryptoTransport, AdmissionAgent, Raft MHT
```

---

## DTU Operation Mapping

| DTU Operation | seL4/CAmkES Equivalent |
|---------------|------------------------|
| SEND | `vdtu_ring_send()` to shared dataport |
| FETCH_MSG | `vdtu_ring_fetch()` from shared dataport |
| ACK_MSG | `vdtu_ring_ack()` (advance tail) |
| READ/WRITE (memory EP) | Direct `memcpy()` on shared dataport |
| wait()/HLT | `seL4_Yield()` (cooperative scheduling on single-core QEMU) |
| WAKEUP_CORE | `seL4_Signal()` on notification |
| Endpoint config | RPC to vDTU -> endpoint table update + channel assignment |

---

## Implementation State

### Complete

| Component | Evidence |
|---|---|
| SPSC ring buffer (vdtu_ring.c) | 10/10 standalone tests pass |
| VDTUService: config_send/recv/mem, invalidate_ep, terminate_ep, wakeup_pe | 9/9 on-target tests |
| ep_state in vdtu_ring_ctrl (shared lifecycle field) | vdtu_ring_send returns -3 on TERMINATED |
| DTU.cc write_mem/read_mem via memory channel dataports | Bounded memcpy through config_mem EP |
| SemperOS arch/sel4/ DTU backend (DTU.cc, ~620 lines) | All control + data plane ops |
| Experiment 1 benchmark harness (6 primitives, rdtsc) | Collected 2026-03-06, see results below |
| WorkLoop + SyscallHandler (19 opcodes) | Fully dispatched |
| CapTable: CREATEGATE, EXCHANGE, REVOKE | Cross-VPE tested |
| Cross-node transport (DTUBridge: E1000 + lwIP UDP) | PING/PONG verified |
| CAmkES build system (x86_64/pc99, Docker/QEMU) | Builds and runs |
| ThreadManager (local ops, single-kernel) | Syscall roundtrips work |
| Kernelcalls::connect() — inter-kernel channel setup | Network dispatch via dispatch_net_krnlc(), 11/11 on-target |
| ThreadManager::wait_for() — cooperative multithreading | Real thread_save/resume/init with x86_64 asm, 2 worker threads |
| Verified vDTU enforcement chain (proofs/) | 4/4 F* modules verified, C header extracted via KaRaMeL |
| AncestryWalk.fst — Raft cache ancestry walk | Termination + completeness + soundness proven |
| EpState.fst + EpState.Low.fst — ep_state safety | Absorbing + send-error + termination-gated proven, Low* extracted |
| EnforcementChain.fst — composition theorem | blocked ancestor => EPERM proven end-to-end |
| SEMPER_BENCH_MODE gating (KLOG_V) | KLOG_V in SyscallHandler, CapTable, DTU, Kernelcalls |
| Experiment 2A local benchmarks (5 benchmarks) | Collected 2026-03-10, see results below |
| XCP-ng ISO build + QEMU smoke test (Task 12) | ISO boots via GRUB, 11/11 tests, benchmarks output on serial |

### Task History

- ~~Task 02: vDTU Prototype~~ (done)
- ~~Task 04: Kernel Integration~~ (done — kernel boots, WorkLoop, NOOP syscall)
- ~~Task 05a: Fix post-reply fault~~ (done — double-ack, _ack flag, EP range)
- ~~Task 05b: CREATEGATE syscall~~ (done — MsgCapability in CapTable)
- ~~Task 05c: REVOKE syscall~~ (done — capability removal, non-existent handling)
- ~~Task 05d: End-to-end test~~ (done — 5/5 tests pass, no faults)
- ~~Task 06: EXCHANGE syscall + cross-VPE revocation~~ (done — 8/8 tests pass)
- ~~Task 07: Cross-node communication~~ (done — DTUBridge + E1000 + lwIP + dual-QEMU, 9/9 tests)
  - ~~07a: Audit~~ — baseline assessment
  - ~~07b: Dual-QEMU Docker~~ — docker-compose.yml + run-qemu.sh
  - ~~07c: E1000 driver~~ — PCI detect, DMA, HW init, link up, TX/RX
  - ~~07d: lwIP UDP~~ — hello exchange between nodes
  - ~~07e: NetBridge~~ — SPSC ring buffers (net_outbound/net_inbound) + PING/PONG demo
- ~~Experiment 1: vDTU Primitive Latency~~ (done — ep_state, write_mem/read_mem, 6 benchmarks in VPE0)
- ~~Task 08: Kernelcalls::connect()~~ (done — dispatch_net_krnlc in WorkLoop.cc, net_poll dispatch in camkes_entry.c)
- ~~Task 09: ThreadManager::wait_for()~~ (done — x86_64 context switching, worker threads, 11/11 tests)
- ~~F* verification (Contribution 2 infrastructure)~~ (done — 4 modules, Low* extraction, wiring spec)
- ~~Task 10: KLOG bench-mode gating~~ (done — SEMPER_BENCH_MODE, clean Exp 2A collected)
- ~~Task 12: XCP-ng ISO port~~ (done — stripped assembly, GRUB ISO, QEMU smoke verified)

### Incomplete — Priority Order

| Task | ID | Priority | Notes |
|---|---|---|---|
| ~~Kernelcalls::connect() — inter-kernel channel setup~~ | Task 08 | ~~P1~~ | Done — dispatch_net_krnlc + net_poll dispatch |
| ~~ThreadManager::wait_for() — multi-kernel blocking~~ | Task 09 | ~~P1~~ | Done — x86_64 cooperative context switching |
| ~~KLOG bench-mode gating + Exp 2A re-run~~ | Task 10 | ~~P1~~ | Done — SEMPER_BENCH_MODE + clean numbers |
| ~~ep_state in vdtu_ring_ctrl struct~~ | Exp1 | ~~P2~~ | Done — volatile uint32_t in ring ctrl |
| ~~DTU.cc write_mem/read_mem — kernel memory EP path~~ | Exp1 | ~~P2~~ | Done — bounded memcpy via mem channel |
| XCP-ng physical boot + Exp 2A collection | Task 13 | P1 | Runbook: docs/XCPNG-RUNBOOK.md, ISO ready |
| Privilege enforcement on config RPCs | — | P2 | pe_privileged[] stored, never checked |
| m3fs as CAmkES VPE component | — | P2 | Needed for application benchmarks |
| Application VPE components (tar, find, SQLite, PostMark) | — | P2 | One component each |
| EverParse 3D spec extension (Contribution 2) | — | P3 | proofs/ infrastructure done; extend HTTP gateway validator to cover DTU message format |
| RaftLogCache (hash table, C-callable) | — | P3 | For vDTU enforcement point; wiring spec at docs/VERIFICATION-WIRING-SPEC.md |
| RaftPD CAmkES component (C3/C4) | — | P4 | Leader election, log replication, MHT state machine. New RaftPD.idl4 interface needed. |
| Two-tier revocation in SemperKernel (C3) | — | P4 | revoke_spanning(), TERMINATE/ACK |
| CryptoTransport in DTUBridge (C4) | — | P4 | HACL* ChaCha20-Poly1305 AEAD. Hook at DTUBridge.c:777 (outbound) and :540 (inbound). Reuse HACL* build pattern from http_gateway_x86. |
| AdmissionAgent CAmkES component (C4) | — | P4 | 802.1X EAP-TLS + Raft AddServer. Reuse EAP state machine from sel4_xcpng, mbedTLS X.509 from http_gateway_x86. |
| 802.1Q VLAN in DTUBridge (C4) | — | P4 | E1000 VFTA hw offload for trusted fabric / quarantine VLANs. Reuse e1000_vfta_set() from sel4_xcpng. |
| revoke_by_pe_range() in CapTable (C4) | — | P4 | Needed for RemoveServer → revoke_all(pe_partition). Current revoke_all() is per-VPE only. |
| Raft-replicated MHT + blocklist (C4) | — | P4 | ADD_SERVER, REMOVE_SERVER, MHT_UPDATE, CAP_BLOCK entries. Design at docs/secure-distributed-capability-architecture.md. |

---

## Experiment 1 Results (collected 2026-03-06)

QEMU q35, single-node Docker, 2 GHz assumed clock. 1000 warmup + 10000 measured iterations.
VDTUService success-path logging disabled (`VDTU_VERBOSE_LOG` not defined).

```
[BENCH] ring_write         min=2706     med=2814     mean=2919     max=64480    cycles  (1.4us median)
[BENCH] ring_read          min=234      med=254      mean=266      max=30298    cycles  (0.1us median)
[BENCH] ring_roundtrip     min=2900     med=3046     mean=3207     max=193798   cycles  (1.5us median)
[BENCH] ep_configure       min=47146    med=49686    mean=51604    max=1153970  cycles  (24.8us median)
[BENCH] ep_terminate       min=46146    med=48940    mean=50420    max=258398   cycles  (24.5us median)
[BENCH] mem_access         min=326      med=346      mean=392      max=232290   cycles  (0.2us median)
```

Note: Ring benchmarks use malloc'd buffer (not CAmkES dataport) due to 4 KiB
dataport size limitation (see open question 3 in TASK-EXP1-REPORT.md). RPC
benchmarks (ep_configure, ep_terminate) use real CAmkES seL4RPCCall path.
To re-enable per-RPC logging for debugging, build with `-DVDTU_VERBOSE_LOG`.

---

## Experiment 2A Results (collected 2026-03-10)

QEMU q35, single-node Docker, SEMPER_BENCH_MODE=ON, 2 GHz assumed clock.

```
[BENCH] local_exchange     med=29952944  cycles  (15.0ms median) [n=1000]
[BENCH] local_revoke       med=29985160  cycles  (15.0ms median) [n=1000]
[BENCH] chain_revoke_10    med=30089216  cycles  (15.0ms median) [n=200]
[BENCH] chain_revoke_50    med=30119794  cycles  (15.1ms median) [n=200]
[BENCH] chain_revoke_100   med=30133564  cycles  (15.1ms median) [n=200]
```

**Key finding:** All 5 benchmarks show identical ~15ms medians regardless of
chain depth. The dominant cost is the seL4_Yield() cooperative scheduling
overhead on single-core QEMU, not the capability tree walk (which adds <0.1ms
even at depth 100). On XCP-ng multi-core hardware, this will be reduced by
orders of magnitude due to notification-driven wakeup.

gem5 comparison (Hille et al. 2019): local exchange 1.8µs, local revoke 1.0µs.
The 8000x overhead is from CAmkES multi-component scheduling on single-core QEMU,
not from the vDTU data structures or algorithms.

### Exp 2A development-mode (KLOG enabled, pre-Task-10)

Initial measurements before SEMPER_BENCH_MODE investigation (included for record):
```
local_exchange:  ~29.9M cycles / ~15.0ms (identical to clean — KLOG was already runtime-gated)
local_revoke:    ~30.0M cycles / ~15.0ms
chain benchmarks: timed out at BENCH_ITERS=10000 (iteration cost ~15ms × 11K = ~165s)
```
Investigation showed KLOG was not the root cause — SYSC/KRNLC/EPS log levels
were already disabled by `KernelLog::level = INFO | ERR`. See docs/TASK10-REPORT.md.

---

## C++ in CAmkES

SemperOS kernel is C++11. CAmkES natively supports `.cc` files via `DeclareCAmkESComponent`. The seL4 toolchain applies `-nostdinc++`, so all C++ standard library headers are replaced:
- `<cassert>` -> `<assert.h>`, `<cstring>` -> `<string.h>`, etc.
- `<functional>` -> `base/util/Functional.h` (custom `std::function`/`std::bind`/`std::move`)
- `operator new/delete` -> musl `malloc`/`free` (in `cxx_runtime.cc`)
- Kernel heap: 8 MiB static BSS buffer in `camkes_entry.c` (CAmkES's `CAmkESDefaultHeapSize` cmake override doesn't work; see heap fix below)
- `<camkes.h>` is not includable from C++ — CAmkES symbols declared manually via `extern "C"` in DTU.cc and camkes_entry.c

See `docs/task04-kernel-integration.md` for full details.

## CAmkES Heap Fix (Critical)

CAmkES forces `LibSel4MuslcSysMorecoreBytes=0` in `camkes.cmake`, disabling
musl's static morecore. CAmkES provides a 1 MiB heap via its
`component.common.c` template, but the `CAmkESDefaultHeapSize` cmake variable
override does NOT propagate to the generated config header. Fix:
`camkes_entry.c` defines an 8 MiB static buffer and a
`__attribute__((constructor(200)))` function that sets
`morecore_area`/`morecore_size` BEFORE musl init (priority 201) and CAmkES init
(priority 202). This ensures the first malloc call uses the 8 MiB buffer.

---

## Known Limitations

- **Inter-kernel protocol partial** — Kernelcalls::connect() dispatch path is wired (Task 08), ThreadManager cooperative threading works (Task 09), but spanning EXCHANGE/REVOKE require session protocol (createsrv/createsess) which has never been exercised. Handlers exist (imported from SemperOS) but no test harness.
- **Same PE layout, all nodes** — Each node has PE IDs 0-3. KERNEL_ID and IPs are compile-time distinct (Task 14), but true multi-kernel needs disjoint PE ranges for the MHT to route correctly.
- **PING/PONG is kernel-level** — not VPE-initiated. Test 9 in VPE0 only verifies local NOOP + remote routing entry; actual cross-node traffic is in net_poll().
- **Legacy dataports retained** — `dtu_out`/`dtu_in` (8 KiB each) and `DTUNetIPC` RPC still wired but unused by the ring buffer path. Could be removed to save 2 seL4SharedData + 1 seL4RPCCall.
- **Polling, not interrupt-driven** — DTUBridge polls outbound ring, kernel polls inbound ring, both with seL4_Yield(). Fine for single-core QEMU; needs notification wakeup for multi-core.
- **1 SYSC_GATE configured** (not 6) to save channel budget. Sufficient for dual-VPE prototype.
- **128 KiB kernel stack** — cross-VPE revocation call depth exceeds 64 KiB. Set via `kernel0._stack_size = 131072`.
- **cmpxchg_mem stub** — `cmpxchg_mem()` is still unimplemented (not used in current prototype).
- **VPE1 is passive** — no shared data channels, doesn't send/receive messages. Only its CapTable is used.
- **Plaintext inter-kernel traffic** — DTUBridge UDP is unauthenticated and unencrypted. CryptoTransport (Contribution 4) will add HACL* AEAD on every packet.
- **No node admission control** — any node with correct IP can join. AdmissionAgent (Contribution 4) will add 802.1X + Raft AddServer.

---

## Reusable Infrastructure from Sister Projects

| Source project | What it provides | For which contribution |
|---|---|---|
| `projects/http_gateway_x86` | HACL* build integration (vendored C, KreMLin runtime, cmake pattern), mbedTLS TLS 1.2 + X.509 | C4: CryptoTransport (HACL*), AdmissionAgent (mbedTLS X.509 for EAP-TLS) |
| `projects/sel4_xcpng` | 802.1X EAP authenticator + RADIUS client, 802.1Q E1000 VLAN hw offload (VFTA), EAPOL framing, SwitchFabric VLAN policy | C4: AdmissionAgent (EAP state machine), DTUBridge VLAN filtering |

Full design: `docs/secure-distributed-capability-architecture.md`

---

## QEMU Testing

**NEVER run QEMU directly** — use Docker test infrastructure.

### Single-node

```bash
cd docker && docker compose run --rm node-a
```

### Dual-node (cross-node communication)

```bash
cd docker && docker compose up 2>&1 | tee qemu-dual.log
```

Both nodes use the same image from `build-node0/images/`. Node A listens on
`:10001`, Node B connects after 3s delay. QEMU socket backend provides virtual
L2 Ethernet. Key flag: `-nic none` suppresses Q35's default e1000e so our
explicit e1000 (82540EM) takes PCI slot 2.

### IP addressing (runtime, from MAC)

- MAC ...:01 -> IP 10.0.0.1 (Node 0), peer 10.0.0.2
- MAC ...:02 -> IP 10.0.0.2 (Node 1), peer 10.0.0.1

### E1000 hardware config (CAmkES)

- PCI Bus 0, Dev 2, Fun 0. Vendor 8086, Device 100E. BAR0 at 0xFEB80000
- MMIO: 128 KiB via `seL4HardwareMMIO` (uncached)
- DMA pool: 2 MiB at paddr 0x4000000 (`dma_pool` + `simple_untyped21_pool=4`)
- IRQ: IOAPIC pin 11 via `seL4HardwareInterrupt`
- PCI config: I/O ports 0xCF8-0xCFF via `seL4HardwareIOPort`

### ISO smoke test (XCP-ng build)

```bash
# Direct multiboot (fastest)
timeout 60 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -no-reboot \
    -kernel build-xcpng/images/kernel-x86_64-pc99 \
    -initrd build-xcpng/images/capdl-loader-image-x86_64-pc99

# ISO boot (tests GRUB chain)
timeout 120 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -boot d -no-reboot \
    -cdrom build-xcpng/semperos-sel4.iso
```

Note: requires `-machine q35` and `-serial mon:stdio`.

---

## Workflow Rules

### Before starting any task
1. Re-read this file — check the task is not already done or in progress.
2. Run the standalone test suite to establish a baseline:
   ```
   cd tests && make test      # expect 10/10
   ```
3. On-target tests (9/9) require a CAmkES build + Docker image — run when
   available via `cd docker && docker compose up`.

### After completing any task
Do all three before finishing the session:

1. **Run the test suite.** A task is not complete if the standalone suite regresses.
   On-target suite too, if a build is available.

2. **Update this file (CLAUDE.md):**
   - Move the completed item from Incomplete to Complete in the table above
   - Update any file locations or line numbers that changed
   - Add any newly discovered incomplete items to the backlog

3. **Write a task report** at `docs/TASKNN-REPORT.md` (or update existing one):
   ```
   # Task NN: [Name]
   ## What was implemented
   ## Files changed (with line ranges)
   ## Test results (paste output)
   ## Open questions or follow-on work
   ```

---

## Code Style

- C: K&R style, no C99 VLAs, explicit casts, assert() on preconditions
- C++: C++11, no exceptions, no RTTI, minimal STL (no heap in kernel paths)
- Comments: explain why, not what
- New files: one-line copyright header matching existing files
