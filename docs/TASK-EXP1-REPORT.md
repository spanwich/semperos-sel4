# Experiment 1: vDTU Primitive Latency

## What was implemented

Three blocking stubs were fixed and a 6-benchmark harness was added to VPE0.

### Part A: ep_state lifecycle field

Added EP lifecycle tracking to the shared ring buffer control struct so both
the control plane (VDTUService) and data plane (ring send path) can
participate in endpoint termination.

**Changes:**
- `vdtu_ring.h`: Added `VDTU_EP_UNCONFIGURED/CONFIGURED/ACTIVE/TERMINATED`
  constants. Added `volatile uint32_t ep_state` to `vdtu_ring_ctrl` (still
  64 bytes after padding adjustment).
- `vdtu_ring.c`: `vdtu_ring_init()` sets `ep_state = VDTU_EP_ACTIVE`.
  `vdtu_ring_send()` returns `-3` if `ep_state == VDTU_EP_TERMINATED`.
  Reads (fetch/ack) are unaffected — drain is permitted after termination.
- `VDTUConfig.idl4` + `semperos-sel4.camkes`: Added `terminate_ep()` RPC
  (returns channel index on success, -1 on error).
- `VDTUService.c`: `config_terminate_ep()` validates PE/EP, checks endpoint
  is configured, returns channel index. VDTUService is control-plane-only
  (no dataport access), so the caller sets `ep_state` in the ring ctrl.
- `semperos-sel4.camkes`: Added `uses VDTUConfig vdtu` to VPE0 component
  and wired `vpe0_config_rpc` connection for benchmark access.

### Part B: Memory EP kernel path

Connected `write_mem()` and `read_mem()` in `DTU.cc` to the shared memory
dataports that `config_mem_local()` already sets up.

**Changes:**
- `DTU.cc`: Added `mem_ep_config` struct (dest_pe, base_addr, size) and
  `ep_mem_config[]` array. `config_mem_local()` now stores these.
  `write_mem()` and `read_mem()` scan for a memory EP matching the target
  PE, compute the offset `(addr - base_addr)`, and do a bounded `memcpy`
  through the channel's shared memory dataport.
- `cmpxchg_mem()` remains a stub (not used in current prototype).

### Part C: Benchmark harness

Added 6 benchmarks to VPE0.c, running after the 9 correctness tests:

| Benchmark | What it measures |
|-----------|-----------------|
| `ring_write` | Write one 2048-byte message to ring buffer slot |
| `ring_read` | Read one message (fetch + ack) from ring buffer slot |
| `ring_roundtrip` | Write + poll detect + read (same-component, no notification) |
| `ep_configure` | CAmkES RPC to VDTUService: `config_recv()` |
| `ep_terminate` | CAmkES RPC to VDTUService: `terminate_ep()` |
| `mem_access` | Read 256B + write 256B through memory dataport (memcpy path) |

**Methodology:**
- `rdtsc` (inline asm) for cycle-accurate timing
- 1000 warmup iterations discarded
- 10000 measured iterations
- Reports min/median/mean/max in cycles and µs (2 GHz assumed)
- Shell sort for median computation (O(n^3/2), 10K elements)
- Ring benchmarks use a local 8 KiB buffer (4 slots × 2048B) to avoid
  CAmkES channel conflicts
- RPC benchmarks use EP 15 on PE 2 (VPE0's own PE, unused slot)

## Files changed

| File | Lines | Change |
|------|-------|--------|
| `components/include/vdtu_ring.h` | 102-120 | ep_state constants + field |
| `src/vdtu_ring.c` | 26-30, 57-59 | init ep_state, terminated check |
| `interfaces/VDTUConfig.idl4` | 96-106 | terminate_ep procedure |
| `semperos-sel4.camkes` | 45, 152-153, 236-237 | terminate_ep, VPE0 vdtu uses, connection |
| `components/VDTUService/VDTUService.c` | 304-322 | config_terminate_ep impl |
| `components/SemperKernel/src/kernel/arch/sel4/DTU.cc` | 46, 91-95, 118, 307-315, 403-440 | terminate_ep decl, mem_ep_config, write_mem, read_mem |
| `components/VPE0/VPE0.c` | 249-720 | benchmark infrastructure + 6 benchmarks |

## Test results

### Standalone ring buffer tests (host)
```
=== vDTU Ring Buffer Tests ===

  TEST: ring_init with valid params                        PASS
  TEST: ring_init rejects bad params                       PASS
  TEST: verify DTU header struct size                      PASS
  TEST: total_size calculation                             PASS
  TEST: send one message, fetch it back                    PASS
  TEST: fill all slots, verify full detection              PASS
  TEST: fetch from empty ring returns NULL                 PASS
  TEST: send with payload exceeding slot size              PASS
  TEST: wraparound: write 2, read 2, write 3, read 3       PASS
  TEST: attach to existing ring buffer                     PASS

=== Results: 10 passed, 0 failed ===
```

### On-target tests (CAmkES/QEMU)

**9/9 PASS** — collected 2026-03-06 on QEMU q35 (single-node Docker).

```
[VPE0] TEST 1: NOOP syscall (3x)                          PASS
[VPE0] TEST 2: CREATEGATE syscall                         PASS
[VPE0] TEST 3: REVOKE syscall                             PASS
[VPE0] TEST 4: REVOKE non-existent capability             PASS
[VPE0] TEST 5: CREATE + REVOKE cycle (x3)                 PASS
[VPE0] TEST 6: EXCHANGE (delegate cap to VPE1)            PASS
[VPE0] TEST 7: Cross-VPE REVOKE                           PASS
[VPE0] TEST 8: EXCHANGE + REVOKE cycle (x3)               PASS
[VPE0] TEST 9: Remote DTU ping (NOOP to PE 4)             PASS
```

### Experiment 1 benchmark results

**Collected 2026-03-06** — QEMU q35, single-node Docker, 2 GHz assumed clock.
Initial run inflated by serial logging in QEMU (`printf` in every VDTUService
RPC handler). Numbers below collected with `VDTU_VERBOSE_LOG` disabled.

```
[BENCH] ring_write         min=2706     med=2814     mean=2919     max=64480    cycles  (1.4us median)
[BENCH] ring_read          min=234      med=254      mean=266      max=30298    cycles  (0.1us median)
[BENCH] ring_roundtrip     min=2900     med=3046     mean=3207     max=193798   cycles  (1.5us median)
[BENCH] ep_configure       min=47146    med=49686    mean=51604    max=1153970  cycles  (24.8us median)
[BENCH] ep_terminate       min=46146    med=48940    mean=50420    max=258398   cycles  (24.5us median)
[BENCH] mem_access         min=326      med=346      mean=392      max=232290   cycles  (0.2us median)
```

**Observations:**
- `ring_write` (1.4 µs): Dominated by 2048-byte memcpy into slot. Write is ~11x slower than read because it copies the full payload.
- `ring_read` (0.1 µs): Fetch returns a pointer (no copy), ack advances tail — minimal work.
- `ring_roundtrip` (1.5 µs): Close to ring_write alone, confirming read overhead is negligible.
- `ep_configure` (24.8 µs): CAmkES seL4RPCCall round-trip. Includes endpoint table lookup, channel allocation, slot parameter computation. ep_configure is ~0.3 µs slower than ep_terminate due to `alloc_msg_channel()` scan.
- `ep_terminate` (24.5 µs): Same RPC path but simpler handler (no channel allocation). Nearly identical to ep_configure — the IPC round-trip dominates both.
- `mem_access` (0.2 µs): 256B read + 256B write via memcpy — faster than ring_write due to smaller payload (512B vs 2048B).

## Open questions and follow-on work

1. ~~**Build and collect numbers**~~: Done — results collected 2026-03-06.

2. **VDTUService has no dataport access**: `terminate_ep()` returns the
   channel index but can't set `ep_state` directly in the ring ctrl.
   The caller (kernel or VPE0) must set it after the RPC returns. This is
   architecturally correct (control plane doesn't touch data plane memory)
   but means the ep_state update isn't atomic with the RPC.

3. **2048-byte slot limitation**: A 4 KiB CAmkES dataport can't fit
   2 × 2048-byte slots + 64-byte header (= 4160 > 4096). Ring benchmarks
   use malloc'd memory to work around this. For production, either use
   8 KiB dataports or accept 512-byte slots for syscall messages.

4. **cmpxchg_mem still stubbed**: Not needed for current benchmarks but
   required for lock-free data structure sharing across PEs.

5. **µs assumption**: Benchmarks assume 2 GHz QEMU clock. Real hardware
   (XCP-ng) will have different clock speeds — the raw cycle counts are
   the primary metric.
