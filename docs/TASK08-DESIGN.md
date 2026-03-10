# Task 08 Design: Kernelcalls::connect() -- Inter-Kernel Channel Setup

## Problem Statement

The `Kernelcalls::connect()` function is the original SemperOS mechanism for
establishing communication channels between kernels on different PEs (nodes).
In the gem5 implementation, kernels send DTU messages directly via hardware
endpoints. On our seL4/CAmkES implementation, inter-kernel messages are
transported via the DTUBridge component (E1000 + lwIP UDP).

Currently, the CONNECT message path exists but is disconnected from the actual
transport layer:

1. `Kernelcalls::connect()` (Kernelcalls.cc:392-427) serializes a CONNECT
   message and calls `DTU::get().send_to()`. For remote PEs (core >= 4),
   `send_to()` routes via `net_ring_send()` (DTU.cc:364-378) to the outbound
   ring buffer, which DTUBridge forwards over UDP.

2. `KernelcallHandler::connect()` (KernelcallHandler.cc:1072-1133) handles
   the incoming CONNECT and creates a KPE entry via `Coordinator::addKPE()`.

The gap: while the CONNECT handshake produces KPE objects with `_localEP` and
`_remoteEP` fields, no DTU endpoint is actually configured for the inter-kernel
channel. The KPE's `sendTo()` calls `DTU::get().send_to()` which routes via
the local shared-memory channel table -- but remote kernels are not in that
table. The net ring path (DTU.cc:364) uses a flat PE threshold (`NUM_LOCAL_PES`)
rather than the KPE endpoint structure.

## Existing Code Analysis

### Kernelcalls::connect() (Kernelcalls.cc:392-427)

Two overloads:

1. **Initial request** (line 392): Takes kernel ID, core, stage, PE list.
   Creates a `ConnectionRequest` object, serializes the CONNECT message with
   membership flags and PE descriptors, then sends via `DTU::send_to()` to
   `SETUP_EP` (EP 13) on the remote kernel.

2. **Reply** (line 417): Simpler -- sends back `myKid`, `myCore`, and
   `epid` (the local endpoint allocated for this peer kernel).

### KernelcallHandler::connect() (KernelcallHandler.cc:1072-1133)

On KREQUEST:
- Reserves a local EP via `reserve_ep(kid)` for the new kernel.
- Creates KPE entry via `Coordinator::addKPE("kernel", kid, core, localEp, epid)`.
- Sends CONNECT KREPLY with the allocated EP.
- Updates membership table and announces existing services.

On KREPLY:
- Looks up the pending `ConnectionRequest` by kid.
- Creates KPE entry via `Coordinator::addKPE("kernel", kid, core, localEp, epid)`.

### DTU.cc Remote Routing (lines 345-378)

The routing logic in `send_to()` uses a simple threshold:
```cpp
#define NUM_LOCAL_PES 4
if (vpe.core >= NUM_LOCAL_PES) {
    int dest_node = (vpe.core - NUM_LOCAL_PES) / NUM_LOCAL_PES;
    net_ring_send(MY_PE, ep, ...);
}
```

This flat routing ignores the KPE endpoint configuration entirely.

### KPE::sendTo() (KPE.cc:56-78)

Calls `DTU::get().send_to(VPEDesc(_core, _id), _remoteEP, ...)`. The
`_core` field is the remote kernel's PE (e.g., 4 for node 1), and
`_remoteEP` is the EP assigned by the remote kernel's connect handler.

### Net Transport (camkes_entry.c + DTUBridge.c)

- **Outbound**: `net_ring_send()` writes to `g_net_out_ring`. DTUBridge
  polls this ring, wraps messages in UDP, and sends to the peer IP.
- **Inbound**: DTUBridge's `dtu_udp_recv_cb()` writes received UDP
  payloads to `g_net_in_ring`. Kernel polls this in `net_poll()`.
- Current `net_poll()` only handles PING/PONG labels. It does not dispatch
  messages to the KernelcallHandler.

## Design

### What Messages Are Exchanged

The CONNECT handshake is a two-message exchange:

```
Kernel A (initiator)                    Kernel B (responder)
  |                                        |
  |--- CONNECT KREQUEST ------------------>|
  |    {kid_A, core_A, epid_A,             |
  |     numPEs, flags, PEDescs[]}          |
  |                                        |
  |    [B reserves EP, creates KPE]        |
  |                                        |
  |<--- CONNECT KREPLY --------------------|
  |     {kid_B, core_B, epid_B}           |
  |                                        |
  |    [A creates KPE, done]               |
```

The message format is already defined in `Kernelcalls::connect()`:
- KREQUEST: `{op=CONNECT, stage=KREQUEST, myKid, myCore, epid, flags, numPEs, PEDescs[]}`
- KREPLY: `{op=CONNECT, stage=KREPLY, myKid, myCore, epid}`

### What State Is Stored After Connection

After a successful connect, both kernels have a KPE entry:

```cpp
// On Kernel A:
KPE("kernel", kid_B, core_B, localEp_A, remoteEp_B);
// stored in Coordinator::_kpes[kid_B]

// On Kernel B:
KPE("kernel", kid_A, core_A, localEp_B, remoteEp_A);
// stored in Coordinator::_kpes[kid_A]
```

Where:
- `kid` = remote kernel's ID
- `core` = remote kernel's PE ID
- `localEp` = EP reserved on this kernel for receiving from the peer
- `remoteEp` = EP the peer allocated for receiving from us

### Implementation Plan

The implementation has three parts:

#### Part 1: Inbound Network Message Dispatch

Currently `net_poll()` in `camkes_entry.c` only handles PING/PONG labels.
For CONNECT (and all other inter-kernel messages) to work, inbound messages
from the network must be dispatched through the DTU message fetch path.

**Change**: Modify `net_poll()` to write inbound messages to a KRNLC receive
channel (one of the 8 pre-allocated message channels). The WorkLoop already
polls KRNLC endpoints (WorkLoop.cc:86-91), so messages placed in a KRNLC
channel will be automatically dispatched to `KernelcallHandler::handle_message()`.

Specifically:
1. Reserve a message channel for network-inbound KRNLC messages.
   Use KRNLC_EP 0 (already configured as a recv endpoint in kernel.cc:59).
2. In `net_poll()`, when a message arrives on the inbound ring, copy its
   payload into the KRNLC recv channel's ring buffer (making it appear as
   if a local DTU send deposited it).
3. The WorkLoop fetches it from that EP and dispatches normally.

**Alternative approach (simpler)**: Instead of injecting into KRNLC channels,
export a C function `dispatch_net_krnlc_msg()` that directly calls
`KernelcallHandler::handle_message()` from `net_poll()`. This avoids channel
contention but requires careful handling of the GateIStream abstraction.

**Chosen approach**: The simpler alternative. We create a C wrapper
`kernel_handle_net_krnlc()` in `camkes_entry.c` that the C++ side calls from
`net_poll()`. The C++ side constructs a GateIStream from the raw message
bytes and dispatches through KernelcallHandler. This mirrors how the WorkLoop
handles local messages.

#### Part 2: KRNLC Endpoint Configuration for Remote Kernels

When a CONNECT completes and a KPE is created, we need to ensure `KPE::sendTo()`
can actually send messages to the remote kernel. Currently `sendTo()` calls
`DTU::get().send_to(VPEDesc(_core, _id), _remoteEP, ...)` which routes via
the PE threshold.

**Change**: The existing routing in `DTU::send_to()` already works for remote
PEs (core >= NUM_LOCAL_PES goes through `net_ring_send()`). The key fix is:

1. Make `net_ring_send()` preserve the destination EP and kernel ID in the
   DTU message header so the receiving side can identify which KRNLC gate
   the message belongs to.
2. Ensure `net_poll()` uses the message header's label/replylabel to route
   to the correct KernelcallHandler endpoint.

The label field in the DTU header carries the sender's kernel ID (`replylbl`
parameter in `send_to()`), which is how the KernelcallHandler identifies the
source kernel (via `is.label()` -> `Coordinator::get().getKPE(is.label())`).

#### Part 3: Integration Testing

A test in VPE0 that triggers the connect sequence:
- This is fundamentally a dual-node test (requires two QEMU instances).
- For single-node verification, we test that:
  1. The connect message can be serialized and sent to the outbound ring.
  2. A loopback test: manually inject a CONNECT KREPLY message into the
     inbound ring and verify the KPE is created.

### How Connection Integrates with WorkLoop Dispatch

The WorkLoop (WorkLoop.cc:64-130) already has the structure:

```
while(has_items()) {
    dtu.wait();

    // Poll KRNLC endpoints (8 gates)
    for(int i = 0; i < DTU::KRNLC_GATES; i++) {
        msg = dtu.fetch_msg(krnlep[i]);
        if(msg) {
            GateIStream is(krnlch.rcvgate(i), msg);
            krnlch.handle_message(is, nullptr);
        }
    }

    // Poll SYSC endpoints (6 gates)
    // ...

    tmng.yield();
    net_poll();  // <-- network polling happens here
}
```

With our change, `net_poll()` will dispatch inbound KRNLC messages directly
by constructing a GateIStream and calling `krnlch.handle_message()`. This
means network messages are dispatched in the same WorkLoop iteration they
arrive, with the same priority as locally-originated KRNLC messages.

### What a Passing Test Looks Like

**Single-node test (Test 10 in VPE0):**
```
[VPE0] Test 10 (CONNECT handshake): PASS
  - CONNECT KREQUEST serialized to outbound ring (N bytes)
  - Coordinator has 0 KPEs before connect (expected: no remote kernels)
```

**Dual-node test (docker compose up):**
```
[Node A SemperKernel] CONNECT KREQUEST sent to node 1
[Node B DTUBridge] NET RX: N bytes (label=CONNECT)
[Node B SemperKernel] kernelcall::connect(KREQUEST, kid=0, core=0, epid=6)
[Node B SemperKernel] Connecting to kernel #0 with EP 6
[Node B SemperKernel] CONNECT KREPLY sent to node 0
[Node A DTUBridge] NET RX: N bytes (label=CONNECT)
[Node A SemperKernel] kernelcall::connect(KREPLY, kid=1, core=4, epid=6)
[Node A SemperKernel] Connecting to kernel #1 with EP 6
```

## Risks and Mitigations

1. **GateIStream from raw bytes**: Constructing a GateIStream from network
   data requires matching the gem5 DTU message format exactly. Mitigated by
   reusing the existing `vdtu_msg_header` struct which is already
   binary-compatible.

2. **Thread safety**: `net_poll()` and WorkLoop run on the same thread
   (single-core QEMU), so no concurrency issues.

3. **Channel budget**: The 8 message channels are finite. CONNECT setup
   does not consume additional channels -- it reuses KRNLC_EP channels
   already configured in `configure_recv_endpoints()`.

## Non-Goals

- Full multi-kernel PE distribution (disjoint PE ranges) -- deferred.
- Service announcements after connect -- already handled by existing code.
- Two-tier revocation integration -- Task 09 + later.
