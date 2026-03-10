# Experiment 2A: Capability Operation Benchmarks (Local, Unverified)

## Overview

Measures local (single-kernel) capability exchange and revocation latency
using rdtsc. All operations are between VPE0 and VPE1 on the same kernel.

Label: `[BENCH-2A-LOCAL-UNVERIFIED]`

## Methodology

- Platform: QEMU q35, x86_64, single-node Docker
- CPU model: qemu64 (assumed 2 GHz clock for us conversion)
- Warmup: variable (100-1000 depending on chain depth)
- Measured iterations: 10000 (local_exchange, local_revoke), 1000 (chain_50),
  500 (chain_100), 10000 (chain_10)
- Timing: x86 rdtsc (cycle-accurate)
- Statistics: min, median, mean, max

## Benchmark descriptions

| Benchmark | Description |
|-----------|-------------|
| local_exchange | VPE0 obtains a capability from VPE1 via EXCHANGE syscall |
| local_revoke | Create gate + delegate to VPE1 + revoke (cascade to child) |
| chain_revoke_10 | Build depth-10 delegation chain, revoke root |
| chain_revoke_50 | Build depth-50 delegation chain, revoke root |
| chain_revoke_100 | Build depth-100 delegation chain, revoke root |

## Results

**Status**: Benchmark code implemented in VPE0.c. On-target collection requires
Docker/QEMU run. CAmkES image built successfully.

Results will be filled in after on-target execution:

```
[BENCH-2A-LOCAL-UNVERIFIED] local_exchange:    (pending)
[BENCH-2A-LOCAL-UNVERIFIED] local_revoke:      (pending)
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_10:   (pending)
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_50:   (pending)
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_100:  (pending)
```

## Paper comparison targets (Hille et al. 2019, gem5 2GHz)

| Operation | gem5 cycles | gem5 latency |
|-----------|-------------|--------------|
| Local exchange | 3597 | 1.8 us |
| Local revoke | 1997 | 1.0 us |
| Chain d=100 | ~200K | ~100 us |

## Implementation notes

1. **Exchange benchmark**: Measures obtain path (EXCHANGE with obtain=true).
   Setup: create gate at sel 200, delegate to VPE1:210. Then repeatedly
   obtain into rotating selectors (300+i%100) and revoke to clean up.

2. **Revoke benchmark**: Measures revoke of a single parent-child pair.
   Setup per iteration: create gate, delegate to VPE1, then time the revoke.

3. **Chain benchmarks**: Build alternating delegate/obtain chains between
   VPE0 and VPE1. For chain depth d, uses capability selectors
   200, 210, 220, ... (depth 10) or 200, 202, 204, ... (depth 50/100).

4. **Iteration counts**: Deep chains (50, 100) use fewer iterations (1000,
   500) because each iteration involves d+1 syscall roundtrips for setup.

## Observations

(To be filled after on-target collection)

1. local_exchange: (pending)
2. local_revoke: (pending)
3. chain_revoke_10: (pending)
4. chain_revoke_50: (pending)
5. chain_revoke_100: (pending)
