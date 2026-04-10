/*
 * VDTUService.c -- Virtual DTU service component (full endpoint management)
 *
 * This component implements the vDTU control plane. It manages a table of
 * endpoint descriptors for all PEs in the system and handles configuration
 * RPCs from the SemperOS kernel.
 *
 * Key behaviors:
 *   - config_recv() allocates a message channel from the free pool
 *   - config_send() returns the SAME channel as the target recv EP
 *   - config_mem() allocates a memory channel from the free pool
 *   - invalidate_ep() frees channels back to the pool (recv/mem types)
 *   - wakeup_pe() emits the appropriate notification
 *
 * The vDTU does NOT sit on the data path and does NOT have access to any
 * shared memory dataports.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"

/* Per-RPC success logging. Disabled by default for clean benchmarks —
 * QEMU serial output adds ~1ms per printf call. Build with
 * -DVDTU_VERBOSE_LOG to enable. */
#ifdef VDTU_VERBOSE_LOG
#define VDTU_LOG(...) printf(__VA_ARGS__)
#else
#define VDTU_LOG(...) ((void)0)
#endif

/* Stub until RaftLogCache is implemented — returns false (no blocked ancestors).
 * This is safe: it means the enforcement chain is permissive (allows all),
 * which matches current behavior. See docs/VERIFICATION-WIRING-SPEC.md. */
static inline bool raft_cache_check_ancestry(uint64_t cap_id)
{
    (void)cap_id;
    return false;
}

/*
 * =========================================================================
 *  Constants
 * =========================================================================
 */

#define MAX_PES             8   /* must accommodate global PE IDs (node1: PEs 4-7) */
#define EP_PER_PE           VDTU_EP_COUNT   /* 16 endpoints per PE */

/* PE IDs for this prototype */
#define PE_KERNEL           0
#define PE_VPE0             1
#define PE_VPE1             3

/* Uniform channel pool: 16 channels per PE pair.
 * Channels 0-15:  kernel ↔ VPE0 (PE 2)
 * Channels 16-31: kernel ↔ VPE1 (PE 3)
 * No msg/mem split — type is tracked per-endpoint, not per-channel. */
#define CHANNELS_PER_PE     16
#define TOTAL_CHANNELS      32

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
    int channel_idx;        /* Index into pre-allocated channel pool (-1 if unassigned) */

    /* Send EP fields */
    int dest_pe;
    int dest_ep;
    int dest_vpe;
    int msg_size;
    uint64_t label;
    int credits;

    /* Receive EP fields */
    int buf_order;
    int msg_order;
    int slot_count;         /* Derived: 1 << (buf_order - msg_order) */
    int slot_size;          /* Derived: 1 << msg_order */
    int flags;

    /* Memory EP fields */
    uint64_t mem_addr;
    uint64_t mem_size;
    int mem_perm;           /* R=1, W=2, RW=3 */
};

/*
 * =========================================================================
 *  Channel Pool Tracking
 * =========================================================================
 */

struct channel_pool {
    int in_use[TOTAL_CHANNELS];          /* 0=free, 1=assigned */
    int assigned_pe[TOTAL_CHANNELS];     /* Which PE owns the recv EP */
    int assigned_ep[TOTAL_CHANNELS];     /* Which recv EP number */
};

/*
 * =========================================================================
 *  Global State
 * =========================================================================
 */

static struct ep_desc endpoints[MAX_PES][EP_PER_PE];
static struct channel_pool pool;
static int pe_vpe_id[MAX_PES];
static int pe_privileged[MAX_PES];

/*
 * =========================================================================
 *  Channel Assignment (free-list based)
 * =========================================================================
 */

/* Map PE to channel pool base. Each PE pair gets 16 channels.
 * VPE1 (local PE 3, regardless of global offset) uses channels 16-31.
 * All other PEs (kernel=0, vDTU=1, VPE0=2) use channels 0-15. */
static int pe_channel_base(int pe)
{
    /* pe is always local PE index (DTU.cc translates global→local) */
    return (pe == PE_VPE1) ? CHANNELS_PER_PE : 0;
}

static int alloc_channel(int pe, int ep)
{
    int base = pe_channel_base(pe);
    int end = base + CHANNELS_PER_PE;
    for (int i = base; i < end; i++) {
        if (!pool.in_use[i]) {
            pool.in_use[i] = 1;
            pool.assigned_pe[i] = pe;
            pool.assigned_ep[i] = ep;
            return i;
        }
    }
    printf("[vDTU] ERROR: no free channels for PE %d (range %d-%d)\n",
           pe, base, end - 1);
    return -1;
}

static void free_channel(int ch)
{
    if (ch >= 0 && ch < TOTAL_CHANNELS) {
        pool.in_use[ch] = 0;
        pool.assigned_pe[ch] = -1;
        pool.assigned_ep[ch] = -1;
    }
}

/*
 * =========================================================================
 *  VDTUConfig RPC Implementation
 * =========================================================================
 */

int config_config_recv(int target_pe, int ep_id,
                       int buf_order, int msg_order, int flags)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: config_recv invalid params (pe=%d, ep=%d)\n",
               target_pe, ep_id);
        return -1;
    }

    /* Enforcement chain: check Raft cache for blocked ancestors (Lemma 1) */
    if (raft_cache_check_ancestry((uint64_t)target_pe << 32 | (uint64_t)ep_id)) {
        printf("[vDTU] EPERM: blocked ancestor for pe=%d ep=%d\n",
               target_pe, ep_id);
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID) {
        printf("[vDTU] ERROR: config_recv pe=%d ep=%d already configured (type=%d)\n",
               target_pe, ep_id, ep->type);
        return -1;
    }

    int ch = alloc_channel(target_pe, ep_id);
    if (ch < 0)
        return -1;

    /* Compute slot parameters from orders */
    uint32_t slot_size  = 1u << msg_order;
    uint32_t slot_count = 1u << (buf_order - msg_order);

    /* Cap to what fits in a 4 KiB dataport */
    size_t needed = vdtu_ring_total_size(slot_count, slot_size);
    if (needed > 4096) {
        slot_count = (4096 - VDTU_RING_CTRL_SIZE) / slot_size;
        uint32_t p = 1;
        while (p * 2 <= slot_count) p *= 2;
        slot_count = p;
        if (slot_count < 2) slot_count = 2;
    }

    ep->type        = EP_RECEIVE;
    ep->channel_idx = ch;
    ep->buf_order   = buf_order;
    ep->msg_order   = msg_order;
    ep->slot_count  = (int)slot_count;
    ep->slot_size   = (int)slot_size;
    ep->flags       = flags;

    printf("[vDTU] config_recv(pe=%d, ep=%d) -> channel %d (%d slots x %dB)\n",
           target_pe, ep_id, ch, (int)slot_count, (int)slot_size);

    return ch;
}

int config_config_recv_at(int target_pe, int ep_id, int channel,
                          int buf_order, int msg_order, int flags)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: config_recv_at invalid params (pe=%d, ep=%d)\n",
               target_pe, ep_id);
        return -1;
    }

    if (channel < 0 || channel >= TOTAL_CHANNELS) {
        printf("[vDTU] ERROR: config_recv_at invalid channel %d\n", channel);
        return -1;
    }

    if (pool.in_use[channel]) {
        printf("[vDTU] ERROR: config_recv_at channel %d already in use\n", channel);
        return -1;
    }

    /* Enforcement chain: check Raft cache for blocked ancestors (Lemma 1) */
    if (raft_cache_check_ancestry((uint64_t)target_pe << 32 | (uint64_t)ep_id)) {
        printf("[vDTU] EPERM: blocked ancestor for pe=%d ep=%d\n",
               target_pe, ep_id);
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID) {
        printf("[vDTU] ERROR: config_recv_at pe=%d ep=%d already configured (type=%d)\n",
               target_pe, ep_id, ep->type);
        return -1;
    }

    /* Mark channel in-use (intentionally allows cross-pool assignment) */
    pool.in_use[channel] = 1;
    pool.assigned_pe[channel] = target_pe;
    pool.assigned_ep[channel] = ep_id;

    /* Compute slot parameters from orders */
    uint32_t slot_size  = 1u << msg_order;
    uint32_t slot_count = 1u << (buf_order - msg_order);

    /* Cap to what fits in a 4 KiB dataport */
    size_t needed = vdtu_ring_total_size(slot_count, slot_size);
    if (needed > 4096) {
        slot_count = (4096 - VDTU_RING_CTRL_SIZE) / slot_size;
        uint32_t p = 1;
        while (p * 2 <= slot_count) p *= 2;
        slot_count = p;
        if (slot_count < 2) slot_count = 2;
    }

    ep->type        = EP_RECEIVE;
    ep->channel_idx = channel;
    ep->buf_order   = buf_order;
    ep->msg_order   = msg_order;
    ep->slot_count  = (int)slot_count;
    ep->slot_size   = (int)slot_size;
    ep->flags       = flags;

    printf("[vDTU] config_recv_at(pe=%d, ep=%d, ch=%d) -> (%d slots x %dB)\n",
           target_pe, ep_id, channel, (int)slot_count, (int)slot_size);

    return channel;
}

int config_config_send(int target_pe, int ep_id,
                       int dest_pe, int dest_ep, int dest_vpe,
                       int msg_size, uint64_t label, int credits)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: config_send invalid params (pe=%d, ep=%d)\n",
               target_pe, ep_id);
        return -1;
    }

    if (dest_pe < 0 || dest_pe >= MAX_PES ||
        dest_ep < 0 || dest_ep >= EP_PER_PE) {
        printf("[vDTU] ERROR: config_send invalid dest (pe=%d, ep=%d)\n",
               dest_pe, dest_ep);
        return -1;
    }

    /* Enforcement chain: check Raft cache for blocked ancestors (Lemma 1) */
    if (raft_cache_check_ancestry((uint64_t)target_pe << 32 | (uint64_t)ep_id)) {
        printf("[vDTU] EPERM: blocked ancestor for pe=%d ep=%d\n",
               target_pe, ep_id);
        return -1;
    }

    /* Look up the destination receive EP */
    struct ep_desc *dest = &endpoints[dest_pe][dest_ep];
    if (dest->type != EP_RECEIVE) {
        printf("[vDTU] ERROR: config_send dest pe=%d ep=%d is not RECEIVE (type=%d)\n",
               dest_pe, dest_ep, dest->type);
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];

    /* If this EP was previously configured, clear it first (but don't free
     * channels for SEND type - they belong to the recv EP) */
    if (ep->type != EP_INVALID) {
        ep->type = EP_INVALID;
        ep->channel_idx = -1;
    }

    ep->type        = EP_SEND;
    ep->channel_idx = dest->channel_idx;  /* Same channel as target recv EP */
    ep->dest_pe     = dest_pe;
    ep->dest_ep     = dest_ep;
    ep->dest_vpe    = dest_vpe;
    ep->msg_size    = msg_size;
    ep->label       = label;
    ep->credits     = credits;

    printf("[vDTU] config_send(pe=%d, ep=%d -> dest pe=%d ep=%d) -> channel %d\n",
           target_pe, ep_id, dest_pe, dest_ep, ep->channel_idx);

    return ep->channel_idx;
}

int config_config_mem(int target_pe, int ep_id,
                      int dest_pe, uint64_t addr, uint64_t size,
                      int dest_vpe, int perm)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        printf("[vDTU] ERROR: config_mem invalid params (pe=%d, ep=%d)\n",
               target_pe, ep_id);
        return -1;
    }

    /* Enforcement chain: check Raft cache for blocked ancestors (Lemma 1) */
    if (raft_cache_check_ancestry((uint64_t)target_pe << 32 | (uint64_t)ep_id)) {
        printf("[vDTU] EPERM: blocked ancestor for pe=%d ep=%d\n",
               target_pe, ep_id);
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    if (ep->type != EP_INVALID) {
        printf("[vDTU] ERROR: config_mem pe=%d ep=%d already configured (type=%d)\n",
               target_pe, ep_id, ep->type);
        return -1;
    }

    int ch = alloc_channel(target_pe, ep_id);
    if (ch < 0)
        return -1;

    ep->type        = EP_MEMORY;
    ep->channel_idx = ch;
    ep->dest_pe     = dest_pe;
    ep->mem_addr    = addr;
    ep->mem_size    = size;
    ep->dest_vpe    = dest_vpe;
    ep->mem_perm    = perm;

    VDTU_LOG("[vDTU] config_mem(pe=%d, ep=%d, dest_pe=%d, addr=0x%lx, size=0x%lx, perm=%d) "
             "-> mem channel %d\n",
             target_pe, ep_id, dest_pe,
             (unsigned long)addr, (unsigned long)size, perm, ch);

    return ch;
}

int config_invalidate_ep(int target_pe, int ep_id)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];

    if (ep->type == EP_RECEIVE) {
        VDTU_LOG("[vDTU] invalidate_ep(pe=%d, ep=%d) -> freed msg channel %d\n",
                 target_pe, ep_id, ep->channel_idx);
        free_channel(ep->channel_idx);
    } else if (ep->type == EP_MEMORY) {
        VDTU_LOG("[vDTU] invalidate_ep(pe=%d, ep=%d) -> freed mem channel %d\n",
                 target_pe, ep_id, ep->channel_idx);
        free_channel(ep->channel_idx);
    } else if (ep->type == EP_SEND) {
        VDTU_LOG("[vDTU] invalidate_ep(pe=%d, ep=%d) -> cleared send EP\n",
                 target_pe, ep_id);
        /* Send EP doesn't own the channel - just clear the entry */
    } else {
        VDTU_LOG("[vDTU] invalidate_ep(pe=%d, ep=%d) -> already invalid\n",
                 target_pe, ep_id);
    }

    memset(ep, 0, sizeof(*ep));
    ep->channel_idx = -1;

    return 0;
}

int config_terminate_ep(int target_pe, int ep_id)
{
    if (target_pe < 0 || target_pe >= MAX_PES ||
        ep_id < 0 || ep_id >= EP_PER_PE) {
        return -1;
    }

    struct ep_desc *ep = &endpoints[target_pe][ep_id];
    if (ep->type == EP_INVALID) {
        printf("[vDTU] ERROR: terminate_ep(pe=%d, ep=%d) not configured\n",
               target_pe, ep_id);
        return -1;
    }

    int ch = ep->channel_idx;
    VDTU_LOG("[vDTU] terminate_ep(pe=%d, ep=%d, type=%d) -> channel %d\n",
             target_pe, ep_id, ep->type, ch);

    /* Return channel index so caller can set ep_state in ring ctrl.
     * VDTUService is control-plane-only: no dataport access. */
    return ch;
}

int config_invalidate_eps(int target_pe, int first_ep)
{
    VDTU_LOG("[vDTU] invalidate_eps(pe=%d, first_ep=%d)\n", target_pe, first_ep);

    if (target_pe < 0 || target_pe >= MAX_PES ||
        first_ep < 0 || first_ep >= EP_PER_PE) {
        return -1;
    }

    for (int i = first_ep; i < EP_PER_PE; i++) {
        config_invalidate_ep(target_pe, i);
    }
    return 0;
}

int config_set_vpe_id(int target_pe, int vpe_id)
{
    VDTU_LOG("[vDTU] set_vpe_id(pe=%d, vpe_id=%d)\n", target_pe, vpe_id);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    pe_vpe_id[target_pe] = vpe_id;
    return 0;
}

int config_set_privilege(int target_pe, int priv)
{
    VDTU_LOG("[vDTU] set_privilege(pe=%d, priv=%d)\n", target_pe, priv);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    pe_privileged[target_pe] = priv;
    return 0;
}

int config_wakeup_pe(int target_pe)
{
    VDTU_LOG("[vDTU] wakeup_pe(pe=%d)\n", target_pe);

    if (target_pe < 0 || target_pe >= MAX_PES)
        return -1;

    if (target_pe == PE_KERNEL) {
        notify_kernel_emit();
    } else if (target_pe == PE_VPE0) {
        notify_vpe0_emit();
    } else if (target_pe == PE_VPE1) {
        notify_vpe1_emit();
    } else {
        printf("[vDTU] WARNING: wakeup_pe for unknown PE %d\n", target_pe);
        return -1;
    }

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
    memset(&pool, 0, sizeof(pool));
    memset(pe_vpe_id, 0, sizeof(pe_vpe_id));
    memset(pe_privileged, 0, sizeof(pe_privileged));

    /* Set all channel_idx to -1 (unassigned) */
    for (int pe = 0; pe < MAX_PES; pe++) {
        for (int ep = 0; ep < EP_PER_PE; ep++) {
            endpoints[pe][ep].channel_idx = -1;
        }
    }
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        pool.assigned_pe[i] = -1;
        pool.assigned_ep[i] = -1;
    }

    printf("[vDTU] Initialized (%d uniform channels available)\n",
           TOTAL_CHANNELS);
}

int run(void)
{
    printf("[vDTU] Ready for configuration requests\n");
    return 0;
}
