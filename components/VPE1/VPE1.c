/*
 * VPE1.c -- Second user VPE for SemperOS on seL4/CAmkES
 *
 * Task 06: Passive VPE for EXCHANGE syscall testing.
 * VPE1's CapTable is manipulated directly by the kernel during EXCHANGE
 * operations initiated by VPE0. VPE1 does not send or receive messages
 * in the EXCHANGE path -- it just needs to exist.
 */

#include <stdio.h>
#include <camkes.h>

/* VPE1 is PE 3 in the platform config */
#define MY_PE       3
#define MY_VPE_ID   1

int run(void)
{
    printf("[VPE1] Starting (PE %d, VPE ID %d) — passive mode for EXCHANGE tests\n",
           MY_PE, MY_VPE_ID);

    /* VPE1 is passive: no syscalls, no messages.
     * The kernel manipulates our CapTable directly during EXCHANGE.
     * Just yield forever so kernel and VPE0 can run. */
    for (;;) {
        seL4_Yield();
    }

    return 0;
}
