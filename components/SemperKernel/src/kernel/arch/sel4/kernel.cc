/*
 * arch/sel4/kernel.cc -- Entry point for SemperOS kernel on seL4/CAmkES
 *
 * Replaces arch/baremetal/kernel.cc. Bridge from CAmkES run() entry
 * to SemperOS kernel initialization and WorkLoop.
 */

extern "C" {
#include <stdio.h>
}

#include <base/log/Kernel.h>

#include "Platform.h"
#include "DTU.h"
#include "pes/PEManager.h"
#include "SyscallHandler.h"
#include "com/RecvBufs.h"
#include "Coordinator.h"
#include "WorkLoop.h"
#include "mem/MainMemory.h"

using namespace kernel;

extern "C" void kernel_start(void) {
    printf("[SemperKernel] Starting SemperOS kernel on seL4/CAmkES\n");

    printf("[SemperKernel] Platform: %zu PEs, kernel PE=%zu\n",
           Platform::pe_count(), Platform::kernel_pe());

    printf("[SemperKernel] Kernel ID: %u\n", Platform::kernelId());

    /* Initialize subsystems */
    RecvBufs::init();
    printf("[SemperKernel] RecvBufs initialized\n");

    PEManager::create();
    printf("[SemperKernel] PEManager created\n");

    printf("[SemperKernel] Entering WorkLoop\n");

    /* TODO: enter WorkLoop when DTU data path is implemented */
    /* For now, just print success and return */
    printf("[SemperKernel] SemperOS kernel init complete (stub)\n");
}
