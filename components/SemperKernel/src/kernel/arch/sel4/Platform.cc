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
     *
     * In multi-node mode, PE IDs are globally unique:
     *   Node 0: PEs 0-3, Node 1: PEs 4-7, Node 2: PEs 8-11
     * The MHT uses these global IDs for capability routing.
     */
#ifndef SEMPER_KERNEL_ID
#define SEMPER_KERNEL_ID 0
#endif

#ifdef SEMPER_MULTI_NODE
    size_t base = SEMPER_KERNEL_ID * NUM_LOCAL_PES;
#else
    size_t base = 0;
#endif
    pe_count = NUM_LOCAL_PES;

    /* Build PEDesc values: core_id in top 10 bits (bits 63:54), type in low 3 bits */
    for (size_t i = 0; i < NUM_LOCAL_PES; i++) {
        pes[i] = m3::PEDesc((static_cast<m3::PEDesc::value_t>(base + i) << 54) |
                 static_cast<m3::PEDesc::value_t>(m3::PEType::COMP_IMEM));
    }

    kernelId = SEMPER_KERNEL_ID;
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
    return pe_base(); /* kernel is always local PE 0, global = pe_base */
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
