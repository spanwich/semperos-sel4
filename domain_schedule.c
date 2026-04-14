/*
 * domain_schedule.c — seL4 kernel domain schedule for SemperOS-seL4
 *
 * Model A: DTUBridge isolated (FPT-177)
 *   Domain 0: SemperKernel, VDTUService, VPE0, VPE1
 *   Domain 1: DTUBridge (E1000 + lwIP network I/O)
 *
 * Ratio: 2:2 (D0 gets 50%, D1 gets 50%)
 * Each slot = KernelTimerTickMS (default 2ms, so 4ms D0 + 4ms D1 = 8ms cycle)
 *
 * DTUBridge needs enough CPU for E1000 IRQ servicing, lwIP timers,
 * and HELLO exchange. With cooperative threading (setjmp), kernel
 * threads block properly, so D0 doesn't need extra slots.
 */

#include <config.h>
#include <object/structures.h>
#include <model/statedata.h>

const dschedule_t ksDomSchedule[] = {
    { .domain = 0, .length = 2 },
    { .domain = 1, .length = 2 },
};

const word_t ksDomScheduleLength = sizeof(ksDomSchedule) / sizeof(dschedule_t);
