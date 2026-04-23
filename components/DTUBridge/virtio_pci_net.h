/* FPT-179: VirtIO PCI network driver for DTUBridge (replacement for E1000).
 *
 * Ported from the existing ARM VirtIO MMIO net driver at
 * vm-examples/apps/Arm/ics_oneway_norm_v3_dual_nic/. The virtqueue layout,
 * descriptor/available/used rings, and lwIP integration are identical
 * between MMIO and PCI; only the transport (device discovery, register
 * access, memory barriers, IRQ delivery) is re-targeted for x86_64 PCI.
 *
 * This driver is selected at build time via -DSEMPER_USE_VIRTIO_NET; when
 * set, DTUBridge.c's E1000-specific code paths are skipped and the hooks
 * below provide an equivalent contract.
 */
#ifndef DTUBRIDGE_VIRTIO_PCI_NET_H
#define DTUBRIDGE_VIRTIO_PCI_NET_H

#ifdef SEMPER_USE_VIRTIO_NET

#include <stdint.h>
#include <stddef.h>
#include <lwip/netif.h>
#include <platsupport/io.h>

/* ==== VirtIO PCI constants ================================================ */

#define VIRTIO_PCI_VENDOR                   0x1af4
#define VIRTIO_PCI_DEVICE_NET_MODERN        0x1041  /* transitional / modern */
#define VIRTIO_PCI_DEVICE_NET_LEGACY        0x1000  /* legacy (pre-1.0) */

/* VirtIO PCI capability types (PCI cfg_type field) */
#define VIRTIO_PCI_CAP_COMMON_CFG           1
#define VIRTIO_PCI_CAP_NOTIFY_CFG            2
#define VIRTIO_PCI_CAP_ISR_CFG               3
#define VIRTIO_PCI_CAP_DEVICE_CFG            4
#define VIRTIO_PCI_CAP_PCI_CFG               5

/* VirtIO device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE           0x01
#define VIRTIO_STATUS_DRIVER                0x02
#define VIRTIO_STATUS_DRIVER_OK             0x04
#define VIRTIO_STATUS_FEATURES_OK           0x08
#define VIRTIO_STATUS_FAILED                0x80

/* VirtIO common feature bits */
#define VIRTIO_F_VERSION_1                  (1ULL << 32)

/* VirtIO net feature bits */
#define VIRTIO_NET_F_MAC                    (1ULL << 5)
#define VIRTIO_NET_F_STATUS                 (1ULL << 16)

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT                   1
#define VIRTQ_DESC_F_WRITE                  2
#define VIRTQ_DESC_F_INDIRECT               4

/* Queue indices */
#define VIRTIO_NET_RXQ                      0
#define VIRTIO_NET_TXQ                      1

/* Packet / ring sizing. 64 descriptors per queue is the same as the
 * E1000 RX ring we replace; keeps DMA footprint to ~256 KB total. */
#define VIRTIO_NET_HDR_SIZE                 12     /* modern (v1) header */
#define VIRTIO_NET_QSIZE                    64     /* desc per queue */
#define VIRTIO_NET_BUFSIZE                  2048   /* per-packet DMA buffer */
#define VIRTIO_NET_RING_ALIGN               4096   /* page-align ring allocs */

/* ==== VirtIO PCI capability header (pci config space) ==================== */

struct virtio_pci_cap {
    uint8_t  cap_vndr;      /* PCI capability ID (0x09 vendor-specific) */
    uint8_t  cap_next;      /* offset to next capability */
    uint8_t  cap_len;        /* length of this capability */
    uint8_t  cfg_type;       /* VIRTIO_PCI_CAP_* */
    uint8_t  bar;            /* which BAR to use */
    uint8_t  padding[3];
    uint32_t offset;         /* offset within BAR */
    uint32_t length;         /* length of structure */
} __attribute__((packed));

/* ==== VirtIO PCI Common Config structure (4.1.4.3 in spec) ================ */

struct virtio_pci_common_cfg {
    /* About the whole device. */
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;

    /* About a specific virtqueue. */
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;      /* avail */
    uint64_t queue_device;       /* used */
} __attribute__((packed));

/* ==== Virtqueue structs (same as MMIO driver) ============================ */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtq {
    unsigned int num;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uintptr_t desc_paddr;
    uintptr_t avail_paddr;
    uintptr_t used_paddr;
    uint16_t last_used_idx;
    /* Notify register for this queue (BAR address inside notify_cfg region). */
    volatile uint16_t *notify_reg;
};

/* ==== virtio_net_hdr (modern, 12 bytes) =================================== */

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

/* ==== Public API — matches the E1000 contract expected by DTUBridge.c ==== */

/* Bring up the VirtIO PCI NIC (PCI scan, cap parse, reset + init, queue
 * setup, register lwIP netif). Returns 0 on success, non-zero on failure.
 * mmio_base points at the CAmkES-mapped dataport for the VirtIO BAR; the
 * driver forces the device's BAR to match this paddr. dma_manager is used
 * for virtqueue + packet buffer DMA allocations. The netif is configured
 * and attached via netif_add() using the device's MAC from DEVICE_CFG.
 * mac_out[6] receives the MAC for external use. */
int  virtio_net_init(struct netif *netif, void *mmio_base,
                     ps_dma_man_t *dma_manager, uint8_t mac_out[6]);

/* lwIP netif init callback. Pass to netif_add() as the init func along
 * with the IP/netmask/gateway. Only valid after virtio_net_init()
 * succeeded. */
err_t virtio_netif_init(struct netif *netif);

/* Called from DTUBridge's eth_irq_handle when SEMPER_USE_VIRTIO_NET is
 * set. Reads the ISR (clears it on read), drains RX used ring into
 * lwIP via netif->input. Safe to call from the CAmkES IRQ handler
 * thread. */
void virtio_net_irq_handle(void);

/* Called from the periodic status print to export counters for the
 * existing "RX-path"/"rx-drain" diagnostic lines. */
uint32_t virtio_net_get_irq_count(void);
uint32_t virtio_net_get_rx_pkts(void);
uint32_t virtio_net_get_tx_pkts(void);

#endif /* SEMPER_USE_VIRTIO_NET */
#endif /* DTUBRIDGE_VIRTIO_PCI_NET_H */
