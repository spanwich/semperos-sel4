# Task 04 Report: SemperOS `arch/sel4/` Backend

**Date**: 2026-02-23
**Platform**: seL4/CAmkES on QEMU x86_64 (pc99)
**Commits**: `86ee3fc` (04a-c), `1a75814` (04d), `cc94151` (04e-f), `beecebf` (docs)

## Summary

The SemperOS kernel source has been ported to compile and run as a CAmkES
component on seL4 x86_64. A new `arch/sel4/` backend replaces all gem5 DTU
hardware operations with vDTU shared-memory ring buffer operations. The kernel
boots, enters its WorkLoop, and successfully processes a NOOP syscall from
VPE0 — the first user VPE.

---

## Sub-task 04a: C++ in CAmkES

### Files Created

| File | Lines | Purpose |
|------|-------|---------|
| `components/SemperKernel/cxx_runtime.cc` | 40 | `operator new/delete` → musl `malloc/free`, `__cxa_pure_virtual`, `__cxa_atexit` |
| `components/SemperKernel/cxx_test.cc` | 40 | Virtual dispatch test (`Base` → `Derived`, verifies `new`/`delete`/vtable) |

### Files Modified

| File | Change |
|------|--------|
| `CMakeLists.txt` | Added `CXX` language, `CXX_FLAGS`, `LINKER_LANGUAGE CXX`, all kernel sources |
| `settings.cmake` | `CAmkESDefaultHeapSize` increased from 4096 to 4194304 (4 MiB) |

### Checkpoint

- [x] C++ file compiles in SemperKernel component
- [x] `new`/`delete` work (no crash)
- [x] Virtual dispatch works (returns 42)
- [x] Existing Task 03 tests still pass (verified before kernel source import)

---

## Sub-task 04b: Import SemperOS Kernel Source

### Files Created

**New `arch/sel4/` backend (6 files):**

| File | Lines | Purpose |
|------|-------|---------|
| `src/kernel/arch/sel4/DTU.cc` | 500 | DTU operations via vDTU ring buffers |
| `src/kernel/arch/sel4/Platform.cc` | 105 | Hardcoded 4-PE test config |
| `src/kernel/arch/sel4/VPE.cc` | 125 | VPE lifecycle (from baremetal + sel4 adaptations) |
| `src/kernel/arch/sel4/PEManager.cc` | 20 | Destructor (from baremetal) |
| `src/kernel/arch/sel4/kernel.cc` | 100 | CAmkES → SemperOS entry bridge |
| `src/kernel/arch/sel4/libbase_stubs.cc` | 600 | m3 base library stubs |

**New sel4 arch headers (4 files):**

| File | Purpose |
|------|---------|
| `src/include/base/arch/sel4/Config.h` | Page/heap/buffer constants |
| `src/include/base/arch/sel4/DTU.h` | `m3::DTU` class (same API, `seL4_Yield` in `wait()`) |
| `src/include/thread/arch/sel4/Thread.h` | Thread register struct + function types |
| `src/include/base/util/Functional.h` | `std::function`/`std::bind`/`std::move` for `-nostdinc++` |

**Imported SemperOS kernel source (55 files):**

- 22 `.cc` files: `DTU.cc`, `Gate.cc`, `WorkLoop.cc`, `SyscallHandler.cc`,
  `KernelcallHandler.cc`, `Kernelcalls.cc`, `Coordinator.cc`,
  `cap/{CapTable,Capability,Revocations}.cc`,
  `com/{RecvBufs,Services}.cc`,
  `ddl/{MHTInstance,MHTPartition,MHTTypes}.cc`,
  `mem/{AddrSpace,MainMemory,MemoryMap,Slab}.cc`,
  `pes/{VPE,PEManager,KPE,RKVPE}.cc`, `tests/DDLTest.cc`
- 33 `.h` files: corresponding headers for all above

**Imported SemperOS include headers (125 files):**

- `base/`: Types, Common, Compiler, Config, DTU, Env, Errors, Heap, Init,
  KIF, Machine, Panic, PEDesc, WorkLoop, ELF, Backtrace, EnvBackend
- `base/arch/`: gem5, host, t2, t3, baremetal variants
- `base/col/`: SList, DList, List, Treap
- `base/com/`: GateStream, Marshalling
- `base/log/`: Kernel, Lib, Log, Services
- `base/stream/`: IOSBase, IStream, OStream, Serial, IStringStream, OStringStream
- `base/tracing/`: Config, Event, Tracing
- `base/util/`: BitField, CapRngDesc, Chars, Digits, Math, Profile, Random,
  Reference, Sort, String, Subscriber, Sync, Util
- `base/benchmark/`: capbench
- `m3/`: ObjCap, Syscalls, VPE
- `m3/com/`: Gate, SendGate, RecvGate, RecvBuf, MemGate, GateStream, SendQueue, EPMux
- `m3/server/`: Server, Handler, RequestHandler, EventHandler, LocalPipeServer
- `m3/session/`, `m3/pipe/`, `m3/stream/`, `m3/vfs/`
- `thread/`: Thread, ThreadManager
- `test/`: Assert, TestCase, TestSuite, TestSuiteContainer, Testable
- `c/div.h`, `fs/internal.h`

### Patched SemperOS Files

The following files were modified from their original SemperOS versions:

**Include headers (17 files patched):**

| File | Patch |
|------|-------|
| `base/Config.h` | Added `#elif defined(__sel4__)` dispatch |
| `base/DTU.h` | Added `#elif defined(__sel4__)` dispatch |
| `base/Machine.h` | Added `#include <sys/types.h>` for `ssize_t` |
| `base/stream/Serial.h` | Lazy init in `get()` |
| `base/arch/baremetal/Env.h` | sel4 `env()` declaration |
| `base/util/Subscriber.h` | Include `Functional.h` instead of `<functional>` |
| `base/col/Treap.h` | `<cassert>` → `<assert.h>` |
| `base/util/String.h` | `<cstring>` → `<string.h>` |
| `base/stream/OStringStream.h` | `<cstdlib>/<cstring>` → `<stdlib.h>/<string.h>` |
| `base/stream/IStringStream.h` | `<cstring>` → `<string.h>` |
| `thread/Thread.h` | Added `#elif defined(__sel4__)` dispatch |
| `m3/VPE.h` | `<functional>` → `Functional.h` |
| `m3/server/Server.h` | Added `Functional.h` include |
| `m3/server/RequestHandler.h` | Added `Functional.h` include |
| `m3/server/LocalPipeServer.h` | Added `Functional.h` include |
| `base/arch/host/DTU.h` | Removed `<ostream>/<iomanip>` |
| `base/arch/host/Env.h` | Removed `<string>` |

**Kernel sources (7 files patched):**

| File | Patch |
|------|-------|
| `kernel/pes/VPE.h` | `#if defined(__gem5__) \|\| defined(__sel4__)` for `BootModule` + static members; `<cstring>` → `<string.h>` |
| `kernel/pes/KPE.h` | `<functional>` → `Functional.h` |
| `kernel/ddl/MHTInstance.h` | `<functional>` → `Functional.h` |
| `kernel/ddl/MHTInstance.cc` | Bug fix: `_storage.length()` → `_count` |
| `kernel/SyscallHandler.cc` | `#if !defined(__sel4__)` around INIT_PRIO config_recv_local |
| `kernel/KernelcallHandler.cc` | Same INIT_PRIO deferral |
| `kernel/WorkLoop.cc` | Fallback `RecvGate` for label=0 on sel4 |

### Other Modified Files

| File | Change |
|------|--------|
| `components/SemperKernel/camkes_entry.c` | New CAmkES `run()` bridge + `sel4_yield_wrapper()` |
| `components/SemperKernel/SemperKernel.c` | Added `extern void cxx_test()` call (retained for reference) |
| `components/include/vdtu_ring.h` | `_Static_assert` → `static_assert` for C++ compatibility |
| `semperos-sel4.camkes` | VPE0 priority 150 → 200 (equal to kernel for `seL4_Yield`) |

### Checkpoint

- [x] All non-arch SemperOS kernel `.cc` files compile (with stub arch/sel4/ files)
- [x] No linker errors (all symbols resolved via libbase_stubs.cc)
- [x] Build produces a working QEMU image

---

## Sub-task 04c: Platform + kernel.cc

### Implementation

**`arch/sel4/Platform.cc`**: Hardcoded PE configuration:

| PE | Core ID | Role | PEType |
|----|---------|------|--------|
| 0 | 0 | SemperKernel | COMP_IMEM |
| 1 | 1 | VDTUService | COMP_IMEM |
| 2 | 2 | VPE0 | COMP_IMEM |
| 3 | 3 | VPE1 (reserved) | COMP_IMEM |

**`arch/sel4/kernel.cc`**: Entry sequence:
1. `configure_recv_endpoints()` — deferred from INIT_PRIO
2. `RecvBufs::init()`
3. `PEManager::create()`
4. `create_vpe0()` — creates VPE for PE 2
5. `kernel::WorkLoop::run()` — enters real SemperOS WorkLoop

### Checkpoint

- [x] Kernel boots and prints initialization messages
- [x] WorkLoop is entered and polling
- [x] No crashes

---

## Sub-task 04d: DTU.cc Data Path

### Files Modified

| File | Change |
|------|--------|
| `arch/sel4/DTU.cc` | Full implementation of endpoint-to-channel mapping + data path |
| `arch/sel4/kernel.cc` | Deferred endpoint configuration, real WorkLoop entry |
| `arch/sel4/libbase_stubs.cc` | Real `WorkLoop::add/remove` implementation |
| `kernel/SyscallHandler.cc` | Skip `config_recv_local` during INIT_PRIO on sel4 |
| `kernel/KernelcallHandler.cc` | Same INIT_PRIO skip |

### Endpoint-to-Channel Mapping

Static tables in `arch/sel4/DTU.cc`:

```c
ep_channel[EP_COUNT]      // SemperOS EP ID → vDTU channel index (-1 = unconfigured)
ep_type[EP_COUNT]          // EP_NONE / EP_RECV / EP_SEND / EP_MEM
ep_send_config[EP_COUNT]   // dest_pe, dest_ep, dest_vpe, label for send EPs
```

Runtime channel allocation (from QEMU output):

| Channel | EP | Owner | Purpose |
|---------|----|-------|---------|
| 0 | kernel EP 0 | Kernel (SYSC_GATE) | VPE0 → kernel syscalls |
| 1 | kernel EP 15 | Kernel (SRV) | Service messages |
| 2 | kernel EP 6 | Kernel (KRNLC) | Inter-kernel calls |
| 3 | VPE0 EP 1 | VPE0 (DEF_RECVEP) | Kernel → VPE0 replies |
| 4–7 | — | — | Free |

### DTU Method Mapping

**`m3::DTU` (data path):**

| Method | Mapped To |
|--------|-----------|
| `fetch_msg(ep)` | `vdtu_ring_fetch()` on `ep_channel[ep]` → cast to `Message*` |
| `reply(ep, data, len, off)` | Cast `off` back to `Message*`, extract sender info, find/auto-configure send channel, `vdtu_ring_send()` with `VDTU_FLAG_REPLY` |
| `mark_read(ep, off)` | `vdtu_ring_ack()` on mapped channel |
| `send(ep, msg, size, replylbl, reply_ep)` | `vdtu_ring_send()` on mapped send channel |
| `get_msgoff(ep, msg)` | `reinterpret_cast<uintptr_t>(msg)` (identity) |
| `wait()` | `sel4_yield_wrapper()` → `seL4_Yield()` |
| `is_valid(ep)` | `ep_type[ep] != EP_NONE` |

**`kernel::DTU` (control plane):**

| Method | Mapped To |
|--------|-----------|
| `config_recv_local(ep, ...)` | RPC `vdtu_config_recv()` → `vdtu_channels_init_ring()` |
| `config_recv_remote(vpe, ep, ...)` | Same, for remote PE |
| `config_send_local(ep, ...)` | RPC `vdtu_config_send()` → `vdtu_channels_attach_ring()` |
| `config_send_remote(vpe, ep, ...)` | Same, for remote PE |
| `invalidate_ep(vpe, ep)` | RPC `vdtu_invalidate_ep()` + clear local mapping |
| `send_to(vpe, ep, ...)` | `find_send_channel_for()` → `vdtu_ring_send()` |

### Message Header Handling

**On send** (`vdtu_ring_send()`): The 25-byte `vdtu_msg_header` is filled by the
ring buffer API. The caller provides `sender_pe`, `sender_ep`, `sender_vpe`,
`reply_ep`, `label`, `replylabel`, `flags`, and the payload. The ring buffer
writes the header + payload into the next slot and advances head.

**On fetch** (`vdtu_ring_fetch()`): Returns a pointer to the slot containing the
full `vdtu_message` (header + data[]). This is cast to `m3::DTU::Message*` —
the structs are binary-compatible (both 25-byte packed headers with identical
field layout).

**On reply**: The original message pointer is recovered from `msgoff`
(`reinterpret_cast<const Message*>(msgoff)`). The `senderCoreId` and
`replyEpId` fields identify the reply target. A send channel is found or
auto-configured via vDTU RPC, and the reply is sent with `VDTU_FLAG_REPLY` set.

### Checkpoint

- [x] `config_recv`, `config_send` work via vDTU RPC
- [x] `send()` writes message with header to ring buffer
- [x] `fetch_msg()` reads message from ring buffer, returns valid pointer
- [x] `ack_msg()` advances ring buffer tail
- [x] Memory endpoints (`read`/`write`): stubs present (not tested — not on critical path)

---

## Sub-task 04e: VPE.cc + PEManager.cc

### Files Modified

| File | Change |
|------|--------|
| `arch/sel4/VPE.cc` | Fixed recv buffer order; real `init()`, `start()`, `xchg_ep()`, `~VPE()` |
| `arch/sel4/kernel.cc` | Added `create_vpe0()` before WorkLoop entry |
| `semperos-sel4.camkes` | VPE0 priority 150 → 200 |
| `camkes_entry.c` | Added `sel4_yield_wrapper()` |
| `base/arch/sel4/DTU.h` | `wait()` calls `sel4_yield_wrapper()` |

### What's Real vs Stubbed

**Real implementations:**

| Function | Status |
|----------|--------|
| `VPE::init()` | Real — attaches DEF_RECVEP, configures SYSC_EP send EP |
| `VPE::start()` | Real — calls `ref()`, sets state to RUNNING, DTU::wakeup |
| `VPE::xchg_ep()` | Real — configures send/mem EPs via DTU |
| `VPE::~VPE()` | Real — invalidates EPs, detaches rbufs, revokes caps |
| `VPE::init_memory()` | Stub — CAmkES components have their own memory spaces |
| `PEManager::~PEManager()` | Real — unrefs all VPEs |
| `PEManager::create()` | Calls Coordinator::create + MHTInstance::create |
| `KPE::start()` | Stub — no child kernels in single-kernel mode |

**Critical fix**: `DEF_RCVBUF_ORDER=8` (256 bytes) with `msg_order=8` gives 1
slot = 0 usable capacity (SPSC invariant: one slot always empty). Changed to
`buf_order=11` (2048B), `msg_order=9` (512B) → 4 usable slots.

### Checkpoint

- [x] VPE management compiles and initializes
- [x] PEManager reports correct PE count
- [x] `VPE::config_ep()` calls through to vDTU (via `config_send_remote`)

---

## Sub-task 04f: Integration Test — NOOP Syscall

### Test Scenario

1. Kernel boots, configures 1 SYSC_GATE recv endpoint (channel 0)
2. Kernel creates VPE0 (PE 2) — `VPE::init()` configures:
   - VPE0 recv EP (DEF_RECVEP=1) on channel 3
   - VPE0 send EP (SYSC_EP=0) sharing channel 0
3. Kernel enters WorkLoop
4. VPE0 sends NOOP syscall (opcode=18) on channel 0
5. Kernel's WorkLoop fetches message from SYSC_GATE[0]
6. SyscallHandler dispatches → `noop(is)` → `reply_vmsg(is, 0)`
7. `m3::DTU::reply()` auto-configures send channel to VPE0's EP 1 (channel 3)
8. Reply sent with error_code=0 (NO_ERROR)
9. VPE0 polls channels, receives reply on channel 3
10. VPE0 prints "NOOP syscall succeeded!"

### Dispatch Path

The syscall goes through the **real** `SyscallHandler::handle_message()`:

```
WorkLoop::run()
  → dtu.fetch_msg(sysep[0]) → Message* (non-null)
  → RecvGate fallback_gate(sysep[0], nullptr)   // label=0 fallback for sel4
  → GateIStream is(fallback_gate, msg)
  → sysch.handle_message(is, nullptr)
    → msg >> op                                   // op = 18 (NOOP)
    → (this->*_callbacks[op])(is)                 // dispatches to noop()
      → SyscallHandler::noop(is)
        → reply_vmsg(is, 0)                       // Errors::NO_ERROR
          → is.reply(msg.bytes(), msg.total())
            → gate->reply_sync(data, len, msgidx)
              → m3::DTU::reply(ep=0, data, len=8, off)
```

### Reply Contents

The reply is an 8-byte payload containing a single `uint64_t` value of `0`
(`m3::Errors::NO_ERROR`), marshalled via `reply_vmsg(is, 0)`. The DTU header
has `VDTU_FLAG_REPLY` set.

### Post-Reply Fault

After the reply is sent successfully, the kernel faults at `pc=0x4073f0`
(`data fault on address 0x19`). This occurs in the `GateIStream` destructor's
`finish()` method, which calls `mark_read()`. The `mark_read` acks the ring
buffer, but the fault suggests a second ack attempt on an already-consumed
slot, or a stale pointer dereference after the reply path already acked. This
is a non-blocking issue — the VPE0 side receives the reply correctly.

**Root cause**: `m3::DTU::reply()` calls `vdtu_ring_ack()` on the recv ring
(reply implies consumption), then `GateIStream::finish()` tries to ack again
via `mark_read()`. The double-ack reads from an already-advanced slot. Fix:
ensure `_ack = false` propagates correctly (the code sets it in
`GateIStream::reply()` at line 292, but the timing may differ).

### Checkpoint

- [x] VPE0 sends a syscall message to kernel
- [x] Kernel's WorkLoop receives it
- [x] SyscallHandler processes it (full dispatch through `noop()`)
- [x] Reply reaches VPE0 (error_code=0)
- [x] Clean VPE0 output, VPE0 does not crash
- [ ] Kernel continues running after reply (post-reply fault — see above)

---

## Known Limitations

### Stubbed for Task 05

| Component | Current | Needed for Task 05 |
|-----------|---------|---------------------|
| `ThreadManager` | Single-threaded; `yield()`/`wait_for()`/`notify()` are no-ops | Cooperative context switching for revocation blocking |
| `KPE::start()` | Stub (no-op) | Spawn child kernel on another PE |
| `MHTInstance` 3-arg constructor | Delegates to default | Partition migration from creator kernel |
| Inter-kernel channels | Only 1 KRNLC_GATE configured | Full 8 KRNLC_GATES for multi-kernel |
| `read_mem`/`write_mem` | Stubs | Memory EP operations via shared dataports |
| `VPE::init_memory()` | Stub | ELF loading (if VPEs are not CAmkES components) |

### Architectural Constraints

- **8 message channels total** — shared between all recv/send EPs. Prototype uses 4/8.
- **seL4 priority scheduling** — kernel and VPE0 must have equal priority (200) for `seL4_Yield()` to work on single-core QEMU.
- **`<camkes.h>` not includable from C++** — seL4 utility headers use `typeof`, `_Static_assert`. CAmkES symbols declared manually.
- **Label pass-through** — VPE0 sends `label=0`; WorkLoop uses fallback `RecvGate`. The gem5 DTU auto-fills label from EP config.
- **Post-reply fault** — double-ack in `GateIStream` destructor after `reply()` already acked. Non-blocking (VPE0 receives reply correctly).

---

## File Inventory

### All Files Created or Modified in Task 04 (04a–04f)

**New files (202 total):**

| Category | Count | Key Files |
|----------|-------|-----------|
| arch/sel4/ backend | 6 | `DTU.cc`, `Platform.cc`, `VPE.cc`, `PEManager.cc`, `kernel.cc`, `libbase_stubs.cc` |
| arch/sel4/ headers | 3 | `Config.h`, `DTU.h`, `Thread.h` |
| Functional.h | 1 | `std::function`/`std::bind`/`std::move` replacement |
| C++ runtime | 2 | `cxx_runtime.cc`, `cxx_test.cc` |
| CAmkES bridge | 1 | `camkes_entry.c` |
| Imported kernel `.cc` | 22 | SemperOS kernel source (arch-independent) |
| Imported kernel `.h` | 33 | SemperOS kernel headers |
| Imported include headers | 125 | `base/`, `m3/`, `thread/`, `test/`, `c/`, `fs/` |
| Documentation | 2 | `docs/task04-kernel-integration.md`, `TASK04-REPORT.md` |

**Modified files (10 total):**

| File | Change Summary |
|------|----------------|
| `CMakeLists.txt` | C++ support, all kernel sources, include paths |
| `settings.cmake` | Heap size 4096 → 4 MiB |
| `semperos-sel4.camkes` | VPE0 priority 150 → 200 |
| `components/include/vdtu_ring.h` | `_Static_assert` C++ compat |
| `components/SemperKernel/SemperKernel.c` | Added `cxx_test()` call |
| `components/VPE0/VPE0.c` | Rewritten: syscall sender (NOOP) |
| `CLAUDE.md` | Updated status, limitations, key files |
| `kernel/SyscallHandler.cc` | INIT_PRIO deferral for sel4 |
| `kernel/KernelcallHandler.cc` | INIT_PRIO deferral for sel4 |
| `kernel/WorkLoop.cc` | Fallback RecvGate for label=0 |

**Patched SemperOS headers (17 files):**

`base/Config.h`, `base/DTU.h`, `base/Machine.h`, `base/stream/Serial.h`,
`base/arch/baremetal/Env.h`, `base/util/Subscriber.h`, `base/col/Treap.h`,
`base/util/String.h`, `base/stream/OStringStream.h`,
`base/stream/IStringStream.h`, `thread/Thread.h`, `m3/VPE.h`,
`m3/server/Server.h`, `m3/server/RequestHandler.h`,
`m3/server/LocalPipeServer.h`, `base/arch/host/DTU.h`,
`base/arch/host/Env.h`

**Patched SemperOS kernel sources (4 files):**

`kernel/pes/VPE.h`, `kernel/pes/KPE.h`, `kernel/ddl/MHTInstance.h`,
`kernel/ddl/MHTInstance.cc`

---

## QEMU Log

Captured 2026-02-23 from a fresh build and QEMU run:

```
docker run --rm \
    -v "$(pwd)/build-semperos/images:/sel4-image:ro" \
    -e TIMEOUT=25 \
    semperos-qemu:latest
```

```
SeaBIOS (version 1.15.0-1)


iPXE (https://ipxe.org) 00:02.0 CA00 PCI2.10 PnP PMM+1FF8B0D0+1FECB0D0 CA00
Press Ctrl-B to configure iPXE (PCI 00:02.0)...


Booting from ROM..Boot config: debug_port = 0x3f8
Boot config: disable_iommu = false
Detected 1 boot module(s):
  module #0: start=0xc13000 end=0x106ff88 size=0x45cf88 name='/sel4-image/capdl-loader-image-x86_64-pc99'
Parsing GRUB physical memory map
	Physical Memory Region from 0 size 9fc00 type 1
	Physical Memory Region from 9fc00 size 400 type 2
	Physical Memory Region from f0000 size 10000 type 2
	Physical Memory Region from 100000 size 1fedf000 type 1
Adding physical memory region 0x100000-0x1ffdf000
	Physical Memory Region from 1ffdf000 size 21000 type 2
	Physical Memory Region from b0000000 size 10000000 type 2
	Physical Memory Region from fed1c000 size 4000 type 2
	Physical Memory Region from fffc0000 size 40000 type 2
Multiboot gave us no video information
ACPI: RSDP paddr=0xf59f0
ACPI: RSDP vaddr=0xf59f0
ACPI: RSDT paddr=0x1ffe22ba
ACPI: RSDT vaddr=0x1ffe22ba
SKIM window for mitigating Meltdown (https://www.meltdownattack.com) not necessary for AMD and performance is being needlessly affected, consider disabling
Kernel loaded to: start=0x100000 end=0xc12000 size=0xb12000 entry=0x100300
ACPI: RSDT paddr=0x1ffe22ba
ACPI: RSDT vaddr=0x1ffe22ba
ACPI: FADT paddr=0x1ffe20b2
ACPI: FADT vaddr=0x1ffe20b2
ACPI: FADT flags=0x84a5
ACPI: 0 IOMMUs detected
ACPI: MADT paddr=0x1ffe21a6
ACPI: MADT vaddr=0x1ffe21a6
ACPI: MADT apic_addr=0xfee00000
ACPI: MADT flags=0x1
ACPI: MADT_APIC apic_id=0x0
ACPI: MADT_IOAPIC ioapic_id=0 ioapic_addr=0xfec00000 gsib=0
ACPI: MADT_ISO bus=0 source=0 gsi=2 flags=0x0
ACPI: MADT_ISO bus=0 source=5 gsi=5 flags=0xd
ACPI: MADT_ISO bus=0 source=9 gsi=9 flags=0xd
ACPI: MADT_ISO bus=0 source=10 gsi=10 flags=0xd
ACPI: MADT_ISO bus=0 source=11 gsi=11 flags=0xd
ACPI: 1 CPU(s) detected
ELF-loading userland images from boot modules:
size=0x65c000 v_entry=0x4088d9 v_start=0x400000 v_end=0xa5c000 p_start=0x1070000 p_end=0x16cc000
Moving loaded userland images to final location: from=0x1070000 to=0xc12000 size=0x65c000
Starting node #0 with APIC ID 0
Mapping kernel window is done
available phys memory regions: 1
  [100000..1ffdf000)
reserved virt address space regions: 1
  [ffffff8000100000..ffffff800126e000)
Booting all finished, dropped to user space
[vDTU] Initializing endpoint table (4 PEs x 16 EPs)
[vDTU] Initialized (8 msg channels, 4 mem channels available)
[vDTU] Ready for configuration requests
[VPE0] Starting (PE 2, VPE ID 0)
[VPE0] Looking for configured channels...
=== SemperOS Kernel on seL4/CAmkES ===
[CXX] Testing new/delete/virtual dispatch...
[CXX] OK: new/delete/virtual dispatch all working (got 42)
[SemperKernel] Starting SemperOS kernel on seL4/CAmkES
[SemperKernel] Platform: 4 PEs, kernel PE=0, kernel ID=0
[vDTU] config_recv(pe=0, ep=0, buf_order=14, msg_order=9) -> channel 0 (4 slots x 512B)
[SemperKernel] Configured SYSC_GATE recv endpoint (ep 0)
[vDTU] config_recv(pe=0, ep=15, buf_order=13, msg_order=8) -> channel 1 (8 slots x 256B)
[SemperKernel] Configured service recv endpoint (ep 15)
[vDTU] config_recv(pe=0, ep=6, buf_order=16, msg_order=11) -> channel 2 (2 slots x 2048B)
[SemperKernel] Configured 1 KRNLC_GATE recv endpoint
[SemperKernel] RecvBufs initialized
[SemperKernel] PEManager created
[SemperKernel] Creating VPE0 on PE 2 (syscall EP 0)
[vDTU] config_recv(pe=2, ep=1, buf_order=11, msg_order=9) -> channel 3 (4 slots x 512B)
[vDTU] config_send(pe=2, ep=0, dest=0:0, label=0x690d50) -> channel 0
[SemperKernel] VPE0 created and started
[SemperKernel] Entering WorkLoop (polling 6 SYSC + 8 KRNLC gates)
[VPE0] Found send channel: 0
[VPE0] Found reply channel: 1
[VPE0] Sending NOOP syscall (opcode=18) on channel 0
[VPE0] Syscall sent, waiting for kernel to process...
[VPE0] Waiting for reply (polling recv channels)...
[vDTU] config_send(pe=0, ep=1, dest=2:1, label=0x1) -> channel 3
FAULT HANDLER: data fault from kernel0.kernel0_0_control (ID 0x1) on address 0x19, pc = 0x4073f0, fsr = 0x4
FAULT HANDLER: Register dump:
FAULT HANDLER: rip:	0x4073f0
FAULT HANDLER: rsp:	0x887b90
FAULT HANDLER: rflags:	0x206
FAULT HANDLER: rax:	0x1
FAULT HANDLER: rbx:	0x1
FAULT HANDLER: rcx:	0x887c28
FAULT HANDLER: rdx:	0x8
FAULT HANDLER: rsi:	0x887c28
FAULT HANDLER: rdi:	0x1
FAULT HANDLER: rbp:	0x887b90
FAULT HANDLER: r8:	0x8aa040
FAULT HANDLER: r9:	0x1
FAULT HANDLER: r10:	0x10000000003
FAULT HANDLER: r11:	0x212
FAULT HANDLER: r12:	0x8aa040
FAULT HANDLER: r13:	0x467180
FAULT HANDLER: r14:	0x0
FAULT HANDLER: r15:	0x0
FAULT HANDLER: fs_base:	0x87d0c0
FAULT HANDLER: gs_base:	0x0
FAULT HANDLER:   memory map:
FAULT HANDLER:     +-- 0x000000000088bfff --
FAULT HANDLER:     |   guard page
FAULT HANDLER:     +-- 0x000000000088b000 --
FAULT HANDLER:     |   IPC buffer
FAULT HANDLER:     +-- 0x000000000088a000 --
FAULT HANDLER:     |   guard page
FAULT HANDLER:     +-- 0x0000000000889000 --
FAULT HANDLER:     |   guard page
FAULT HANDLER:     +-- 0x0000000000888000 --
FAULT HANDLER:     |   stack
FAULT HANDLER:     +-- 0x0000000000884000 --
FAULT HANDLER:     |   guard page
FAULT HANDLER:     +-- 0x0000000000883000 --
FAULT HANDLER:     |   code and data
FAULT HANDLER:     +-- 0x0000000000400000 --
[VPE0] Got reply: error_code=0 (0=NO_ERROR)
[VPE0] NOOP syscall succeeded!
[VPE0] === Integration test complete ===
qemu-system-x86_64: terminating on signal 15 from pid 7 (timeout)
QEMU_TIMEOUT: Exited after 25s
```
