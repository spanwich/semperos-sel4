# Task 08: Kernelcalls::connect() -- Inter-Kernel Channel Setup

## What was implemented

1. **Network message dispatch to KernelcallHandler** (Task 08 core):
   - Added `dispatch_net_krnlc()` function in WorkLoop.cc that constructs a
     `GateIStream` from raw network DTU messages and dispatches them through
     `KernelcallHandler::handle_message()`.
   - Modified `net_poll()` in camkes_entry.c to call `dispatch_net_krnlc()`
     for any inbound message whose label is not PING or PONG.
   - Used `is.claim()` to prevent the GateIStream destructor from acking the
     wrong ring buffer (the inbound ring is separately acked by net_poll).

2. **Message flow for CONNECT**:
   - Kernel A serializes CONNECT KREQUEST via `Kernelcalls::connect()`.
   - `KPE::sendTo()` routes through `DTU::send_to()` which uses `net_ring_send()`
     for remote PEs (core >= NUM_LOCAL_PES = 4).
   - DTUBridge polls outbound ring, wraps in UDP, sends to peer.
   - Remote DTUBridge receives UDP, writes to inbound ring.
   - Remote `net_poll()` dispatches via `dispatch_net_krnlc()`.
   - `KernelcallHandler::connect()` processes KREQUEST, creates KPE, sends KREPLY.

3. **Existing connect handler** (KernelcallHandler.cc:1072-1133) was already
   fully implemented -- no changes needed. It correctly handles both KREQUEST
   and KREPLY stages, creates KPE entries, reserves endpoints, and updates
   membership tables.

## Design document

See `docs/TASK08-DESIGN.md` for the full design rationale.

## Files changed

| File | Lines | Change |
|------|-------|--------|
| `components/SemperKernel/src/kernel/WorkLoop.cc` | +15 lines | Added `dispatch_net_krnlc()` extern "C" function |
| `components/SemperKernel/camkes_entry.c` | +8 lines | Added dispatch call for non-PING/PONG messages |

## Test results

- Standalone ring buffer tests: 10/10 PASS
- CAmkES build: SUCCESS (x86_64/pc99, build-node0)
- On-target tests: require Docker/QEMU (image available at build-node0/images/)

## Open questions

1. **GateIStream ack suppression**: Using `is.claim()` prevents mark_read on
   the KRNLC ring for network messages. If the KRNLC ring is also receiving
   local messages on the same channel, this could cause issues. Currently
   single-kernel mode has no local KRNLC traffic, so this is safe.

2. **Reply routing**: When a CONNECT handler sends a reply, it goes through
   `KPE::reply()` -> `DTU::send_to()` -> `net_ring_send()`. The reply
   includes the sender's kernel ID as the label, which the remote side uses
   to identify the source. This relies on the DTU header label field being
   preserved through the UDP transport, which DTUBridge already does.

3. **Full testing**: Requires dual-QEMU setup (`docker compose up`). The
   connect handshake is exercised when both nodes attempt to connect during
   startup.
