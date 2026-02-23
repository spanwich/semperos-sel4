# Task 05 Report: Basic Capability Operations

**Date**: 2026-02-23
**Platform**: seL4/CAmkES on QEMU x86_64 (pc99)
**Commits**: (pending)

## Summary

Fixed the post-reply kernel fault from Task 04, then demonstrated the SemperOS
capability subsystem working on seL4 by executing CREATEGATE, REVOKE, and NOOP
syscalls through the real SyscallHandler. All 5 tests pass with no kernel
faults.

---

## Sub-task 05a: Fix Post-Reply Fault

### Root Cause

Two bugs contributed to the Task 04 fault:

1. **Double-ack**: `m3::DTU::reply()` called `vdtu_ring_ack()` to consume the
   received message, then `GateIStream::finish()` tried to ack again via
   `mark_read()`. Fixed by removing the ack from `reply()` — `finish()` handles
   consumption.

2. **GateIStream `_ack` flag**: `GateIStream::reply()` sets `_ack = false`
   (line 292 in GateStream.h) which prevents `finish()` from acking. On gem5
   this is correct (reply hardware-acks), but on sel4 it means the message is
   **never consumed**. The WorkLoop fetches the same message forever. Fixed by
   keeping `_ack = true` on sel4 so `finish()` properly consumes the message.

3. **Auto-reply EP collision**: The auto-reply send EP was allocated at EP 1,
   which is in the SYSC_GATE range (0-5). The WorkLoop polls EP 1 and would
   fetch the kernel's own reply from VPE0's recv channel. Fixed by starting the
   free EP scan from `FIRST_FREE_EP` (14) instead of 0.

### Files Modified

| File | Change |
|------|--------|
| `arch/sel4/DTU.cc` | Removed `vdtu_ring_ack()` from `reply()`; scan EPs from FIRST_FREE_EP |
| `base/com/GateStream.h` | `#if !defined(__sel4__)` around `_ack = false` in `reply()` |

### Checkpoint

- [x] Post-reply fault is gone
- [x] Kernel continues running after replying
- [x] VPE0 can send multiple syscalls (3 NOOPs in a loop)

---

## Sub-task 05b: CREATEGATE Syscall

### Implementation

VPE0 sends a CREATEGATE syscall with the following message format:

```
Payload (48 bytes, 6 × uint64_t):
  [0] opcode  = 4 (CREATEGATE)
  [1] tcap    = 0 (VPE capability at selector 0 = self)
  [2] dstcap  = 5 (destination capability slot)
  [3] label   = 0xCAFE (routing label)
  [4] epid    = 2 (target endpoint on VPE)
  [5] credits = 32 (message credits)
```

### WorkLoop VPE Session Fix

The SyscallHandler reads `vpe = is.gate().session<VPE>()` to identify which VPE
sent the syscall. The gate's session must point to the VPE object. Previously
the fallback gate had `session = nullptr` (crash on any handler that accesses
`vpe->name()`).

Fixed by looking up the VPE from `msg->senderCoreId` via `PEManager::get()`.
VPE0 is now created via `PEManager::create()` (instead of direct `new VPE()`)
so it's stored in `_vpes[core]` and findable at runtime.

### What Happens Internally

1. WorkLoop fetches message from SYSC_GATE[0]
2. Looks up VPE from `senderCoreId=2` → `PEManager::get().vpe(2)` → VPE0
3. `SyscallHandler::handle_message()` reads opcode=4 → `creategate()`
4. Handler reads: `tcap=0, dstcap=5, label=0xCAFE, epid=2, credits=32`
5. Validates: `tcap` resolves to VPECapability at selector 0 (VPE0 itself)
6. Creates `MsgCapability` at selector 5 in VPE0's CapTable
7. Replies with `Errors::NO_ERROR`

### Files Modified

| File | Change |
|------|--------|
| `arch/sel4/kernel.cc` | Use `PEManager::create()` for VPE0 (stores in `_vpes[]`) |
| `kernel/WorkLoop.cc` | Look up VPE from `senderCoreId` via PEManager |
| `components/VPE0/VPE0.c` | Added `send_creategate()` function |

### Checkpoint

- [x] VPE0 sends CREATEGATE with correct message format
- [x] Kernel dispatches through real `SyscallHandler::creategate()`
- [x] MsgCapability created in VPE0's CapTable at selector 5
- [x] Reply reaches VPE0 with NO_ERROR

---

## Sub-task 05c: REVOKE Syscall

### Implementation

VPE0 sends a REVOKE syscall with the following message format:

```
Payload (32 bytes):
  [0..7]   opcode     = 16 (REVOKE)
  [8..19]  CapRngDesc = { type=OBJ(0), start=5, count=1 } (12 bytes)
  [20..23] padding    (4 bytes to align)
  [24..31] own        = 1 (true = revoke own copies)
```

`CapRngDesc` is a 12-byte struct (`uint32_t type, uint32_t start, uint32_t count`)
read as raw bytes then advanced by `round_up(12, 8) = 16` bytes.

### Stack Overflow Fix

REVOKE caused a stack overflow with the default 16 KiB CAmkES stack. The
revocation code path with logging (`KLOG` → `OStringStream` → `alloca()`)
requires more stack space. Fixed by setting `kernel0._stack_size = 65536` (64
KiB) in the CAmkES assembly.

### Non-Existent Capability Revoke

Revoking selector 99 (no capability) returns `NO_ERROR`. This is correct
SemperOS behavior — the distributed revocation protocol treats absent
capabilities as a no-op (they may be in-flight in a remote revocation). The
kernel does not crash.

### Files Modified

| File | Change |
|------|--------|
| `semperos-sel4.camkes` | `kernel0._stack_size = 65536` |
| `components/VPE0/VPE0.c` | Added `send_revoke()` function |

### Checkpoint

- [x] VPE0 sends REVOKE with correct message format
- [x] Kernel dispatches through real `SyscallHandler::revoke()` → `CapTable::revoke()`
- [x] Capability removed from CapTable
- [x] Reply reaches VPE0 with NO_ERROR
- [x] Revoking non-existent selector returns NO_ERROR (no crash)

---

## Sub-task 05d: End-to-End Capability Test

### Test Results

```
[VPE0] Test 1 (NOOP x3): PASS
[VPE0] Test 2 (CREATEGATE sel=5): PASS (err=0)
[VPE0] Test 3 (REVOKE sel=5): PASS (err=0)
[VPE0] Test 4 (REVOKE non-existent sel=99): PASS (err=0, no crash)
[VPE0] Test 5 (CREATE+REVOKE x3): PASS
[VPE0] === 5 passed, 0 failed ===
```

### Checkpoint

- [x] All 5 tests pass
- [x] No kernel faults
- [x] Kernel remains alive after VPE0 finishes tests
- [x] Output clearly shows PASS/FAIL for each test

---

## Known Limitations

| Component | Status | Notes |
|-----------|--------|-------|
| ThreadManager | Single-threaded stub | Safe for Task 05 (no blocking revocations) |
| KRNLC gates | Only 1 configured | Multi-kernel needs all 8 |
| VPE label | VPE0 sends label=0 | Kernel looks up VPE via PEManager instead |
| Stack size | 64 KiB | Revocation + logging needs > 16 KiB default |
| Memory EPs | Stubs | `read_mem`/`write_mem` not implemented |
| OBTAIN/DELEGATE | Not tested | Requires second VPE (Task 06) |

---

## File Inventory

### Files Modified in Task 05

| File | Change |
|------|--------|
| `components/SemperKernel/src/kernel/arch/sel4/DTU.cc` | Remove double-ack; EP scan from FIRST_FREE_EP; remove debug prints |
| `components/SemperKernel/src/kernel/arch/sel4/kernel.cc` | Use PEManager::create() for VPE0 |
| `components/SemperKernel/src/kernel/WorkLoop.cc` | VPE lookup via PEManager for label=0 msgs |
| `components/SemperKernel/src/include/base/com/GateStream.h` | Keep `_ack=true` on sel4 after reply |
| `components/VPE0/VPE0.c` | Full test harness: NOOP, CREATEGATE, REVOKE |
| `semperos-sel4.camkes` | 64 KiB stack for kernel |

---

## QEMU Log

Captured 2026-02-23 from a fresh build and QEMU run:

```
docker run --rm \
    -v "$(pwd)/build-semperos/images:/sel4-image:ro" \
    -e TIMEOUT=30 \
    semperos-qemu:latest
```

```
SeaBIOS (version 1.15.0-1)

Booting from ROM..
Booting all finished, dropped to user space
[vDTU] Initializing endpoint table (4 PEs x 16 EPs)
[vDTU] Initialized (8 msg channels, 4 mem channels available)
[vDTU] Ready for configuration requests
[VPE0] Starting (PE 2, VPE ID 0)
[VPE0] Waiting for channels...
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
[vDTU] config_recv(pe=2, ep=1, buf_order=11, msg_order=9) -> channel 3 (4 slots x 512B)
[vDTU] config_send(pe=2, ep=0, dest=0:0, label=0x690d70) -> channel 0
[SemperKernel] Created VPE0 on PE 2 (id=2)
[SemperKernel] VPE0 started
[SemperKernel] Entering WorkLoop (polling 6 SYSC + 8 KRNLC gates)
[VPE0] Send channel: 0
[vDTU] config_send(pe=0, ep=14, dest=2:1, label=0x0) -> channel 3
[VPE0] Test 1 (NOOP x3): PASS
[VPE0] Test 2 (CREATEGATE sel=5): PASS (err=0)
[VPE0] Test 3 (REVOKE sel=5): PASS (err=0)
[VPE0] Test 4 (REVOKE non-existent sel=99): PASS (err=0, no crash)
[VPE0] Test 5 (CREATE+REVOKE x3): PASS
[VPE0] === 5 passed, 0 failed ===
QEMU_TIMEOUT: Exited after 30s
```
