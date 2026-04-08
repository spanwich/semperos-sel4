# Experiment Results — SemperOS-seL4

Load this file when keywords: "benchmark", "experiment", "results", "cycles", "latency", "Hille"

---

## TSC Calibration

TSC frequency: 2100 MHz (dom0-verified constant).
Method: dom0-verified-2100mhz. Intel Xeon E5-2695 v4, invariant TSC.
Implementation: header-only `tsc_calibrate.h` with `#define TSC_FREQ_KHZ 2100000ULL`.
No runtime calibration — PIT speaker gate and Xen PV CPUID both fail on
XCP-ng HVM guests (Xen PIT emulation gap). Cycle counts are correct;
µs values use the verified 2100 MHz constant.

---

## Experiment 1 Results (collected 2026-03-06)

QEMU q35, single-node Docker, 2 GHz assumed clock. 1000 warmup + 10000 measured iterations.
VDTUService success-path logging disabled (`VDTU_VERBOSE_LOG` not defined).

```
[BENCH] ring_write         min=2706     med=2814     mean=2919     max=64480    cycles  (1.4us median)
[BENCH] ring_read          min=234      med=254      mean=266      max=30298    cycles  (0.1us median)
[BENCH] ring_roundtrip     min=2900     med=3046     mean=3207     max=193798   cycles  (1.5us median)
[BENCH] ep_configure       min=47146    med=49686    mean=51604    max=1153970  cycles  (24.8us median)
[BENCH] ep_terminate       min=46146    med=48940    mean=50420    max=258398   cycles  (24.5us median)
[BENCH] mem_access         min=326      med=346      mean=392      max=232290   cycles  (0.2us median)
```

Note: Ring benchmarks use malloc'd buffer (not CAmkES dataport) due to 4 KiB
dataport size limitation (see open question 3 in TASK-EXP1-REPORT.md). RPC
benchmarks (ep_configure, ep_terminate) use real CAmkES seL4RPCCall path.
To re-enable per-RPC logging for debugging, build with `-DVDTU_VERBOSE_LOG`.

---

## Experiment 2A Results — QEMU (collected 2026-03-10)

QEMU q35, single-node Docker, SEMPER_BENCH_MODE=ON, 2 GHz assumed clock.

```
[BENCH] local_exchange     med=29952944  cycles  (15.0ms median) [n=1000]
[BENCH] local_revoke       med=29985160  cycles  (15.0ms median) [n=1000]
[BENCH] chain_revoke_10    med=30089216  cycles  (15.0ms median) [n=200]
[BENCH] chain_revoke_50    med=30119794  cycles  (15.1ms median) [n=200]
[BENCH] chain_revoke_100   med=30133564  cycles  (15.1ms median) [n=200]
```

**Key finding:** All 5 benchmarks show identical ~15ms medians regardless of
chain depth. The dominant cost is the seL4_Yield() cooperative scheduling
overhead on single-core QEMU, not the capability tree walk (which adds <0.1ms
even at depth 100). On XCP-ng multi-core hardware, this will be reduced by
orders of magnitude due to notification-driven wakeup.

gem5 comparison (Hille et al. 2019): local exchange 1.8µs, local revoke 1.0µs.
The 8000x overhead is from CAmkES multi-component scheduling on single-core QEMU,
not from the vDTU data structures or algorithms.

### Exp 2A development-mode (KLOG enabled, pre-Task-10)

Initial measurements before SEMPER_BENCH_MODE investigation (included for record):
```
local_exchange:  ~29.9M cycles / ~15.0ms (identical to clean — KLOG was already runtime-gated)
local_revoke:    ~30.0M cycles / ~15.0ms
chain benchmarks: timed out at BENCH_ITERS=10000 (iteration cost ~15ms × 11K = ~165s)
```
Investigation showed KLOG was not the root cause — SYSC/KRNLC/EPS log levels
were already disabled by `KernelLog::level = INFO | ERR`. See docs/TASK10-REPORT.md.

---

## Experiment 2A Results — XCP-ng (paper-ready, collected 2026-03-25/26)

XCP-ng 3-node cluster, kernel-internal rdtsc, SEMPER_BENCH_MODE=ON.
TSC: 2100 MHz (dom0-verified, Intel Xeon E5-2695 v4, invariant TSC).
PIT speaker gate and Xen PV CPUID both failed on XCP-ng HVM — Xen's PIT
emulation does not set OUT2 bit, CPUID 0x40000003 returns 0.

| Benchmark | node0 med | node1 med | node2 med | µs @ 2100 MHz |
|---|---|---|---|---|
| local_exchange_kernel | 366 | 352 | 346 | 0.17 µs |
| local_revoke_kernel | 6,096 | 6,562 | 6,502 | 3.0 µs |
| chain_revoke_10_kernel | 30,748 | 29,894 | 33,116 | 14.6 µs |
| chain_revoke_25_kernel | 76,166 | 72,972 | 80,932 | 36.3 µs |
| chain_revoke_50_kernel | 153,500 | 146,052 | 163,378 | 73.1 µs |
| chain_revoke_100_kernel | 315,638 | 308,088 | 326,686 | 150.3 µs |

Comparison to Hille et al. (gem5, 2 GHz):
- local_exchange: 0.10x Hille (350 vs 3,597 cycles) — our brackets measure
  pure CapTable ops; Hille includes full syscall handler overhead
- local_revoke: 3.16x Hille — the ~2,943-cycle invalidate_ep CAmkES RPC
  accounts for the overhead
- chain_revoke: 1.64x Hille — constant across all depths, attributable to
  O(1) CAmkES RPC per revoked capability

Spanning exchange and revoke: NOT YET MEASURED. Blocked on direct exchange
protocol implementation (Contribution 4). Session protocol has been replaced
— see Known Limitations in CLAUDE.md.
