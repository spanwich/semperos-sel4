/*
 * arch/sel4/kernel.cc -- Entry point for SemperOS kernel on seL4/CAmkES
 *
 * Replaces arch/baremetal/kernel.cc. Bridge from CAmkES run() entry
 * to SemperOS kernel initialization and WorkLoop.
 *
 * On sel4, endpoint configuration is deferred to kernel_start() because
 * CAmkES RPC calls require run() context.
 */

extern "C" {
#include <stdio.h>
}

#include <base/log/Kernel.h>
#include <base/util/Math.h>
#include <base/util/String.h>

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
 * Deferred from INIT_PRIO constructors — CAmkES RPC not available
 * during static initialization.
 */
static void configure_recv_endpoints(void)
{
    SyscallHandler &sysch = SyscallHandler::get();

    /* Configure 1 SYSC_GATE recv endpoint (for syscalls from VPEs).
     * Prototype uses 1 gate (channel budget: 8 total).
     * Channel allocation: 1 SYSC + 1 SRV + 1 KRNLC + 1 VPE0_RECV + 1 VPE0_SEND = 5/8 */
    int buford = m3::getnextlog2(m3::DTU::MAX_MSG_SLOTS) + VPE::SYSC_CREDIT_ORD;
    DTU::get().config_recv_local(sysch.epid(0), 0, buford, VPE::SYSC_CREDIT_ORD, 0);
    printf("[SemperKernel] Configured SYSC_GATE recv endpoint (ep %zu)\n", sysch.epid(0));

    /* Configure service recv endpoint */
    int srv_buford = m3::nextlog2<Service::SRV_MSG_SIZE * m3::DTU::MAX_MSG_SLOTS>::val;
    DTU::get().config_recv_local(sysch.srvepid(), 0, srv_buford,
                                 m3::nextlog2<Service::SRV_MSG_SIZE>::val, 0);
    printf("[SemperKernel] Configured service recv endpoint (ep %zu)\n", sysch.srvepid());

    /* Configure 1 KRNLC_GATE (single-kernel) */
    KernelcallHandler &krnlch = KernelcallHandler::get();
    int kbuford = m3::getnextlog2(m3::DTU::MAX_MSG_SLOTS) + Kernelcalls::MSG_ORD;
    DTU::get().config_recv_local(krnlch.epid(0), 0, kbuford, Kernelcalls::MSG_ORD, 0);
    printf("[SemperKernel] Configured 1 KRNLC_GATE recv endpoint\n");
}

/*
 * Create and start VPE0 (the first user VPE).
 *
 * VPE0 is PE 2 in our platform config. Creating the VPE object
 * calls VPE::init() which:
 *   1. Attaches the default receive endpoint (DEF_RECVEP on VPE0)
 *   2. Configures the syscall send endpoint (SYSC_EP on VPE0)
 *
 * After init, VPE0's CAmkES component can send syscalls to the kernel.
 */
static VPE *create_vpe0(void)
{
    SyscallHandler &sysch = SyscallHandler::get();

    /* Reserve a syscall EP slot for VPE0 */
    int vpe_id = 0;
    int sysc_ep = sysch.reserve_ep(vpe_id);
    if (sysc_ep < 0) {
        printf("[SemperKernel] ERROR: no syscall EP available for VPE0\n");
        return nullptr;
    }

    /* VPE0 is on PE 2 (core 2) */
    size_t vpe0_core = 2;

    printf("[SemperKernel] Creating VPE0 on PE %zu (syscall EP %d)\n", vpe0_core, sysc_ep);

    /* Create the VPE object. The constructor calls init() which:
     * - RecvBufs::attach() for DEF_RECVEP
     * - config_send_remote() for SYSC_EP -> kernel's sysc_ep */
    VPE *vpe0 = new VPE(m3::String("VPE0"), vpe_id, vpe0_core, false, sysc_ep);

    /* Start VPE0 (calls DTU::wakeup which is a no-op on sel4,
     * since CAmkES components are always running) */
    vpe0->start(0, nullptr, 0);

    printf("[SemperKernel] VPE0 created and started\n");

    return vpe0;
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

    /* Create and start VPE0 */
    VPE *vpe0 = create_vpe0();
    if (!vpe0) {
        printf("[SemperKernel] FATAL: Failed to create VPE0\n");
        return;
    }

    printf("[SemperKernel] Entering WorkLoop (polling %d SYSC + %d KRNLC gates)\n",
           DTU::SYSC_GATES, DTU::KRNLC_GATES);

    /* Enter the real kernel WorkLoop */
    static kernel::WorkLoop kworkloop;
    kworkloop.add(nullptr, false);
    kworkloop.run();

    printf("[SemperKernel] WorkLoop exited\n");
}
