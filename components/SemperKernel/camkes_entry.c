/*
 * camkes_entry.c -- CAmkES run() bridge to SemperOS C++ kernel
 *
 * CAmkES generates a C entry point (run). This file bridges to the
 * C++ kernel_start() function, and also calls the old test suite if
 * SEMPER_RUN_VDTU_TESTS is defined.
 */

#include <stdio.h>
#include <camkes.h>
#include <sel4/sel4.h>
#include <stddef.h>

/* C++ functions */
extern void cxx_test(void);
extern void kernel_start(void);

/*
 * Heap for the SemperOS kernel (4 MiB).
 *
 * CAmkES forces LibSel4MuslcSysMorecoreBytes=0 and provides a 1 MiB heap
 * via its component.common.c template (CONFIG_CAMKES_DEFAULT_HEAP_SIZE).
 * The SemperOS kernel needs ~4 MiB for capability tables, VPE objects,
 * receive buffers, and the DDL hash table.
 *
 * We override the heap via a constructor at priority 200, which runs BEFORE:
 *   - musl libc init (priority 201)
 *   - CAmkES init (priority 202)
 *   - SemperOS INIT_PRIO_USER constructors (priority 201+)
 *
 * This ensures that the first call to malloc/brk finds our 4 MiB buffer
 * via morecore_area/morecore_size (musl's init_morecore_region is lazy).
 */
#define KERNEL_HEAP_SIZE  (4 * 1024 * 1024)  /* 4 MiB */

static char __attribute__((aligned(4096)))
    kernel_heap[KERNEL_HEAP_SIZE];

/* Defined in libsel4muslcsys/src/sys_morecore.c */
extern char *morecore_area;
extern size_t morecore_size;

static void __attribute__((constructor(200))) init_kernel_heap(void)
{
    morecore_area = kernel_heap;
    morecore_size = KERNEL_HEAP_SIZE;
}

/* Yield wrapper callable from C++ (sel4.h can't be included in C++) */
void sel4_yield_wrapper(void)
{
    seL4_Yield();
}

/*
 * Incoming network DTU message buffer.
 * DTUBridge deposits a message in the dtu_in dataport and signals us.
 * We copy it to this static buffer so the WorkLoop can poll it.
 *
 * Simple single-slot buffer for Tier 1 prototype.
 * The net_msg_arrived notification handler sets net_msg_pending=1.
 * The WorkLoop (or kernel polling loop) checks this flag.
 */
#include <string.h>
#include "vdtu_ring.h"

static volatile int net_msg_pending = 0;
static uint8_t net_msg_buf[2048];
static uint16_t net_msg_len = 0;

/* CAmkES notification handler: DTUBridge -> SemperKernel */
void net_msg_arrived_handle(void)
{
    volatile uint8_t *src = (volatile uint8_t *)dtu_in;

    /* Read length header (first 2 bytes of dtu_in) */
    uint16_t len = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));

    if (len > 0 && len <= sizeof(net_msg_buf)) {
        memcpy(net_msg_buf, (const void *)(src + 2), len);
        net_msg_len = len;
        __sync_synchronize();
        net_msg_pending = 1;

        /* Parse and log the DTU header */
        if (len >= VDTU_HEADER_SIZE) {
            const struct vdtu_msg_header *hdr = (const struct vdtu_msg_header *)net_msg_buf;
            printf("[SemperKernel] NET RX: from PE %u EP %u, label=0x%lx, payload=%u bytes\n",
                   hdr->sender_core_id, hdr->sender_ep_id,
                   (unsigned long)hdr->label, hdr->length);
        }
    }
}

/* Accessors for C++ code */
int net_msg_is_pending(void)
{
    return net_msg_pending;
}

const uint8_t *net_msg_get_buf(void)
{
    return net_msg_buf;
}

uint16_t net_msg_get_len(void)
{
    return net_msg_len;
}

void net_msg_clear(void)
{
    net_msg_pending = 0;
    net_msg_len = 0;
}

int run(void)
{
    printf("=== SemperOS Kernel on seL4/CAmkES ===\n");

    /* Verify C++ runtime is working */
    cxx_test();

    /* Start the SemperOS kernel */
    kernel_start();

    return 0;
}
