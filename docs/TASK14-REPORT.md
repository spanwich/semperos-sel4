# Task 14: Parameterised KERNEL_ID/PEER_IP Build — 3 Node ISOs

## Identity configuration

| Node | KERNEL_ID | SELF_IP | PEER_IP_0 | PEER_IP_1 |
|------|-----------|---------|-----------|-----------|
| node0 | 0 | 192.168.100.10 | 192.168.100.11 | 192.168.100.12 |
| node1 | 1 | 192.168.100.11 | 192.168.100.10 | 192.168.100.12 |
| node2 | 2 | 192.168.100.12 | 192.168.100.10 | 192.168.100.11 |

## Build artefacts

| Image | Size | SHA256 |
|-------|------|--------|
| semperos-node0.iso | 19M | 2035adb7f8ee1dfbe57a39cf2dda1f267879c60fd131175542ae7009b76988fc |
| semperos-node1.iso | 19M | 1611cf60af889732c8770639a8f3b5e62307f172493519f3788ab30a5ba4f7b7 |
| semperos-node2.iso | 19M | 050d4382ebddf7d8c6737795b2219f033ef834d828cbca726bb5a0adda9d6659 |

## Code changes made

| File | Change |
|------|--------|
| CMakeLists.txt | Added cmake variables: `KERNEL_ID`, `SELF_IP`, `PEER_IP_0`, `PEER_IP_1`. Pass `SEMPER_KERNEL_ID` to SemperKernel CXX_FLAGS. Pass `DTUB_SELF_IP`, `DTUB_PEER_IP_0`, `DTUB_PEER_IP_1`, `KERNEL_ID` to DTUBridge C_FLAGS. |
| Platform.cc | Changed `kernelId = 0` to `kernelId = SEMPER_KERNEL_ID` with `#ifndef` fallback. |
| DTUBridge.c | Replaced MAC-derived IP scheme with compile-time `DTUB_SELF_IP`, `DTUB_PEER_IP_0`, `DTUB_PEER_IP_1` string defines. Added `parse_ip4()` helper to convert IP strings to `ip4_addr_t` at init time. Replaced scalar `peer_ip_last` with `peer_addrs[NUM_PEERS]` array. Updated `send_hello()` to loop over both peers. Updated `net_net_send()` RPC to index `peer_addrs[dest_node]`. Updated ring-buffer outbound poll to use `peer_addrs[0]`. |

### Assumptions about DTUBridge peer routing

- The ring-buffer outbound path (kernel -> DTUBridge -> UDP) currently routes
  all messages to peer 0. Multi-peer routing by dest PE in the DTU header is
  future work — the kernel's `send_to()` in DTU.cc computes `dest_node` and
  can be extended to encode it in the ring message.
- The RPC path (`net_net_send`) correctly indexes `peer_addrs[dest_node]`,
  supporting routing to either peer.
- Hello exchange sends to all peers.

## Smoke test result

### node0 (KERNEL_ID=0)
- **11 passed, 0 failed**
- `[SemperKernel] Platform: 4 PEs, kernel PE=0, kernel ID=0`
- DTUBridge PCI probe fails gracefully with `-nic none` (expected)
- Exp 1 + Exp 2A benchmarks produce output

### node1 (KERNEL_ID=1)
- **11 passed, 0 failed**
- `[SemperKernel] Platform: 4 PEs, kernel PE=0, kernel ID=1`

### node2
- Built successfully, not smoke tested (identical code path as node0/node1
  with KERNEL_ID=2)

## What these images do NOT yet exercise

1. **No inter-node UDP** — the single-node QEMU smoke test runs with `-nic none`,
   so DTUBridge cannot reach peers. Cross-node capability operations will only
   be verified on XCP-ng with all three VMs on the same 192.168.100.0/24 network.

2. **No E1000 on XCP-ng** — XCP-ng HVM guests get a Realtek 8139 or Xen netfront
   by default. The E1000 must be explicitly configured in the VM definition, or
   the NIC type in DTUBridge must be changed. If using the full (non-xcpng)
   assembly, E1000 MMIO address may differ from QEMU's 0xFEB80000.

3. **Ring-buffer routing is single-peer** — outbound ring messages go to peer 0
   only. The RPC path (`net_net_send`) supports multi-peer. Full multi-peer
   routing through the ring path requires encoding the destination in the
   message header or ring metadata.

4. **Same PE layout on all nodes** — all three nodes have PE IDs 0-3. True
   multi-kernel requires disjoint PE ranges (e.g., node0: 0-3, node1: 4-7).
   This is a known limitation documented in CLAUDE.md.

5. **node2 is reserved** — currently runs the same test suite as node0/node1.
   It will be repurposed as the RaftPD quorum node for Contribution 3.
