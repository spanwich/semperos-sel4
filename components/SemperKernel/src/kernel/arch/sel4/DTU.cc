/*
 * arch/sel4/DTU.cc -- DTU operations via vDTU shared memory
 *
 * Replaces arch/gem5/DTU.cc. All DTU hardware register accesses are replaced
 * with vDTU RPC calls (for configuration) and shared memory ring buffer
 * operations (for data path).
 *
 * STUB: minimal implementations to get kernel compiling.
 * Real implementations in sub-task 04d.
 */

#include <base/log/Kernel.h>
#include <base/Panic.h>

#include "DTU.h"
#include "pes/VPE.h"
#include "mem/MainMemory.h"

namespace kernel {

/* Note: DTU::_inst is defined in top-level DTU.cc with INIT_PRIO_USER(1) */

void DTU::init() {
    /* Note: don't use KLOG here — called during static init before ThreadManager */
}

int DTU::log_to_phys(int pe) {
    return pe; /* identity mapping on sel4 */
}

void DTU::deprivilege(int) {
    /* no-op on sel4 */
}

void DTU::privilege(int) {
    /* no-op on sel4 */
}

void DTU::set_vpeid(const VPEDesc &) {
    /* stub */
}

void DTU::unset_vpeid(const VPEDesc &) {
    /* stub */
}

void DTU::wakeup(const VPEDesc &) {
    /* stub - on sel4 VPEs are always running */
}

void DTU::suspend(const VPEDesc &) {
    /* stub */
}

void DTU::injectIRQ(const VPEDesc &) {
    /* stub */
}

void DTU::set_rw_barrier(const VPEDesc &, uintptr_t) {
    /* stub */
}

void DTU::config_pf_remote(const VPEDesc &, uint64_t, int) {
    /* stub - no page faults in sel4 prototype */
}

void DTU::config_pt_remote(const VPEDesc &, uint64_t) {
    /* stub */
}

void DTU::map_page(const VPEDesc &, uintptr_t, uintptr_t, int) {
    /* stub */
}

void DTU::map_kernel_page(const VPEDesc &, uintptr_t, uintptr_t, int, KernelAllocation &) {
    /* stub */
}

void DTU::unmap_page(const VPEDesc &, uintptr_t) {
    /* stub */
}

void DTU::invalidate_ep(const VPEDesc &, int) {
    /* stub - TODO: RPC to vDTU invalidate_ep */
}

void DTU::invalidate_eps(const VPEDesc &, int) {
    /* stub - TODO: RPC to vDTU invalidate_eps */
}

void DTU::config_recv_local(int ep, uintptr_t buf, uint order, uint msgorder, int flags) {
    KLOG(EPS, "config_recv_local(ep=" << ep << ") [sel4 stub]");
    /* stub - TODO: RPC to vDTU config_recv */
}

void DTU::config_recv_remote(const VPEDesc &, int ep, uintptr_t, uint, uint, int, bool) {
    KLOG(EPS, "config_recv_remote(ep=" << ep << ") [sel4 stub]");
    /* stub */
}

void DTU::config_send_local(int ep, label_t label, int dstcore, int dstvpe,
    int dstep, size_t msgsize, word_t credits) {
    KLOG(EPS, "config_send_local(ep=" << ep << " -> core=" << dstcore << " ep=" << dstep << ") [sel4 stub]");
    /* stub - TODO: RPC to vDTU config_send */
}

void DTU::config_send_remote(const VPEDesc &, int ep, label_t, int, int, int, size_t, word_t) {
    KLOG(EPS, "config_send_remote(ep=" << ep << ") [sel4 stub]");
    /* stub */
}

void DTU::config_mem_local(int ep, int dstcore, int dstvpe, uintptr_t addr, size_t size) {
    KLOG(EPS, "config_mem_local(ep=" << ep << ") [sel4 stub]");
    /* stub */
}

void DTU::config_mem_remote(const VPEDesc &, int, int, int, uintptr_t, size_t, int) {
    /* stub */
}

void DTU::send_to(const VPEDesc &, int, label_t, const void *, size_t, label_t, int) {
    /* stub */
}

void DTU::reply_to(const VPEDesc &, int, int, word_t, label_t, const void *, size_t) {
    /* stub */
}

void DTU::write_mem(const VPEDesc &, uintptr_t, const void *, size_t) {
    /* stub */
}

void DTU::read_mem(const VPEDesc &, uintptr_t, void *, size_t) {
    /* stub */
}

void DTU::cmpxchg_mem(const VPEDesc &, uintptr_t, const void *, size_t, size_t, size_t) {
    /* stub */
}

/* Private helpers that gem5 uses for register manipulation */
void DTU::config_recv(void *, uintptr_t, uint, uint, int) {
    /* stub */
}

void DTU::config_send(void *, label_t, int, int, int, size_t, word_t) {
    /* stub */
}

void DTU::config_mem(void *, int, int, uintptr_t, size_t, int) {
    /* stub */
}

} // namespace kernel

/* m3::DTU instance and methods for sel4 */
namespace m3 {

DTU DTU::inst;

Errors::Code DTU::send(int, const void *, size_t, label_t, int) {
    return Errors::NO_ERROR;
}

Errors::Code DTU::reply(int, const void *, size_t, size_t) {
    return Errors::NO_ERROR;
}

Errors::Code DTU::read(int, void *, size_t, size_t) {
    return Errors::NO_ERROR;
}

Errors::Code DTU::write(int, const void *, size_t, size_t) {
    return Errors::NO_ERROR;
}

bool DTU::is_valid(int) const {
    return false;
}

DTU::Message *DTU::fetch_msg(int) const {
    return nullptr;
}

void DTU::mark_read(int, size_t) {
    /* stub */
}

} // namespace m3
