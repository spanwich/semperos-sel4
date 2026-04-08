/*
 * vdtu_channels.c -- Uniform channel table implementation
 */

#include "vdtu_channels.h"
#include <string.h>
#include <stdio.h>

int vdtu_channels_init(struct vdtu_channel_table *ct,
                       volatile void *dataports[], int count)
{
    if (!ct)
        return -1;

    memset(ct, 0, sizeof(*ct));

    int n = (count > VDTU_TOTAL_CHANNELS) ? VDTU_TOTAL_CHANNELS : count;
    for (int i = 0; i < n; i++) {
        ct->ch[i] = dataports ? dataports[i] : NULL;
    }

    return 0;
}

struct vdtu_ring *vdtu_channels_get_ring(struct vdtu_channel_table *ct,
                                         int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_TOTAL_CHANNELS)
        return NULL;

    if (!ct->rings[channel_idx].ctrl)
        return NULL;

    return &ct->rings[channel_idx];
}

int vdtu_channels_init_ring(struct vdtu_channel_table *ct, int channel_idx,
                            uint32_t slot_count, uint32_t slot_size)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_TOTAL_CHANNELS)
        return -1;

    void *mem = (void *)ct->ch[channel_idx];
    if (!mem)
        return -1;

    return vdtu_ring_init(&ct->rings[channel_idx], mem,
                          slot_count, slot_size);
}

int vdtu_channels_attach_ring(struct vdtu_channel_table *ct, int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_TOTAL_CHANNELS)
        return -1;

    void *mem = (void *)ct->ch[channel_idx];
    if (!mem)
        return -1;

    return vdtu_ring_attach(&ct->rings[channel_idx], mem);
}

volatile void *vdtu_channels_get_mem(struct vdtu_channel_table *ct,
                                     int channel_idx)
{
    if (!ct || channel_idx < 0 || channel_idx >= VDTU_TOTAL_CHANNELS)
        return NULL;

    return ct->ch[channel_idx];
}
