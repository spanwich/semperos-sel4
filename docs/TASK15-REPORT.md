# Task 15: XCP-ng Runtime Fixes

## Fix 1 — DTUBridge PCI slot scan

**Problem:** Hardcoded PCI address 0:2.0 works on QEMU q35 but not on XCP-ng
where the e1000 lands at 0:4.0.

**Fix:** Replaced single-slot probe with a bus 0 scan (slots 0-31). The scan
logs every device found, stops at the first 8086:100E match, and stores the
discovered slot in `e1000_pci_bus/dev/fun` globals used by all subsequent PCI
config operations.

**File:** `components/DTUBridge/DTUBridge.c` — `e1000_pci_init()` rewritten.

**QEMU verification:** With `-nic none`, scan correctly reports no 82540EM found
after enumerating Q35 host bridge (0:0.0), VGA (0:1.0), and LPC (0:31.0).

## Fix 2 — VPE0 startup race retry loop

**Problem:** On XCP-ng cold boot, VPE0 fires before SemperKernel enters its
WorkLoop. Tests 1 and 2 fail because the syscall gate is not yet open.

**Fix:** Added a NOOP retry loop (20 attempts, 100K spin iterations between)
before Test 1. No fixed sleep — the retry converges as soon as the kernel is
ready. If all retries fail, a WARNING is printed but tests proceed.

**File:** `components/VPE0/VPE0.c` — before Test 1 block.

**QEMU verification:** 11/11 tests PASS. The retry loop adds negligible latency
on clean boots (kernel is typically ready on the first attempt).

## Fix 3 — TSC frequency probe

**Problem:** All benchmark µs values assumed hardcoded 2.0 GHz (QEMU default).
On XCP-ng, actual TSC rate depends on the host CPU.

**Fix:** `probe_tsc_frequency()` tries CPUID leaf 0x16 (Processor Frequency
Information, available on Skylake+ and Xen HVM), then leaf 0x15 (TSC/Crystal
Ratio), then falls back to 2000 MHz.

**TSC method used:**
- QEMU qemu64: `fallback-2ghz` (CPUID max leaf < 0x15)
- XCP-ng Intel: expected `cpuid-0x16` (returns actual base MHz)

**File:** `components/VPE0/VPE0.c` — added `probe_tsc_frequency()`, `cycles_to_us()`,
removed `BENCH_GHZ` constant.

**QEMU verification:** Reports `TSC frequency: 2000 MHz (method: fallback-2ghz)`.
All benchmark µs values unchanged (same 2 GHz denominator as before).

## Files changed

| File | Change |
|------|--------|
| `components/DTUBridge/DTUBridge.c` | PCI scan loop replacing hardcoded 0:2.0 |
| `components/VPE0/VPE0.c` | NOOP retry loop + TSC probe + `cycles_to_us()` |

## Smoke test

```
[VPE0] TSC frequency: 2000 MHz (method: fallback-2ghz)
[DTUBridge] Scanning PCI bus 0 for 82540EM (8086:100E)...
[DTUBridge] PCI 0:0.0: vendor=0x8086 device=0x29c0
[DTUBridge] PCI 0:1.0: vendor=0x1234 device=0x1111
[DTUBridge] PCI 0:31.0: vendor=0x8086 device=0x2918
[DTUBridge] ERROR: No 82540EM found on PCI bus 0
[VPE0] === 11 passed, 0 failed ===
```

Standalone tests: 10/10 PASS.
