/*
 * VPE1.c -- Service VPE for SemperOS spanning exchange (FPT-176)
 *
 * VPE1 handles OPEN/OBTAIN/DELEGATE service requests forwarded by the
 * kernel. The kernel registers VPE1's service at boot (kernel.cc).
 *
 * Reply format must match GateIStream serialization (8-byte aligned):
 *   OPEN reply:    [uint64_t NO_ERROR] [uint64_t session_ident]
 *   OBTAIN reply:  [uint64_t NO_ERROR] [uint32_t type, uint32_t start, uint32_t count, uint32_t pad]
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

/* VPE1 is PE 3 in the platform config (local index) */
#define MY_PE       3
#define MY_VPE_ID   1

/* Service protocol commands (must match m3::KIF::Service in KIF.h:87-95) */
#define SRV_CMD_OPEN     0
#define SRV_CMD_OBTAIN   1
#define SRV_CMD_DELEGATE 2
#define SRV_CMD_CLOSE    3

/* Endpoint where service messages arrive (must match kernel.cc SRV_EP) */
#define SRV_EP  2

/* CapRngDesc type for object capabilities */
#define CAP_TYPE_OBJ 0

static struct vdtu_channel_table channels;
static int srv_ring_ch = -1;  /* channel index for service recv EP */

static void init_channels(void)
{
    volatile void *msg[] = {
        (volatile void *)msgchan_v1_0,
        (volatile void *)msgchan_v1_1,
        (volatile void *)msgchan_v1_2,
        (volatile void *)msgchan_v1_3,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
    };
    volatile void *mem[] = {
        (volatile void *)memep_v1_0,
        (volatile void *)memep_v1_1,
        NULL, NULL,
        NULL, NULL,
    };
    vdtu_channels_init(&channels, msg, mem);
}

/*
 * Send a reply on the same ring the request came from.
 * Uses the replylabel from the original message header for callback routing.
 */
static int send_reply(struct vdtu_ring *ring, const struct vdtu_message *orig,
                      const void *data, uint16_t len)
{
    return vdtu_ring_send(ring,
                          MY_PE, SRV_EP, MY_VPE_ID,
                          orig->hdr.reply_ep_id,
                          orig->hdr.replylabel, 0,
                          VDTU_FLAG_REPLY,
                          data, len);
}

/*
 * Poll service recv channels for incoming requests.
 * The kernel's SendGate::send() delivers messages to VPE1's SRV_EP.
 * We need to find which channel the vDTU assigned to this EP.
 */
static void service_poll(void)
{
    /* Try all channels — we don't know which one vDTU assigned to SRV_EP */
    for (int ch = 0; ch < 4; ch++) {
        if (!channels.msg[ch]) continue;

        /* Lazily attach to rings that the kernel initialized */
        if (!channels.msg_rings[ch].ctrl)
            vdtu_channels_attach_ring(&channels, ch);

        struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, ch);
        if (!ring) continue;

        const struct vdtu_message *msg = vdtu_ring_fetch(ring);
        if (!msg) continue;

        /* Parse service command — first field is uint64_t (8-byte aligned) */
        if (msg->hdr.length < sizeof(uint64_t)) {
            printf("[VPE1] Short message (%u bytes), skipping\n", msg->hdr.length);
            vdtu_ring_ack(ring);
            continue;
        }

        uint64_t cmd;
        memcpy(&cmd, msg->data, sizeof(cmd));

        switch (cmd) {
        case SRV_CMD_OPEN: {
            printf("[VPE1] OPEN (label=0x%lx, replylabel=0x%lx)\n",
                   (unsigned long)msg->hdr.label,
                   (unsigned long)msg->hdr.replylabel);
            /* Reply: [uint64_t NO_ERROR=0] [uint64_t session_ident=1] */
            uint64_t reply[2];
            reply[0] = 0;  /* m3::Errors::NO_ERROR */
            reply[1] = 1;  /* session ident */
            send_reply(ring, msg, reply, sizeof(reply));
            break;
        }

        case SRV_CMD_OBTAIN:
        case SRV_CMD_DELEGATE: {
            printf("[VPE1] %s (label=0x%lx)\n",
                   cmd == SRV_CMD_OBTAIN ? "OBTAIN" : "DELEGATE",
                   (unsigned long)msg->hdr.label);
            /* Reply: [uint64_t NO_ERROR] [CapRngDesc: type(4) start(4) count(4) pad(4)] */
            struct __attribute__((packed)) {
                uint64_t error;
                uint32_t type;
                uint32_t start;
                uint32_t count;
                uint32_t pad;
            } reply;
            reply.error = 0;             /* m3::Errors::NO_ERROR */
            reply.type  = CAP_TYPE_OBJ;  /* object capabilities */
            reply.start = 10;            /* starting cap selector */
            reply.count = 1;             /* one capability */
            reply.pad   = 0;
            send_reply(ring, msg, &reply, sizeof(reply));
            break;
        }

        case SRV_CMD_CLOSE:
            printf("[VPE1] CLOSE\n");
            break;

        default:
            printf("[VPE1] Unknown cmd %lu\n", (unsigned long)cmd);
            break;
        }

        vdtu_ring_ack(ring);
    }
}

int run(void)
{
    printf("[VPE1] Starting (PE %d, VPE ID %d)\n", MY_PE, MY_VPE_ID);

    init_channels();
    printf("[VPE1] Channels initialized, polling for service requests\n");

    for (;;) {
        service_poll();
        seL4_Yield();
    }

    return 0;
}
