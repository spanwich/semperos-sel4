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
 * sufficient for the current prototype (one active, one sleeping).
 */
#define NUM_WORKER_THREADS 2

static kernel::WorkLoop *g_kworkloop = nullptr;

static void worker_thread_func(void *) {
    /* Run the WorkLoop. Each iteration polls SYSC + KRNLC endpoints,
     * processes one message, runs net_poll, then yields. When the
     * main thread is woken, yield() in the WorkLoop switches to it. */
    if (g_kworkloop)
        g_kworkloop->run();
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
    /* FPT-175: Register peer kernel for 2-node spanning protocol.
     * Each kernel statically knows the other's PE range and registers
     * a KPE handle so Kernelcalls can route messages to it. */
    {
        size_t my_kid = Platform::kernelId();
        size_t my_base = Platform::pe_base();

        /* For a 2-node topology: node 0 peers with node 1, node 1 peers with node 0.
         * Generalize later for 3+ nodes. */
        size_t peer_kid = (my_kid == 0) ? 1 : 0;
        size_t peer_base = peer_kid * NUM_LOCAL_PES;

        /* Update MHT: mark peer's PE range as belonging to peer kernel.
         * Cannot use updateMembership(start, krnl, ...) because it calls
         * Platform::pe_by_core() which only knows local PEs. Instead,
         * construct PEDesc array with peer's global PE IDs and use the
         * array overload directly. */
        {
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
        }
        printf("[SemperKernel] MHT: registered peer kernel %zu (PEs %zu-%zu)\n",
               peer_kid, peer_base, peer_base + NUM_LOCAL_PES - 1);
        MHTInstance::getInstance().printMembership();

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
#endif

    /* Create worker threads for cooperative blocking (Task 09).
     * Must be created BEFORE the WorkLoop starts so wait_for() has
     * sleeping threads to switch to. */
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        new m3::Thread(worker_thread_func, nullptr);
    }
    printf("[SemperKernel] Created %d worker threads (thread count: %zu)\n",
           NUM_WORKER_THREADS, m3::ThreadManager::get().thread_count());

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

#ifdef SEMPER_MULTI_NODE
    /* FPT-176: Register VPE1 as a service provider ("testsrv").
     * The kernel creates the service directly rather than having VPE1
     * send a createsrv syscall, because VPE1's syscall channel setup
     * would require additional infrastructure. */
    if (vpe1) {
        /* Service recv EP on VPE1 — EP 2 (after DEF_RECVEP=1 and SYSC_EP=0) */
        const int SRV_EP = 2;
        int buf_order = 11;  /* 2048B buffer */
        int msg_order = 9;   /* 512B slots → 4 slots */
        DTU::get().config_recv_remote(
            vpe1->desc(), SRV_EP, 0, buf_order, msg_order, 0, true);

        /* Configure kernel send channel to VPE1's service recv EP.
         * SendGate::send() calls DTU::send_to() which needs
         * find_send_channel_for(local_pe, ep) to succeed.
         * Use SETUP_EP (13) which is reserved for child kernel spawning
         * (not used in this prototype). FIRST_FREE_EP (14) must stay
         * free for m3::DTU::reply() auto-configuration. */
        DTU::get().config_send_local(
            DTU::SETUP_EP,       /* kernel EP for this send channel */
            0x54455354,          /* label (must match Service SendGate) */
            vpe1->core(),        /* dest PE (global, translated in DTU.cc) */
            vpe1->id(),          /* dest VPE */
            SRV_EP,              /* dest EP (2) */
            Service::SRV_MSG_SIZE,
            1 << 9);             /* credits */

        /* Service capability selectors in VPE1's CapTable */
        const capsel_t GATE_SEL = 5;
        const capsel_t SRV_SEL = 6;

        /* Create MsgCapability pointing to VPE1's service recv EP */
        label_t srv_label = 0x54455354; /* "TEST" */
        mht_key_t gate_key = HashUtil::structured_hash(
            vpe1->core(), vpe1->id(), MSGOBJ, srv_label);
        vpe1->objcaps().set(GATE_SEL,
            new MsgCapability(&vpe1->objcaps(), GATE_SEL, srv_label,
                vpe1->core(), vpe1->id(), SRV_EP, 1 << 9, gate_key, 0));

        /* Register service in ServiceList */
        mht_key_t srv_id = HashUtil::structured_hash(
            vpe1->core(), vpe1->id(), SERVICE, SRV_SEL);
        m3::String srvname("testsrv");
        Service *srv = ServiceList::get().add(
            *vpe1, SRV_SEL, srvname,
            SRV_EP, srv_label, 1 /* capacity */, srv_id);

        /* Create ServiceCapability in VPE1's CapTable */
        mht_key_t srvcap_key = HashUtil::structured_hash(
            vpe1->core(), vpe1->id(), SRVCAP, SRV_SEL);
        vpe1->objcaps().set(SRV_SEL,
            new ServiceCapability(&vpe1->objcaps(), SRV_SEL, srv, srvcap_key));

        printf("[SemperKernel] Registered service 'testsrv' on VPE1 (EP %d, label=0x%x)\n",
               SRV_EP, (unsigned)srv_label);

        /* Note: service announcement to peer deferred to runtime.
         * broadcastAnnounceSrv at boot would block (net rings not attached,
         * no peer connected). The peer discovers "testsrv" via the
         * broadcastCreateSess fallback when VPE0 calls createsess. */
    }
#endif

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
