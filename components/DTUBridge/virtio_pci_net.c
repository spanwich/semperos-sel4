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
#include "pci_helpers.h"

#define VNET_LOG(fmt, ...) \
    printf("[VirtIO-PCI] " fmt, ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * Discovered device location (filled in by virtio_pci_scan).
 * ----------------------------------------------------------------------- */
static uint8_t  g_pci_bus = 0;
static uint8_t  g_pci_dev = 0;
static uint8_t  g_pci_fun = 0;
static uint16_t g_pci_device_id = 0;  /* 0x1041 modern, 0x1000 legacy */

/* Capability locations we care about. Each is (bar index, offset, length). */
struct virtio_cap_info {
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
    int      present;
};
static struct virtio_cap_info g_cap_common  = {0};
static struct virtio_cap_info g_cap_notify  = {0};
static struct virtio_cap_info g_cap_isr     = {0};
static struct virtio_cap_info g_cap_device  = {0};
static uint32_t                g_notify_off_mult = 0;

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
 * Phase 2a: PCI bus scan + capability-list walk.
 *
 * Scans bus 0 for a VirtIO net device (vendor 0x1af4, device 0x1041 modern
 * or 0x1000 legacy). If found, walks the capability list looking for the
 * VirtIO-specific vendor (0x09) capabilities and records their BAR / offset
 * / length for the next phase. Prints everything it finds so the operator
 * can verify the device is present and the BAR addresses match what the
 * CAmkES ADL will need to map.
 * ----------------------------------------------------------------------- */

static const char *cfg_type_name(uint8_t type)
{
    switch (type) {
    case VIRTIO_PCI_CAP_COMMON_CFG: return "COMMON_CFG";
    case VIRTIO_PCI_CAP_NOTIFY_CFG: return "NOTIFY_CFG";
    case VIRTIO_PCI_CAP_ISR_CFG:    return "ISR_CFG";
    case VIRTIO_PCI_CAP_DEVICE_CFG: return "DEVICE_CFG";
    case VIRTIO_PCI_CAP_PCI_CFG:    return "PCI_CFG";
    default:                        return "UNKNOWN";
    }
}

static int virtio_pci_scan(void)
{
    VNET_LOG("Scanning PCI bus 0 for VirtIO net (1af4:1041 modern / 1af4:1000 legacy)...\n");

    int found = 0;
    for (int slot = 0; slot < 32 && !found; slot++) {
        uint16_t vendor = pci_cfg_read16(0, (uint8_t)slot, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;
        uint16_t device = pci_cfg_read16(0, (uint8_t)slot, 0, PCI_DEVICE_ID);
        VNET_LOG("  PCI 0:%d.0 vendor=0x%04x device=0x%04x\n",
                 slot, vendor, device);
        if (vendor == VIRTIO_PCI_VENDOR &&
            (device == VIRTIO_PCI_DEVICE_NET_MODERN ||
             device == VIRTIO_PCI_DEVICE_NET_LEGACY)) {
            g_pci_bus = 0;
            g_pci_dev = (uint8_t)slot;
            g_pci_fun = 0;
            g_pci_device_id = device;
            found = 1;
        }
    }
    if (!found) {
        VNET_LOG("ERROR: no VirtIO net device found on PCI bus 0\n");
        return -1;
    }
    VNET_LOG("Found VirtIO net at PCI 0:%d.0, device-id=0x%04x (%s)\n",
             g_pci_dev, g_pci_device_id,
             g_pci_device_id == VIRTIO_PCI_DEVICE_NET_MODERN ? "modern" : "legacy");

    /* Legacy (device 0x1000) uses IO port BAR — we don't handle it yet.
     * Modern (device 0x1041) exposes VirtIO caps in the capability list. */
    if (g_pci_device_id == VIRTIO_PCI_DEVICE_NET_LEGACY) {
        VNET_LOG("WARNING: legacy VirtIO PCI not yet supported — build requires modern.\n");
        return -1;
    }

    /* Walk the capability list. PCI capability list head is at PCI_CAP_PTR
     * when PCI_STATUS.CAP_LIST is set. Each cap has: [0] cap-id [1] next. */
    uint16_t status = pci_cfg_read16(g_pci_bus, g_pci_dev, g_pci_fun, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        VNET_LOG("ERROR: device reports no capability list\n");
        return -1;
    }
    uint8_t cap_off = pci_cfg_read8(g_pci_bus, g_pci_dev, g_pci_fun, PCI_CAP_PTR) & 0xFC;

    VNET_LOG("Walking capability list starting at 0x%02x:\n", cap_off);
    int safety = 48;  /* bounded walk — at most 48 caps on a PCIe device */
    while (cap_off != 0 && safety-- > 0) {
        uint8_t cap_id   = pci_cfg_read8(g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 0);
        uint8_t cap_next = pci_cfg_read8(g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 1) & 0xFC;
        if (cap_id == PCI_CAP_ID_VENDOR) {
            uint8_t cap_len  = pci_cfg_read8 (g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 2);
            uint8_t cfg_type = pci_cfg_read8 (g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 3);
            uint8_t bar      = pci_cfg_read8 (g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 4);
            uint32_t offset  = pci_cfg_read32(g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 8);
            uint32_t length  = pci_cfg_read32(g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 12);

            VNET_LOG("  @0x%02x vendor-cap len=%u type=%u (%s) bar=%u offset=0x%08x length=0x%08x\n",
                     cap_off, cap_len, cfg_type, cfg_type_name(cfg_type),
                     bar, offset, length);

            struct virtio_cap_info *slot = NULL;
            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG: slot = &g_cap_common; break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG: slot = &g_cap_notify; break;
            case VIRTIO_PCI_CAP_ISR_CFG:    slot = &g_cap_isr;    break;
            case VIRTIO_PCI_CAP_DEVICE_CFG: slot = &g_cap_device; break;
            }
            if (slot) {
                slot->bar = bar;
                slot->offset = offset;
                slot->length = length;
                slot->present = 1;
            }
            if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && cap_len >= 20) {
                g_notify_off_mult = pci_cfg_read32(g_pci_bus, g_pci_dev, g_pci_fun, cap_off + 16);
                VNET_LOG("    notify_off_multiplier=%u\n", g_notify_off_mult);
            }
        } else {
            VNET_LOG("  @0x%02x cap_id=0x%02x (skip)\n", cap_off, cap_id);
        }
        cap_off = cap_next;
    }

    /* Report BAR paddrs so the operator can check the CAmkES ADL mapping. */
    for (int b = 0; b < 6; b++) {
        uint32_t bar = pci_cfg_read32(g_pci_bus, g_pci_dev, g_pci_fun, PCI_BAR(b));
        if (bar == 0 || bar == 0xFFFFFFFF) continue;
        if (bar & 1) {
            VNET_LOG("  BAR%d = 0x%08x (IO, skipped)\n", b, bar);
        } else {
            VNET_LOG("  BAR%d = 0x%08x (MMIO)\n", b, bar & ~0xF);
        }
    }

    if (!g_cap_common.present || !g_cap_notify.present ||
        !g_cap_isr.present    || !g_cap_device.present) {
        VNET_LOG("ERROR: missing required VirtIO capabilities (common=%d notify=%d "
                 "isr=%d device=%d)\n",
                 g_cap_common.present, g_cap_notify.present,
                 g_cap_isr.present,    g_cap_device.present);
        return -1;
    }
    VNET_LOG("All four required VirtIO PCI caps located.\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Public entry points.
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

    int rc = virtio_pci_scan();
    if (rc != 0) {
        VNET_LOG("init failed at PCI scan (rc=%d).\n", rc);
        return rc;
    }

    VNET_LOG("init: Phase 2a complete (device located + caps parsed). "
             "Phase 2b (BAR mapping + VirtIO init sequence) not yet wired; "
             "driver will not bring up the link.\n");

    /* MAC placeholder until DEVICE_CFG read lands in Phase 2c. */
    g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
    g_mac[3] = 0xff; g_mac[4] = 0xff; g_mac[5] = 0xff;
    memcpy(mac_out, g_mac, 6);

    return -1;  /* still fail-closed — BAR mapping + init not ready */
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
