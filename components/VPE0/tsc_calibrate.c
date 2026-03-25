/*
 * tsc_calibrate.c — TSC frequency calibration for SemperOS-seL4
 *
 * Method 1: PIT speaker gate (10ms gate time)
 *   Programs PIT channel 2 in one-shot mode. Uses the speaker gate
 *   (port 0x61) to start/stop the countdown. Measures TSC ticks over
 *   the known 10ms interval.
 *
 * Method 2: Xen PV clock CPUID
 *   If running under Xen, CPUID leaf 0x40000003 may expose TSC frequency.
 *
 * Method 3: Fallback
 *   Hardcoded 2000 MHz. Cycle counts remain correct; us values are
 *   approximate.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "tsc_calibrate.h"
#include <camkes.h>

/* Global result */

tsc_cal_result_t g_tsc_cal = {
    .freq_khz = 2000000,
    .freq_mhz = 2000,
    .method   = "fallback-2ghz"
};

/* x86 helpers */

static inline uint64_t rdtsc_ordered(void) {
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

/* PIT constants */

#define PIT_FREQ_HZ     1193182ULL
#define PIT_CH2_DATA    0x42
#define PIT_CMD         0x43
#define PORT_SPKR       0x61

/*
 * I/O port access via CAmkES-generated IOPort wrappers.
 * VPE0 has two IOPort capabilities:
 *   pit_port:  0x42-0x43 (PIT channel 2 data + command)
 *   spkr_port: 0x61      (speaker gate control)
 * Raw inb/outb would cause a GP fault — seL4 requires IOPort caps.
 */
/*
 * CAmkES IOPort _offset variants — offset is relative to the port range base.
 * PIT range: 0x42-0x43 → offset 0 = port 0x42, offset 1 = port 0x43
 * Speaker range: 0x61 → offset 0 = port 0x61
 */
static inline void pit_outb(uint8_t val, uint16_t port) {
    pit_port_out8_offset(port - PIT_CH2_DATA, val);
}

static inline uint8_t pit_inb(uint16_t port) {
    return pit_port_in8_offset(port - PIT_CH2_DATA);
}

static inline void spkr_outb(uint8_t val) {
    spkr_port_out8_offset(0, val);
}

static inline uint8_t spkr_inb(void) {
    return spkr_port_in8_offset(0);
}

/* 0xB0 = channel 2, lobyte/hibyte, mode 0 (one-shot), binary */
#define PIT_CMD_CH2_MODE0  0xB0

#define GATE_MS         10
#define PIT_COUNT       ((uint16_t)((PIT_FREQ_HZ * GATE_MS + 500) / 1000))

#define PIT_SAMPLES     3

#define FREQ_MIN_MHZ    500
#define FREQ_MAX_MHZ    6000

/* Method 1: PIT speaker gate */

static uint64_t pit_measure_once(void) {
    uint8_t spkr;
    uint64_t tsc_start, tsc_end;
    uint32_t timeout;

    /* Disable gate and speaker */
    spkr = spkr_inb();
    spkr &= 0xFC;
    spkr_outb(spkr);

    /* Program PIT channel 2 */
    pit_outb(PIT_CMD_CH2_MODE0, PIT_CMD);
    pit_outb((uint8_t)(PIT_COUNT & 0xFF), PIT_CH2_DATA);
    pit_outb((uint8_t)((PIT_COUNT >> 8) & 0xFF), PIT_CH2_DATA);

    /* Enable gate to start countdown */
    spkr_outb(spkr | 0x01);

    tsc_start = rdtsc_ordered();

    /* Poll until PIT output goes high (bit 5 of 0x61) */
    timeout = 100000000U;
    while (!(spkr_inb() & 0x20)) {
        if (--timeout == 0) {
            return 0;
        }
    }

    tsc_end = rdtsc_ordered();

    /* Disable gate */
    spkr_outb(spkr);

    return tsc_end - tsc_start;
}

static uint64_t pit_calibrate_khz(void) {
    uint64_t samples[PIT_SAMPLES];
    int valid = 0;

    for (int i = 0; i < PIT_SAMPLES; i++) {
        uint64_t ticks = pit_measure_once();
        if (ticks == 0) continue;

        uint64_t khz = (ticks * 1000ULL) / GATE_MS;
        uint32_t mhz = (uint32_t)(khz / 1000);

        if (mhz >= FREQ_MIN_MHZ && mhz <= FREQ_MAX_MHZ) {
            samples[valid++] = khz;
        }
    }

    if (valid == 0) return 0;

    /* Simple sort + median */
    for (int i = 0; i < valid - 1; i++) {
        for (int j = i + 1; j < valid; j++) {
            if (samples[j] < samples[i]) {
                uint64_t tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }
    return samples[valid / 2];
}

/* Method 2: Xen PV clock CPUID */

static uint64_t xen_pv_calibrate_khz(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(0x40000000, &eax, &ebx, &ecx, &edx);

    /* "XenV" = 0x566e6558 */
    if (ebx != 0x566e6558) return 0;

    if (eax < 0x40000003) return 0;

    cpuid(0x40000003, &eax, &ebx, &ecx, &edx);

    if (ebx > 0) {
        uint64_t khz = (uint64_t)ebx;
        uint32_t mhz = (uint32_t)(khz / 1000);
        if (mhz >= FREQ_MIN_MHZ && mhz <= FREQ_MAX_MHZ) {
            return khz;
        }
    }

    if (eax > 0) {
        uint64_t khz = (uint64_t)eax;
        uint32_t mhz = (uint32_t)(khz / 1000);
        if (mhz >= FREQ_MIN_MHZ && mhz <= FREQ_MAX_MHZ) {
            return khz;
        }
    }

    return 0;
}

/* Main entry point */

int tsc_calibrate(void) {
    uint64_t khz;

    /* Method 1: PIT speaker gate */
    khz = pit_calibrate_khz();
    if (khz > 0) {
        g_tsc_cal.freq_khz = khz;
        g_tsc_cal.freq_mhz = (uint32_t)(khz / 1000);
        g_tsc_cal.method   = "pit-speaker-gate";
        return 0;
    }

    /* Method 2: Xen PV clock CPUID */
    khz = xen_pv_calibrate_khz();
    if (khz > 0) {
        g_tsc_cal.freq_khz = khz;
        g_tsc_cal.freq_mhz = (uint32_t)(khz / 1000);
        g_tsc_cal.method   = "xen-pv-cpuid";
        return 0;
    }

    /* Method 3: Fallback */
    g_tsc_cal.freq_khz = 2000000;
    g_tsc_cal.freq_mhz = 2000;
    g_tsc_cal.method   = "fallback-2ghz";
    return 0;
}
