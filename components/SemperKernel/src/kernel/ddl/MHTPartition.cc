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

#include <base/log/Kernel.h>
#include <base/col/SList.h>
#include <base/util/Util.h>
#include <base/Heap.h>
#include <base/Errors.h>
#include <thread/ThreadManager.h>

#include "ddl/MHTPartition.h"
#include "ddl/MHTInstance.h"

namespace kernel {

const MHTItem MHTPartition::emptyIndicator;

struct MHTItemStorable : m3::SListItem {
    MHTItemStorable(MHTItem &&dat) : data(m3::Util::move(dat)) {}
    MHTItem data;
};

MHTPartition::~MHTPartition() {
    for(size_t b = 0; b < NUM_BUCKETS; b++) {
        for(auto it = _buckets[b].begin(); it != _buckets[b].end(); it++) {
            if(it.operator ->() != nullptr)
                m3::Heap::free(it.operator ->());
        }
        _buckets[b].remove_all();
    }
}

m3::Errors::Code MHTPartition::put(MHTItem &&kv_pair, uint lockHandle) {
    size_t idx = bucket_index(kv_pair._mht_key);
    // check if this replaces another item
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == kv_pair._mht_key) {
            if(!it->data.islocked() || lockHandle == it->data.getLockHandle()) {
                if(lockHandle)
                    it->data.lockHandle = 0;
                if(it->data.data)
                    m3::Heap::free(it->data.data);
                it->data.transferData(kv_pair);
                return m3::Errors::NO_ERROR;
            } else {
                if(lockHandle == it->data.getLockHandle())
                KLOG(MHT, "Inserting MHTItem failed! Item is locked. mhtKey: " <<
                    PRINT_HASH(kv_pair._mht_key) << " lockHandle: " << kv_pair.getLockHandle() <<
                    " (" << lockHandle << ")");
                return m3::Errors::NO_PERM;
            }
        }
    }
    // otherwise create a new item for the kv_pair
    MHTItemStorable *item = new MHTItemStorable(m3::Util::move(kv_pair));
    if(!item) {
        KLOG(ERR, "Could not get storage for item in MHT!");
        return m3::Errors::OUT_OF_MEM;
    }
    _buckets[idx].append(item);
    _count++;
    return m3::Errors::NO_ERROR;
}

const MHTItem &MHTPartition::get(mht_key_t mht_key, bool locking) {
    size_t idx = bucket_index(mht_key);
    // Note: we enforce the locking policy here
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key) {
            if(it->data.islocked()) {
                // let this thread wait until the item is unlocked
                it->data.enqueueTicket();
            }
            if(locking)
                it->data.lock();

            return it->data;
        }
    }
    // not found - return an empty item
    return emptyIndicator;
}

bool MHTPartition::remove(mht_key_t mht_key) {
    size_t idx = bucket_index(mht_key);
    // TODO
    // if there are waiting requests, delete them and answer them as failed
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key) {
            MHTItemStorable *item = it.operator ->();
            if(item != nullptr) {
                // Remove from list first, then free (avoids use-after-free)
                _buckets[idx].remove(item);
                _count--;
                delete item;
                return true;
            }
            return false;
        }
    }
    return false;
}

int MHTPartition::lock(mht_key_t mht_key) {
    size_t idx = bucket_index(mht_key);
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key) {
            if(it->data.islocked()) {
                return -1;
            }
            return it->data.lock();
        }
    }
    // not found - locking impossible
    KLOG(MHT, "MHT: Could not lock key " << PRINT_HASH(mht_key) << " (not found).");
    return 0;
}

bool MHTPartition::unlock(mht_key_t mht_key, uint lockHandle) {
    size_t idx = bucket_index(mht_key);
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key)
            return it->data.unlock(lockHandle);
    }
    // not found - unlocking succeeds
    return true;
}

uint MHTPartition::reserve(mht_key_t mht_key) {
    size_t idx = bucket_index(mht_key);
    // check whether there exists an item already
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++)
        if(it->data._mht_key == mht_key)
            return 0;

    // reserve the slot
    MHTItem placeholder(nullptr, 0, mht_key);
    placeholder.reservation = true;
    uint reservationNr = placeholder.lock();
    _buckets[idx].append(new MHTItemStorable(m3::Util::move(placeholder)));
    _count++;
    return reservationNr;
}

m3::Errors::Code MHTPartition::release(mht_key_t mht_key, uint reservation) {
    size_t idx = bucket_index(mht_key);
    m3::Errors::Code err = m3::Errors::NO_ERROR;
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key) {
            if(it->data.unlock(reservation)) {
                delete &(*it);
                _count--;
                break;
            }
            else
                err = m3::Errors::NO_PERM;
        }
    }
    return err;
}

void MHTPartition::enqueueTicket(mht_key_t mht_key) {
    size_t idx = bucket_index(mht_key);
    for(auto it = _buckets[idx].begin(); it != _buckets[idx].end(); it++){
        if(it->data._mht_key == mht_key)
            it->data.enqueueTicket();
    }
}

size_t MHTPartition::serializedSize() {
    size_t size = m3::ostreamsize<membership_entry::pe_id_t, size_t>();
    for(size_t b = 0; b < NUM_BUCKETS; b++)
        for(auto it = _buckets[b].begin(); it != _buckets[b].end(); it++)
            size += it->data.serializedSize();
    return size;
}

void MHTPartition::serialize(GateOStream &ser) {
    ser << _id << _count;
    for(size_t b = 0; b < NUM_BUCKETS; b++)
        for(auto it = _buckets[b].begin(); it != _buckets[b].end(); it++)
            it->data.serialize(ser);
}

template<class T>
m3::Errors::Code MHTPartition::deserialize(T &ser) {
    size_t numItems;
    ser >> _id >> numItems;
    for(size_t i = 0; i < numItems; i++) {
        MHTItem it(ser);
        MHTItemStorable *item = new MHTItemStorable(m3::Util::move(it));
        if(!item) {
            KLOG(ERR, "Could not get storage for item in MHT!");
            for(size_t b = 0; b < NUM_BUCKETS; b++) {
                for(auto jt = _buckets[b].begin(); jt != _buckets[b].end(); ) {
                    auto old = jt++;
                    delete &(*old);
                }
                _buckets[b].remove_all();
            }
            _count = 0;
            return m3::Errors::OUT_OF_MEM;
        }
        size_t idx = bucket_index(item->data._mht_key);
        _buckets[idx].append(item);
        _count++;
    }
    return m3::Errors::NO_ERROR;
}
template m3::Errors::Code MHTPartition::deserialize<m3::Unmarshaller>(m3::Unmarshaller &ser);
template m3::Errors::Code MHTPartition::deserialize<GateIStream>(GateIStream &ser);

void MHTPartition::printItems() {
    KLOG(MHT, "-- Printing Items of partition #" << _id);
    for(size_t b = 0; b < NUM_BUCKETS; b++)
        for(auto it = _buckets[b].begin(); it != _buckets[b].end(); it++)
            it->data.printState();
}

}
