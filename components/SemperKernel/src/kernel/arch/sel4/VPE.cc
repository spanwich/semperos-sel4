/*
 * arch/sel4/VPE.cc -- VPE management for SemperOS on seL4/CAmkES
 *
 * Combines the arch-specific functions from baremetal/VPE.cc and
 * gem5/VPE.cc, adapted for the sel4 vDTU prototype.
 */

#include <base/util/Sync.h>
#include <base/log/Kernel.h>
#include <base/Panic.h>
#include <string.h>

#include "com/RecvBufs.h"
#include "pes/PEManager.h"
#include "pes/VPE.h"
#include "DTU.h"
#include "Platform.h"
#include "SyscallHandler.h"
#include "ddl/MHTInstance.h"
#include "mem/MainMemory.h"

namespace kernel {

/* Static members (gem5-only in VPE.h, enabled for sel4 too) */
size_t VPE::count = 0;
BootModule VPE::mods[Platform::MAX_MODS];
uint64_t VPE::loaded = 0;
BootModule *VPE::idles[Platform::MAX_PES];

BootModule *get_mod(const char *, bool *first) {
    /* No boot modules on sel4 — VPEs are CAmkES components */
    *first = true;
    return nullptr;
}

void read_from_mod(BootModule *, void *, size_t, size_t) {
    /* stub */
}

void copy_clear(const VPEDesc &, uintptr_t, uintptr_t, size_t, bool) {
    /* stub */
}

void map_idle(VPE &) {
    /* No idle thread on sel4 */
}

void map_idle(const VPEDesc &, KernelAllocation &, bool) {
    /* stub */
}

void VPE::init_memory(int, const char *) {
    if(_flags & MEMINIT)
        return;
    _flags |= MEMINIT;
    KLOG(KENV, "VPE::init_memory [sel4] - VPEs are CAmkES components");
}

/* --- Functions from baremetal/VPE.cc, adapted for sel4 --- */

void VPE::init() {
    /* Attach default receive endpoint.
     * Use buf_order=11 (2048B buffer), msg_order=9 (512B slots) → 4 slots.
     * DEF_RCVBUF_ORDER=8 is too small (256B = 1 slot = 0 usable capacity). */
    int buf_order = 11;  /* 2048 bytes total */
    int msg_order = VPE::SYSC_CREDIT_ORD;  /* 512 byte slots */
    UNUSED m3::Errors::Code res = RecvBufs::attach(
        *this, m3::DTU::DEF_RECVEP, Platform::def_recvbuf(core()),
        buf_order, msg_order, 0);
    /* Non-fatal if no channels available */
    if (res != m3::Errors::NO_ERROR)
        KLOG(ERR, "DEF_RECVEP attach failed for PE " << core());

    /* Configure syscall endpoint */
    DTU::get().config_send_remote(
        desc(), m3::DTU::SYSC_EP, reinterpret_cast<label_t>(&syscall_gate()),
        Platform::kernel_pe(), Platform::kernelId(),
        _syscEP, 1 << SYSC_CREDIT_ORD, 1 << SYSC_CREDIT_ORD);
}

void VPE::activate_sysc_ep() {
    /* no-op on sel4 */
}

void VPE::start(int argc, UNUSED char **argv, int) {
    ref();
    DTU::get().wakeup(desc());
    _state = RUNNING;
    KLOG(VPES, "Started VPE '" << _name << "' [id=" << id() << "]");
}

m3::Errors::Code VPE::xchg_ep(size_t epid, MsgCapability *, MsgCapability *n) {
    KLOG(EPS, "Setting ep " << epid << " of VPE " << id() << " to " << (n ? n->sel() : -1));

    if(n) {
        if(n->type() & Capability::MEM) {
            uintptr_t addr = n->obj->label & ~m3::KIF::Perm::RWX;
            int perm = n->obj->label & m3::KIF::Perm::RWX;
            DTU::get().config_mem_remote(desc(), epid,
                n->obj->core, n->obj->vpe, addr, n->obj->credits, perm);
        }
        else {
            DTU::get().config_send_remote(desc(), epid,
                n->obj->label, n->obj->core, n->obj->vpe, n->obj->epid,
                n->obj->credits, n->obj->credits);
        }
    }
    else
        DTU::get().invalidate_ep(desc(), epid);
    return m3::Errors::NO_ERROR;
}

VPE::~VPE() {
    KLOG(VPES, "Deleting VPE '" << _name << "' [id=" << id() << "]");
    DTU::get().invalidate_eps(desc());
    detach_rbufs();
    free_reqs();
    _objcaps.revoke_all();
    _mapcaps.revoke_all();
    if(_as) {
        DTU::get().suspend(desc());
        delete _as;
    }
}

} // namespace kernel
