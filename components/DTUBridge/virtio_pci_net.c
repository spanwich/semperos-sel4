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
#include <platsupport/io.h>
#include <lwip/pbuf.h>
#include <lwip/etharp.h>
#include <netif/ethernet.h>
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
static ps_dma_man_t g_dma;

/* Per-descriptor packet buffers (virtual + physical address arrays). */
static void     *g_rx_buf_vaddr[VIRTIO_NET_QSIZE];
static uintptr_t g_rx_buf_paddr[VIRTIO_NET_QSIZE];
static void     *g_tx_buf_vaddr[VIRTIO_NET_QSIZE];
static uintptr_t g_tx_buf_paddr[VIRTIO_NET_QSIZE];
static uint16_t  g_tx_next = 0;  /* next TX descriptor index */

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
 * Phase 2c: VirtIO driver init handshake (spec 1.2 §3.1.1).
 *
 *   RESET → poll status == 0
 *   ACKNOWLEDGE
 *   DRIVER
 *   negotiate features (read device_feature low+high, write driver_feature)
 *   FEATURES_OK
 *   verify FEATURES_OK still set (else device rejected our feature set)
 *
 * Leaves the driver in DRIVER+FEATURES_OK state, ready for queue setup.
 * Sets DRIVER_OK only after queues are configured (Phase 3). The features
 * we request here — VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC — are the
 * minimum needed for the modern transport with a fixed MAC; offload
 * features (CSUM, TSO, MRG_RX_BUF) are deliberately not requested so the
 * device presents untagged 1500-byte frames without segmentation, which
 * matches what our lwIP + HACL* CryptoTransport code expects.
 *
 * x86 memory barriers: we use __sync_synchronize() (= mfence) before and
 * after every MMIO access that needs ordering. Most writes to the
 * common_cfg region are bytewise/16-bit/32-bit stores to memory that
 * qemu-dm intercepts; the MMIO write itself is serialising enough on x86,
 * but we leave the fences in for explicitness.
 * ----------------------------------------------------------------------- */

static int virtio_pci_handshake(void)
{
    if (!g_common_cfg) {
        VNET_LOG("handshake: common_cfg not mapped\n");
        return -1;
    }

    /* 1. RESET: write 0 to device_status, then wait for it to read back 0. */
    g_common_cfg->device_status = 0;
    __sync_synchronize();
    int spin = 1000000;
    while (g_common_cfg->device_status != 0 && spin-- > 0) {
        __asm__ volatile("pause");
    }
    if (g_common_cfg->device_status != 0) {
        VNET_LOG("handshake: device did not accept RESET (status=0x%02x)\n",
                 g_common_cfg->device_status);
        return -1;
    }
    VNET_LOG("handshake: RESET acknowledged (status=0)\n");

    /* 2. ACKNOWLEDGE */
    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    __sync_synchronize();

    /* 3. DRIVER */
    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __sync_synchronize();

    /* 4. Feature negotiation — read device features (64 bit, split 32-bit). */
    g_common_cfg->device_feature_select = 0;
    __sync_synchronize();
    uint64_t dev_features = g_common_cfg->device_feature;
    g_common_cfg->device_feature_select = 1;
    __sync_synchronize();
    dev_features |= ((uint64_t)g_common_cfg->device_feature) << 32;
    VNET_LOG("handshake: device features = 0x%08x%08x\n",
             (uint32_t)(dev_features >> 32), (uint32_t)dev_features);

    /* Pick the minimal set that makes the device usable for us. VERSION_1
     * is mandatory for modern. NET_F_MAC lets us read the MAC we already
     * peeked above (pure sanity). Do NOT ask for MRG_RXBUF, CSUM, or TSO
     * — we want the device to hand us a single-descriptor frame per
     * packet so the RX path stays simple and matches the MMIO reference. */
    uint64_t wanted = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC;
    uint64_t missing = (wanted & ~dev_features);
    if (missing) {
        VNET_LOG("handshake: device lacks required features 0x%08x%08x\n",
                 (uint32_t)(missing >> 32), (uint32_t)missing);
        g_common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    uint64_t driver_features = wanted;

    g_common_cfg->driver_feature_select = 0;
    __sync_synchronize();
    g_common_cfg->driver_feature = (uint32_t)driver_features;
    __sync_synchronize();
    g_common_cfg->driver_feature_select = 1;
    __sync_synchronize();
    g_common_cfg->driver_feature = (uint32_t)(driver_features >> 32);
    __sync_synchronize();
    VNET_LOG("handshake: driver features = 0x%08x%08x\n",
             (uint32_t)(driver_features >> 32), (uint32_t)driver_features);

    /* 5. FEATURES_OK */
    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                                  VIRTIO_STATUS_DRIVER |
                                  VIRTIO_STATUS_FEATURES_OK;
    __sync_synchronize();

    /* 6. Re-read status to verify FEATURES_OK is still set. If the device
     * cleared it, it means the features we requested aren't supported as
     * a set; we must fail. */
    if (!(g_common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        VNET_LOG("handshake: device rejected FEATURES_OK (status=0x%02x)\n",
                 g_common_cfg->device_status);
        g_common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    VNET_LOG("handshake: FEATURES_OK confirmed (status=0x%02x)\n",
             g_common_cfg->device_status);

    /* Report the queue shape so Phase 3 knows how big to make the rings. */
    g_common_cfg->queue_select = VIRTIO_NET_RXQ;
    __sync_synchronize();
    uint16_t rx_qmax = g_common_cfg->queue_size;
    g_common_cfg->queue_select = VIRTIO_NET_TXQ;
    __sync_synchronize();
    uint16_t tx_qmax = g_common_cfg->queue_size;
    uint16_t num_queues = g_common_cfg->num_queues;
    VNET_LOG("handshake: num_queues=%u rx_queue_size=%u tx_queue_size=%u\n",
             num_queues, rx_qmax, tx_qmax);

    return 0;
}

/* -----------------------------------------------------------------------
 * Phase 3: virtqueue allocation + RX/TX paths.
 *
 * Direct port of the ARM VirtIO MMIO reference driver at
 * vm-examples/apps/Arm/ics_oneway_norm_v3_dual_nic/components/VirtIO_Net0_Driver/virtio_net0_driver.c.
 * Virtqueue data structures and ring management are identical; only the
 * transport-layer register accesses change from MMIO register offsets to
 * writes into the VirtIO PCI common_cfg struct, and ARM `dmb sy` barriers
 * become x86 `mfence` (__sync_synchronize).
 * ----------------------------------------------------------------------- */

static int alloc_queue(struct virtq *vq, uint16_t qsize)
{
    size_t desc_sz  = sizeof(struct virtq_desc) * qsize;
    size_t avail_sz = sizeof(struct virtq_avail) + sizeof(uint16_t) * qsize +
                      sizeof(uint16_t); /* trailing used_event */
    size_t used_sz  = sizeof(struct virtq_used) +
                      sizeof(struct virtq_used_elem) * qsize +
                      sizeof(uint16_t); /* trailing avail_event */

    vq->num = qsize;
    vq->last_used_idx = 0;

    vq->desc = ps_dma_alloc(&g_dma, desc_sz, VIRTIO_NET_RING_ALIGN, 0, PS_MEM_NORMAL);
    if (!vq->desc) { VNET_LOG("desc dma alloc failed\n"); return -1; }
    vq->desc_paddr = ps_dma_pin(&g_dma, vq->desc, desc_sz);
    memset(vq->desc, 0, desc_sz);

    vq->avail = ps_dma_alloc(&g_dma, avail_sz, VIRTIO_NET_RING_ALIGN, 0, PS_MEM_NORMAL);
    if (!vq->avail) { VNET_LOG("avail dma alloc failed\n"); return -1; }
    vq->avail_paddr = ps_dma_pin(&g_dma, vq->avail, avail_sz);
    memset(vq->avail, 0, avail_sz);

    vq->used = ps_dma_alloc(&g_dma, used_sz, VIRTIO_NET_RING_ALIGN, 0, PS_MEM_NORMAL);
    if (!vq->used) { VNET_LOG("used dma alloc failed\n"); return -1; }
    vq->used_paddr = ps_dma_pin(&g_dma, vq->used, used_sz);
    memset(vq->used, 0, used_sz);

    return 0;
}

/* Program the device with the queue paddrs + size and enable it. Returns
 * the location of the notify register for this queue. */
static int configure_queue(uint16_t qidx, struct virtq *vq)
{
    g_common_cfg->queue_select = qidx;
    __sync_synchronize();
    uint16_t qmax = g_common_cfg->queue_size;
    if (qmax < VIRTIO_NET_QSIZE) {
        VNET_LOG("queue %u: device max=%u < requested %u\n",
                 qidx, qmax, VIRTIO_NET_QSIZE);
        return -1;
    }
    g_common_cfg->queue_size   = VIRTIO_NET_QSIZE;
    g_common_cfg->queue_desc   = (uint64_t)vq->desc_paddr;
    g_common_cfg->queue_driver = (uint64_t)vq->avail_paddr;
    g_common_cfg->queue_device = (uint64_t)vq->used_paddr;
    g_common_cfg->queue_msix_vector = 0xFFFF;  /* disable MSI-X (we use INTx) */
    __sync_synchronize();
    uint16_t noff = g_common_cfg->queue_notify_off;
    vq->notify_reg = (volatile uint16_t *)(g_notify_base +
                                           noff * g_notify_off_multiplier);
    g_common_cfg->queue_enable = 1;
    __sync_synchronize();
    VNET_LOG("queue %u: configured (desc=0x%lx avail=0x%lx used=0x%lx notify=%p)\n",
             qidx,
             (unsigned long)vq->desc_paddr,
             (unsigned long)vq->avail_paddr,
             (unsigned long)vq->used_paddr,
             (void *)vq->notify_reg);
    return 0;
}

static void kick(struct virtq *vq, uint16_t qidx)
{
    __sync_synchronize();
    *vq->notify_reg = qidx;
}

/* Populate the RX queue with empty buffers the device can fill. One
 * descriptor per buffer, sized for header + MTU. Flag WRITE so device
 * writes into it. This is the same pattern as the MMIO refill_rx_queue
 * helper. */
static void refill_rx_queue(void)
{
    struct virtq *vq = &g_rx_vq;
    for (uint16_t i = 0; i < vq->num; i++) {
        vq->desc[i].addr  = (uint64_t)g_rx_buf_paddr[i];
        vq->desc[i].len   = VIRTIO_NET_HDR_SIZE + 1500;
        vq->desc[i].flags = VIRTQ_DESC_F_WRITE;
        vq->desc[i].next  = 0;
        vq->avail->ring[vq->avail->idx % vq->num] = i;
        vq->avail->idx++;
    }
    __sync_synchronize();
    kick(vq, VIRTIO_NET_RXQ);
}

/* Drain the used ring. For each completed RX descriptor: strip the
 * VirtIO net header, pbuf_alloc, feed to lwIP via g_netif->input.
 * Recycle buffers back into the available ring. */
static void process_rx_packets(void)
{
    struct virtq *vq = &g_rx_vq;
    while (vq->last_used_idx != vq->used->idx) {
        __sync_synchronize();
        uint16_t slot = vq->last_used_idx % vq->num;
        struct virtq_used_elem *ue = &vq->used->ring[slot];
        uint16_t desc_idx = (uint16_t)ue->id;
        uint32_t total_len = ue->len;

        if (total_len <= VIRTIO_NET_HDR_SIZE) {
            goto recycle;
        }
        uint8_t *buf = (uint8_t *)g_rx_buf_vaddr[desc_idx];
        uint8_t *pkt = buf + VIRTIO_NET_HDR_SIZE;
        uint16_t plen = (uint16_t)(total_len - VIRTIO_NET_HDR_SIZE);

        struct pbuf *p = pbuf_alloc(PBUF_RAW, plen, PBUF_POOL);
        if (p) {
            if (pbuf_take(p, pkt, plen) == ERR_OK && g_netif && g_netif->input) {
                if (g_netif->input(p, g_netif) != ERR_OK) {
                    pbuf_free(p);
                }
            } else {
                pbuf_free(p);
            }
            g_rx_pkts++;
        }
recycle:
        /* Hand the buffer back to the device. */
        vq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
        vq->desc[desc_idx].len   = VIRTIO_NET_HDR_SIZE + 1500;
        vq->avail->ring[vq->avail->idx % vq->num] = desc_idx;
        __sync_synchronize();
        vq->avail->idx++;
        vq->last_used_idx++;
    }
    kick(vq, VIRTIO_NET_RXQ);
}

/* lwIP TX callback (netif->linkoutput). Copy pbuf into a pre-allocated
 * TX buffer, build a 1-descriptor chain containing [virtio_net_hdr |
 * packet], add to available ring, kick. The virtio_net_hdr sits in the
 * first VIRTIO_NET_HDR_SIZE bytes of the same buffer as the packet,
 * zeroed (no offload requested). */
static err_t virtio_netif_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    struct virtq *vq = &g_tx_vq;

    /* Find a TX descriptor that's free. We use round-robin and assume
     * slots wrap before the device falls behind; if the ring fills we
     * drop here, same as the E1000 path under pressure. */
    uint16_t desc_idx = g_tx_next;
    g_tx_next = (uint16_t)((g_tx_next + 1) % vq->num);

    uint8_t *buf = (uint8_t *)g_tx_buf_vaddr[desc_idx];
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);  /* header: no offload */
    uint16_t tot = (uint16_t)(p->tot_len);
    if (tot > VIRTIO_NET_BUFSIZE - VIRTIO_NET_HDR_SIZE) {
        return ERR_BUF;
    }
    if (pbuf_copy_partial(p, buf + VIRTIO_NET_HDR_SIZE, tot, 0) != tot) {
        return ERR_BUF;
    }

    vq->desc[desc_idx].addr  = (uint64_t)g_tx_buf_paddr[desc_idx];
    vq->desc[desc_idx].len   = (uint32_t)(VIRTIO_NET_HDR_SIZE + tot);
    vq->desc[desc_idx].flags = 0;  /* device reads — no NEXT, no WRITE */
    vq->desc[desc_idx].next  = 0;

    vq->avail->ring[vq->avail->idx % vq->num] = desc_idx;
    __sync_synchronize();
    vq->avail->idx++;
    kick(vq, VIRTIO_NET_TXQ);

    g_tx_pkts++;
    return ERR_OK;
}

/* lwIP netif init callback. Called once from netif_add(). Exposed via
 * the header so DTUBridge can pass it as the init-func argument. */
err_t virtio_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, g_mac, 6);
    netif->linkoutput = virtio_netif_output;
    netif->output = etharp_output;
    return ERR_OK;
}

/* -----------------------------------------------------------------------
 * Public entry points.
 * ----------------------------------------------------------------------- */

int virtio_net_init(struct netif *netif, void *mmio_base,
                    ps_dma_man_t *dma_manager, uint8_t mac_out[6])
{
    g_netif = netif;
    g_dma = *dma_manager;

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

    /* Peek MAC from DEVICE_CFG (first 6 bytes). VIRTIO_NET_F_MAC is
     * always offered by QEMU/Xen for the static MAC configured at
     * VM creation. */
    for (int i = 0; i < 6; i++) {
        g_mac[i] = g_device_cfg[i];
    }
    VNET_LOG("device MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    memcpy(mac_out, g_mac, 6);

    /* Phase 2c: handshake through FEATURES_OK. */
    rc = virtio_pci_handshake();
    if (rc != 0) {
        VNET_LOG("init failed at VirtIO handshake (rc=%d).\n", rc);
        return rc;
    }

    /* Phase 3a: allocate + configure RX and TX virtqueues. */
    rc = alloc_queue(&g_rx_vq, VIRTIO_NET_QSIZE);
    if (rc != 0) return rc;
    rc = alloc_queue(&g_tx_vq, VIRTIO_NET_QSIZE);
    if (rc != 0) return rc;

    rc = configure_queue(VIRTIO_NET_RXQ, &g_rx_vq);
    if (rc != 0) return rc;
    rc = configure_queue(VIRTIO_NET_TXQ, &g_tx_vq);
    if (rc != 0) return rc;

    /* Allocate per-descriptor packet buffers for each queue. */
    for (uint16_t i = 0; i < VIRTIO_NET_QSIZE; i++) {
        g_rx_buf_vaddr[i] = ps_dma_alloc(&g_dma, VIRTIO_NET_BUFSIZE, 64,
                                         0, PS_MEM_NORMAL);
        if (!g_rx_buf_vaddr[i]) { VNET_LOG("rx buf %u alloc failed\n", i); return -1; }
        g_rx_buf_paddr[i] = ps_dma_pin(&g_dma, g_rx_buf_vaddr[i],
                                       VIRTIO_NET_BUFSIZE);

        g_tx_buf_vaddr[i] = ps_dma_alloc(&g_dma, VIRTIO_NET_BUFSIZE, 64,
                                         0, PS_MEM_NORMAL);
        if (!g_tx_buf_vaddr[i]) { VNET_LOG("tx buf %u alloc failed\n", i); return -1; }
        g_tx_buf_paddr[i] = ps_dma_pin(&g_dma, g_tx_buf_vaddr[i],
                                       VIRTIO_NET_BUFSIZE);
    }

    /* DRIVER_OK — device starts processing queues. */
    g_common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                                  VIRTIO_STATUS_DRIVER |
                                  VIRTIO_STATUS_FEATURES_OK |
                                  VIRTIO_STATUS_DRIVER_OK;
    __sync_synchronize();
    VNET_LOG("DRIVER_OK set (status=0x%02x)\n", g_common_cfg->device_status);

    /* Pre-populate RX queue so the device has empty buffers to fill. */
    refill_rx_queue();

    /* Attach the lwIP netif. Caller will set the IP via netif_add's args. */
    VNET_LOG("netif attached, link UP\n");
    return 0;
}

void virtio_net_irq_handle(void)
{
    g_irq_count++;
    /* Read the ISR — this has the side-effect of clearing it on the
     * device, so we must always do it even if we polled the used ring
     * independently. */
    if (g_isr_cfg) {
        (void)*g_isr_cfg;
    }
    /* Drain RX used ring and repost buffers. process_rx_packets()
     * internally kicks the device once at the end after reposting. */
    process_rx_packets();
}

uint32_t virtio_net_get_irq_count(void) { return g_irq_count; }
uint32_t virtio_net_get_rx_pkts(void)   { return g_rx_pkts; }
uint32_t virtio_net_get_tx_pkts(void)   { return g_tx_pkts; }

#endif /* SEMPER_USE_VIRTIO_NET */
