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

#pragma once

#include <base/col/SList.h>

#include "ddl/MHTTypes.h"

namespace kernel {

struct Revocation;

struct RevocationSub : m3::SListItem {
    explicit RevocationSub(Revocation *_rev) : rev(_rev) {}

    Revocation *rev;
};

struct Revocation {
    explicit Revocation(mht_key_t _capID, mht_key_t _parent, mht_key_t _origin, int _awaitedResp, int _tid)
    : capID(_capID), parent(_parent), origin(_origin), awaitedResp(_awaitedResp), tid(_tid), subscribers() {
#ifndef NDEBUG
        // correctness checks
        // we only use generic cap IDs for revocations
        if(HashUtil::hashToType(_capID) == ItemType::MAPCAP)
            assert((_capID & TYPE_MASK_MCAP) == TYPE_MASK_MCAP);
        else
            assert((_capID & TYPE_MASK_OCAP) == TYPE_MASK_OCAP);
        if(HashUtil::hashToType(_parent) == ItemType::MAPCAP)
            assert((_parent & TYPE_MASK_MCAP) == TYPE_MASK_MCAP);
        else
            assert((_parent & TYPE_MASK_OCAP) == TYPE_MASK_OCAP);
        if(HashUtil::hashToType(_origin) == ItemType::MAPCAP)
            assert((_origin & TYPE_MASK_MCAP) == TYPE_MASK_MCAP);
        else
            assert((_origin & TYPE_MASK_OCAP) == TYPE_MASK_OCAP);
#endif
    }

    void subscribe(Revocation *sub) {
        subscribers.append(new RevocationSub(sub));
    }
    void notifySubscribers();

    mht_key_t capID;
    mht_key_t parent;
    mht_key_t origin; // cap which started revocation
    int awaitedResp; // own awaited resps
    int tid; // tid of origin's thread
    m3::SList<RevocationSub> subscribers; // revocations waiting for this one to finish
};

class RevocationList {
    // Open-addressing hash table size (must be power of 2).
    // Bounded by: threads * kernels in-flight revocations. 256 is generous.
    static const size_t HASH_CAP = 256;
    static const size_t HASH_MASK = HASH_CAP - 1;

    // Fibonacci hash for 64-bit keys
    static size_t hash_index(mht_key_t key) {
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 56) & HASH_MASK;
    }

    explicit RevocationList() : _count(0) {
        for(size_t i = 0; i < HASH_CAP; i++)
            _buckets[i] = nullptr;
    }
public:

    static RevocationList &get() {
        return _inst;
    }

    Revocation *add(mht_key_t cap, mht_key_t parent, mht_key_t origin) {
#ifndef NDEBUG
        // make sure there's only one entry per cap ID, otherwise the revoke algorithm fails
        if(find_exact(cap) != nullptr)
            PANIC("Cannot insert second entry for revocation of cap: " << PRINT_HASH(cap));
#endif
        if(_count >= HASH_CAP)
            PANIC("RevocationList full (" << _count << "/" << HASH_CAP
                << ") while inserting cap: " << PRINT_HASH(cap));
        Revocation *rev = new Revocation(cap, parent, origin, 0,
                (origin == cap) ? m3::ThreadManager::get().current()->id() : -1);
        size_t idx = hash_index(cap);
        while(_buckets[idx] != nullptr)
            idx = (idx + 1) & HASH_MASK;
        _buckets[idx] = rev;
        _count++;
        return rev;
    }

    Revocation *find(mht_key_t cap) {
        cap = (HashUtil::hashToType(cap) == ItemType::MAPCAP) ?
            (cap | TYPE_MASK_MCAP) : (cap | TYPE_MASK_OCAP);
        return find_exact(cap);
    }

    /**
     * Deletes the ongoing revocation of the given cap with the given origin.
     *
     * @param cap       The cap ID to remove
     */
    void remove(mht_key_t cap) {
        size_t idx = hash_index(cap);
        while(_buckets[idx] != nullptr) {
            if(_buckets[idx]->capID == cap) {
                Revocation *rev = _buckets[idx];
                _buckets[idx] = nullptr;
                _count--;
                // Re-insert displaced entries (Robin Hood fixup)
                size_t next = (idx + 1) & HASH_MASK;
                while(_buckets[next] != nullptr) {
                    Revocation *displaced = _buckets[next];
                    _buckets[next] = nullptr;
                    _count--;
                    reinsert(displaced);
                    next = (next + 1) & HASH_MASK;
                }
                delete rev;
                return;
            }
            idx = (idx + 1) & HASH_MASK;
        }
    }

private:
    Revocation *find_exact(mht_key_t cap) {
        size_t idx = hash_index(cap);
        while(_buckets[idx] != nullptr) {
            if(_buckets[idx]->capID == cap)
                return _buckets[idx];
            idx = (idx + 1) & HASH_MASK;
        }
        return nullptr;
    }

    void reinsert(Revocation *rev) {
        size_t idx = hash_index(rev->capID);
        while(_buckets[idx] != nullptr)
            idx = (idx + 1) & HASH_MASK;
        _buckets[idx] = rev;
        _count++;
    }

    Revocation *_buckets[HASH_CAP];
    size_t _count;
    static RevocationList _inst;
};

}
