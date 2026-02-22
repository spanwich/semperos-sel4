/*
 * vdtu_channels.c -- Channel table implementation
 */

#include "vdtu_channels.h"
#include <string.h>
#include <stdio.h>

int vdtu_channels_init(struct vdtu_channel_table *ct,
                       volatile void *msg_dataports[],
                       volatile void *mem_dataports[])
{
    if (!ct)
        return -1;

    memset(ct, 0, sizeof(*ct));

    for (int i = 0; i < VDTU_MSG_CHANNELS; i++) {
        ct->msg[i] = msg_dataports ? msg_dataports[i] : NULL;
    }
    for (int i = 0; i < VDTU_MEM_CHANNELS; i++) {
        ct->mem[i] = mem_dataports ? mem_dataports[i] : NULL;
    }

    return 0;
}

struct vdtu_ring *vdtu_channels_get_ring(struct vdtu_channel_table *ct,
                                         int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_MSG_CHANNELS)
        return NULL;

    /* Check that the ring has been initialized or attached */
    if (!ct->msg_rings[channel_idx].ctrl)
        return NULL;

    return &ct->msg_rings[channel_idx];
}

int vdtu_channels_init_ring(struct vdtu_channel_table *ct, int channel_idx,
                            uint32_t slot_count, uint32_t slot_size)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_MSG_CHANNELS)
        return -1;

    void *mem = (void *)ct->msg[channel_idx];
    if (!mem)
        return -1;

    return vdtu_ring_init(&ct->msg_rings[channel_idx], mem,
                          slot_count, slot_size);
}

int vdtu_channels_attach_ring(struct vdtu_channel_table *ct, int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_MSG_CHANNELS)
        return -1;

    void *mem = (void *)ct->msg[channel_idx];
    if (!mem)
        return -1;

    return vdtu_ring_attach(&ct->msg_rings[channel_idx], mem);
}

volatile void *vdtu_channels_get_mem(struct vdtu_channel_table *ct,
                                     int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_MEM_CHANNELS)
        return NULL;

    return ct->mem[channel_idx];
}
