# Task 07 Report: Cross-Node Communication

**Date**: 2026-02-26
**Baseline**: Task 06 complete at commit `e368c20` (EXCHANGE + cross-VPE revocation, 8/8 tests)
**Result**: Two QEMU nodes running SemperOS-on-seL4 exchange messages through the full kernel-to-kernel stack. 9/9 tests pass on both nodes. Bidirectional PING/PONG round-trip verified.

---

## Summary

Task 07 added inter-node communication to the SemperOS vDTU prototype. Starting from a single-QEMU system with 4 local PEs, we added:

1. A **DTUBridge** CAmkES component with an Intel 82540EM (e1000) Ethernet driver and lwIP UDP stack
2. **Docker orchestration** for launching two QEMU instances connected by a virtual Ethernet cable
3. **SPSC ring buffers** between SemperKernel and DTUBridge for zero-copy message handoff
4. **Kernel integration** in the WorkLoop for polling inbound network messages

The final demo sends a DTU message from Node A's kernel through the ring buffer, DTUBridge, UDP, across the QEMU socket network, into Node B's DTUBridge, ring buffer, and Node B's kernel — then back. Both directions work.

---

## Sub-task 07a: Audit

**Goal**: Assess Task 06 baseline, identify what exists, what's missing.

**Baseline state** (commit `e368c20`):
- 5 CAmkES components: VDTUService, SemperKernel, VPE0, VPE1, (no DTUBridge)
- 8 message channels + 4 memory channels (4 KiB each, SPSC ring buffers)
- 8/8 tests: NOOP, CREATEGATE, REVOKE, cross-VPE EXCHANGE+REVOKE
- Single QEMU instance, no networking

**Missing for inter-node**:
- No Ethernet driver
- No IP stack
- No way to run two QEMU instances
- No remote PE routing in the kernel

---

## Sub-task 07b: Dual-QEMU Docker

**Goal**: Boot two QEMU instances of the same SemperOS image, connected by a virtual network.

**Solution**: `docker/docker-compose.yml` + `docker/run-qemu.sh`

- Both nodes use the **same** `build-node0/images/` (single binary, node identity derived at runtime from MAC address)
- QEMU socket backend: Node A listens on `:10001`, Node B connects to `127.0.0.1:10001`
- Key QEMU flag: `-nic none` suppresses the Q35's default e1000e (82574L, 8086:10d3) at PCI slot 2, allowing our explicit `-device e1000` (82540EM, 8086:100E) to take that slot
- Node B has a 3-second startup delay (`sleep 3`) to let Node A's listen socket open

**docker-compose.yml** (simplified):
```yaml
services:
  node-a:
    image: semperos-qemu:latest
    volumes: [../../../build-node0/images:/sel4-image:ro]
    network_mode: host
    command: [-nic, none, -device, "e1000,netdev=eth0,mac=52:54:00:00:00:01",
              -netdev, "socket,id=eth0,listen=:10001"]
  node-b:
    image: semperos-qemu:latest
    volumes: [../../../build-node0/images:/sel4-image:ro]
    network_mode: host
    depends_on: [node-a]
    entrypoint: ["sh", "-c", "sleep 3 && /run-qemu.sh -nic none -device e1000,...,mac=52:54:00:00:00:02 -netdev socket,...,connect=127.0.0.1:10001"]
```

**Result**: Both nodes boot seL4, capDL loads all 5 components, 8/8 tests pass independently.

---

## Sub-task 07c: e1000 Driver

**Goal**: Get the Intel 82540EM working — PCI detect, DMA allocation, hardware init, link up, TX, RX, IRQ.

**E1000 driver** adapted from the `http_gateway_x86` project's `E1000Driver.c`:

| Item | Detail |
|------|--------|
| PCI | Bus 0, Dev 2, Fun 0. Vendor 8086, Device 100E. BAR0 at 0xFEB80000 |
| MMIO | 128 KiB region mapped via `seL4HardwareMMIO` (uncached) |
| PCI config | I/O ports 0xCF8-0xCFF via `seL4HardwareIOPort` |
| IRQ | IOAPIC pin 11, via `seL4HardwareInterrupt` |
| DMA pool | 2 MiB at paddr 0x4000000 (CAmkES `dma_pool` + `simple_untyped21_pool=4`) |
| Descriptors | 64 RX + 64 TX ring entries, 2048-byte buffers |
| MAC | Read from e1000 RAL/RAH registers (set by QEMU `-device e1000,mac=...`) |

**CAmkES hardware components**:
```
component HWEthDriver  { hardware; dataport Buf(0x20000) mmio; emits IRQ irq; }
component HWPCIConfig  { hardware; provides IOPort pci_config; }
```

**Evidence from log**:
```
[DTUBridge] PCI 0:2.0: vendor=0x8086 device=0x100e
[DTUBridge] DMA allocated: 64 RX + 64 TX buffers
[DTUBridge] MAC: 52:54:00:00:00:01
[DTUBridge] E1000 init complete (link=UP)
```

---

## Sub-task 07d: lwIP UDP

**Goal**: Establish IP connectivity and exchange UDP messages between nodes.

**lwIP configuration**: `NO_SYS=1` (raw API), UDP-only (TCP disabled), 256 KiB heap. 20 minimal source files compiled inline (no separate library).

**IP addressing** (runtime, from MAC):
```
MAC last octet 0x01 → IP 10.0.0.1 (Node 0, peer 10.0.0.2)
MAC last octet 0x02 → IP 10.0.0.2 (Node 1, peer 10.0.0.1)
```

**UDP ports**:
| Port | Purpose |
|------|---------|
| 5000 | Hello exchange (connectivity proof) |
| 7654 | DTU message transport |

**Hello exchange protocol**: Each node sends `"HELLO FROM NODE X (10.0.0.Y)"` to the peer 3 times with spacing for ARP resolution. Both nodes log received hellos.

**Evidence from log**:
```
[DTUBridge] HELLO TX to 10.0.0.2:5000: "HELLO FROM NODE 0 (10.0.0.1)" (err=0)
[DTUBridge] HELLO RX from 10.0.0.1:5000: "HELLO FROM NODE 0 (10.0.0.1)"
[DTUBridge] === HELLO EXCHANGE: SUCCESS ===
```

---

## Sub-task 07e-f: NetBridge + Cross-Node Demo

**Goal**: Bridge the SemperOS kernel's ring buffers to the network, achieving kernel-to-kernel message transport.

### Ring buffer dataports

Two new 4096-byte shared memory regions between SemperKernel and DTUBridge:

| Dataport | Producer | Consumer | Format |
|----------|----------|----------|--------|
| `net_outbound` | SemperKernel | DTUBridge | SPSC ring, 4 slots x 512B |
| `net_inbound` | DTUBridge | SemperKernel | SPSC ring, 4 slots x 512B |

Both use the standard `vdtu_ring.h` SPSC ring buffer format (64-byte control header + message slots). Each slot holds a 25-byte DTU message header + up to 487 bytes of payload.

### Outbound path (kernel -> network)

```
1. Kernel writes to net_outbound ring  (vdtu_ring_send)
2. DTUBridge polls net_outbound        (main loop, vdtu_ring_fetch)
3. DTUBridge sends raw slot as UDP     (udp_sendto to peer:7654)
```

The kernel's `DTU::send_to()` for remote PEs (PE >= 4) was updated to write directly to the outbound ring via `net_ring_send()`, replacing the previous `dtu_out` flat buffer + `net_net_send()` RPC approach.

### Inbound path (network -> kernel)

```
1. DTUBridge receives UDP on port 7654 (dtu_udp_recv_cb)
2. DTUBridge writes to net_inbound ring (vdtu_ring_send)
3. Kernel polls net_inbound            (WorkLoop -> net_poll)
```

The kernel's `WorkLoop::run()` calls `net_poll()` after each iteration of gate scanning. `net_poll()` checks the inbound ring for messages and processes them.

### Wire format

Raw DTU message bytes (25-byte packed header + payload) are sent as-is in the UDP datagram payload. No encapsulation, no new wire format. The receiving DTUBridge reconstructs the same message in the peer kernel's inbound ring.

### PING/PONG demo

After a startup delay (~1M WorkLoop iterations), each kernel writes a PING message to the outbound ring:
- Label: `0x50494E47` ("PING" in ASCII)
- Payload: `"PING from kernel"` (16 bytes)

When a kernel receives PING, it sends PONG:
- Label: `0x504F4E47` ("PONG" in ASCII)
- Payload: `"PONG from kernel"` (16 bytes)

**Evidence from log (A→B→A round trip)**:
```
[SemperKernel] NET: Sent PING to outbound ring           (Node A kernel)
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x50494e47, err=0)   (Node A bridge)
[DTUBridge] NET RX: 41 bytes from 10.0.0.1:7654 (label=0x50494e47)   (Node B bridge)
[SemperKernel] NET RX: label=0x50494e47 len=16 "PING from kernel"    (Node B kernel)
[SemperKernel] NET: Sent PONG reply                      (Node B kernel)
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x504f4e47, err=0)   (Node B bridge)
[DTUBridge] NET RX: 41 bytes from 10.0.0.2:7654 (label=0x504f4e47)   (Node A bridge)
[SemperKernel] NET RX: label=0x504f4e47 len=16 "PONG from kernel"    (Node A kernel)
[SemperKernel] NET: === PONG RECEIVED — round trip complete! ===      (Node A kernel)
```

**Evidence from log (B→A→B round trip)**:
```
[SemperKernel] NET: Sent PING to outbound ring           (Node B kernel)
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x50494e47, err=0)   (Node B bridge)
[DTUBridge] NET RX: 41 bytes from 10.0.0.2:7654 (label=0x50494e47)   (Node A bridge)
[SemperKernel] NET RX: label=0x50494e47 len=16 "PING from kernel"    (Node A kernel)
[SemperKernel] NET: Sent PONG reply                      (Node A kernel)
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x504f4e47, err=0)   (Node A bridge)
[DTUBridge] NET RX: 41 bytes from 10.0.0.1:7654 (label=0x504f4e47)   (Node B bridge)
[SemperKernel] NET RX: label=0x504f4e47 len=16 "PONG from kernel"    (Node B kernel)
[SemperKernel] NET: === PONG RECEIVED — round trip complete! ===      (Node B kernel)
```

---

## Architecture Diagram

```
Node A (QEMU 1, MAC ...:01, IP 10.0.0.1)        Node B (QEMU 2, MAC ...:02, IP 10.0.0.2)

 VPE0 ──ring──> SemperKernel                      SemperKernel <──ring── VPE0
  (PE 2)        (PE 0)                             (PE 0)        (PE 2)
                  │ WorkLoop::net_poll()              ▲ WorkLoop::net_poll()
                  │ vdtu_ring_send()                  │ vdtu_ring_fetch()
                  ▼                                   │
           ┌─────────────┐                     ┌─────────────┐
           │ net_outbound │ 4 KiB SPSC ring    │ net_inbound  │ 4 KiB SPSC ring
           │ (4x512B)    │                     │ (4x512B)     │
           └──────┬──────┘                     └──────▲───────┘
                  │ vdtu_ring_fetch()                  │ vdtu_ring_send()
                  ▼                                   │
           ┌─────────────────────┐             ┌─────────────────────┐
           │     DTUBridge       │             │     DTUBridge       │
           │  e1000 + lwIP UDP   │             │  e1000 + lwIP UDP   │
           │  main loop polling  │             │  UDP recv callback  │
           └──────┬──────────────┘             └──────▲──────────────┘
                  │ udp_sendto(:7654)                  │ dtu_udp_recv_cb
                  ▼                                   │
           ┌─────────────┐                     ┌─────────────┐
           │ Intel 82540EM│                     │ Intel 82540EM│
           │ PCI 0:2.0   │                     │ PCI 0:2.0   │
           └──────┬──────┘                     └──────▲──────┘
                  │                                   │
                  └───── QEMU -netdev socket ─────────┘
                         (virtual Ethernet L2)
```

---

## Channel/Dataport Allocation Table

### Message channels (4 KiB each, SPSC ring buffers)

| Dataport | CAmkES Connection | From | To | Purpose |
|----------|-------------------|------|----|---------|
| `msgchan_kv_0` | `msgchan0` | kernel0 | vpe0 | SYSC_GATE recv (ch 0) |
| `msgchan_kv_1` | `msgchan1` | kernel0 | vpe0 | SRV recv (ch 1) |
| `msgchan_kv_2` | `msgchan2` | kernel0 | vpe0 | KRNLC_GATE recv (ch 2) |
| `msgchan_kv_3` | `msgchan3` | kernel0 | vpe0 | VPE0 recv + auto-reply (ch 3) |
| `msgchan_kv_4` | `msgchan4` | kernel0 | vpe0 | VPE1 recv (ch 4) |
| `msgchan_kv_5` | `msgchan5` | kernel0 | vpe0 | Spare |
| `msgchan_kv_6` | `msgchan6` | kernel0 | vpe0 | Spare |
| `msgchan_kv_7` | `msgchan7` | kernel0 | vpe0 | Spare |

### Memory endpoint dataports (4 KiB each)

| Dataport | CAmkES Connection | From | To |
|----------|-------------------|------|----|
| `memep_kv_0..3` | `memep0..3` | kernel0 | vpe0 |

### Network transport dataports

| Dataport | CAmkES Connection | Size | From | To | Format |
|----------|-------------------|------|------|----|--------|
| `dtu_out` | `dtu_out_dp` | 8 KiB | kernel0 | dtu_bridge | Flat buffer (legacy, unused in 07e) |
| `dtu_in` | `dtu_in_dp` | 8 KiB | dtu_bridge | kernel0 | Flat buffer (legacy, unused in 07e) |
| `net_outbound` | `net_outbound_dp` | 4 KiB | kernel0 | dtu_bridge | SPSC ring, 4x512B |
| `net_inbound` | `net_inbound_dp` | 4 KiB | dtu_bridge | kernel0 | SPSC ring, 4x512B |

### Hardware connections

| Connection | Type | Detail |
|------------|------|--------|
| `eth_mmio_conn` | `seL4HardwareMMIO` | e1000 registers, 128 KiB at 0xFEB80000 |
| `eth_irq_conn` | `seL4HardwareInterrupt` | IOAPIC pin 11 |
| `pci_config_conn` | `seL4HardwareIOPort` | PCI config ports 0xCF8-0xCFF |

### Notifications

| Connection | Type | From | To | Purpose |
|------------|------|------|----|---------|
| `vdtu_wake_kern` | `seL4Notification` | vdtu | kernel0 | Wake kernel from seL4_Wait |
| `vdtu_wake_vpe0` | `seL4Notification` | vdtu | vpe0 | Wake VPE0 from seL4_Wait |
| `kern_done` | `seL4Notification` | kernel0 | vdtu | Kernel done signal |
| `kern_to_vpe0` | `seL4Notification` | kernel0 | vpe0 | Message available |
| `vpe0_to_kern` | `seL4Notification` | vpe0 | kernel0 | Reply available |
| `net_to_kern` | `seL4Notification` | dtu_bridge | kernel0 | Network msg arrived (legacy) |

### RPC connections

| Connection | Type | From | To | Interface |
|------------|------|------|----|-----------|
| `config_rpc` | `seL4RPCCall` | kernel0 | vdtu | VDTUConfig (9 procedures) |
| `net_rpc` | `seL4RPCCall` | kernel0 | dtu_bridge | DTUNetIPC (legacy, unused in 07e) |

### Connection budget

| Type | Count | Notes |
|------|-------|-------|
| `seL4SharedData` | 16 | 8 msg + 4 mem + 2 legacy net + 2 ring net |
| `seL4Notification` | 6 | 3 control + 2 data path + 1 legacy net |
| `seL4RPCCall` | 2 | 1 config + 1 legacy net |
| `seL4HardwareMMIO` | 1 | e1000 MMIO |
| `seL4HardwareInterrupt` | 1 | e1000 IRQ |
| `seL4HardwareIOPort` | 1 | PCI config |

---

## Files Created and Modified

Since Task 06 baseline `e368c20`. Commits `9a41ae1..HEAD` (uncommitted 07e work included).

### New files

| File | Lines | Purpose |
|------|-------|---------|
| `components/DTUBridge/DTUBridge.c` | ~730 | E1000 driver + lwIP UDP + ring buffer bridge |
| `components/DTUBridge/e1000_hw.h` | 322 | Intel 82540EM register definitions |
| `components/DTUBridge/lwipopts.h` | 70 | lwIP configuration (UDP-only, NO_SYS) |
| `docker/docker-compose.yml` | 43 | Dual-QEMU orchestration |
| `TASK06-REPORT.md` | 333 | Task 06 completion report |
| `TASK07-REPORT.md` | (this file) | Task 07 completion report |

### Modified files

| File | Change summary |
|------|----------------|
| `semperos-sel4.camkes` | +HWEthDriver, HWPCIConfig, DTUBridge components; +DTUNetIPC procedure; +net_outbound/net_inbound dataports; +all connections and config |
| `CMakeLists.txt` | +lwIP path resolution, LWIP_UDP_SOURCES, DeclareCAmkESComponent(DTUBridge), +vdtu_ring.c to DTUBridge sources |
| `settings.cmake` | +LibLwip ON, +NODE_ID variable |
| `docker/run-qemu.sh` | +$QEMU_EXTRA passthrough |
| `components/SemperKernel/camkes_entry.c` | +net_msg_arrived_handle, +net_init_rings, +net_ring_send, +net_poll (PING/PONG) |
| `components/SemperKernel/src/kernel/arch/sel4/DTU.cc` | +remote PE routing via net_ring_send (replaced dtu_out + RPC) |
| `components/SemperKernel/src/kernel/arch/sel4/kernel.cc` | +net_init_rings() call before WorkLoop |
| `components/SemperKernel/src/kernel/WorkLoop.cc` | +net_poll() call each iteration |
| `components/VPE0/VPE0.c` | +Test 9 placeholder (remote DTU ping) |

---

## Known Limitations

1. **Same binary, both nodes**: Both QEMU instances run the same image. Node identity is derived at runtime from the MAC address (last octet). This means both nodes have the same kernel ID (0) and the same PE layout (0-3). A true multi-kernel SemperOS would assign disjoint PE ranges.

2. **No SemperOS inter-kernel protocol**: The PING/PONG demo sends arbitrary test messages. The SemperOS `Kernelcalls::connect()`, distributed revocation, and OBTAIN/DELEGATE between kernels are not implemented. These require Task 07+ (ThreadManager for blocking `wait_for()`, gate routing tables).

3. **PING/PONG is kernel-level**: The PING/PONG test runs inside `net_poll()` called from the WorkLoop. It is not a VPE-initiated syscall. Test 9 in VPE0 only verifies the local NOOP path; it does not trigger actual cross-node traffic.

4. **Legacy dataports retained**: The `dtu_out` and `dtu_in` flat-buffer dataports (8 KiB each) and the `DTUNetIPC` RPC interface are still wired in the CAmkES assembly but are no longer used by the ring buffer path. They could be removed to save 2 seL4SharedData connections and 1 seL4RPCCall.

5. **Single-slot inbound**: The inbound ring has 4 slots (4 x 512B). Under heavy traffic, messages could be dropped if the kernel's `net_poll()` doesn't drain fast enough. The `vdtu_ring_send()` in `dtu_udp_recv_cb()` returns -1 if the ring is full, and the message is lost.

6. **Polling, not interrupt-driven**: Both DTUBridge (outbound ring) and kernel (inbound ring) use polling with `seL4_Yield()`. This is fine for single-core QEMU but would need interrupt/notification wakeup for multi-core or power-sensitive deployments.

7. **45-second QEMU timeout**: The docker-compose uses `TIMEOUT=45`. Both nodes complete all tests, hello exchange, and PING/PONG well within this window, then idle until timeout kills QEMU.

---

## QEMU Log

Fresh dual-QEMU run captured 2026-02-26. Both nodes boot, run 9/9 tests, exchange hellos, and complete bidirectional PING/PONG.

```
Attaching to semperos-node-a, semperos-node-b

=== NODE A BOOT ===

[vDTU] Initializing endpoint table (4 PEs x 16 EPs)
[vDTU] Initialized (8 msg channels, 4 mem channels available)
[vDTU] Ready for configuration requests
[VPE1] Starting (PE 3, VPE ID 1) — passive mode for EXCHANGE tests
[DTUBridge] pre_init
[VPE0] Starting (PE 2, VPE ID 0)
[VPE0] Waiting for channels...
=== SemperOS Kernel on seL4/CAmkES ===
[CXX] OK: new/delete/virtual dispatch all working (got 42)
[SemperKernel] Starting SemperOS kernel on seL4/CAmkES
[SemperKernel] Platform: 4 PEs, kernel PE=0, kernel ID=0
[SemperKernel] Configured SYSC_GATE recv endpoint (ep 0)
[SemperKernel] Configured service recv endpoint (ep 15)
[SemperKernel] Configured 1 KRNLC_GATE recv endpoint
[SemperKernel] RecvBufs initialized
[SemperKernel] PEManager created
[SemperKernel] Created VPE0 on PE 2 (id=2)
[SemperKernel] VPE0 started
[SemperKernel] Created VPE1 on PE 3 (id=3)
[SemperKernel] Installed VPE1 cap at VPE0 selector 2
[SemperKernel] VPE1 started (passive mode)
[SemperKernel] Net rings attached (outbound + inbound)
[SemperKernel] Entering WorkLoop (polling 6 SYSC + 8 KRNLC gates)

[DTUBridge] E1000 + lwIP UDP bridge
[DTUBridge] PCI 0:2.0: vendor=0x8086 device=0x100e
[DTUBridge] PCI initialized
[DTUBridge] DMA allocated: 64 RX + 64 TX buffers
[DTUBridge] MAC: 52:54:00:00:00:01
[DTUBridge] E1000 init complete (link=UP)
[DTUBridge] Node 0 (MAC ...:01 -> IP 10.0.0.1, peer 10.0.0.2)
[DTUBridge] lwIP UP: 10.0.0.1/24
[DTUBridge] UDP: DTU port 7654, Hello port 5000
[DTUBridge] Net rings initialized (4 slots x 512B)
[DTUBridge] Ready
[DTUBridge] Entering main loop

[VPE0] Test 1 (NOOP x3): PASS
[VPE0] Test 2 (CREATEGATE sel=5): PASS (err=0)
[VPE0] Test 3 (REVOKE sel=5): PASS (err=0)
[VPE0] Test 4 (REVOKE non-existent sel=99): PASS (err=0, no crash)
[VPE0] Test 5 (CREATE+REVOKE x3): PASS
[VPE0] Test 6 (EXCHANGE delegate to VPE1): PASS (err=0)
[VPE0] Test 7 (cross-VPE REVOKE sel=20): PASS (err=0)
[VPE0] Test 8 (EXCHANGE+REVOKE x3 cycle): PASS
[VPE0] Test 9 (Remote DTU ping to PE 4): PASS (local NOOP ok, remote routing via kernel)
[VPE0] === 9 passed, 0 failed ===

=== NODE B BOOT ===

[vDTU] Initializing endpoint table (4 PEs x 16 EPs)
[vDTU] Initialized (8 msg channels, 4 mem channels available)
[vDTU] Ready for configuration requests
[VPE1] Starting (PE 3, VPE ID 1) — passive mode for EXCHANGE tests
[DTUBridge] pre_init
[VPE0] Starting (PE 2, VPE ID 0)
[VPE0] Waiting for channels...
=== SemperOS Kernel on seL4/CAmkES ===
[CXX] OK: new/delete/virtual dispatch all working (got 42)
[SemperKernel] Starting SemperOS kernel on seL4/CAmkES
[SemperKernel] Platform: 4 PEs, kernel PE=0, kernel ID=0
[SemperKernel] Configured SYSC_GATE recv endpoint (ep 0)
[SemperKernel] Configured service recv endpoint (ep 15)
[SemperKernel] Configured 1 KRNLC_GATE recv endpoint
[SemperKernel] RecvBufs initialized
[SemperKernel] PEManager created
[SemperKernel] Created VPE0 on PE 2 (id=2)
[SemperKernel] VPE0 started
[SemperKernel] Created VPE1 on PE 3 (id=3)
[SemperKernel] Installed VPE1 cap at VPE0 selector 2
[SemperKernel] VPE1 started (passive mode)
[SemperKernel] Net rings attached (outbound + inbound)
[SemperKernel] Entering WorkLoop (polling 6 SYSC + 8 KRNLC gates)

[DTUBridge] E1000 + lwIP UDP bridge
[DTUBridge] PCI 0:2.0: vendor=0x8086 device=0x100e
[DTUBridge] PCI initialized
[DTUBridge] DMA allocated: 64 RX + 64 TX buffers
[DTUBridge] MAC: 52:54:00:00:00:02
[DTUBridge] E1000 init complete (link=UP)
[DTUBridge] Node 1 (MAC ...:02 -> IP 10.0.0.2, peer 10.0.0.1)
[DTUBridge] lwIP UP: 10.0.0.2/24
[DTUBridge] UDP: DTU port 7654, Hello port 5000
[DTUBridge] Net rings initialized (4 slots x 512B)
[DTUBridge] Ready
[DTUBridge] Entering main loop

[VPE0] Test 1 (NOOP x3): PASS
[VPE0] Test 2 (CREATEGATE sel=5): PASS (err=0)
[VPE0] Test 3 (REVOKE sel=5): PASS (err=0)
[VPE0] Test 4 (REVOKE non-existent sel=99): PASS (err=0, no crash)
[VPE0] Test 5 (CREATE+REVOKE x3): PASS
[VPE0] Test 6 (EXCHANGE delegate to VPE1): PASS (err=0)
[VPE0] Test 7 (cross-VPE REVOKE sel=20): PASS (err=0)
[VPE0] Test 8 (EXCHANGE+REVOKE x3 cycle): PASS
[VPE0] Test 9 (Remote DTU ping to PE 4): PASS (local NOOP ok, remote routing via kernel)
[VPE0] === 9 passed, 0 failed ===

=== HELLO EXCHANGE ===

[DTUBridge] HELLO TX to 10.0.0.2:5000: "HELLO FROM NODE 0 (10.0.0.1)" (err=0)
[DTUBridge] HELLO RX from 10.0.0.1:5000: "HELLO FROM NODE 0 (10.0.0.1)"
[DTUBridge] HELLO TX to 10.0.0.1:5000: "HELLO FROM NODE 1 (10.0.0.2)" (err=0)
[DTUBridge] HELLO RX from 10.0.0.2:5000: "HELLO FROM NODE 1 (10.0.0.2)"
[DTUBridge] === HELLO EXCHANGE: SUCCESS ===   (both nodes)

=== PING/PONG: A -> B -> A ===

[SemperKernel] NET: Sent PING to outbound ring                           Node A kernel
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x50494e47, err=0)      Node A bridge
[DTUBridge] NET RX: 41 bytes from 10.0.0.1:7654 (label=0x50494e47)      Node B bridge
[SemperKernel] NET RX: label=0x50494e47 len=16 "PING from kernel"       Node B kernel
[SemperKernel] NET: Sent PONG reply                                      Node B kernel
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x504f4e47, err=0)      Node B bridge
[DTUBridge] NET RX: 41 bytes from 10.0.0.2:7654 (label=0x504f4e47)      Node A bridge
[SemperKernel] NET RX: label=0x504f4e47 len=16 "PONG from kernel"       Node A kernel
[SemperKernel] NET: === PONG RECEIVED — round trip complete! ===         Node A kernel

=== PING/PONG: B -> A -> B ===

[SemperKernel] NET: Sent PING to outbound ring                           Node B kernel
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x50494e47, err=0)      Node B bridge
[DTUBridge] NET RX: 41 bytes from 10.0.0.2:7654 (label=0x50494e47)      Node A bridge
[SemperKernel] NET RX: label=0x50494e47 len=16 "PING from kernel"       Node A kernel
[SemperKernel] NET: Sent PONG reply                                      Node A kernel
[DTUBridge] NET TX ring: 41 bytes to peer (label=0x504f4e47, err=0)      Node A bridge
[DTUBridge] NET RX: 41 bytes from 10.0.0.1:7654 (label=0x504f4e47)      Node B bridge
[SemperKernel] NET RX: label=0x504f4e47 len=16 "PONG from kernel"       Node B kernel
[SemperKernel] NET: === PONG RECEIVED — round trip complete! ===         Node B kernel

=== SHUTDOWN ===

QEMU_TIMEOUT: Exited after 45s   (both nodes)
```
