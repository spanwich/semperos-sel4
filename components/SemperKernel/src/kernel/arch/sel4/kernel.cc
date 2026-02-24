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
    /* Create VPE0 via PEManager::create() so it's stored in _vpes[core].
     * This is needed so the WorkLoop can look up the VPE from the
     * sender's core ID to find the correct RecvGate session. */
    m3::PEDesc pe(m3::PEType::COMP_IMEM);
    VPE *vpe0 = PEManager::get().create(m3::String("VPE0"), pe, -1, m3::KIF::INV_SEL);
    if (!vpe0) {
        printf("[SemperKernel] ERROR: PEManager::create() failed for VPE0\n");
        return nullptr;
    }

    printf("[SemperKernel] Created VPE0 on PE %zu (id=%zu)\n", vpe0->core(), vpe0->id());

    /* Start VPE0 */
    vpe0->start(0, nullptr, 0);
    printf("[SemperKernel] VPE0 started\n");

    return vpe0;
}

/*
 * Create VPE1 (the second user VPE) and install cross-VPE capabilities.
 *
 * VPE1 is PE 3 in our platform config. It is a passive VPE whose CapTable
 * is manipulated by the kernel during EXCHANGE syscalls from VPE0.
 *
 * After creating VPE1, we install a VPECapability at selector 2 in VPE0's
 * CapTable. This allows VPE0 to reference VPE1 as the target of EXCHANGE
 * syscalls (the tcap parameter).
 *
 * Note: VPE::init() configures endpoints via vDTU RPC. Since VPE1 has no
 * CAmkES shared data channels, the endpoints are bookkeeping-only. VPE1
 * never sends or receives messages in the EXCHANGE path.
 */
static VPE *create_vpe1(VPE *vpe0)
{
    m3::PEDesc pe(m3::PEType::COMP_IMEM);
    VPE *vpe1 = PEManager::get().create(m3::String("VPE1"), pe, -1, m3::KIF::INV_SEL);
    if (!vpe1) {
        printf("[SemperKernel] ERROR: PEManager::create() failed for VPE1\n");
        return nullptr;
    }

    printf("[SemperKernel] Created VPE1 on PE %zu (id=%zu)\n", vpe1->core(), vpe1->id());

    /* Install VPECapability for VPE1 in VPE0's CapTable at selector 2.
     * VPE0 already has: sel 0 = self VPECap, sel 1 = MemCap.
     * This mimics what createvpe() does: obtain() clones the VPE1's self-cap
     * (at sel 0 in VPE1's table) into VPE0's table at the target selector.
     * This establishes a parent-child relationship for revocation. */
    vpe0->objcaps().obtain(2, vpe1->objcaps().get(0));
    printf("[SemperKernel] Installed VPE1 cap at VPE0 selector 2\n");

    /* Start VPE1 */
    vpe1->start(0, nullptr, 0);
    printf("[SemperKernel] VPE1 started (passive mode)\n");

    return vpe1;
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

    /* Create VPE1 and install cross-VPE capabilities (Task 06) */
    VPE *vpe1 = create_vpe1(vpe0);
    if (!vpe1) {
        printf("[SemperKernel] WARNING: Failed to create VPE1 (EXCHANGE tests unavailable)\n");
    }

    printf("[SemperKernel] Entering WorkLoop (polling %d SYSC + %d KRNLC gates)\n",
           DTU::SYSC_GATES, DTU::KRNLC_GATES);

    /* Enter the real kernel WorkLoop */
    static kernel::WorkLoop kworkloop;
    kworkloop.add(nullptr, false);
    kworkloop.run();

    printf("[SemperKernel] WorkLoop exited\n");
}
