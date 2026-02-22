# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Virtual Data Transfer Unit (vDTU) prototype that virtualizes the SemperOS gem5-simulated DTU hardware on seL4/CAmkES for x86_64. Currently on **Task 04** (SemperOS kernel integration). See `DESIGN.md` for the full architecture specification and `docs/task04-kernel-integration.md` for kernel porting details.

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
| **VPE0** | Application echo server (PE 1): receives messages, sends replies | 150 |

### Connections

- **RPC:** SemperKernel → VDTUService (`VDTUConfig` interface in `interfaces/VDTUConfig.idl4`)
- **Shared Memory:** 8 message channels (4 KiB each) + 4 memory channels — pre-allocated in CAmkES, assigned dynamically by vDTU at runtime
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
| `components/SemperKernel/camkes_entry.c` | CAmkES run() → C++ kernel_start() bridge |
| `components/SemperKernel/src/kernel/arch/sel4/` | sel4 backend: DTU, Platform, VPE, kernel entry, libbase stubs |
| `components/SemperKernel/src/include/` | SemperOS headers (base/, m3/, thread/) with sel4 patches |
| `components/SemperKernel/SemperKernel.c` | Old vDTU test stub (retained for reference) |
| `components/VPE0/VPE0.c` | Echo server: attach to rings, poll, reply |
| `interfaces/VDTUConfig.idl4` | CAmkES RPC interface (9 procedures) |
| `DESIGN.md` | Full architecture specification and DTU-to-CAmkES mapping |

## DTU Operation Mapping

| DTU Operation | seL4/CAmkES Equivalent |
|---------------|------------------------|
| SEND | `vdtu_ring_send()` to shared dataport |
| FETCH_MSG | `vdtu_ring_fetch()` from shared dataport |
| ACK_MSG | `vdtu_ring_ack()` (advance tail) |
| READ/WRITE (memory EP) | Direct `memcpy()` on shared dataport |
| wait()/HLT | `seL4_Wait()` on notification |
| WAKEUP_CORE | `seL4_Signal()` on notification |
| Endpoint config | RPC to vDTU → endpoint table update + channel assignment |

## C++ in CAmkES

SemperOS kernel is C++11. CAmkES natively supports `.cc` files via `DeclareCAmkESComponent`. The seL4 toolchain applies `-nostdinc++`, so all C++ standard library headers are replaced:
- `<cassert>` → `<assert.h>`, `<cstring>` → `<string.h>`, etc.
- `<functional>` → `base/util/Functional.h` (custom `std::function`/`std::bind`/`std::move`)
- `operator new/delete` → musl `malloc`/`free` (in `cxx_runtime.cc`)
- `CAmkESDefaultHeapSize` must be ≥ 4 MiB (kernel allocates receive buffers during init)

See `docs/task04-kernel-integration.md` for full details.

## Status

Current: **Task 04 (Kernel Integration)** — sub-tasks 04a–04c complete (kernel compiles and boots).

- ~~Task 02: vDTU Prototype~~ (done)
- ~~Task 04a: C++ in CAmkES~~ (done)
- ~~Task 04b: Import kernel source~~ (done)
- ~~Task 04c: Platform + kernel entry~~ (done)
- Task 04d: DTU data path (vDTU ring buffer integration)
- Task 04e: VPE + PEManager real implementations
- Task 04f: Integration test (VPE0 syscall → kernel → reply)
- Task 05: Multi-kernel with inter-kernel channels and DDL protocol
