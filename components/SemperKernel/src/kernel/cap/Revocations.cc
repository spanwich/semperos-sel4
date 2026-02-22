/*
 * Copyright (C) 2019, Matthias Hille <matthias.hille@tu-dresden.de>,
 * Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of SemperOS.
 *
 * SemperOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * SemperOS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#include "cap/Revocations.h"
#include "ddl/MHTInstance.h"

namespace kernel {

RevocationList RevocationList::_inst;

void Revocation::notifySubscribers() {
    for (auto sub = subscribers.begin(); sub != subscribers.end();) {
        auto curSub = sub++;
        Revocation *entry = RevocationList::get().find(curSub->rev->capID);
        if(entry) {
            entry->awaitedResp--;

            if (entry->awaitedResp == 0) {
                // notify our subscribers too
                // Note: this will inform local parents
                entry->notifySubscribers();

                // If the parent is local
                membership_entry::krnl_id_t parentAuthority =
                    MHTInstance::getInstance().responsibleKrnl(HashUtil::hashToPeId(entry->parent));

                // if there is a thread ID this is the entry of a revocation root
                if(entry->tid != -1) {
                    assert(entry->capID == entry->origin);
                    m3::ThreadManager::get().notify(reinterpret_cast<void*> (entry->tid));
                }
                else if(parentAuthority != Coordinator::get().kid()) {
                    // we only need to inform remote parents here
                    // if we have a local parent it subscribed to this
                    // revocation before and is informed by notifySubscribers()
                    assert(entry->parent != 0);
                    Kernelcalls::get().revokeFinish(Coordinator::get().getKPE(parentAuthority),
                        entry->parent, -1, false);
                }
            }
        }

        subscribers.remove(&*curSub);
        delete &*curSub;
    }
}


}
