# FPT-176 Jira Comment: vDTU Channel Architecture Redesign (2026-04-09)

## Finding: vDTU does not virtualise the gem5 DTU — it implements a different device

### The gem5 DTU model (Hille et al. 2019, Section 2.2)

Each PE has a DTU with **16 identical endpoint slots** (`EP_COUNT = 16`, gem5/Config.h:31). Each slot can be configured at runtime as SEND, RECEIVE, or MEMORY — the PE's software decides, not the hardware. The DTU hardware provides:

- 16 uniform endpoint registers per PE
- Any endpoint can be any type (send/recv/mem)
- The kernel configures endpoints on behalf of VPEs
- Once configured, applications use endpoints without kernel involvement
- The NoC performs DMA-copy from sender's local buffer to receiver's local buffer

### What our vDTU currently provides

- VPE0: 8 pre-allocated "message channels" + 4 pre-allocated "memory channels" = 12 channels
- VPE1: 4 "message channels" + 2 "memory channels" = 6 channels
- The msg/mem type is baked into the CAmkES assembly at compile time
- VPE1 cannot use more than 4 message endpoints even if it needs 8
- VPE1 has fewer channels than VPE0 — asymmetric, unlike gem5

This is **not virtualisation** — it's a different device with fixed-function channels and arbitrary capacity limits. If a VPE application needs 12 message endpoints and 0 memory endpoints, our design prevents it.

### The correct vDTU design

**16 uniform 4 KiB dataports per PE pair**, matching `EP_COUNT = 16`:

- Each dataport is a "virtual NoC link" between two PEs
- No msg/mem distinction — every channel is the same physical resource
- VDTUService allocates from a **flat per-PE pool of 16**
- The endpoint type (send/recv/mem) is tracked in VDTUService's endpoint table, decided at runtime by the PE's software
- Both VPEs get equal 16-channel capacity — uniform, like gem5

### CAmkES assembly changes

```
Kernel component:     32 dataports (16 for VPE0 links + 16 for VPE1 links)
VPE0 component:       16 dataports
VPE1 component:       16 dataports
VDTUService pool:     32 channels (16 per PE pair)
```

Each `seL4SharedData` connection = one virtual NoC point-to-point link. The channel index within each PE's pool (0-15) corresponds to gem5's EP register index.

Cost: 32 dataports × 4 KiB = 128 KiB physical memory. Trivial on 512 MiB.

### What this also fixes

The Layer 3 blocker (VPE1 syscalls invisible to kernel) is fixed by this redesign. The kernel allocates per-VPE SYSC recv EPs, each on a channel from that VPE's pool. This matches Hille's design: the kernel PE has dedicated recv endpoints per sending VPE, and each VPE-to-kernel link is a separate NoC connection.

### Paper framing

"The vDTU virtualises the M3 DTU's per-PE endpoint model. Each CAmkES shared memory dataport represents one NoC point-to-point link. Every PE is provisioned with 16 uniform dataport channels, matching the gem5 DTU's 16-endpoint hardware register file. Endpoint type (send, receive, memory) is determined at runtime by the kernel's configuration RPCs, preserving the gem5 DTU's dynamic endpoint semantics. The VDTUService manages endpoint-to-channel routing, replacing the DTU hardware's NoC address decoding."

### Implementation sequence

1. Redesign `vdtu_channels.h`: single `VDTU_CHANNELS_PER_PE = 16` pool, remove msg/mem split
2. CAmkES assembly: 16 uniform dataports per PE pair
3. VDTUService: flat per-PE channel allocation (PE 0-2 → channels 0-15, PE 3 → channels 16-31)
4. DTU.cc: single channel array init, remove msg/mem arrays
5. VPE0.c / VPE1.c: uniform channel init
6. Kernel: per-VPE SYSC recv EP configuration
7. Verify Layer 3 (VPE1 createsrv) works
