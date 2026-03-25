/*
 * tsc_calibrate.h — TSC frequency for SemperOS-seL4
 *
 * Evaluation platform: Intel Xeon E5-2695 v4 @ 2.10 GHz (invariant TSC).
 * Frequency verified from XCP-ng dom0:
 *   - /proc/cpuinfo: cpu MHz = 2099.998
 *   - xenpm get-cpufreq-para: cur = 2101000 kHz
 *   - model name: Intel(R) Xeon(R) CPU E5-2695 v4 @ 2.10GHz
 *
 * Xen HVM does not expose TSC frequency to guests via CPUID (leaves 0x15,
 * 0x16, and 0x40000003 all return 0). PIT speaker gate calibration also
 * fails (Xen's emulated PIT does not set the OUT2 status bit for channel 2
 * mode 0). The frequency is therefore set as a verified constant.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef TSC_CALIBRATE_H
#define TSC_CALIBRATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intel Xeon E5-2695 v4 — invariant TSC @ 2100 MHz */
#define TSC_FREQ_KHZ  2100000ULL
#define TSC_FREQ_MHZ  2100U
#define TSC_METHOD    "dom0-verified-2100mhz"

/*
 * Convert TSC cycle count to microseconds.
 *   freq_khz = cycles per millisecond
 *   cycles * 1000 / freq_khz = us
 */
static inline uint64_t tsc_cycles_to_us(uint64_t cycles) {
    return (cycles * 1000ULL) / TSC_FREQ_KHZ;
}

/*
 * Convert TSC cycle count to nanoseconds.
 */
static inline uint64_t tsc_cycles_to_ns(uint64_t cycles) {
    return (cycles * 1000000ULL) / TSC_FREQ_KHZ;
}

#ifdef __cplusplus
}
#endif

#endif /* TSC_CALIBRATE_H */
