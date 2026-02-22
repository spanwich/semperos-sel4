# Task 04: SemperOS Kernel Integration — Implementation Details

## Overview

Task 04 ports the SemperOS kernel source to compile and boot as a CAmkES
component (`SemperKernel`) on seL4 x86_64. This replaces the test stub from
Task 02/03 with the real SemperOS kernel code, using a new `arch/sel4/` backend
that substitutes gem5 DTU hardware operations with vDTU shared-memory
operations.

**Status**: Sub-tasks 04a–04c complete. Kernel compiles, links, and boots on
QEMU. Sub-tasks 04d–04f (DTU data path, VPE management, integration test)
remain.

## Sub-task 04a: C++ in CAmkES

### Problem

SemperOS is C++11 (`-fno-exceptions -fno-rtti`). CAmkES generates C entry
points and glue code. The seL4 toolchain uses `-nostdinc++`, so no C++ standard
library headers are available.

### Solution

CAmkES natively supports C++ source files in `DeclareCAmkESComponent`:

```cmake
DeclareCAmkESComponent(SemperKernel
    SOURCES  foo.c  bar.cc
    CXX_FLAGS  -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    LINKER_LANGUAGE  CXX
)
```

Key points:
- `.cc` files are detected by CMake and compiled with `g++` automatically.
- `CXX_FLAGS` are applied via `$<COMPILE_LANGUAGE:CXX>` generator expression
  (see `camkes-gen.cmake` line 374).
- `LINKER_LANGUAGE CXX` ensures the final link uses `g++` for C++ runtime
  symbols.
- The CAmkES `run()` entry point is a C function. Bridge via `extern "C"` in a
  `.c` file (`camkes_entry.c`).

### C++ Runtime (`cxx_runtime.cc`)

Provides:
- `operator new/delete` — delegates to musl libc `malloc`/`free` (not a bump
  allocator; SemperOS allocates large receive buffers via `new uint8_t[N]`).
- `__cxa_pure_virtual` — abort on pure virtual call.
- `__cxa_atexit` — no-op (kernel never exits).

### Heap Size

SemperOS `SyscallHandler` allocates `DTU::SYSC_GATES × MAX_MSG_SLOTS` receive
buffer slots during static init. With `SYSC_GATES=6` and slot size 512 B, this
requires ~1 MiB. The CAmkES default heap (`CAmkESDefaultHeapSize`) must be at
least 4 MiB:

```cmake
set(CAmkESDefaultHeapSize 4194304 CACHE STRING "" FORCE)
```

## Sub-task 04b: Importing SemperOS Source

### File Inventory

| Category | Count | Location |
|----------|-------|----------|
| Kernel `.cc` files (shared) | 22 | `src/kernel/` |
| Kernel `.h` files | 33 | `src/kernel/` |
| `base/` headers | ~50 | `src/include/base/` |
| `m3/` headers | ~50 | `src/include/m3/` |
| `thread/` headers | 3 | `src/include/thread/` |
| `arch/sel4/` backend | 6 | `src/kernel/arch/sel4/` |
| `arch/sel4/` headers | 3 | `src/include/base/arch/sel4/`, `thread/arch/sel4/` |

Total: ~170 files copied from `~/SemperOS/src/`.

### Architecture Dispatch Headers

SemperOS uses preprocessor dispatch for architecture-specific headers:

```c
// base/Config.h
#if defined(__gem5__)
#   include <base/arch/gem5/Config.h>
#elif defined(__sel4__)        // ← added
#   include <base/arch/sel4/Config.h>
#endif
```

Modified dispatch headers:
- `base/Config.h` — `#elif defined(__sel4__)`
- `base/DTU.h` — `#elif defined(__sel4__)`
- `thread/Thread.h` — `#elif defined(__sel4__)`

The `__sel4__` define is passed via `CXX_FLAGS -D__sel4__`.

### Conditional Guards in `pes/VPE.h`

`BootModule` struct and VPE static members (`count`, `mods`, `loaded`, `idles`)
were `#if defined(__gem5__)` only. Changed to
`#if defined(__gem5__) || defined(__sel4__)`.

### C++ Standard Library Replacements

Since `-nostdinc++` strips all C++ headers:

| Original | Replacement |
|----------|-------------|
| `<cassert>` | `<assert.h>` |
| `<cstring>` | `<string.h>` |
| `<cstdlib>` | `<stdlib.h>` |
| `<functional>` | `base/util/Functional.h` (custom) |
| `<algorithm>` | Removed (unused) |

### `Functional.h` — `std::function` / `std::bind` Replacement

Provides a minimal type-erased callable wrapper for `-nostdinc++` environments:

- `std::function<R(Args...)>` — 128-byte inline storage, no heap allocation for
  small captures. Supports lambdas with captures (critical for SemperOS's
  `subscribe()` pattern).
- `std::bind` — member-function binding with 2 placeholders (covers
  `Server::handle_message` pattern).
- `std::move` / `std::forward` — via custom `remove_reference` trait.
- `std::placeholders::_1`, `_2` — tag types.

Placement new is guarded with `#ifndef _PLACEMENT_NEW_DEFINED` to avoid
redefinition across translation units.

### `libbase_stubs.cc` — SemperOS Base Library Stubs

SemperOS links against `libbase.a` which provides runtime services. On seL4 we
stub these (~560 lines):

| Subsystem | Approach |
|-----------|----------|
| `m3::Heap` | Delegates to musl `malloc`/`free` |
| `m3::Serial` | Lazy-init singleton, `write()` → `putchar()` |
| `m3::OStream` | Full print methods (`printn`, `printu`, `printptr`, etc.) |
| `m3::ThreadManager` | Single-threaded stub; `yield()`/`wait_for()` are no-ops |
| `m3::Thread` | Trivial constructor; `thread_save`/`thread_resume` return immediately |
| `m3::Machine` | `shutdown()` loops forever; `write()` → `putchar` loop |
| `m3::Env` | Static instance (not at `RT_START` = 0x3000) |
| `m3::WorkLoop` | `add()`/`remove()`/`run()` are no-ops |
| `m3::Backtrace` | No-op |
| `m3::Random` | Static LCG state |
| `m3::Errors` | `last` static, `to_string()` returns code as string |

### Critical Bug Fix: `MHTPartition::_storage`

The original SemperOS code references `it->partition->_storage.length()` in
`MHTInstance::printContents()`, but `MHTPartition` has no `_storage` member.
Fixed to `it->partition->_count`.

## Sub-task 04c: Platform + Kernel Entry

### `arch/sel4/Platform.cc`

Hardcoded PE configuration:

| PE | Role | PEType |
|----|------|--------|
| 0 | SemperKernel | COMP_IMEM |
| 1 | VDTUService | COMP_IMEM |
| 2 | VPE0 | COMP_IMEM |
| 3 | VPE1 (reserved) | COMP_IMEM |

`kernel_pe()` → 0, `first_pe_id()` → 2, `kernelId` → 0.

The `KEnv` default constructor uses `memset` + hardcoded values instead of
reading from DTU hardware registers.

### `arch/sel4/kernel.cc`

Entry point bridge: `camkes_entry.c` → `run()` → `cxx_test()` → `kernel_start()`.

`kernel_start()` sequence:
1. `RecvBufs::init()` — allocates per-PE receive buffer descriptors
2. `PEManager::create()` — initializes PE pool
3. (Stub) print "Entering WorkLoop" and return

### Static Initialization Pitfalls

SemperOS uses `INIT_PRIO_USER(N)` (`__attribute__((init_priority(...)))`) for
global singletons. Initialization order:

1. DTU (prio 65434)
2. MainMemory (prio 65434)
3. Platform::_kenv (prio 65533)
4. SyscallHandler (prio 65632)
5. KernelcallHandler (prio 65632)
6. Kernelcalls (prio TBD)

Problems encountered:
- **Serial null deref**: `KLOG` calls `Serial::get()` which dereferences
  `_inst` (nullptr). Fix: lazy init in `get()`.
- **`m3::env()` null deref**: `env()` returns `reinterpret_cast<Env*>(0x3000)`
  which is unmapped on seL4. Fix: `#if defined(__sel4__)` branch returns static
  `Env` instance.
- **Heap exhaustion**: 2 MiB bump allocator insufficient. Fix: use musl
  `malloc` + 4 MiB CAmkES heap.

### `m3::env()` Override

The `base/arch/baremetal/Env.h` inline `env()` is replaced for sel4:

```cpp
#if defined(__sel4__)
Env *env();  // defined in libbase_stubs.cc
#else
static inline Env *env() { return reinterpret_cast<Env*>(RT_START); }
#endif
```

The sel4 `env()` returns a pointer to a static `Env` with a `Sel4EnvBackend`
and a static `WorkLoop`.

## Build and Test

### Build

```bash
cd camkes-vm-examples
rm -rf build-semperos && mkdir build-semperos && cd build-semperos
cmake -G Ninja -DPLATFORM=pc99 \
      -C ../projects/semperos-sel4/settings.cmake \
      ../projects/semperos-sel4
ninja
```

### Test

```bash
docker run --rm \
    -v "$(pwd)/build-semperos/images:/sel4-image:ro" \
    -e TIMEOUT=20 \
    semperos-qemu:latest
```

### Expected Output

```
=== SemperOS Kernel on seL4/CAmkES ===
[CXX] OK: new/delete/virtual dispatch all working (got 42)
[SemperKernel] Starting SemperOS kernel on seL4/CAmkES
[SemperKernel] Platform: 4 PEs, kernel PE=0
[SemperKernel] Kernel ID: 0
[SemperKernel] RecvBufs initialized
[SemperKernel] PEManager created
[SemperKernel] Entering WorkLoop
[SemperKernel] SemperOS kernel init complete (stub)
```

## File Map

### New Files (arch/sel4/ backend)

| File | Lines | Purpose |
|------|-------|---------|
| `arch/sel4/kernel.cc` | 45 | CAmkES → SemperOS entry bridge |
| `arch/sel4/DTU.cc` | 190 | DTU stubs (all ops are no-ops) |
| `arch/sel4/Platform.cc` | 105 | Hardcoded 4-PE test config |
| `arch/sel4/VPE.cc` | 120 | VPE lifecycle (from baremetal + stubs) |
| `arch/sel4/PEManager.cc` | 20 | Destructor (from baremetal) |
| `arch/sel4/libbase_stubs.cc` | ~580 | m3 base library stubs |

### New Headers (arch/sel4/)

| File | Purpose |
|------|---------|
| `base/arch/sel4/Config.h` | Page/heap/buffer constants |
| `base/arch/sel4/DTU.h` | m3::DTU class (same API, no MMIO) |
| `thread/arch/sel4/Thread.h` | Thread regs + function types |
| `base/util/Functional.h` | std::function/bind/move replacement |

### Modified SemperOS Headers

| File | Change |
|------|--------|
| `base/Config.h` | Added `__sel4__` dispatch |
| `base/DTU.h` | Added `__sel4__` dispatch |
| `thread/Thread.h` | Added `__sel4__` dispatch |
| `base/arch/baremetal/Env.h` | sel4 `env()` declaration |
| `base/stream/Serial.h` | Lazy init in `get()` |
| `base/util/Subscriber.h` | Include `Functional.h` |
| `base/Machine.h` | Include `sys/types.h` for ssize_t |
| `base/col/Treap.h` | `<cassert>` → `<assert.h>` |
| `base/util/String.h` | `<cstring>` → `<string.h>` |
| `base/stream/OStringStream.h` | `<cstdlib>/<cstring>` → C |
| `base/stream/IStringStream.h` | `<cstring>` → `<string.h>` |
| `kernel/pes/VPE.h` | `__sel4__` guards, `<cstring>` → C |
| `kernel/pes/KPE.h` | `Functional.h` include |
| `kernel/ddl/MHTInstance.h` | `Functional.h` include |
| `kernel/ddl/MHTInstance.cc` | `_storage` → `_count` bug fix |
| `m3/VPE.h` | `Functional.h` include |
| `m3/server/*.h` | `Functional.h` include |

## Remaining Work (04d–04f)

### 04d: DTU Data Path

Replace DTU stubs with vDTU ring buffer operations:
- `config_recv_local` → RPC `vdtu_config_recv()` + `init_ring()`
- `config_send_local` → RPC `vdtu_config_send()` + `attach_ring()`
- `fetch_msg` → `vdtu_ring_fetch()` on mapped channel
- `send_to` → `vdtu_ring_send()` with DTU header
- `ack_msg` → `vdtu_ring_ack()`

### 04e: VPE + PEManager

Fill in real endpoint configuration so VPEs can exchange messages with kernel.

### 04f: Integration Test

VPE0 sends a CREATE syscall → kernel WorkLoop fetches → SyscallHandler
dispatches → reply sent → VPE0 receives.
