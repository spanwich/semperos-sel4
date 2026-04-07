# FPT-175: Hille-on-Hardware Feasibility Spike

**Status:** GO  
**Date:** 2026-04-07  
**Timebox:** 3 days (actual: 1 day for assessment + wiring)

## Question

Can we wire the original SemperOS session protocol (createsrv, createsess, exchange_over_sess) on our CAmkES/XCP-ng hardware with acceptable effort, to produce an unsecured spanning exchange/revoke baseline on real hardware?

## Decision: GO

Total wiring effort is **5-8 working days**. Code reuse from imported SemperOS source is extremely high — the session protocol, exchange, and revocation handlers are 100% implemented with zero gem5 dependencies.

## Evidence

### Session Protocol Code — Fully Implemented, No gem5 Dependencies

| Function | Location | Status | gem5 Deps |
|----------|----------|--------|-----------|
| `createsrv` (opcode 1) | SyscallHandler.cc:153-189 | Complete | None (minor `#if __host__`) |
| `createsess` (opcode 2) | SyscallHandler.cc:229-371 | Complete (local + remote paths) | None |
| `exchange_over_sess` | SyscallHandler.cc:789-950 | Complete (3-party protocol) | None |
| `do_exchange` | SyscallHandler.cc:715-745 | Complete | None |

### Spanning Revoke — Fully Implemented

| Handler | Location | Status |
|---------|----------|--------|
| `Kernelcalls::revoke` | Kernelcalls.cc:321-350 | Complete |
| `KernelcallHandler::revoke` | KernelcallHandler.cc:816-905 | Complete |
| `revokeFinish` | KernelcallHandler.cc:915+ | Complete |
| `RevocationList` subscriptions | Revocations.cc:25-60 | Complete |

### Cross-Node Transport — Already Working

- DTUBridge E1000 + lwIP UDP: verified (PING/PONG + Kernelcalls::connect)
- `net_ring_send()` / `vdtu_ring_fetch()`: bidirectional kernel-to-kernel messages
- `dispatch_net_krnlc()`: WorkLoop integration complete
- ThreadManager::wait_for() with 2 worker threads: blocking RPC functional

### gem5 Isolation — Clean

All gem5 MMIO code behind `#if defined(__gem5__)` guards. sel4 DTU backend (`arch/sel4/DTU.cc`, `DTU.h`) is a clean vDTU implementation. No gem5 code compiled on sel4 target.

## What Was Missing (and What We Wired)

### 1. Disjoint PE Ranges (DONE)

Both nodes booted with PEs 0-3. MHT key hashing embeds PE ID — collisions break cross-node routing.

**Fix:** Platform.cc offsets PEDesc core_ids by `KERNEL_ID * NUM_LOCAL_PES`. DTU.cc translates global→local for VDTUService RPCs. Guarded by `SEMPER_MULTI_NODE` cmake flag.

### 2. Static MHT + KPE Registration (DONE)

MHT had no knowledge of peer kernel's PE range. Coordinator had no KPE handle for peer.

**Fix:** kernel.cc calls `MHTInstance::updateMembership()` + `Coordinator::addKPE()` after PEManager::create. 2-node hardcoded topology.

### 3. VPE1 Service Provider (DONE)

VPE1 was passive (no dataports, no messages). Session protocol requires VPE1 to handle OPEN/OBTAIN/DELEGATE.

**Fix:** Added 4 message channels + 2 memory endpoints between kernel↔VPE1. VDTUService pool expanded 8→12 msg, 4→6 mem. VPE1.c rewritten with service event loop.

### 4. RemoteServiceList Population (Already Working)

`KernelcallHandler::announceSrv()` at line 804 already calls `RemoteServiceList::get().add(name, id)`. No changes needed.

### 5. Message Buffering (Not Needed)

`net_poll()` uses `vdtu_ring_fetch()` on a 4KB ring buffer (~30 slots). No single-buffer problem.

## Remaining Work (Implementation Story)

1. **VPE1 createsrv integration** — VPE1 must call the `createsrv` syscall to register "testsrv". Requires VPE1 to have a configured syscall endpoint (VPE::init already handles this).
2. **Service reply protocol** — VPE1's reply to OPEN/OBTAIN/DELEGATE must match the exact `GateIStream` format expected by the kernel callback.
3. **Integration test (spanning exchange)** — VPE0 on node 0 calls `createsess("testsrv")` + `obtain()`. Verify capability transfer.
4. **Integration test (spanning revoke)** — Revoke obtained cap, verify propagation to node 1.
5. **Benchmark instrumentation** — rdtsc timing around spanning ops, 1000 iterations.
6. **XCP-ng deployment + collection** — `/build-isos` + `/xcpng-deploy`.

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| PE range change breaks existing tests | HIGH | Guarded by `SEMPER_MULTI_NODE` (OFF by default) |
| VPE1 service reply format mismatch | MEDIUM | Must match `GateIStream` serialization exactly |
| Message drops during multi-round protocol | LOW | Ring buffer has ~30 slots (adequate) |
| CAmkES untyped memory exhaustion | LOW | VPE1 adds 6 dataports (24 KiB) — within budget |

## Paper Value

Three-point comparison for spanning exchange/revoke:

1. **Hille-on-hardware:** Original SemperOS session protocol on XCP-ng — never published before
2. **Raft version (future):** v3.2 architecture with direct exchange + CryptoTransport
3. **Local baseline (collected):** ~350 cycles exchange, ~6,200 cycles revoke

The Hille baseline isolates "real network" cost from "security" cost, preventing reviewer dismissal of overhead as hardware difference.

## Sprint Plan

| Day | Task | Deliverable |
|-----|------|-------------|
| 1 | VPE1 createsrv syscall integration | Service registered on node 1 |
| 2 | Service reply protocol (match GateIStream) | OPEN/OBTAIN/DELEGATE replies work |
| 3 | Spanning exchange integration test | Cap transferred node 1 → node 0 |
| 4 | Spanning revoke integration test | Revocation propagates |
| 5 | rdtsc instrumentation + QEMU smoke test | Benchmarks collect cleanly |
| 6 | Build ISOs, deploy XCP-ng, collect | Paper-ready data |

## Files Modified

- `CMakeLists.txt` — SEMPER_MULTI_NODE option, VPE1 sources
- `semperos-sel4.camkes` — VPE1 dataports/signals, assembly connections
- `Platform.h` — NUM_LOCAL_PES, pe_base()
- `Platform.cc` — Global PE ID offsets
- `DTU.cc` — is_local_pe/to_local_pe helpers, VPE1 channel init
- `kernel.cc` — MHT updateMembership, KPE registration
- `vdtu_channels.h` — Pool size increase
- `VDTUService.c` — Pool size increase, VPE1 wakeup
- `VPE1.c` — Service event loop
