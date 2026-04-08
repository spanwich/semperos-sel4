/*
 * domain_schedule.c — seL4 kernel domain schedule for SemperOS-seL4
 *
 * Model A: DTUBridge isolated (FPT-177)
 *   Domain 0: SemperKernel, VDTUService, VPE0, VPE1
 *   Domain 1: DTUBridge (E1000 + lwIP network I/O)
 *
 * Ratio: 3:1 (D0 gets 75%, D1 gets 25%)
 * Each slot = KernelTimerTickMS (default 2ms, so 6ms D0 + 2ms D1 = 8ms cycle)
 *
 * DTUBridge needs guaranteed CPU for:
 *   - E1000 interrupt handling
 *   - lwIP timer callbacks (ARP, UDP)
 *   - Hello exchange (peer discovery)
 *   - Outbound ring poll (kernel→network)
 *   - Inbound packet receive (network→kernel)
 */

#include <config.h>
#include <object/structures.h>
#include <model/statedata.h>

const dschedule_t ksDomSchedule[] = {
    { .domain = 0, .length = 3 },
    { .domain = 1, .length = 1 },
};

const word_t ksDomScheduleLength = sizeof(ksDomSchedule) / sizeof(dschedule_t);
