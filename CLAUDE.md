# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Virtual Data Transfer Unit (vDTU) prototype that virtualizes the SemperOS gem5-simulated DTU hardware on seL4/CAmkES for x86_64. **Task 07 complete** — two QEMU nodes exchange DTU messages over UDP with bidirectional PING/PONG verified, 9/9 tests pass on both nodes. See `DESIGN.md` for architecture and `TASK07-REPORT.md` for the cross-node communication report.

SemperOS source code is at `~/SemperOS`.

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
cd tests && make test
```

Compiles and runs 10 host-side unit tests (no seL4 required). Tests cover init validation, SPSC send/fetch/ack, full/empty detection, wraparound, and attach.

## Architecture

**Control plane + data plane separation.** The vDTU handles endpoint configuration via RPC; after setup, messages flow directly through shared memory ring buffers without vDTU involvement.

### Components

| Component | Role | Priority |
|-----------|------|----------|
| **VDTUService** | Control plane: endpoint table, channel assignment via RPC | 250 |
| **SemperKernel** | SemperOS kernel (PE 0): C++11, arch/sel4/ backend, vDTU data path | 200 |
| **VPE0** | First user VPE (PE 2): sends syscalls to kernel, receives replies | 200 |
| **VPE1** | Second user VPE (PE 3): passive EXCHANGE target, no dataports | 200 |
| **DTUBridge** | E1000 + lwIP UDP bridge: inter-node DTU message transport | 200 |

### Connections

- **RPC:** SemperKernel → VDTUService (`VDTUConfig` interface in `interfaces/VDTUConfig.idl4`)
- **Shared Memory:** 8 message channels (4 KiB each) + 4 memory channels — pre-allocated in CAmkES, assigned dynamically by vDTU at runtime
- **Network Transport:** 2 SPSC ring dataports (net_outbound, net_inbound) between SemperKernel and DTUBridge for kernel-to-network message handoff
- **Hardware:** E1000 MMIO (128 KiB at 0xFEB80000), IOAPIC IRQ 11, PCI config ports (0xCF8-0xCFF)
- **Notifications:** vDTU → kernel, vDTU → VPE0, kernel → vDTU (done signal)

### Ring Buffer (SPSC)

- 64-byte control header (cache-line aligned) + 4 × 512-byte slots per 4 KiB dataport
- DTU message header is exactly 25 bytes packed (`struct vdtu_msg_header`) — binary-compatible with gem5 DTU
- Full detection: `(head + 1) & mask == tail`
- x86 TSO: only compiler barriers needed, no hardware memory fences

### Key Design Pattern: Pre-allocation

CAmkES requires static connections, but SemperOS creates endpoints dynamically. Solution: pre-allocate a pool of dataports in the `.camkes` assembly, then vDTU assigns them to endpoints at runtime via `assign_msg_channel()` / `assign_mem_channel()`.

## Key Files

| File | Purpose |
|------|---------|
| `semperos-sel4.camkes` | CAmkES assembly: component wiring, dataport pool, notifications |
| `components/include/vdtu_ring.h` | Ring buffer API + DTU message header struct |
| `src/vdtu_ring.c` | Ring buffer implementation (shared across all components) |
| `components/VDTUService/VDTUService.c` | Endpoint table management, RPC handlers, channel assignment |
| `components/SemperKernel/camkes_entry.c` | CAmkES run() → C++ kernel_start() bridge + net_poll/net_ring_send |
| `components/SemperKernel/src/kernel/arch/sel4/` | sel4 backend: DTU, Platform, VPE, kernel entry, libbase stubs |
| `components/SemperKernel/src/kernel/arch/sel4/DTU.cc` | DTU operations incl. remote PE routing via net_ring_send |
| `components/SemperKernel/src/kernel/WorkLoop.cc` | Main polling loop, calls net_poll() for inbound network messages |
| `components/SemperKernel/src/include/` | SemperOS headers (base/, m3/, thread/) with sel4 patches |
| `components/DTUBridge/DTUBridge.c` | E1000 driver + lwIP UDP + ring buffer bridge (~730 lines) |
| `components/DTUBridge/e1000_hw.h` | Intel 82540EM register definitions |
| `components/DTUBridge/lwipopts.h` | lwIP configuration (UDP-only, NO_SYS=1) |
| `components/VPE0/VPE0.c` | Syscall sender: 9 tests (NOOP, CREATEGATE, REVOKE, EXCHANGE, remote) |
| `docker/docker-compose.yml` | Dual-QEMU orchestration (socket backend L2 Ethernet) |
| `docker/run-qemu.sh` | QEMU launcher with $QEMU_EXTRA passthrough |
| `docs/task04-kernel-integration.md` | Kernel porting details, architecture decisions, file map |
| `interfaces/VDTUConfig.idl4` | CAmkES RPC interface (9 procedures) |
| `DESIGN.md` | Full architecture specification and DTU-to-CAmkES mapping |
| `TASK07-REPORT.md` | Cross-node communication report with QEMU logs |

## DTU Operation Mapping

| DTU Operation | seL4/CAmkES Equivalent |
|---------------|------------------------|
| SEND | `vdtu_ring_send()` to shared dataport |
| FETCH_MSG | `vdtu_ring_fetch()` from shared dataport |
| ACK_MSG | `vdtu_ring_ack()` (advance tail) |
| READ/WRITE (memory EP) | Direct `memcpy()` on shared dataport |
| wait()/HLT | `seL4_Yield()` (cooperative scheduling on single-core QEMU) |
| WAKEUP_CORE | `seL4_Signal()` on notification |
| Endpoint config | RPC to vDTU → endpoint table update + channel assignment |

## C++ in CAmkES

SemperOS kernel is C++11. CAmkES natively supports `.cc` files via `DeclareCAmkESComponent`. The seL4 toolchain applies `-nostdinc++`, so all C++ standard library headers are replaced:
- `<cassert>` → `<assert.h>`, `<cstring>` → `<string.h>`, etc.
- `<functional>` → `base/util/Functional.h` (custom `std::function`/`std::bind`/`std::move`)
- `operator new/delete` → musl `malloc`/`free` (in `cxx_runtime.cc`)
- Kernel heap: 4 MiB static BSS buffer in `camkes_entry.c` (CAmkES's `CAmkESDefaultHeapSize` cmake override doesn't work; see heap fix below)

See `docs/task04-kernel-integration.md` for full details.

## CAmkES Heap Fix (Critical)

CAmkES forces `LibSel4MuslcSysMorecoreBytes=0` in `camkes.cmake`, disabling musl's static morecore. CAmkES provides a 1 MiB heap via its `component.common.c` template, but the `CAmkESDefaultHeapSize` cmake variable override does NOT propagate to the generated config header. Fix: `camkes_entry.c` defines a 4 MiB static buffer and a `__attribute__((constructor(200)))` function that sets `morecore_area`/`morecore_size` BEFORE musl init (priority 201) and CAmkES init (priority 202). This ensures the first malloc call uses the 4 MiB buffer.

## Known Limitations

- **No SemperOS inter-kernel protocol** — PING/PONG demo sends test messages. Kernelcalls::connect(), distributed revocation, and cross-kernel OBTAIN/DELEGATE are not implemented. These need ThreadManager (setjmp/longjmp blocking) and gate routing tables.
- **Same binary, both nodes** — Node identity derived at runtime from MAC last octet. Both kernels have ID 0, same PE layout (0-3). True multi-kernel needs disjoint PE ranges.
- **PING/PONG is kernel-level** — not VPE-initiated. Test 9 in VPE0 only verifies local NOOP + remote routing entry; actual cross-node traffic is in net_poll().
- **Legacy dataports retained** — `dtu_out`/`dtu_in` (8 KiB each) and `DTUNetIPC` RPC still wired but unused by the ring buffer path. Could be removed to save 2 seL4SharedData + 1 seL4RPCCall.
- **Polling, not interrupt-driven** — DTUBridge polls outbound ring, kernel polls inbound ring, both with seL4_Yield(). Fine for single-core QEMU; needs notification wakeup for multi-core.
- **1 SYSC_GATE configured** (not 6) to save channel budget. Sufficient for dual-VPE prototype.
- **128 KiB kernel stack** — cross-VPE revocation call depth exceeds 64 KiB. Set via `kernel0._stack_size = 131072`.
- **No memory EPs** — `read_mem`/`write_mem` are stubs.
- **`<camkes.h>` not includable from C++** — CAmkES symbols declared manually via `extern "C"`.
- **VPE1 is passive** — no shared data channels, doesn't send/receive messages. Only its CapTable is used.

## QEMU Testing

**NEVER run QEMU directly** — use Docker test infrastructure.

### Single-node

```bash
cd camkes-vm-examples/projects/semperos-sel4/docker
docker-compose run --rm node-a
```

### Dual-node (cross-node communication)

```bash
cd camkes-vm-examples/projects/semperos-sel4/docker
docker-compose up 2>&1 | tee qemu-dual.log
```

Both nodes use the same image from `build-node0/images/`. Node A listens on `:10001`, Node B connects after 3s delay. QEMU socket backend provides virtual L2 Ethernet. Key flag: `-nic none` suppresses Q35's default e1000e so our explicit e1000 (82540EM) takes PCI slot 2.

### IP addressing (runtime, from MAC)

- MAC ...:01 → IP 10.0.0.1 (Node 0), peer 10.0.0.2
- MAC ...:02 → IP 10.0.0.2 (Node 1), peer 10.0.0.1

### E1000 hardware config (CAmkES)

- PCI Bus 0, Dev 2, Fun 0. Vendor 8086, Device 100E. BAR0 at 0xFEB80000
- MMIO: 128 KiB via `seL4HardwareMMIO` (uncached)
- DMA pool: 2 MiB at paddr 0x4000000 (`dma_pool` + `simple_untyped21_pool=4`)
- IRQ: IOAPIC pin 11 via `seL4HardwareInterrupt`
- PCI config: I/O ports 0xCF8-0xCFF via `seL4HardwareIOPort`

## Status

Current: **Task 07 complete** — Cross-node communication, bidirectional PING/PONG over UDP, 9/9 tests pass on both nodes.

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
- Task 08: SemperOS inter-kernel protocol (Kernelcalls::connect, distributed revocation) — future
- Task 09: ThreadManager (setjmp/longjmp cooperative threading) — future
