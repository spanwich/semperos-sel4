/* FPT-179: VirtIO PCI network driver for DTUBridge.
 *
 * Phase 1 (this commit): scaffold + stubs. The driver is selected at build
 * time via -DSEMPER_USE_VIRTIO_NET, but until Phase 2 lands the stubs
 * always return failure so that attempting to use the flag today flags the
 * configuration as incomplete rather than silently running with no NIC.
 *
 * Reference: ARM VirtIO MMIO driver at
 * vm-examples/apps/Arm/ics_oneway_norm_v3_dual_nic/components/VirtIO_Net0_Driver/virtio_net0_driver.c
 * (used for the virtqueue + lwIP integration; transport layer re-targeted
 * for x86_64 PCI.)
 */

#include "virtio_pci_net.h"

#ifdef SEMPER_USE_VIRTIO_NET

#include <stdio.h>
#include <string.h>

#define VNET_LOG(fmt, ...) \
    printf("[VirtIO-PCI] " fmt, ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * Driver state (file-scope). One instance per DTUBridge component. The
 * MMIO reference driver uses the same pattern; see virtio_net0_driver.c.
 * ----------------------------------------------------------------------- */

static volatile struct virtio_pci_common_cfg *g_common_cfg = NULL;
static volatile uint32_t                     *g_isr_cfg    = NULL;
static volatile uint8_t                      *g_notify_base = NULL;
static uint32_t                               g_notify_off_multiplier = 0;
static volatile uint8_t                      *g_device_cfg = NULL;

static struct virtq g_rx_vq;
static struct virtq g_tx_vq;

static uint32_t g_irq_count = 0;
static uint32_t g_rx_pkts   = 0;
static uint32_t g_tx_pkts   = 0;
static uint8_t  g_mac[6]    = {0};

static struct netif *g_netif = NULL;

/* -----------------------------------------------------------------------
 * Public entry points — Phase 1 stubs. Real implementations land in
 * Phase 2 (device discovery) and Phase 3 (virtqueue I/O).
 * ----------------------------------------------------------------------- */

int virtio_net_init(struct netif *netif, uint8_t mac_out[6])
{
    (void)g_common_cfg;
    (void)g_isr_cfg;
    (void)g_notify_base;
    (void)g_notify_off_multiplier;
    (void)g_device_cfg;
    (void)g_rx_vq;
    (void)g_tx_vq;
    g_netif = netif;

    VNET_LOG("init: SCAFFOLD ONLY — Phase 2 (PCI discovery + VirtIO init) "
             "not yet implemented. Configure the build without "
             "SEMPER_USE_VIRTIO_NET or wait for the next commit.\n");

    /* Return a synthesized MAC so callers that only read it during boot
     * don't crash dereferencing. When Phase 2 lands, this will come from
     * VIRTIO_NET_F_MAC config space. */
    g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
    g_mac[3] = 0xff; g_mac[4] = 0xff; g_mac[5] = 0xff;
    memcpy(mac_out, g_mac, 6);

    return -1;  /* fail closed until real init lands */
}

void virtio_net_irq_handle(void)
{
    g_irq_count++;
    /* Phase 3: read ISR, drain used ring, pbuf_alloc + netif->input. */
}

uint32_t virtio_net_get_irq_count(void) { return g_irq_count; }
uint32_t virtio_net_get_rx_pkts(void)   { return g_rx_pkts; }
uint32_t virtio_net_get_tx_pkts(void)   { return g_tx_pkts; }

#endif /* SEMPER_USE_VIRTIO_NET */
