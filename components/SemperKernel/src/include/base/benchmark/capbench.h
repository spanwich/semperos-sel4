/*
 * Copyright (C) 2019, Matthias Hille <matthias.hille@tu-dresden.de>, 
 * Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of SemperOS.
 *
 * SemperOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * SemperOS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */
#pragma once

#include <base/util/Profile.h>

#define CAP_BENCH_TRACE_S(marker)     m3::Profile::start(marker)
#define CAP_BENCH_TRACE_F(marker)     m3::Profile::stop(marker)

#ifdef SEMPER_BENCH_MODE
    extern "C" uint64_t _cap_bench_t0;
    extern "C" uint64_t _cap_bench_cycles;

    #define CAP_BENCH_TRACE_X_S(tag)                                    \
        do {                                                             \
            uint32_t _lo, _hi;                                          \
            __asm__ volatile("lfence\n\trdtsc"                          \
                         : "=a"(_lo), "=d"(_hi) :: "memory");          \
            _cap_bench_t0 = ((uint64_t)_hi << 32) | _lo;               \
        } while(0)

    #define CAP_BENCH_TRACE_X_F(tag)                                    \
        do {                                                             \
            uint32_t _lo, _hi;                                          \
            __asm__ volatile("lfence\n\trdtsc"                          \
                         : "=a"(_lo), "=d"(_hi) :: "memory");          \
            _cap_bench_cycles =                                         \
                (((uint64_t)_hi << 32) | _lo) - _cap_bench_t0;        \
        } while(0)
#else
    #define CAP_BENCH_TRACE_X_S(tag) ((void)0)
    #define CAP_BENCH_TRACE_X_F(tag) ((void)0)
#endif

// trace markers for capbench
#define TRACE_START_ANALYSIS        10
#define TRACE_FINISH_ANALYSIS       11
// obtain
#define APP_OBT_START               12
#define KERNEL_OBT_SYSC_RCV         13
#define KERNEL_OBT_TO_SRV           14
#define KERNEL_OBT_TO_RKERNEL       15
#define RKERNEL_OBT_TO_SRV          16
#define RKERNEL_OBT_FROM_SRV        17
#define KERNEL_OBT_FROM_RKERNEL     18
#define KERNEL_OBT_THRD_WAKEUP      19
#define KERNEL_OBT_FROM_SRV         20
#define KERNEL_OBT_SYSC_RESP        21
#define APP_OBT_FINISH              22

// exchange (local)
#define KERNEL_EXC_SYSC_RCV         30
#define KERNEL_EXC_SYSC_RESP        31

// revoke
#define APP_REV_START               23
#define KERNEL_REV_SYSC_RCV         24
#define KERNEL_REV_TO_RKERNEL       25
#define KERNEL_REV_FROM_RKERNEL     26
#define KERNEL_REV_THRD_WAKEUP      27
#define KERNEL_REV_SYSC_RESP        28
#define APP_REV_FINISH              29