# SemperOS on seL4/CAmkES: System Design

## 1. Overview

This document describes the architecture for running SemperOS on seL4 using CAmkES, with a virtual DTU (vDTU) replacing the gem5-simulated hardware DTU.

### Problem Statement

SemperOS depends on a Data Transfer Unit (DTU) — a hardware communication engine that was simulated in gem5 but never manufactured. The DTU provides:
- **Message passing:** Send/receive messages between Processing Elements (PEs)
- **Memory access:** Read/write remote PE memory
- **Endpoint management:** 16 configurable endpoints per PE
- **Notifications:** Wake sleeping PEs when messages arrive

We must virtualize all of these on seL4/CAmkES for x86_64.

### Solution Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Control Plane                         │
│                                                         │
│  ┌───────────────┐     RPC calls      ┌──────────────┐ │
│  │ SemperKernel  │ ──────────────────► │  vDTU        │ │
│  │   (PE 0)      │  config_send()      │  Service     │ │
│  │               │  config_recv()      │              │ │
│  │               │  config_mem()       │  Endpoint    │ │
│  │               │  invalidate_ep()    │  Table       │ │
│  └───────┬───────┘                     └──────────────┘ │
│          │                                               │
├──────────┼───────────────────────────────────────────────┤
│          │           Data Plane                          │
│          │                                               │
│          │  shared memory     ┌──────────────┐          │
│          ├───────────────────►│   VPE0       │          │
│          │   ring buffers     │   (PE 1)     │          │
│          │◄───────────────────│              │          │
│          │   (SPSC, no vDTU   │              │          │
│          │    involvement)    └──────────────┘          │
└─────────────────────────────────────────────────────────┘
```

**Key insight:** The vDTU is on the **control plane only**. After configuring an endpoint pair (send EP on PE A → recv EP on PE B), messages flow directly through shared memory ring buffers without any vDTU involvement. This avoids the vDTU becoming a bottleneck.

## 2. DTU-to-CAmkES Mapping

### 2.1 DTU Concepts → CAmkES Primitives

| DTU Concept | CAmkES Primitive | Notes |
|-------------|-----------------|-------|
| PE (Processing Element) | CAmkES component | Each PE is a separate protection domain |
| DTU MMIO registers | vDTU RPC + local state | No MMIO; register writes become RPC calls or local struct updates |
| SEND command | `vdtu_ring_send()` into shared dataport | Direct shared memory write, no RPC |
| FETCH_MSG command | `vdtu_ring_fetch()` from shared dataport | Direct shared memory read, no RPC |
| ACK_MSG command | `vdtu_ring_ack()` | Advance tail pointer in shared memory |
| READ/WRITE (memory EP) | Direct access to shared dataport | Memory EPs map to `seL4SharedData` frames |
| DTU wait()/HLT | `seL4_Wait()` on Notification | Blocked until signal; replaces busy-wait on MSGCNT |
| WAKEUP_CORE ext cmd | `seL4_Signal()` on Notification | Wake a blocked component |
| Endpoint configuration | RPC to vDTU → updates endpoint table | vDTU assigns pre-allocated channels |
| Credits (flow control) | Ring buffer full detection | `vdtu_ring_is_full()` replaces credit exhaustion |

### 2.2 Message Header Compatibility

The ring buffer uses the **exact same message header struct** as the gem5 DTU (`struct Header` from `DTU.h:150-161`):

```c
struct __attribute__((packed)) vdtu_msg_header {
    uint8_t  flags;            // 1 byte   offset 0
    uint16_t sender_core_id;   // 2 bytes  offset 1
    uint8_t  sender_ep_id;     // 1 byte   offset 3
    uint8_t  reply_ep_id;      // 1 byte   offset 4
    uint16_t length;           // 2 bytes  offset 5
    uint16_t sender_vpe_id;    // 2 bytes  offset 7
    uint64_t label;            // 8 bytes  offset 9
    uint64_t replylabel;       // 8 bytes  offset 17
};
// Total: 25 bytes (PACKED), sizeof(Header) = 25
```

This ensures SemperOS code that casts raw message pointers to `DTU::Message*` will work unchanged. The `vdtu_ring_send()` function fills in these header fields automatically, simulating the DTU hardware's auto-fill behavior.

## 3. Pre-allocation Strategy

### 3.1 The Problem

CAmkES requires all connections (shared memory, notifications, RPCs) to be declared statically in the `.camkes` assembly file. But SemperOS creates DTU endpoint channels dynamically at runtime via `config_send_remote()` / `config_recv_remote()`.

### 3.2 The Solution

We pre-allocate a pool of shared memory dataports and notifications in the CAmkES assembly, and the vDTU assigns them to DTU endpoints at runtime.

**Pool sizing for the minimal prototype (1 kernel + 1 VPE):**

| Resource | Count | Purpose |
|----------|-------|---------|
| Message channels (kernel ↔ VPE0) | 8 | Covers SYSC_GATES (6) + DEF_RECVEP (1) + spare (1) |
| Memory dataports (kernel ↔ VPE0) | 4 | For DTU memory endpoint operations |
| Notifications | 3 | kernel wake, VPE0 wake, kernel-done signal |
| RPC connection | 1 | Config calls from kernel to vDTU |

Each message channel is a 4 KiB `seL4SharedData` dataport containing a ring buffer with 4 × 512-byte slots (2,112 bytes used of 4,096 available).

### 3.3 Channel Assignment Flow

```
1. Kernel calls vdtu.config_recv(PE=1, EP=0, buf_order=11, msg_order=9)
   → vDTU assigns channel_idx=0, returns it
   → Kernel now knows: "VPE0 EP 0 receives on dataport msgchan_kv_0"

2. Kernel calls vdtu.config_send(PE=0, EP=0, dest_pe=1, dest_ep=0, ...)
   → vDTU records: "Kernel EP 0 sends to VPE0 EP 0 (channel 0)"
   → Kernel now knows: "to send to VPE0 EP 0, write to msgchan_kv_0"

3. Kernel writes message to msgchan_kv_0 via vdtu_ring_send()
   → VPE0 reads from msgchan_kv_0 via vdtu_ring_fetch()
   → No vDTU involvement on the data path
```

### 3.4 Scaling

For a larger system (N kernels + M VPEs), the pre-allocated pool must cover all PE pairs:
- Message channels: `N * M * ep_estimate` (e.g., 8 per pair)
- Memory channels: `N * M * 4` per pair
- This is quadratic, but manageable for small systems (< 16 PEs)

For very large systems, CAmkES's static allocation becomes a bottleneck. Solutions include:
- Dynamic CAmkES (experimental seL4 feature)
- Hierarchical vDTU (one vDTU per cluster of PEs)
- Compile-time generation of the `.camkes` file from a topology description

## 4. Ring Buffer Design

### 4.1 Layout

```
┌──────────────────────── 4 KiB Dataport ─────────────────────┐
│                                                              │
│  ┌──────────────── Ring Control (64 bytes) ──────────────┐  │
│  │ head (4B) │ tail (4B) │ slot_count │ slot_size │ mask │  │
│  │ [pad to 64 bytes for cache line alignment]            │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌──────────────── Slot 0 (512 bytes) ──────────────────┐  │
│  │ ┌─── DTU Header (25 bytes) ────┐                     │  │
│  │ │ flags │ coreid │ epid │ ...  │                     │  │
│  │ └──────────────────────────────┘                     │  │
│  │ ┌─── Payload (up to 487 bytes) ┐                     │  │
│  │ │ [syscall opcode + args]       │                     │  │
│  │ └──────────────────────────────┘                     │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌──────────────── Slot 1 ──────────────────────────────┐  │
│  │ ...                                                   │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌──────────────── Slot 2 ──────────────────────────────┐  │
│  │ ...                                                   │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌──────────────── Slot 3 ──────────────────────────────┐  │
│  │ ...                                                   │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                              │
│  [Unused: 4096 - 64 - 4*512 = 1984 bytes]                  │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 SPSC Protocol

The ring buffer uses a classic single-producer single-consumer protocol:

- **Producer** writes to `head`, **consumer** writes to `tail`
- Empty: `head == tail`
- Full: `(head + 1) & mask == tail` (one slot always empty to distinguish full from empty)
- Usable capacity: `slot_count - 1` messages

This matches the DTU's circular buffer semantics. The DTU uses separate read/write position registers; we use head/tail in shared memory.

### 4.3 Memory Ordering

On x86, stores are not reordered with other stores (Total Store Order). We only need compiler barriers (`asm volatile("" ::: "memory")`) to prevent the compiler from reordering:
- Before advancing `head`: ensure all slot data is written
- Before reading slot data: ensure we've read the latest `head`
- Before advancing `tail`: ensure we've finished reading the slot

## 5. Component Design

### 5.1 vDTU Service

**Responsibilities:**
- Maintain endpoint table: `endpoints[pe][ep]` with type, configuration, channel assignment
- Handle config RPCs from kernel(s)
- Assign pre-allocated channels to endpoint pairs
- Initialize ring buffers in dataports

**Not responsible for:**
- Data path (message sending/receiving)
- Polling or forwarding messages
- Credit enforcement (handled by ring buffer full detection)

**State:**
```c
struct ep_desc endpoints[MAX_PES][EP_PER_PE];   // 4 PEs × 16 EPs
int pe_vpe_id[MAX_PES];                          // VPE ID per PE
int pe_privileged[MAX_PES];                      // Privilege flag per PE
```

### 5.2 SemperKernel

**In this prototype:** Pure C test stub that exercises the CAmkES plumbing.

**In the real system (Task 04):** The actual SemperOS kernel compiled as a CAmkES component with:
- `arch/sel4/DTU.cc` replacing `arch/gem5/DTU.cc`
- `config_send_local()` → vDTU RPC + ring buffer init
- `send_to()` → `vdtu_ring_send()` to appropriate dataport
- `fetch_msg()` → `vdtu_ring_fetch()` from appropriate ring
- `wait()` → `seL4_Wait()` on notification

### 5.3 VPE0

**In this prototype:** Simple echo server that reads messages and sends replies.

**In the real system (Task 04):** A SemperOS application binary with:
- The m3 user-space library compiled for seL4
- `m3::DTU::send()` → `vdtu_ring_send()` to syscall ring
- `m3::DTU::fetch_msg()` → `vdtu_ring_fetch()` from receive ring

## 6. DTU Operation Mapping (Detailed)

### 6.1 SEND (data path)

**gem5:** Write DATA_ADDR, DATA_SIZE, REPLY_LABEL, REPLY_EP, COMMAND(SEND) to MMIO registers. DTU hardware copies message to destination receive buffer, fills header.

**seL4:** `vdtu_ring_send(ring, sender_pe, sender_ep, ..., payload, len)`. Writes directly to shared memory. Optionally followed by `seL4_Signal(notification)` to wake receiver.

### 6.2 FETCH_MSG (data path)

**gem5:** Write COMMAND(FETCH_MSG) to MMIO. Read OFFSET register for message pointer.

**seL4:** `vdtu_ring_fetch(ring)`. Returns pointer into shared memory, or NULL if empty.

### 6.3 ACK_MSG (data path)

**gem5:** Write OFFSET, COMMAND(ACK_MSG) to MMIO.

**seL4:** `vdtu_ring_ack(ring)`. Advances tail pointer.

### 6.4 READ/WRITE (memory endpoints)

**gem5:** Configure memory EP, then issue READ/WRITE commands. DTU does DMA.

**seL4:** The kernel configures a memory EP via vDTU RPC. The vDTU assigns a shared dataport. Both sides then access the dataport directly via `memcpy()`. No DMA needed — it's shared memory.

### 6.5 wait() (blocking)

**gem5:** Check MSGCNT register. If 0, execute HLT. DTU wakes CPU on message arrival.

**seL4:** `seL4_Wait(notification)`. Blocked until another component calls `seL4_Signal(notification)`. The sender signals after writing to the ring buffer.

### 6.6 Endpoint Configuration (control plane)

**gem5:** Kernel writes 3 × 64-bit EP registers via MMIO (local) or via memory EP to remote PE.

**seL4:** Kernel calls `vdtu.config_send()` / `vdtu.config_recv()` RPC. The vDTU updates its endpoint table and assigns a shared memory channel.

## 7. Test Plan

### 7.1 Standalone Ring Buffer Test

Run `tests/test_ring.c` on the host (no seL4 needed):

```
$ cd tests && make test
=== vDTU Ring Buffer Tests ===
  TEST: ring_init with valid params                        PASS
  TEST: ring_init rejects bad params                       PASS
  TEST: verify DTU header struct size                      PASS
  TEST: total_size calculation                             PASS
  TEST: send one message, fetch it back                    PASS
  TEST: fill all slots, verify full detection              PASS
  TEST: fetch from empty ring returns NULL                 PASS
  TEST: send with payload exceeding slot size              PASS
  TEST: wraparound: write 2, read 2, write 3, read 3      PASS
  TEST: attach to existing ring buffer                     PASS
=== Results: 10 passed, 0 failed ===
```

### 7.2 CAmkES System Test

Boot on QEMU x86_64. Expected serial output:

```
[vDTU] Initialized, managing endpoint table
[vDTU] config_recv(target_pe=1, ep=0, buf_order=11, msg_order=9, flags=0)
[vDTU]   -> assigned channel 0 (slot_count=4, slot_size=512)
[vDTU] config_recv(target_pe=0, ep=1, buf_order=11, msg_order=9, flags=0)
[vDTU]   -> assigned channel 1 (slot_count=4, slot_size=512)
[vDTU] config_send(target_pe=0, ep=0, dest_pe=1, dest_ep=0, ...)
[SemperKernel] Sending test message: "HELLO_VPE"
[VPE0] Received message: "HELLO_VPE" (len=9)
[VPE0] Sending reply: "ACK"
[SemperKernel] Received reply: "ACK"
[SemperKernel] Basic DTU channel test PASSED
```

## 8. Future Work (Tasks 03-05)

1. **Task 03: vDTU Full Implementation** — Complete the vDTU with proper channel management, notification-based waking, and memory endpoint support.

2. **Task 04: SemperOS Integration** — Create `arch/sel4/` platform files that compile the real SemperOS kernel and m3 library against the vDTU ring buffer API.

3. **Task 05: Multi-Kernel** — Add SemperKernel1, inter-kernel message channels, and the DDL coordination protocol.
