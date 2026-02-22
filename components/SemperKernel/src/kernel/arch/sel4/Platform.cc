/*
 * arch/sel4/Platform.cc -- Platform init for SemperOS on seL4/CAmkES
 *
 * Replaces arch/gem5/Platform.cc. Hardcodes the test configuration
 * instead of reading from DTU hardware registers.
 */

#include <base/Init.h>
#include <string.h>

#include "mem/MainMemory.h"
#include "mem/MemoryModule.h"
#include "pes/VPE.h"
#include "DTU.h"
#include "Platform.h"

namespace kernel {

INIT_PRIO_USER(2) Platform::KEnv Platform::_kenv;

static size_t _first_pe_id;

/* Default constructor — sets up hardcoded test config */
Platform::KEnv::KEnv() {
    memset(this, 0, sizeof(*this));

    /* PE configuration for sel4 prototype:
     *   PE 0 = kernel (SemperKernel CAmkES component)
     *   PE 1 = vDTU service (not a user PE, but in the PE array)
     *   PE 2 = VPE0 (first user VPE)
     *   PE 3 = VPE1 (second user VPE, if configured)
     */
    pe_count = 4;

    /* Build PEDesc values: core_id in top 10 bits (bits 63:54), type in low 3 bits */
    pes[0] = m3::PEDesc((static_cast<m3::PEDesc::value_t>(0) << 54) |
             static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));
    pes[1] = m3::PEDesc((static_cast<m3::PEDesc::value_t>(1) << 54) |
             static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));
    pes[2] = m3::PEDesc((static_cast<m3::PEDesc::value_t>(2) << 54) |
             static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));
    pes[3] = m3::PEDesc((static_cast<m3::PEDesc::value_t>(3) << 54) |
             static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));

    kernelId = 0;
    creatorKernelId = 0;
    creatorCore = 0;
    creatorThread = -1;
    creatorEp = -1;

    /* No memory modules for now — we use CAmkES heap instead */
    /* No boot modules */

    _first_pe_id = 2; /* First user PE */
}

/* Multi-argument constructor (used when spawning child kernels) */
Platform::KEnv::KEnv(uintptr_t* _mods, size_t _pe_count, m3::PEDesc _pes[], uint32_t _kernelId,
    uint32_t _creatorKernelId, uint32_t _creatorCore, int32_t _creatorThread,
    int32_t _creatorEp, MemoryModule* _mem_mods[], size_t _memOffset)
{
    memset(mods, 0, sizeof(mods));
    for(uint i = 0; i < MAX_MODS && _mods[i]; i++)
        mods[i] = _mods[i];
    pe_count = _pe_count;
    kernelId = _kernelId;
    creatorKernelId = _creatorKernelId;
    creatorCore = _creatorCore;
    creatorThread = _creatorThread;
    creatorEp = _creatorEp;
    memset(pes, 0, sizeof(pes));
    for(uint i = 0; i < pe_count; i++)
        pes[i] = _pes[i];
    memset(mem_mods, 0, sizeof(mem_mods));
    for(uint i = 0; i < MAX_MEM_MODS; i++) {
        if(!_mem_mods[i])
            break;
        mem_mods[i] = {_mem_mods[i]->pe(), _mem_mods[i]->addr(), _mem_mods[i]->size()};
    }
    memOffset = _memOffset;
    memberTable = 0;
    ddlPartitions = 0;
}

size_t Platform::kernel_pe() {
    return 0; /* kernel is always PE 0 */
}

m3::PEDesc Platform::first_pe() {
    return _kenv.pes[_first_pe_id];
}

size_t Platform::first_pe_id() {
    return _first_pe_id;
}

size_t Platform::last_pe() {
    return _kenv.pe_count - 1;
}

uintptr_t Platform::def_recvbuf(size_t) {
    return RECVBUF_SPACE;
}

uintptr_t Platform::rw_barrier(size_t) {
    return RECVBUF_SPACE;
}

} // namespace kernel
