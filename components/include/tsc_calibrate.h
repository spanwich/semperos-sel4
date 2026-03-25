/*
 * tsc_calibrate.h — TSC frequency calibration for SemperOS-seL4
 *
 * Measures the TSC frequency using the PIT speaker gate method.
 * Falls back to Xen PV clock CPUID, then to a hardcoded 2000 MHz constant.
 *
 * Usage: Call tsc_calibrate() once during VPE0 init, before any channels
 * are opened or benchmarks started. The result is stored in a global
 * that capbench.h macros can reference for us conversion.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef TSC_CALIBRATE_H
#define TSC_CALIBRATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Calibration result — set by tsc_calibrate(), read by benchmark macros.
 */
typedef struct {
    uint64_t freq_khz;       /* TSC frequency in kHz (e.g. 2000000 for 2 GHz) */
    uint32_t freq_mhz;       /* TSC frequency in MHz (e.g. 2000) — for quick display */
    const char *method;      /* "pit-speaker-gate" | "xen-pv-cpuid" | "fallback-2ghz" */
} tsc_cal_result_t;

/* Global calibration result. Valid after tsc_calibrate() returns. */
extern tsc_cal_result_t g_tsc_cal;

/*
 * Run TSC calibration. Tries methods in order:
 *   1. PIT speaker gate (10ms gate, I/O ports 0x42/0x43/0x61)
 *   2. Xen PV clock via CPUID leaf 0x40000003 (if running under Xen)
 *   3. Fallback: assume 2000 MHz
 *
 * Returns 0 on success (always succeeds — fallback is unconditional).
 * Populates g_tsc_cal.
 *
 * Requirements:
 *   - x86_64
 *   - I/O port access to 0x42, 0x43, 0x61 (for PIT method)
 *     On seL4/CAmkES: requires IOPort capabilities
 */
int tsc_calibrate(void);

/*
 * Convert TSC cycle count to microseconds using the calibrated frequency.
 * Requires tsc_calibrate() to have been called first.
 */
static inline uint64_t tsc_cycles_to_us(uint64_t cycles) {
    if (g_tsc_cal.freq_khz == 0) return 0;
    return (cycles * 1000ULL) / g_tsc_cal.freq_khz;
}

/*
 * Convert TSC cycle count to nanoseconds using the calibrated frequency.
 */
static inline uint64_t tsc_cycles_to_ns(uint64_t cycles) {
    if (g_tsc_cal.freq_khz == 0) return 0;
    return (cycles * 1000000ULL) / g_tsc_cal.freq_khz;
}

#ifdef __cplusplus
}
#endif

#endif /* TSC_CALIBRATE_H */
