# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Virtual Data Transfer Unit (vDTU) prototype that virtualizes the SemperOS gem5-simulated DTU hardware on seL4/CAmkES for x86_64. **Task 04 complete** — real SemperOS kernel boots and processes syscalls. See `DESIGN.md` for architecture and `docs/task04-kernel-integration.md` for kernel porting details.

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
| `components/VPE0/VPE0.c` | Syscall sender: sends NOOP to kernel, receives reply |
| `docs/task04-kernel-integration.md` | Kernel porting details, architecture decisions, file map |
| `interfaces/VDTUConfig.idl4` | CAmkES RPC interface (9 procedures) |
| `DESIGN.md` | Full architecture specification and DTU-to-CAmkES mapping |

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
- `CAmkESDefaultHeapSize` must be ≥ 4 MiB (kernel allocates receive buffers during init)

See `docs/task04-kernel-integration.md` for full details.

## Known Limitations

- **Single kernel only** — ThreadManager is stubbed (single-threaded). Cooperative threading needed for multi-kernel revocation (Section 4.3.3). Safe for Tasks 04-05 since all operations are local.
- **1 SYSC_GATE configured** (not 6) to save channel budget (8 channels total). Sufficient for single-VPE prototype.
- **seL4_Yield scheduling** — kernel and VPE0 must have equal priority (200) for `seL4_Yield()` on single-core QEMU.
- **Label pass-through** — VPE0 sends `label=0`; WorkLoop looks up VPE from `senderCoreId` via PEManager.
- **64 KiB kernel stack** — revocation + logging call depth exceeds 16 KiB default. Set via `kernel0._stack_size = 65536` in CAmkES assembly.
- **No memory EPs** — `read_mem`/`write_mem` are stubs.
- **`<camkes.h>` not includable from C++** — CAmkES symbols declared manually via `extern "C"`.
- **OBTAIN/DELEGATE not tested** — requires second VPE (Task 06).

## Status

Current: **Task 05 complete** — capability operations (CREATEGATE + REVOKE) working on seL4.

- ~~Task 02: vDTU Prototype~~ (done)
- ~~Task 04: Kernel Integration~~ (done — kernel boots, WorkLoop, NOOP syscall)
- ~~Task 05a: Fix post-reply fault~~ (done — double-ack, _ack flag, EP range)
- ~~Task 05b: CREATEGATE syscall~~ (done — MsgCapability in CapTable)
- ~~Task 05c: REVOKE syscall~~ (done — capability removal, non-existent handling)
- ~~Task 05d: End-to-end test~~ (done — 5/5 tests pass, no faults)
- Task 06: Multi-VPE with OBTAIN/DELEGATE, cooperative threading
