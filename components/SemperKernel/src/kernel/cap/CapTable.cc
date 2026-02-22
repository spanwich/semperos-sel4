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

#include <base/util/Util.h>
#include <base/log/Kernel.h>
#include <base/benchmark/capbench.h>

#include "cap/CapTable.h"
#include "pes/PEManager.h"
#include "cap/Revocations.h"

namespace kernel {

void CapTable::revoke_all() {
    Capability *c;
    // TODO it might be better to do that in a different order, because it is more expensive to
    // remove a node that has two childs (it requires a rotate). Thus, it would be better to start
    // with leaf nodes.
    while((c = static_cast<Capability*>(_caps.remove_root())) != nullptr) {
        // Clean up any RevocationList entry for this cap that may be left over
        // from a previously blocked revocation root thread (e.g., if the VPE was
        // killed while waiting for remote revokeFinish responses).
        mht_key_t ocapKey = c->_id | TYPE_MASK_OCAP;
        mht_key_t mcapKey = c->_id | TYPE_MASK_MCAP;
        Revocation *staleOcap = RevocationList::get().find(ocapKey);
        if(staleOcap) {
            staleOcap->notifySubscribers();
            RevocationList::get().remove(ocapKey);
        }
        Revocation *staleMcap = RevocationList::get().find(mcapKey);
        if(staleMcap) {
            staleMcap->notifySubscribers();
            RevocationList::get().remove(mcapKey);
        }
        revoke(c, c->_id, c->_id);
        delete c;
    }
}

Capability *CapTable::obtain(capsel_t dst, Capability *c) {
    Capability *nc = c;
    if(c) {
        nc = c->clone(this, dst);
        if(nc)
            inherit_and_set(c, nc, dst);
    }
    return nc;
}

void CapTable::inherit_and_set(Capability *parent, Capability *child, capsel_t dst) {
    child->_parent = parent->_id;
    set(dst, child);
    if(MHTInstance::getInstance().keyLocality(parent->_id))
        parent->addChild(child->_id);
}

void CapTable::setparent_and_set(mht_key_t parent, Capability *child, capsel_t dst) {
    child->_parent = parent;
    set(dst, child);
}

/**
 * Recursive revocation algorithm (cf. Hille et al., USENIX ATC 2019, Section 4.3).
 *
 * Protocol state machine for a single capability revocation:
 *
 *   [START] --move children--> [REVOKE_SELF] --unset cap--> [PROCESS_CHILDREN]
 *       |                                                        |
 *       |   For each child:                                      |
 *       |     local child found  --> recurse revoke_rec()        |
 *       |     local child gone   --> subscribe to ongoing revoke |
 *       |     remote child       --> batch by kernel, send       |
 *       |                            revoke/revokeBatch msg      |
 *       v                                                        v
 *   [WAIT_REMOTE] <-- if root && remote revokes pending --------|
 *       |
 *       | (thread blocks; woken by revokeFinish messages)
 *       v
 *   [CLEANUP] --> notify subscribers --> remove from RevocationList
 *       |
 *       v
 *   [REMOVE_CHILD_PTR] --> tell parent to remove child pointer (local or remote)
 *
 * Concurrent revocations on overlapping subtrees:
 *   If a child is currently being revoked (found in RevocationList but not in MHT),
 *   the current revocation subscribes to the ongoing one. When the ongoing revocation
 *   completes, it decrements the subscriber's awaitedResp counter.
 *
 * Thread model:
 *   Only the revocation root blocks a thread (wait_for). Intermediate nodes in the
 *   revocation tree return their awaitedResp count to the caller, avoiding thread
 *   exhaustion during deep cascading revocations.
 *
 * @param c       The capability to revoke (must not be nullptr)
 * @param origin  The DDL key of the revocation root (for tracking)
 * @param type    OBJ or MAP capability type
 * @return        Number of outstanding remote revocations (0 if all completed locally)
 */
int CapTable::revoke_rec(Capability *c, mht_key_t origin, m3::CapRngDesc::Type type) {
    mht_key_t parent = (type == m3::CapRngDesc::Type::OBJ) ?
        (c->_parent | TYPE_MASK_OCAP) : (c->_parent | TYPE_MASK_MCAP);
    mht_key_t id = (type == m3::CapRngDesc::Type::OBJ) ?
        (c->_id | TYPE_MASK_OCAP) : (c->_id | TYPE_MASK_MCAP);
    Revocation *ongoing = nullptr;

    // reset the child-pointer since we're revoking all childs
    // note that we would need to do much more if delegatable capabilities could deny a revoke
    c->setRevoking(true);
    m3::SList<Capability::Child> children = m3::Util::move(c->_children);

    m3::Errors::Code res = c->revoke();
    // actually, this is a bit specific for service+session. although it failed to revoke the service
    // we want to revoke all childs, i.e. the sessions to remove them from the service.
    // TODO if there are other failable revokes, we need to reconsider that
    if(res == m3::Errors::NO_ERROR)
        c->table()->unset(c->sel());
    else {
        // Fail fast here to speed up automated benchmarks
        KLOG(ERR, "Error (" << res << ") during revocation of cap " << PRINT_HASH(id));
        m3::Machine::shutdown();
    }
    // TODO
    // change revocation of service capabilities so revoke is actually error free

    // Collect remote children grouped by destination kernel for batched sending.
    // Use a simple array since max kernels is bounded by KRNLC_SLOTS.
    static const uint MAX_BATCH_CAPIDS = 64;
    struct RemoteBatch {
        membership_entry::krnl_id_t krnlId;
        mht_key_t capIDs[MAX_BATCH_CAPIDS];
        uint count;
    };
    RemoteBatch batches[KernelcallHandler::KRNLC_SLOTS];
    uint numBatches = 0;

    // Helper lambda to flush a single batch (send its accumulated capIDs)
    auto flushBatch = [&ongoing, &id, &parent, &origin](RemoteBatch &batch) {
        if(!ongoing)
            ongoing = RevocationList::get().add(id, parent, origin);
        ongoing->awaitedResp += batch.count;
        if(batch.count == 1) {
            Kernelcalls::get().revoke(Coordinator::get().getKPE(batch.krnlId),
                batch.capIDs[0], id, origin);
        }
        else {
            Kernelcalls::get().revokeBatch(Coordinator::get().getKPE(batch.krnlId),
                id, origin, batch.capIDs, batch.count);
        }
        batch.count = 0;
    };

    for(auto it : children) {
        membership_entry::krnl_id_t authority = MHTInstance::getInstance().responsibleMember(it.id);
        if(authority == Coordinator::get().kid()) {
            const MHTItem &childIt = MHTInstance::getInstance().get(it.id);
            if(childIt.isEmpty()) {
                // check whether this child is part of an ongoing revocation
                Revocation *childRevoke = RevocationList::get().find(childIt.getKey());
                if(childRevoke) {
                    if(!ongoing)
                        ongoing = RevocationList::get().add(id, parent, origin);
                    childRevoke->subscribe(ongoing);
                    ongoing->awaitedResp++;
                }
            }
            else {
                int add = revoke_rec(childIt.getData<Capability>(), origin, type);
                // If the revocation of that child caused remote requests,
                // we have to subscribe to that revocation.
                if(add) {
                    if(!ongoing)
                        ongoing = RevocationList::get().add(id, parent, origin);
                    ongoing->awaitedResp += add;
                    RevocationList::get().find(it.id)->subscribe(ongoing);
                }
            }
        }
        else {
            // Find or create batch for this kernel
            uint batchIdx = numBatches;
            for(uint b = 0; b < numBatches; b++) {
                if(batches[b].krnlId == authority) {
                    batchIdx = b;
                    break;
                }
            }
            if(batchIdx == numBatches) {
                batches[numBatches].krnlId = authority;
                batches[numBatches].count = 0;
                numBatches++;
            }
            // Flush batch if full to prevent buffer overflow
            if(batches[batchIdx].count >= MAX_BATCH_CAPIDS)
                flushBatch(batches[batchIdx]);
            batches[batchIdx].capIDs[batches[batchIdx].count++] = it.id;
        }
    }

    // Send remaining batched revocation messages
    for(uint b = 0; b < numBatches; b++) {
        if(batches[b].count == 0)
            continue;
        if(!ongoing)
            ongoing = RevocationList::get().add(id, parent, origin);
        ongoing->awaitedResp += batches[b].count;
        if(batches[b].count == 1) {
            Kernelcalls::get().revoke(Coordinator::get().getKPE(batches[b].krnlId),
                batches[b].capIDs[0], id, origin);
        }
        else {
            Kernelcalls::get().revokeBatch(Coordinator::get().getKPE(batches[b].krnlId),
                id, origin, batches[b].capIDs, batches[b].count);
        }
    }

    // Once all directly reachable children are done, check if some of them were remote.
    // 1. If we are the revocation root this thread is going to wait for incoming responses.
    // 2. If we are not the revocation root: (TL;DR: do nothing)
    // 2.1. If the parent is local, the number of awaited responses will be returned by
    //      revoke_rec() and thus added to the parent's awaitedResp counter.
    // 2.2. If the parent is remote this thread is done.
    //      The incoming responses will be handled by the KernelcallHandler.



    if(id == origin) {
        if(ongoing) {
            if( ongoing->awaitedResp > 0) { // remote revokes appeared
                // wait for the outstanding revokes to finish
                int mytid = m3::ThreadManager::get().current()->id();
                m3::ThreadManager::get().wait_for(reinterpret_cast<void*>(mytid));
                CAP_BENCH_TRACE_X_F(KERNEL_REV_THRD_WAKEUP);
                // this thread will be notified once all responses arrived
                KLOG(KRNLC, "Continued revoke for cap " << PRINT_HASH(id) << ". Finishing revoke");
            }
        // remove ongoing entry
            ongoing->notifySubscribers();
            RevocationList::get().remove(id);
            ongoing = nullptr;
        }

        // revocation finished, tell parent to removes the child ptr to this cap
        if(parent & ~TYPE_MASK_CAP) {
            membership_entry::krnl_id_t parentAuthority =
                MHTInstance::getInstance().responsibleKrnl(HashUtil::hashToPeId(parent));
            if(parentAuthority == Coordinator::get().kid())
                MHTInstance::getInstance().get(parent).getData<Capability>()->removeChildAllTypes(id);
            else
                Kernelcalls::get().removeChildCapPtr(Coordinator::get().getKPE(parentAuthority),
                    DDLCapRngDesc(parent, 1), DDLCapRngDesc(id, 1));
        }
    }

    return ongoing ? ongoing->awaitedResp : 0;
}

int CapTable::revoke(Capability *c, mht_key_t capID, mht_key_t origin) {
    int res = 0;
    if(_type == m3::CapRngDesc::Type::OBJ)
        origin |= TYPE_MASK_OCAP;
    else
        origin |= TYPE_MASK_MCAP;

    if(c) {
        res = revoke_rec(c, origin, _type);
    }
    else {
        // check whether this cap is currently being revoked
        // if so, we want to subscribe to the revocation.
        // this happens when:
        //  1. we are the revocation root
        //  2. if we get a request from another kernel
        //     for a cap that is currently being revoked

        if(_type == m3::CapRngDesc::Type::OBJ)
            capID |= TYPE_MASK_OCAP;
        else
            capID |= TYPE_MASK_MCAP;
        Revocation *ongoingRevoke = RevocationList::get().find(capID);
        if(ongoingRevoke) {
            // Note: We only add this revocation as a subscriber to the existing
            //       Revocation entry (which is for the same capID)
            Revocation *subscriber = new Revocation(capID, ongoingRevoke->parent, origin, 1,
                (capID == origin) ? m3::ThreadManager::get().current()->id() : -1);
            ongoingRevoke->subscribe(subscriber);


            if(capID == origin) {
                // If this cap is the revocation root, this thread waits for it to finish.
                // Otherwise this revoke was initiated by a request from another kernel.
                // The parent of the capability will be informed by the kernelcall handler
                // once the revocation finished. This way we don't waste a thread.
                int mytid = m3::ThreadManager::get().current()->id();
                m3::ThreadManager::get().wait_for(reinterpret_cast<void*>(mytid));
                KLOG(KRNLC, "Continued revoke for cap " << PRINT_HASH(capID));
                // delete the finished revocation
                RevocationList::get().remove(capID);
            }
            else {
                res = 1;
            }
        }
    }
    return res;
}

m3::Errors::Code CapTable::revoke(const m3::CapRngDesc &crd, bool own) {
    for(capsel_t i = 0; i < crd.count(); ++i) {
        m3::Errors::Code res = m3::Errors::NO_ERROR;
        if(own) {
            Capability *cap = get(i + crd.start());
            // we don't necessarily have a cap ID here if the cap is either
            // non-existent or currently under revocation, but we can construct the ID:
            // PE ID | VPE ID | generic map/obj cap type | selector
            revoke(cap, cap ? cap->_id :
                    HashUtil::structured_hash(_id, _id,
                        (_type == m3::CapRngDesc::Type::MAP) ? ItemType::GENERICOCAP : ItemType::MAPCAP,
                        i + crd.start()),
                cap ? cap->_id :
                    HashUtil::structured_hash(_id, _id,
                        (_type == m3::CapRngDesc::Type::MAP) ? ItemType::GENERICOCAP : ItemType::MAPCAP,
                        i + crd.start()));
        }
        else {
            Capability *c = get(i + crd.start());
            if(c) {
                // Revoke only children, preserving the parent capability.
                // Move children list atomically to prevent new exchanges from referencing them.
                m3::SList<Capability::Child> children = m3::Util::move(c->_children);

                mht_key_t id = (_type == m3::CapRngDesc::Type::OBJ) ?
                    (c->_id | TYPE_MASK_OCAP) : (c->_id | TYPE_MASK_MCAP);
                mht_key_t origin = id; // this cap is the revocation origin
                Revocation *ongoing = nullptr;

                for(auto it : children) {
                    membership_entry::krnl_id_t authority = MHTInstance::getInstance().responsibleMember(it.id);
                    if(authority == Coordinator::get().kid()) {
                        const MHTItem &childIt = MHTInstance::getInstance().get(it.id);
                        if(childIt.isEmpty()) {
                            Revocation *childRevoke = RevocationList::get().find(childIt.getKey());
                            if(childRevoke) {
                                if(!ongoing)
                                    ongoing = RevocationList::get().add(id, id, origin);
                                childRevoke->subscribe(ongoing);
                                ongoing->awaitedResp++;
                            }
                        }
                        else {
                            int add = revoke_rec(childIt.getData<Capability>(), origin, _type);
                            if(add) {
                                if(!ongoing)
                                    ongoing = RevocationList::get().add(id, id, origin);
                                ongoing->awaitedResp += add;
                                RevocationList::get().find(it.id)->subscribe(ongoing);
                            }
                        }
                    }
                    else {
                        if(!ongoing)
                            ongoing = RevocationList::get().add(id, id, origin);
                        ongoing->awaitedResp++;
                        Kernelcalls::get().revoke(Coordinator::get().getKPE(authority), it.id, id, origin);
                    }
                }

                // Wait for remote revocations to complete (same as revoke_rec root behavior)
                if(ongoing) {
                    if(ongoing->awaitedResp > 0) {
                        int mytid = m3::ThreadManager::get().current()->id();
                        m3::ThreadManager::get().wait_for(reinterpret_cast<void*>(mytid));
                    }
                    ongoing->notifySubscribers();
                    RevocationList::get().remove(id);
                }
                res = m3::Errors::NO_ERROR;
            }
        }
        if(res != m3::Errors::NO_ERROR) {
            // TODO
            // there should never happen an error (except for the Service cap)
            PANIC("Error (" << res << ") while revoking capRng=" << crd << " at index " << i);
        }
    }
    return m3::Errors::NO_ERROR;
}

m3::OStream &operator<<(m3::OStream &os, const CapTable &ct) {
    os << "CapTable[" << ct.id() << "]:\n";
    ct._caps.print(os, false);
    return os;
}

}
