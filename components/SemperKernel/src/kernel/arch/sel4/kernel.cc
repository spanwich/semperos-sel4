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
#include "vdtu_ring.h"
#include "vdtu_channels.h"
/* VDTUService RPC stubs */
int vdtu_config_recv(int target_pe, int ep_id, int buf_order, int msg_order, int flags);
int vdtu_config_recv_at(int target_pe, int ep_id, int channel, int buf_order, int msg_order, int flags);
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
#include "ddl/MHTInstance.h"
#include "cap/Capability.h"
#include <thread/ThreadManager.h>

using namespace kernel;

/*
 * Configure syscall and kernelcall receive endpoints.
 * Deferred from INIT_PRIO constructors — CAmkES RPC not available
 * during static initialization.
 */
static void configure_recv_endpoints(void)
{
    SyscallHandler &sysch = SyscallHandler::get();

    /* Configure per-VPE SYSC recv endpoints (gem5: one recv EP per sending VPE).
     * EP 0: reads from VPE0's channel pool (channels 0-15)
     * EP 1: reads from VPE1's channel pool (channels 16-31)
     * The WorkLoop polls all SYSC_GATES, so both are checked each iteration. */
    int buford = m3::getnextlog2(m3::DTU::MAX_MSG_SLOTS) + VPE::SYSC_CREDIT_ORD;

    /* SYSC EP 0: for VPE0 syscalls (channel from PE 0/2 pool) */
    DTU::get().config_recv_local(sysch.epid(0), 0, buford, VPE::SYSC_CREDIT_ORD, 0);
    printf("[SemperKernel] Configured SYSC_GATE[0] for VPE0 (ep %zu)\n", sysch.epid(0));

    /* SYSC EP 1: for VPE1 syscalls.
     * Register recv EP at kernel PE 0 EP 1 on VPE1's channel 16 via
     * config_recv_at. VDTUService knows the exact channel, so when VPE1
     * calls config_send targeting PE 0 EP 1, it gets channel 16 — the
     * correct shared page between kernel and VPE1. */
    {
        int sysc_ep1 = (int)sysch.epid(1);
        int vpe1_ch = VDTU_CHANNELS_PER_PE;  /* channel 16 */
        vdtu_config_recv_at(0, sysc_ep1, vpe1_ch, buford, VPE::SYSC_CREDIT_ORD, 0);
        uint32_t sc = 1u << (buford - VPE::SYSC_CREDIT_ORD);
        uint32_t ss = 1u << VPE::SYSC_CREDIT_ORD;
        size_t avail = 4096 - VDTU_RING_CTRL_SIZE;
        while (sc * ss > avail && sc > 2) sc >>= 1;
        DTU::get().init_recv_channel(sysc_ep1, vpe1_ch, sc, ss);
        printf("[SemperKernel] Configured SYSC_GATE[1] for VPE1 (ep %d, ch=%d)\n",
               sysc_ep1, vpe1_ch);
    }

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
    /* Force VPE1 to use SYSC EP 1 (kernel's per-VPE1 recv EP on channel 16).
     * PEManager::create uses reserve_ep which would assign EP 0 (same as VPE0).
     * We create VPE1 directly with syscEP=1 to match SYSC_GATE[1]. */
    size_t core_id = PEManager::get().free_core(pe);
    if (core_id == Platform::MAX_PES) return nullptr;
    VPE *vpe1 = new VPE(m3::String("VPE1"), core_id, core_id, false,
                         1 /* syscEP = SYSC_GATE[1] */, -1, m3::KIF::INV_SEL);
    PEManager::get().set_vpe(core_id, vpe1);
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

/* Network ring buffer init (07e) — defined in camkes_entry.c */
extern "C" void net_init_rings(void);

/*
 * Worker thread for cooperative multithreading (Task 09).
 *
 * When the main kernel thread blocks in ThreadManager::wait_for()
 * (e.g., waiting for remote revocation responses), control transfers
 * to this worker thread. The worker runs the WorkLoop so incoming
 * messages continue to be processed. When the blocking thread is
 * notified (via ThreadManager::notify), the next yield() switches
 * back to it.
 *
 * We create NUM_WORKER_THREADS workers at startup so that nested
 * blocking (e.g., main blocks, worker handles a message that also
 * blocks) has a sleeping thread to switch to. Two workers is
 * Workers for multi-node: cross-kernel operations block threads for
 * the full KRNLC round-trip. Each pending createsess, connect, or
 * revoke consumes a thread. Need enough idle workers to keep the
 * WorkLoop + net_poll running under concurrent cross-node load.
 */
#define NUM_WORKER_THREADS 10

static kernel::WorkLoop *g_kworkloop = nullptr;

static void worker_thread_func(void *) {
    /* Run the WorkLoop. Each iteration polls SYSC + KRNLC endpoints,
     * processes one message, runs net_poll, then yields. When the
     * main thread is woken, yield() in the WorkLoop switches to it. */
    if (g_kworkloop)
        g_kworkloop->run();
}

/* Layer 2: kernel-to-kernel ping. Called from net_poll() in camkes_entry.c. */
extern "C" int krnlc_ping_peer(void) {
#ifdef SEMPER_MULTI_NODE
    size_t peer_kid = (Platform::kernelId() == 0) ? 1 : 0;
    KPE *peer = Coordinator::get().tryGetKPE(peer_kid);
    if (!peer) return -1;
    Kernelcalls::get().ping(peer);  /* blocks in wait_for() until reply */
    return 0;
#else
    return -1;
#endif
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

#ifdef SEMPER_MULTI_NODE
    /* FPT-175 / FPT-179 Stage 4: Register ALL peer kernels for spanning
     * protocol (N-node, not just 2). Each kernel statically knows every
     * other kernel's PE range and registers MHT + KPE entries so that
     * Kernelcalls can route messages to any of them. NUM_KERNELS is
     * NUM_PEERS + 1 (kernels are peers of each other plus self). */
    {
        size_t my_kid = Platform::kernelId();
        (void)Platform::pe_base();  /* referenced by macros below */

        /* Cluster size from cmake (-DSEMPER_NUM_NODES). Default 3. */
#ifndef SEMPER_NUM_NODES
#define SEMPER_NUM_NODES 3
#endif
        const size_t NUM_KERNELS = SEMPER_NUM_NODES;

        for (size_t peer_kid = 0; peer_kid < NUM_KERNELS; peer_kid++) {
            if (peer_kid == my_kid) continue;
            size_t peer_base = peer_kid * NUM_LOCAL_PES;

            /* Update MHT: mark peer's PE range as belonging to peer kernel.
             * Cannot use updateMembership(start, krnl, ...) because it calls
             * Platform::pe_by_core() which only knows local PEs. Instead,
             * construct PEDesc array with peer's global PE IDs. */
            m3::PEDesc peer_pes[NUM_LOCAL_PES];
            for (int i = 0; i < NUM_LOCAL_PES; i++) {
                peer_pes[i] = m3::PEDesc(
                    (static_cast<m3::PEDesc::value_t>(peer_base + i) << 54) |
                    static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));
            }
            MHTInstance::getInstance().updateMembership(
                peer_pes, NUM_LOCAL_PES,
                static_cast<membership_entry::krnl_id_t>(peer_kid),
                static_cast<membership_entry::pe_id_t>(peer_base),
                MembershipFlags::NONE,
                false /* don't broadcast — static config */);
            printf("[SemperKernel] MHT: registered peer kernel %zu (PEs %zu-%zu)\n",
                   peer_kid, peer_base, peer_base + NUM_LOCAL_PES - 1);

            /* Register KPE for peer kernel so Coordinator::getKPE() succeeds */
            Coordinator::get().addKPE(
                m3::String("kernel"),
                peer_kid,
                peer_base, /* peer's kernel core (global PE) */
                DTU::KRNLC_EP,
                DTU::KRNLC_EP);
            printf("[SemperKernel] Registered KPE for peer kernel %zu (core %zu)\n",
                   peer_kid, peer_base);
        }
        MHTInstance::getInstance().printMembership();
    }
#endif

    /* Create worker threads for cooperative blocking (Task 09).
     * Must be created BEFORE the WorkLoop starts so wait_for() has
     * sleeping threads to switch to. */
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        m3::Thread *t = new m3::Thread(worker_thread_func, nullptr);
        m3::ThreadManager::get().add_sleeping(t);
    }
    printf("[SemperKernel] Created %d worker threads (sleep count: %zu)\n",
           NUM_WORKER_THREADS, m3::ThreadManager::get().sleeping_count());

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

    /* Service registration removed — VPE1 calls createsrv via syscall (Layer 3).
     * See Confluence: "SemperOS-seL4 — Multi-Node Communication Test Suite Design" */

    /* Attach to network ring buffers (DTUBridge initialized them in post_init) */
    net_init_rings();

    printf("[SemperKernel] Entering WorkLoop (polling %d SYSC + %d KRNLC gates)\n",
           DTU::SYSC_GATES, DTU::KRNLC_GATES);

    /* Enter the real kernel WorkLoop */
    static kernel::WorkLoop kworkloop;
    g_kworkloop = &kworkloop;
    kworkloop.add(nullptr, false);
    kworkloop.run();

    printf("[SemperKernel] WorkLoop exited\n");
}
