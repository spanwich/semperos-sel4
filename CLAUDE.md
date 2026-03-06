# CLAUDE.md — semperos-sel4 Project Context

> This file is read automatically by Claude Code at the start of every session.
> It is a living document. Claude Code updates it as tasks are completed.
> Do not treat any section as permanent — update it when reality changes.

---

## Research Context

PhD project. 6-month paper submission target, 18-month thesis target.

SemperOS is a distributed capability OS from TU Dresden that has only ever
run inside gem5 simulation. This project is the first to run it on real
x86_64 hardware. Three contributions:

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
├── VPE0              ← test harness
├── VPE1              ← passive exchange target
└── RaftPD            ← [PLANNED] revocation audit log
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
components/VPE0/VPE0.c               <- 9-test harness
tests/                                <- standalone ring tests (host)
docker/                               <- dual-QEMU orchestration
docker/docker-compose.yml             <- dual-node services (node-a, node-b)
docker/run-qemu.sh                    <- QEMU launcher with $QEMU_EXTRA
docs/                                 <- task reports and design docs
DESIGN.md                             <- full architecture specification
TASK07-REPORT.md                      <- cross-node communication report
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
| VDTUService: config_send/recv/mem, invalidate_ep, wakeup_pe | 9/9 on-target tests |
| SemperOS arch/sel4/ DTU backend (DTU.cc, 591 lines) | All control plane ops |
| WorkLoop + SyscallHandler (19 opcodes) | Fully dispatched |
| CapTable: CREATEGATE, EXCHANGE, REVOKE | Cross-VPE tested |
| Cross-node transport (DTUBridge: E1000 + lwIP UDP) | PING/PONG verified |
| CAmkES build system (x86_64/pc99, Docker/QEMU) | Builds and runs |
| ThreadManager (local ops, single-kernel) | Syscall roundtrips work |

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

### Incomplete — Priority Order

| Task | ID | Priority | Notes |
|---|---|---|---|
| Kernelcalls::connect() — inter-kernel channel setup | Task 08 | P1 | Stub at Kernelcalls.cc:321-328 |
| ThreadManager::wait_for() — multi-kernel blocking | Task 09 | P1 | Stub; silent early-return bug |
| ep_state in vdtu_ring_ctrl struct | — | P2 | Move from DTU.cc local to shared struct |
| DTU.cc write_mem/read_mem — kernel memory EP path | — | P2 | Empty stubs at DTU.cc:403-413 |
| Privilege enforcement on config RPCs | — | P2 | pe_privileged[] stored, never checked |
| m3fs as CAmkES VPE component | — | P2 | Needed for application benchmarks |
| Application VPE components (tar, find, SQLite, PostMark) | — | P2 | One component each |
| EverParse 3D spec extension (Contribution 2) | — | P3 | Extend HTTP gateway validator |
| RaftLogCache (hash table, C-callable) | — | P3 | For vDTU enforcement point |
| RaftPD CAmkES component | — | P4 | Leader election + log replication |
| Two-tier revocation in SemperKernel (Contribution 3) | — | P4 | revoke_spanning(), TERMINATE/ACK |

---

## C++ in CAmkES

SemperOS kernel is C++11. CAmkES natively supports `.cc` files via `DeclareCAmkESComponent`. The seL4 toolchain applies `-nostdinc++`, so all C++ standard library headers are replaced:
- `<cassert>` -> `<assert.h>`, `<cstring>` -> `<string.h>`, etc.
- `<functional>` -> `base/util/Functional.h` (custom `std::function`/`std::bind`/`std::move`)
- `operator new/delete` -> musl `malloc`/`free` (in `cxx_runtime.cc`)
- Kernel heap: 4 MiB static BSS buffer in `camkes_entry.c` (CAmkES's `CAmkESDefaultHeapSize` cmake override doesn't work; see heap fix below)
- `<camkes.h>` is not includable from C++ — CAmkES symbols declared manually via `extern "C"` in DTU.cc and camkes_entry.c

See `docs/task04-kernel-integration.md` for full details.

## CAmkES Heap Fix (Critical)

CAmkES forces `LibSel4MuslcSysMorecoreBytes=0` in `camkes.cmake`, disabling
musl's static morecore. CAmkES provides a 1 MiB heap via its
`component.common.c` template, but the `CAmkESDefaultHeapSize` cmake variable
override does NOT propagate to the generated config header. Fix:
`camkes_entry.c` defines a 4 MiB static buffer and a
`__attribute__((constructor(200)))` function that sets
`morecore_area`/`morecore_size` BEFORE musl init (priority 201) and CAmkES init
(priority 202). This ensures the first malloc call uses the 4 MiB buffer.

---

## Known Limitations

- **No SemperOS inter-kernel protocol** — PING/PONG demo sends test messages. Kernelcalls::connect(), distributed revocation, and cross-kernel OBTAIN/DELEGATE are not implemented. These need ThreadManager (setjmp/longjmp blocking) and gate routing tables.
- **Same binary, both nodes** — Node identity derived at runtime from MAC last octet. Both kernels have ID 0, same PE layout (0-3). True multi-kernel needs disjoint PE ranges.
- **PING/PONG is kernel-level** — not VPE-initiated. Test 9 in VPE0 only verifies local NOOP + remote routing entry; actual cross-node traffic is in net_poll().
- **Legacy dataports retained** — `dtu_out`/`dtu_in` (8 KiB each) and `DTUNetIPC` RPC still wired but unused by the ring buffer path. Could be removed to save 2 seL4SharedData + 1 seL4RPCCall.
- **Polling, not interrupt-driven** — DTUBridge polls outbound ring, kernel polls inbound ring, both with seL4_Yield(). Fine for single-core QEMU; needs notification wakeup for multi-core.
- **1 SYSC_GATE configured** (not 6) to save channel budget. Sufficient for dual-VPE prototype.
- **128 KiB kernel stack** — cross-VPE revocation call depth exceeds 64 KiB. Set via `kernel0._stack_size = 131072`.
- **No memory EPs** — `write_mem`/`read_mem` are stubs (DTU.cc:403-413).
- **VPE1 is passive** — no shared data channels, doesn't send/receive messages. Only its CapTable is used.

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
