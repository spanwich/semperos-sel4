/*
 * vdtu_channels.h -- Uniform channel table for vDTU
 *
 * Virtualises the gem5 DTU's per-PE endpoint model. Each channel is a
 * 4 KiB CAmkES shared memory dataport representing one NoC point-to-point
 * link. Every PE gets VDTU_CHANNELS_PER_PE (16) uniform channels, matching
 * gem5's EP_COUNT = 16. Channel type (send/recv/mem) is determined at
 * runtime by VDTUService, not by the channel table.
 */

#ifndef VDTU_CHANNELS_H
#define VDTU_CHANNELS_H

#include "vdtu_ring.h"

/* Matches gem5 EP_COUNT = 16 (gem5/Config.h:31) */
#define VDTU_CHANNELS_PER_PE  16

/* Total channels = per-PE channels × number of VPE links from kernel.
 * Kernel has links to VPE0 (PE 2) and VPE1 (PE 3) = 2 links × 16 = 32.
 * VPE0 and VPE1 each have 16 channels (their link to the kernel). */
#define VDTU_TOTAL_CHANNELS   32

struct vdtu_channel_table {
    volatile void *ch[VDTU_TOTAL_CHANNELS];       /* uniform dataport pointers */
    struct vdtu_ring rings[VDTU_TOTAL_CHANNELS];  /* ring buffers (for msg/recv EPs) */
};

/*
 * Initialize the channel table with dataport pointers.
 * count = number of entries in the dataports array.
 */
int vdtu_channels_init(struct vdtu_channel_table *ct,
                       volatile void *dataports[], int count);

/*
 * Get the ring buffer for a given channel index.
 * Returns NULL if channel not initialized or index out of range.
 */
struct vdtu_ring *vdtu_channels_get_ring(struct vdtu_channel_table *ct,
                                         int channel_idx);

/*
 * Initialize a ring buffer in a channel (receiver side).
 */
int vdtu_channels_init_ring(struct vdtu_channel_table *ct, int channel_idx,
                            uint32_t slot_count, uint32_t slot_size);

/*
 * Attach to an existing ring buffer in a channel (sender side).
 */
int vdtu_channels_attach_ring(struct vdtu_channel_table *ct, int channel_idx);

/*
 * Get the raw dataport pointer for a channel (used for memory EPs).
 * Returns NULL if channel_idx is out of range.
 */
volatile void *vdtu_channels_get_mem(struct vdtu_channel_table *ct,
                                     int channel_idx);

#endif /* VDTU_CHANNELS_H */
