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

int run(void)
{
    printf("=== SemperOS Kernel on seL4/CAmkES ===\n");

    /* Verify C++ runtime is working */
    cxx_test();

    /* Start the SemperOS kernel */
    kernel_start();

    return 0;
}
