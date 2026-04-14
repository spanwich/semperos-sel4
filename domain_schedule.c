/*
 * domain_schedule.c — seL4 kernel domain schedule for SemperOS-seL4
 *
 * Model A: DTUBridge isolated (FPT-177)
 *   Domain 0: SemperKernel, VDTUService, VPE0, VPE1
 *   Domain 1: DTUBridge (E1000 + lwIP network I/O)
 *
 * Ratio: 1:1 (D0 gets 50%, D1 gets 50%)
 * Each slot = 1 tick = KernelTimerTickMS (2ms). Cycle = 4ms.
 * Max freeze per domain = 2ms. Faster interleaving reduces E1000
 * packet loss from domain-0 idle spinning while kernel threads
 * are blocked in wait_for.
 */

#include <config.h>
#include <object/structures.h>
#include <model/statedata.h>

const dschedule_t ksDomSchedule[] = {
    { .domain = 0, .length = 1 },
    { .domain = 1, .length = 1 },
};

const word_t ksDomScheduleLength = sizeof(ksDomSchedule) / sizeof(dschedule_t);
