/*
 * sel4/DTU.h -- DTU interface for SemperOS on seL4/CAmkES
 *
 * Mirrors base/arch/gem5/DTU.h. The actual hardware-level DTU operations
 * are replaced by vDTU shared memory operations in kernel/arch/sel4/DTU.cc.
 * This header preserves the m3::DTU class with identical Header/Message
 * structs and public API for type compatibility.
 */

#pragma once

#include <base/Common.h>
#include <base/Env.h>
#include <base/util/Util.h>
#include <base/util/Sync.h>
#include <base/Errors.h>
#include <assert.h>

/* Yield wrapper (implemented in camkes_entry.c which can include sel4 headers) */
extern "C" void sel4_yield_wrapper(void);

#define DTU_PKG_SIZE        (static_cast<size_t>(8))

/* Bit-field constants from gem5 — kept for compatibility */
#define ID_BITS         64
#define RESERVED_BITS   5
#define VALID_BITS      1
#define CORE_BITS       10
#define VPE_BITS        11
#define OFFSET_BITS     (ID_BITS - RESERVED_BITS - VALID_BITS - CORE_BITS - VPE_BITS)

#define VALID_SHIFT     (ID_BITS - RESERVED_BITS)
#define CORE_SHIFT      (VALID_SHIFT - CORE_BITS)
#define VPE_SHIFT       (CORE_SHIFT - VPE_BITS)

#define CORE_VALID_OFFSET     (static_cast<unsigned long>(1) << (CORE_BITS + 1))

#define REG_SIZE        64
#define EP_BITS         8
#define MAX_MSG_SZ_BITS 16
#define CREDITS_BITS    16
#define FLAGS_BITS      4

namespace kernel {
class DTU;
}

namespace m3 {

class DTU {
    friend class kernel::DTU;

    explicit DTU() {
    }

public:
    typedef uint64_t reg_t;

    enum class EpType {
        INVALID,
        SEND,
        RECEIVE,
        MEMORY
    };

    /* Page table constants — kept for compatibility */
    typedef uint64_t pte_t;

    enum {
        PTE_BITS            = 3,
        PTE_SIZE            = 1 << PTE_BITS,
        LEVEL_CNT           = 2,
        LEVEL_BITS          = PAGE_BITS - PTE_BITS,
        LEVEL_MASK          = (1 << LEVEL_BITS) - 1,
        PTE_REC_IDX         = LEVEL_MASK,
    };

    enum {
        PTE_R               = 1,
        PTE_W               = 2,
        PTE_X               = 4,
        PTE_I               = 8,
        PTE_GONE            = 16,
        PTE_RW              = PTE_R | PTE_W,
        PTE_RWX             = PTE_RW | PTE_X,
        PTE_IRWX            = PTE_RWX | PTE_I,
    };

    struct Header {
        uint8_t flags; // if bit 0 is set its a reply, if bit 1 is set we grant credits
        uint16_t senderCoreId;
        uint8_t senderEpId;
        uint8_t replyEpId; // for a normal message this is the reply epId
                           // for a reply this is the enpoint that receives credits
        uint16_t length;
        uint16_t senderVpeId;

        uint64_t label;
        uint64_t replylabel;
    } PACKED;

    struct Message : Header {
        int send_epid() const {
            return senderEpId;
        }
        int reply_epid() const {
            return replyEpId;
        }

        unsigned char data[];
    } PACKED;

    static const size_t HEADER_SIZE         = sizeof(Header);

    static const int FLAG_NO_RINGBUF        = 0;
    static const int FLAG_NO_HEADER         = 1;

    static const size_t CREDITS_UNLIM       = ((static_cast<size_t>(1) << CREDITS_BITS) - 1);

    static const int MEM_EP                 = 0;
    static const int SYSC_EP                = 0;
    static const int DEF_RECVEP             = 1;
    static const int FIRST_FREE_EP          = 2;
    static const int MAX_MSG_SLOTS          = 32;

    static DTU &get() {
        return inst;
    }

    static size_t noc_to_pe(uint64_t noc) {
        return (noc >> CORE_SHIFT) - CORE_VALID_OFFSET;
    }
    static uintptr_t noc_to_virt(uint64_t noc) {
        return noc & ((static_cast<uint64_t>(1) << CORE_SHIFT) - 1);
    }
    static uint64_t build_noc_addr(int pe, uintptr_t virt) {
        return (static_cast<uintptr_t>(CORE_VALID_OFFSET + pe) << CORE_SHIFT) | virt;
    }

    /* On sel4 these are implemented via vDTU shared memory */
    Errors::Code send(int ep, const void *msg, size_t size, label_t replylbl, int reply_ep);
    Errors::Code reply(int ep, const void *msg, size_t size, size_t off);
    Errors::Code read(int ep, void *msg, size_t size, size_t off);
    Errors::Code write(int ep, const void *msg, size_t size, size_t off);
    Errors::Code cmpxchg(int, const void *, size_t, size_t, size_t) {
        return Errors::NO_ERROR;
    }

    bool is_valid(int epid) const;
    Message *fetch_msg(int epid) const;
    size_t get_msgoff(int, const Message *msg) const {
        return reinterpret_cast<uintptr_t>(msg);
    }
    void mark_read(int ep, size_t off);

    bool wait() const {
        /* Yield to let other CAmkES components (VPE0) run.
         * On single-core QEMU, busy-polling starves lower-priority threads. */
        sel4_yield_wrapper();
        return true;
    }
    void wait_until_ready(int) const {
        /* no-op on sel4 */
    }
    bool wait_for_mem_cmd() const {
        return true;
    }

    uintptr_t get_last_pf() const {
        return 0; /* no page faults on sel4 prototype */
    }

    void debug_msg(uint) {
        /* no-op */
    }

private:
    static DTU inst;
};

}
