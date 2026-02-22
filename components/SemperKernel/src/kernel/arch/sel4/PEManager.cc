/*
 * arch/sel4/PEManager.cc -- PE pool management for SemperOS on seL4/CAmkES
 *
 * Replaces arch/baremetal/PEManager.cc.
 */

#include <base/util/Sync.h>

#include "pes/PEManager.h"
#include "Platform.h"

namespace kernel {

PEManager::~PEManager() {
    for(size_t i = 0; i < Platform::pe_count(); ++i) {
        if(_vpes[i])
            _vpes[i]->unref();
    }
}

} // namespace kernel
