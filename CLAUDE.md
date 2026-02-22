# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Virtual Data Transfer Unit (vDTU) prototype that virtualizes the SemperOS gem5-simulated DTU hardware on seL4/CAmkES for x86_64. This is **Task 02** in a 5-task roadmap toward full SemperOS integration. See `DESIGN.md` for the full architecture specification.

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
| **SemperKernel** | Kernel test stub (PE 0): configures endpoints, sends test messages | 200 |
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
| `components/SemperKernel/SemperKernel.c` | Kernel stub: config sequence + test message exchange |
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

## Status

Current: **Task 02 (vDTU Prototype)** — proof-of-concept with test stubs.

- Task 03: Full vDTU (notification waking, memory endpoints)
- Task 04: Real SemperOS kernel + m3 library integration
- Task 05: Multi-kernel with inter-kernel channels and DDL protocol
