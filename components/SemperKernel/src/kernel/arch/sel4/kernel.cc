/*
 * arch/sel4/kernel.cc -- Entry point for SemperOS kernel on seL4/CAmkES
 *
 * Replaces arch/baremetal/kernel.cc. Bridge from CAmkES run() entry
 * to SemperOS kernel initialization and WorkLoop.
 *
 * On sel4, endpoint configuration (config_recv_local) is deferred to
 * kernel_start() because CAmkES RPC calls require run() context —
 * they cannot be made during INIT_PRIO static initialization.
 */

extern "C" {
#include <stdio.h>
}

#include <base/log/Kernel.h>
#include <base/util/Math.h>

#include "Platform.h"
#include "DTU.h"
#include "pes/PEManager.h"
#include "pes/VPE.h"
#include "SyscallHandler.h"
#include "KernelcallHandler.h"
#include "Kernelcalls.h"
#include "com/RecvBufs.h"
#include "com/Services.h"
#include "Coordinator.h"
#include "WorkLoop.h"
#include "mem/MainMemory.h"

using namespace kernel;

/*
 * Configure syscall and kernelcall receive endpoints.
 * This is deferred from INIT_PRIO constructors because CAmkES RPC
 * (vdtu_config_recv) is not available during static initialization.
 */
static void configure_recv_endpoints(void)
{
    SyscallHandler &sysch = SyscallHandler::get();

    /* Configure SYSC_GATE recv endpoints (6 gates for syscalls from VPEs) */
    int buford = m3::getnextlog2(m3::DTU::MAX_MSG_SLOTS) + VPE::SYSC_CREDIT_ORD;
    for (uint i = 0; i < DTU::SYSC_GATES; i++) {
        DTU::get().config_recv_local(sysch.epid(i), 0, buford, VPE::SYSC_CREDIT_ORD, 0);
    }
    printf("[SemperKernel] Configured %d SYSC_GATE recv endpoints\n", DTU::SYSC_GATES);

    /* Configure service recv endpoint */
    int srv_buford = m3::nextlog2<Service::SRV_MSG_SIZE * m3::DTU::MAX_MSG_SLOTS>::val;
    DTU::get().config_recv_local(sysch.srvepid(), 0, srv_buford,
                                 m3::nextlog2<Service::SRV_MSG_SIZE>::val, 0);
    printf("[SemperKernel] Configured service recv endpoint (ep %zu)\n", sysch.srvepid());

    /* Configure KRNLC_GATE recv endpoints (for inter-kernel calls).
     * Only configure the first gate for Task 04 (single-kernel). */
    KernelcallHandler &krnlch = KernelcallHandler::get();
    int kbuford = m3::getnextlog2(m3::DTU::MAX_MSG_SLOTS) + Kernelcalls::MSG_ORD;
    /* Only configure 1 KRNLC gate to save channels (we have 8 channels total) */
    DTU::get().config_recv_local(krnlch.epid(0), 0, kbuford, Kernelcalls::MSG_ORD, 0);
    printf("[SemperKernel] Configured 1 KRNLC_GATE recv endpoint\n");
}

extern "C" void kernel_start(void) {
    printf("[SemperKernel] Starting SemperOS kernel on seL4/CAmkES\n");
    printf("[SemperKernel] Platform: %zu PEs, kernel PE=%zu, kernel ID=%u\n",
           Platform::pe_count(), Platform::kernel_pe(), Platform::kernelId());

    /* Configure recv endpoints (deferred from INIT_PRIO) */
    configure_recv_endpoints();

    /* Initialize subsystems */
    RecvBufs::init();
    printf("[SemperKernel] RecvBufs initialized\n");

    PEManager::create();
    printf("[SemperKernel] PEManager created\n");

    printf("[SemperKernel] Entering WorkLoop (polling %d SYSC + %d KRNLC gates)\n",
           DTU::SYSC_GATES, DTU::KRNLC_GATES);

    /* Enter the real kernel WorkLoop — polls all recv endpoints.
     * Must use kernel::WorkLoop (overrides m3::WorkLoop::run()),
     * NOT m3::env()->workloop() which is a plain m3::WorkLoop. */
    static kernel::WorkLoop kworkloop;
    /* Transfer the permanent work items so has_items() returns true */
    kworkloop.add(nullptr, false);
    kworkloop.run();

    printf("[SemperKernel] WorkLoop exited\n");
}
