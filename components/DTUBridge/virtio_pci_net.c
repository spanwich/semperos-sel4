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
 * Phase 2b: pin the VirtIO MMIO BAR to the CAmkES-mapped paddr, enable
 * memory-space + bus-master, and set file-scope pointers into the BAR
 * at each capability's offset.
 *
 * The CAmkES dataport `eth_mmio` is pre-mapped at DTUB_MMIO_PADDR (set by
 * CMake). To use it from C code, the PCI BAR the device responds on must
 * match that paddr. For VirtIO modern the caps live in BAR4, so we force
 * BAR4 to DTUB_MMIO_PADDR. This is exactly the same trick the E1000 path
 * uses for BAR0 in e1000_pci_init().
 * ----------------------------------------------------------------------- */

static int virtio_pci_map_bars(void *mmio_base)
{
    const uint32_t target_paddr = (uint32_t)(uintptr_t)DTUB_MMIO_PADDR;
    const uint8_t  target_bar = 4;  /* modern VirtIO caps live in BAR4 on QEMU Q35 */

    /* Every required cap should reference BAR4. If the device places a cap
     * in a different BAR we'd need a multi-BAR mapping (not wired today). */
    const struct virtio_cap_info *caps[] = {
        &g_cap_common, &g_cap_notify, &g_cap_isr, &g_cap_device
    };
    for (unsigned i = 0; i < sizeof(caps)/sizeof(caps[0]); i++) {
        if (caps[i]->bar != target_bar) {
            VNET_LOG("ERROR: cap #%u is on BAR%u but we only map BAR%u\n",
                     i, caps[i]->bar, target_bar);
            return -1;
        }
    }

    /* Force the BAR to our known-mapped paddr. On QEMU the BAR is freely
     * remappable as long as PCI_COMMAND.MEM_SPACE is cleared first. We
     * clear MEM_SPACE, rewrite the BAR, then re-enable MEM + bus-master. */
    uint16_t cmd = pci_cfg_read16(g_pci_bus, g_pci_dev, g_pci_fun, PCI_COMMAND);
    pci_cfg_write16(g_pci_bus, g_pci_dev, g_pci_fun, PCI_COMMAND,
                    (uint16_t)(cmd & ~(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER)));

    pci_cfg_write32(g_pci_bus, g_pci_dev, g_pci_fun, PCI_BAR(target_bar), target_paddr);
    /* VirtIO modern BAR4 may be 64-bit — write 0 to the upper half. */
    pci_cfg_write32(g_pci_bus, g_pci_dev, g_pci_fun, PCI_BAR(target_bar + 1), 0);

    pci_cfg_write16(g_pci_bus, g_pci_dev, g_pci_fun, PCI_COMMAND,
                    (uint16_t)(cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER));

    uint32_t readback = pci_cfg_read32(g_pci_bus, g_pci_dev, g_pci_fun, PCI_BAR(target_bar));
    VNET_LOG("BAR%u pinned to 0x%08x (readback 0x%08x), cmd=0x%04x\n",
             target_bar, target_paddr, readback & ~0xF,
             pci_cfg_read16(g_pci_bus, g_pci_dev, g_pci_fun, PCI_COMMAND));

    /* Set pointers into the mapped region. */
    uint8_t *base = (uint8_t *)mmio_base;
    g_common_cfg = (volatile struct virtio_pci_common_cfg *)(base + g_cap_common.offset);
    g_isr_cfg    = (volatile uint32_t *)(base + g_cap_isr.offset);
    g_notify_base = base + g_cap_notify.offset;
    g_device_cfg = (volatile uint8_t *)(base + g_cap_device.offset);
    g_notify_off_multiplier = g_notify_off_mult;

    VNET_LOG("cfg pointers: common=%p isr=%p notify_base=%p device=%p\n",
             (void *)g_common_cfg, (void *)g_isr_cfg,
             (void *)g_notify_base, (void *)g_device_cfg);
    return 0;
}

/* -----------------------------------------------------------------------
 * Public entry points.
 * ----------------------------------------------------------------------- */

int virtio_net_init(struct netif *netif, void *mmio_base, uint8_t mac_out[6])
{
    (void)g_rx_vq;
    (void)g_tx_vq;
    g_netif = netif;

    int rc = virtio_pci_scan();
    if (rc != 0) {
        VNET_LOG("init failed at PCI scan (rc=%d).\n", rc);
        return rc;
    }

    rc = virtio_pci_map_bars(mmio_base);
    if (rc != 0) {
        VNET_LOG("init failed at BAR mapping (rc=%d).\n", rc);
        return rc;
    }

    /* Read device MAC from the DEVICE_CFG cap. On VirtIO net, the first 6
     * bytes of device config space are the MAC when VIRTIO_NET_F_MAC is
     * negotiated (it's always offered by QEMU/Xen). This peek happens
     * before feature negotiation, which is fine for a ROM read. */
    for (int i = 0; i < 6; i++) {
        g_mac[i] = g_device_cfg[i];
    }
    VNET_LOG("device MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    memcpy(mac_out, g_mac, 6);

    VNET_LOG("init: Phase 2b complete (BARs pinned, cfg pointers set, MAC read). "
             "Phase 2c (VirtIO init handshake) + Phase 3 (virtqueue setup) "
             "not yet wired; driver will not bring up the link.\n");

    return -1;  /* still fail-closed — init handshake + queues not ready */
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
