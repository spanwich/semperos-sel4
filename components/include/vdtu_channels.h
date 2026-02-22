/*
 * vdtu_channels.h -- Channel index -> dataport lookup table
 *
 * Each CAmkES component that participates in the data plane populates
 * this table at startup with pointers to its CAmkES-generated dataports.
 * The vDTU returns channel indices from config_recv/config_send/config_mem;
 * the component uses this table to find the actual shared memory.
 */

#ifndef VDTU_CHANNELS_H
#define VDTU_CHANNELS_H

#include "vdtu_ring.h"

#define VDTU_MSG_CHANNELS  8
#define VDTU_MEM_CHANNELS  4

struct vdtu_channel_table {
    volatile void *msg[VDTU_MSG_CHANNELS];
    volatile void *mem[VDTU_MEM_CHANNELS];
    struct vdtu_ring msg_rings[VDTU_MSG_CHANNELS];
};

/*
 * Initialize the channel table with CAmkES dataport pointers.
 * Called once at component startup.
 */
int vdtu_channels_init(struct vdtu_channel_table *ct,
                       volatile void *msg_dataports[],
                       volatile void *mem_dataports[]);

/*
 * Get the ring buffer for a given message channel index.
 * Returns NULL if channel not initialized.
 */
struct vdtu_ring *vdtu_channels_get_ring(struct vdtu_channel_table *ct,
                                         int channel_idx);

/*
 * Initialize a ring buffer in a message channel (receiver side).
 * slot_count and slot_size come from the vDTU config_recv response.
 */
int vdtu_channels_init_ring(struct vdtu_channel_table *ct, int channel_idx,
                            uint32_t slot_count, uint32_t slot_size);

/*
 * Attach to an existing ring buffer in a message channel (sender side).
 */
int vdtu_channels_attach_ring(struct vdtu_channel_table *ct, int channel_idx);

/*
 * Get the raw dataport pointer for a memory channel index.
 * Returns NULL if channel_idx is out of range.
 */
volatile void *vdtu_channels_get_mem(struct vdtu_channel_table *ct,
                                     int channel_idx);

#endif /* VDTU_CHANNELS_H */
