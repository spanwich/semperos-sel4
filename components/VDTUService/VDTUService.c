/*
 * VDTUService.c -- Virtual DTU service component
 *
 * This component implements the vDTU control plane. It manages a table of
 * endpoint descriptors for all PEs in the system and handles configuration
 * RPCs from the SemperOS kernel.
 *
 * When the kernel configures a send endpoint on PE A targeting a receive
 * endpoint on PE B, the vDTU:
 *   1. Records the endpoint configuration in its table
 *   2. Identifies which pre-allocated dataport connects A and B
 *   3. Initializes the ring buffer in that dataport (if not already done)
 *
 * The vDTU does NOT sit on the data path: after setup, messages flow
 * directly through shared memory ring buffers between components.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"

/*
 * =========================================================================
 *  Constants
 * =========================================================================
 */

#define MAX_PES         4       /* Max PEs in this prototype */
#define EP_PER_PE       VDTU_EP_COUNT   /* 16 endpoints per PE */

/* PE IDs for this prototype */
#define PE_KERNEL       0
#define PE_VPE0         1
#define PE_VDTU         2       /* vDTU itself (not a real PE) */

/* Pre-allocated channel count */
#define NUM_MSG_CHANNELS    8
#define NUM_MEM_CHANNELS    4

/*
 * =========================================================================
 *  Endpoint Descriptor
 * =========================================================================
 */

enum ep_type {
    EP_INVALID = 0,
    EP_SEND    = 1,
    EP_RECEIVE = 2,
    EP_MEMORY  = 3,
};

struct ep_desc {
    enum ep_type type;

    union {
        struct {
            int      dest_pe;
            int      dest_ep;
            int      dest_vpe;
            int      msg_size;
            uint64_t label;
            int      credits;
        } send;

        struct {
            int buf_order;
            int msg_order;
            int flags;
            int channel_idx;    /* which pre-allocated channel to use */
        } recv;

        struct {
            int      dest_pe;
            uint64_t addr;
            uint64_t size;
            int      dest_vpe;
            int      perm;
            int      channel_idx;
        } mem;
    };
};

/*
 * =========================================================================
 *  Global State
 * =========================================================================
 */

/* Endpoint table: endpoints[pe][ep] */
static struct ep_desc endpoints[MAX_PES][EP_PER_PE];

/* VPE ID for each PE */
static int pe_vpe_id[MAX_PES];

/* Privilege flag for each PE */
static int pe_privileged[MAX_PES];

/* Track which message channels have been initialized */
static int msg_channel_initialized[NUM_MSG_CHANNELS];

/* Next free message channel index */
static int next_msg_channel = 0;

/* Next free memory channel index */
static int next_mem_channel = 0;

/*
 * =========================================================================
 *  Channel Assignment
 *
 *  When a recv endpoint is configured, the vDTU assigns one of the
 *  pre-allocated dataports as the ring buffer backing store. When a
 *  send endpoint is later configured to target that recv endpoint,
 *  the sender knows which shared memory to write to.
 * =========================================================================
 */

static int assign_msg_channel(void)
{
    if (next_msg_channel >= NUM_MSG_CHANNELS) {
        printf("[vDTU] ERROR: no free message channels\n");
        return -1;
    }
    return next_msg_channel++;
}

static int assign_mem_channel(void)
{
    if (next_mem_channel >= NUM_MEM_CHANNELS) {
        printf("[vDTU] ERROR: no free memory channels\n");
        return -1;
    }
    return next_mem_channel++;
}

/*
 * =========================================================================
 *  VDTUConfig RPC Implementation
 * =========================================================================
 */

int config_config_send(int target_pe, int ep_id,
                       int dest_pe, int dest_ep, int dest_vpe,
                       int msg_size, uint64_t label, int credits)
{
    printf("[vDTU] config_send(target_pe=%d, ep=%d, dest_pe=%d, dest_ep=%d, "
           "dest_vpe=%d, msg_size=%d, label=0x%lx, credits=%d)\n",
           target_pe, ep_id, dest_pe, dest_ep, dest_vpe,
           msg_size, (unsigned long)label, credits);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: invalid PE or EP index\n");
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    ep->type = EP_SEND;
    ep->send.dest_pe   = dest_pe;
    ep->send.dest_ep   = dest_ep;
    ep->send.dest_vpe  = dest_vpe;
    ep->send.msg_size  = msg_size;
    ep->send.label     = label;
    ep->send.credits   = credits;

    return 0;
}

int config_config_recv(int target_pe, int ep_id,
                       int buf_order, int msg_order, int flags)
{
    printf("[vDTU] config_recv(target_pe=%d, ep=%d, buf_order=%d, "
           "msg_order=%d, flags=%d)\n",
           target_pe, ep_id, buf_order, msg_order, flags);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: invalid PE or EP index\n");
        return -1;
    }

    int ch = assign_msg_channel();
    if (ch < 0)
        return -1;

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    ep->type = EP_RECEIVE;
    ep->recv.buf_order   = buf_order;
    ep->recv.msg_order   = msg_order;
    ep->recv.flags       = flags;
    ep->recv.channel_idx = ch;

    /* Initialize the ring buffer in the assigned dataport.
     * slot_count = 1 << (buf_order - msg_order)
     * slot_size  = 1 << msg_order
     * For the prototype, we cap at what fits in a 4 KiB dataport. */
    uint32_t slot_size  = 1u << msg_order;
    uint32_t slot_count = 1u << (buf_order - msg_order);

    /* Cap to what fits in a 4 KiB page */
    size_t needed = vdtu_ring_total_size(slot_count, slot_size);
    if (needed > 4096) {
        /* Reduce slot count to fit */
        slot_count = (4096 - VDTU_RING_CTRL_SIZE) / slot_size;
        /* Round down to power of 2 */
        uint32_t p = 1;
        while (p * 2 <= slot_count) p *= 2;
        slot_count = p;
        if (slot_count < 2) slot_count = 2;
    }

    printf("[vDTU]   -> assigned channel %d (slot_count=%u, slot_size=%u)\n",
           ch, slot_count, slot_size);

    /* TODO: In the real implementation, initialize the ring buffer in the
     * dataport memory. For now, we just record the assignment.
     * The SemperKernel and VPE0 stubs will initialize their own view
     * of the ring buffer using the channel index. */
    msg_channel_initialized[ch] = 1;

    return ch;  /* Return channel index so caller can find the dataport */
}

int config_config_mem(int target_pe, int ep_id,
                      int dest_pe, uint64_t addr, uint64_t size,
                      int dest_vpe, int perm)
{
    printf("[vDTU] config_mem(target_pe=%d, ep=%d, dest_pe=%d, "
           "addr=0x%lx, size=0x%lx, dest_vpe=%d, perm=%d)\n",
           target_pe, ep_id, dest_pe,
           (unsigned long)addr, (unsigned long)size, dest_vpe, perm);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        return -1;
    }

    int ch = assign_mem_channel();
    if (ch < 0)
        return -1;

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    ep->type = EP_MEMORY;
    ep->mem.dest_pe     = dest_pe;
    ep->mem.addr        = addr;
    ep->mem.size        = size;
    ep->mem.dest_vpe    = dest_vpe;
    ep->mem.perm        = perm;
    ep->mem.channel_idx = ch;

    printf("[vDTU]   -> assigned memory channel %d\n", ch);
    return ch;
}

int config_invalidate_ep(int target_pe, int ep_id)
{
    printf("[vDTU] invalidate_ep(target_pe=%d, ep=%d)\n", target_pe, ep_id);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        return -1;
    }

    memset(&endpoints[target_pe][ep_id], 0, sizeof(struct ep_desc));
    return 0;
}

int config_invalidate_eps(int target_pe, int first_ep)
{
    printf("[vDTU] invalidate_eps(target_pe=%d, first_ep=%d)\n",
           target_pe, first_ep);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        first_ep < 0 || first_ep >= EP_PER_PE) {
        return -1;
    }

    for (int i = first_ep; i < EP_PER_PE; i++) {
        memset(&endpoints[target_pe][i], 0, sizeof(struct ep_desc));
    }
    return 0;
}

int config_set_vpe_id(int target_pe, int vpe_id)
{
    printf("[vDTU] set_vpe_id(target_pe=%d, vpe_id=%d)\n", target_pe, vpe_id);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    pe_vpe_id[target_pe] = vpe_id;
    return 0;
}

int config_set_privilege(int target_pe, int priv)
{
    printf("[vDTU] set_privilege(target_pe=%d, priv=%d)\n", target_pe, priv);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    pe_privileged[target_pe] = priv;
    return 0;
}

int config_wakeup_pe(int target_pe)
{
    printf("[vDTU] wakeup_pe(target_pe=%d)\n", target_pe);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    /* In the real implementation, this would signal the target PE's
     * notification to wake it from a seL4_Wait(). For now, no-op. */
    return 0;
}

int config_get_ep_count(void)
{
    return VDTU_EP_COUNT;
}

/*
 * =========================================================================
 *  Component Entry Point
 * =========================================================================
 */

void pre_init(void)
{
    printf("[vDTU] Initializing endpoint table (%d PEs x %d EPs)\n",
           MAX_PES, EP_PER_PE);

    memset(endpoints, 0, sizeof(endpoints));
    memset(pe_vpe_id, 0, sizeof(pe_vpe_id));
    memset(pe_privileged, 0, sizeof(pe_privileged));
    memset(msg_channel_initialized, 0, sizeof(msg_channel_initialized));

    printf("[vDTU] Initialized, managing endpoint table\n");
}

int run(void)
{
    /* The vDTU service is entirely RPC-driven.
     * After initialization, it waits for config RPCs from the kernel.
     * The CAmkES runtime handles the RPC dispatch loop automatically.
     *
     * For the prototype, the run() function just confirms startup
     * and then returns (CAmkES handles the event loop). */
    printf("[vDTU] Ready for configuration requests\n");
    return 0;
}
