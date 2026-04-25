/*
 * DTUBridge.c — CAmkES component: E1000 + lwIP UDP transport for inter-node DTU
 *
 * Bridges SemperOS DTU messages between the local SemperKernel and a remote
 * SemperOS node via UDP over an Intel 82540EM NIC.
 *
 * E1000 driver code adapted from http_gateway_x86/E1000Driver.c
 * (standalone 82540EM driver based on Zephyr's eth_e1000.c).
 *
 * Architecture:
 *   SemperKernel --[RPC: net_send]--> DTUBridge --[UDP]--> remote node
 *   remote node  --[UDP]--> DTUBridge --[notification]--> SemperKernel
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <camkes.h>
#include <camkes/dma.h>
#include <camkes/io.h>
#include <camkes/irq.h>
#include <sel4/sel4.h>
#include <utils/util.h>
#include <platsupport/io.h>

/* lwIP headers */
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/udp.h>
#include <lwip/timeouts.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>

#include "e1000_hw.h"
#include "vdtu_ring.h"
#include "crypto_transport.h"

#define COMPONENT_NAME "DTUBridge"

/* DTU transport UDP port */
#define DTU_UDP_PORT   7654

/* Hello exchange UDP port. Was 5000, but port-5000 packets appeared to be
 * dropped for reasons outside our code (tested across all 3 pairs; only
 * .10→.12 reached the destination callback at port 5000 in Stage 4 fix-5
 * diagnostics, even though port-7654 packets between the same IPs worked
 * fine). Switching to 7655 to test if port 5000 is filtered. */
#define HELLO_UDP_PORT 7655

/*
 * Network identity — compile-time constants from cmake.
 * DTUB_SELF_IP, DTUB_PEER_IP_0, DTUB_PEER_IP_1 are string literals
 * like "192.168.100.10", passed via -D flags.
 */
#ifndef DTUB_SELF_IP
#define DTUB_SELF_IP    "10.0.0.1"
#endif
#ifndef DTUB_PEER_IP_0
#define DTUB_PEER_IP_0  "10.0.0.2"
#endif
#ifndef DTUB_PEER_IP_1
#define DTUB_PEER_IP_1  "10.0.0.3"
#endif
#ifndef KERNEL_ID
#define KERNEL_ID 0
#endif

/* Cluster size from cmake (-DSEMPER_NUM_NODES). NUM_PEERS = nodes − self.
 * Default 3 preserves the FPT-179 Stage-3/4 multi-peer behaviour for the
 * XCP-ng path; legacy/hille-baseline overrides to 2 or 3 per docker run. */
#ifndef SEMPER_NUM_NODES
#define SEMPER_NUM_NODES 3
#endif
#define NUM_PEERS (SEMPER_NUM_NODES - 1)

static ip4_addr_t self_ip_addr;
static ip4_addr_t peer_addrs[NUM_PEERS];
static uint8_t my_node_id = 0;

/* Map peer array index to KERNEL_ID (for CryptoTransport peer_id).
 * Populated at init time: peer_kernel_id[i] is the KERNEL_ID of peer_addrs[i]. */
static uint8_t peer_kernel_id[NUM_PEERS];

/* Parse "A.B.C.D" into an ip4_addr_t. Returns 0 on success. */
static int parse_ip4(const char *s, ip4_addr_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    IP4_ADDR(out, a, b, c, d);
    return 0;
}

/* Frame MTU */
#define FRAME_MTU 1536

/*
 * ============================================================
 *  PCI Configuration (verbatim from E1000Driver.c)
 * ============================================================
 */

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_BAR0            0x10

#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004

#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_ID     0x100E  /* 82540EM */

#define E1000_BAR0_ADDR     0xfeb80000

/* Discovered PCI slot — filled in by e1000_pci_scan() */
static uint8_t e1000_pci_bus = 0;
static uint8_t e1000_pci_dev = 0;
static uint8_t e1000_pci_fun = 0;

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    pci_config_out32_offset(0, addr);
    return pci_config_in32_offset(4);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t val = pci_cfg_read32(bus, dev, func, offset & 0xFC);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val)
{
    uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    pci_config_out32_offset(0, addr);
    pci_config_out32_offset(4, val);
}

static void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val)
{
    uint32_t addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    pci_config_out32_offset(0, addr);
    uint32_t old = pci_config_in32_offset(4);
    int shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFF << shift;
    uint32_t newval = (old & ~mask) | ((uint32_t)val << shift);
    pci_config_out32_offset(4, newval);
}

static int e1000_pci_init(void)
{
    printf("[%s] Scanning PCI bus 0 for 82540EM (8086:100E)...\n", COMPONENT_NAME);

    int found = 0;
    for (int slot = 0; slot < 32 && !found; slot++) {
        uint16_t vendor = pci_cfg_read16(0, (uint8_t)slot, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;
        uint16_t device = pci_cfg_read16(0, (uint8_t)slot, 0, PCI_DEVICE_ID);
        printf("[%s] PCI 0:%d.0: vendor=0x%04x device=0x%04x\n",
               COMPONENT_NAME, slot, vendor, device);
        if (vendor == E1000_VENDOR_ID && device == E1000_DEVICE_ID) {
            printf("[%s] Found 82540EM at PCI 0:%d.0\n", COMPONENT_NAME, slot);
            e1000_pci_bus = 0;
            e1000_pci_dev = (uint8_t)slot;
            e1000_pci_fun = 0;
            found = 1;
        }
    }

    if (!found) {
        printf("[%s] ERROR: No 82540EM found on PCI bus 0\n", COMPONENT_NAME);
        return -1;
    }

    uint32_t bar0 = pci_cfg_read32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, PCI_BAR0);
    if ((bar0 & ~0xF) == 0 || bar0 == 0xFFFFFFFF) {
        pci_cfg_write32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, PCI_BAR0, E1000_BAR0_ADDR);
    }

    uint16_t cmd = pci_cfg_read16(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, PCI_COMMAND, cmd);

    printf("[%s] PCI initialized (0:%d.0)\n", COMPONENT_NAME, e1000_pci_dev);
    return 0;
}

/*
 * ============================================================
 *  E1000 Driver State and Helpers
 * ============================================================
 */

#define DMB() __asm__ volatile("mfence" ::: "memory")

struct e1000_driver {
    volatile void *mmio;
    ps_dma_man_t dma_manager;
    struct e1000_rx_desc *rx_ring;
    uintptr_t rx_ring_phys;
    void *rx_bufs[E1000_NUM_RX_DESC];
    uintptr_t rx_buf_phys[E1000_NUM_RX_DESC];
    uint32_t rx_tail;
    struct e1000_tx_desc *tx_ring;
    uintptr_t tx_ring_phys;
    void *tx_bufs[E1000_NUM_TX_DESC];
    uintptr_t tx_buf_phys[E1000_NUM_TX_DESC];
    uint32_t tx_tail;
    uint32_t tx_head;
    uint8_t mac_addr[6];
    uint32_t rx_pkts;
    uint32_t tx_pkts;
    uint32_t rx_dropped;
    uint32_t irq_count;
};

static struct e1000_driver g_drv;
static volatile bool driver_ready = false;

/* lwIP state */
static struct netif g_netif;
static struct udp_pcb *g_udp_pcb;       /* DTU transport on port 7654 */
static struct udp_pcb *g_hello_pcb;     /* Hello exchange on port 5000 */
/* Per-peer mutual HELLO state machine (FPT-179 Stage 3):
 *   HELLO_INIT:     sending HELLO, waiting for peer's HELLO
 *   HELLO_GOT_PEER: received peer's HELLO, now sending HELLO_ACK
 *   HELLO_COMPLETE: received peer's HELLO_ACK, exchange done
 * Each peer has its own state + grace counter. We must drive every
 * peer to COMPLETE independently — a global state machine only worked
 * for 2-node because the first completed pair silenced the sender
 * before the second peer could hear back. */
enum hello_state { HELLO_INIT = 0, HELLO_GOT_PEER = 1, HELLO_COMPLETE = 2 };
static volatile enum hello_state peer_hello_phase[NUM_PEERS];
static int peer_hello_grace[NUM_PEERS];  /* ACK rounds remaining per-peer */

/* FPT-179: session epoch for peer-reboot detection.
 *
 * The "zombie handshake" bug: once peer_hello_phase[peer] reaches
 * HELLO_COMPLETE, there is no backward transition. Combined with
 * CryptoTransport's monotonic rx_seq_max (never reset after boot), an
 * unchanged VM holds stale state for a peer that reboots: the peer's new
 * tx_seq=1 packets are rejected as replay for N attempts, while HELLO is
 * never re-entered. Swap test (FPT-179 comment 10500) confirmed this
 * manifests as apparent L2 isolation on the unchanged VM.
 *
 * Fix: every HELLO/HELLO_ACK carries a 32-bit epoch chosen once at
 * DTUBridge init (rdtsc-seeded, unique per boot). On receiving a HELLO
 * with an epoch different from the last-seen epoch for that peer, we
 * treat it as a peer reboot: reset HELLO phase to INIT, clear CT replay
 * window for that peer, refresh peer_kernel_id from the HELLO payload. */
static uint32_t g_my_epoch = 0;                  /* our epoch — set once at init */
static uint32_t peer_epoch[NUM_PEERS];           /* peers' last-seen epochs (0 = unknown) */
static uint32_t g_peer_reboots_detected = 0;    /* diagnostic */
/* HELLO_GRACE_COUNT is the number of HELLO_ACK rounds we send after going
 * COMPLETE. It was 10, but with 3+ nodes contending for NIC bandwidth that
 * window is too narrow — all 10 ACKs could be dropped and the peer would
 * never converge. Bumping to 100 (≈10s at the 100k-iter send cadence) plus
 * grace-refresh-on-RX covers straggler peers. */
#define HELLO_GRACE_COUNT 100

/* Network ring buffers for kernel <-> DTUBridge transport (07e) */
static struct vdtu_ring g_net_out_ring;  /* consumer: bridge reads kernel's outbound */
static struct vdtu_ring g_net_in_ring;   /* producer: bridge writes incoming network msgs */
static volatile bool net_rings_ready = false;

/* lwIP time tracking */
static volatile uint32_t lwip_time_ms = 0;

uint32_t sys_now(void)
{
    lwip_time_ms++;
    return lwip_time_ms;
}

static inline uint32_t e1000_rd(struct e1000_driver *drv, uint32_t reg)
{
    return e1000_read_reg(drv->mmio, reg);
}

static inline void e1000_wr(struct e1000_driver *drv, uint32_t reg, uint32_t val)
{
    e1000_write_reg(drv->mmio, reg, val);
}

/* FPT-179 diagnostics: dump PCI configuration values that matter for IRQ/BAR
 * routing under Xen HVM. Called once from post_init() after e1000_hw_init().
 * The pre-mapped eth_mmio paddr (0xfeb80000 in .camkes) must match BAR0 or the
 * driver talks to dead memory. The IRQ_LINE gives us the vector Xen assigned. */
static void e1000_pci_debug_dump(void)
{
    uint32_t bar0  = pci_cfg_read32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x10);
    uint32_t bar1  = pci_cfg_read32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x14);
    uint32_t bar2  = pci_cfg_read32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x18);
    uint16_t cmd   = pci_cfg_read16(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x04);
    uint16_t stat  = pci_cfg_read16(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x06);
    uint32_t intr  = pci_cfg_read32(e1000_pci_bus, e1000_pci_dev, e1000_pci_fun, 0x3C);
    uint8_t  iline = (uint8_t)(intr & 0xFF);
    uint8_t  ipin  = (uint8_t)((intr >> 8) & 0xFF);

    printf("[%s] FPT-179 PCI DUMP: BAR0=0x%08x BAR1=0x%08x BAR2=0x%08x\n",
           COMPONENT_NAME, bar0, bar1, bar2);
    printf("[%s] FPT-179 PCI DUMP: CMD=0x%04x STATUS=0x%04x IntrLine=0x%02x IntrPin=0x%02x\n",
           COMPONENT_NAME, cmd, stat, iline, ipin);
    printf("[%s] FPT-179 PCI DUMP: pre-mapped eth_mmio paddr (.camkes) = 0xfeb80000\n",
           COMPONENT_NAME);
    printf("[%s] FPT-179 PCI DUMP: configured IRQ ioapic_pin=11 vector=11 (.camkes)\n",
           COMPONENT_NAME);
}

static void e1000_reg_debug_dump(struct e1000_driver *drv, const char *tag)
{
    uint32_t ctrl   = e1000_rd(drv, E1000_CTRL);
    uint32_t status = e1000_rd(drv, E1000_STATUS);
    uint32_t ims    = e1000_rd(drv, E1000_IMS);
    uint32_t icr    = e1000_rd(drv, E1000_ICR);  /* NB: read clears pending */
    uint32_t rctl   = e1000_rd(drv, E1000_RCTL);
    uint32_t tctl   = e1000_rd(drv, E1000_TCTL);
    uint32_t rdh    = e1000_rd(drv, E1000_RDH);
    uint32_t rdt    = e1000_rd(drv, E1000_RDT);
    uint32_t rdbal  = e1000_rd(drv, E1000_RDBAL);
    uint32_t rdbah  = e1000_rd(drv, E1000_RDBAH);
    uint32_t tdh    = e1000_rd(drv, E1000_TDH);
    uint32_t tdt    = e1000_rd(drv, E1000_TDT);

    printf("[%s] FPT-179 REG[%s]: CTRL=0x%08x STATUS=0x%08x IMS=0x%08x ICR=0x%08x\n",
           COMPONENT_NAME, tag, ctrl, status, ims, icr);
    printf("[%s] FPT-179 REG[%s]: RCTL=0x%08x TCTL=0x%08x\n",
           COMPONENT_NAME, tag, rctl, tctl);
    printf("[%s] FPT-179 REG[%s]: RDH=%u RDT=%u RDBA=0x%08x%08x TDH=%u TDT=%u\n",
           COMPONENT_NAME, tag, rdh, rdt, rdbah, rdbal, tdh, tdt);
}

static void e1000_read_mac(struct e1000_driver *drv)
{
    uint32_t ral = e1000_rd(drv, E1000_RAL);
    uint32_t rah = e1000_rd(drv, E1000_RAH);
    drv->mac_addr[0] = (ral >> 0) & 0xFF;
    drv->mac_addr[1] = (ral >> 8) & 0xFF;
    drv->mac_addr[2] = (ral >> 16) & 0xFF;
    drv->mac_addr[3] = (ral >> 24) & 0xFF;
    drv->mac_addr[4] = (rah >> 0) & 0xFF;
    drv->mac_addr[5] = (rah >> 8) & 0xFF;
}

static void e1000_write_mac(struct e1000_driver *drv)
{
    uint32_t ral = drv->mac_addr[0] |
                   ((uint32_t)drv->mac_addr[1] << 8) |
                   ((uint32_t)drv->mac_addr[2] << 16) |
                   ((uint32_t)drv->mac_addr[3] << 24);
    uint32_t rah = drv->mac_addr[4] |
                   ((uint32_t)drv->mac_addr[5] << 8) |
                   E1000_RAH_AV;
    e1000_wr(drv, E1000_RAL, ral);
    e1000_wr(drv, E1000_RAH, rah);
}

static int e1000_alloc_dma(struct e1000_driver *drv, ps_dma_man_t *dma)
{
    drv->dma_manager = *dma;

    size_t rx_ring_size = E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc);
    drv->rx_ring = ps_dma_alloc(dma, rx_ring_size, E1000_DESC_ALIGN, 0, PS_MEM_NORMAL);
    if (!drv->rx_ring) return -1;
    memset(drv->rx_ring, 0, rx_ring_size);
    drv->rx_ring_phys = ps_dma_pin(dma, drv->rx_ring, rx_ring_size);

    size_t tx_ring_size = E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc);
    drv->tx_ring = ps_dma_alloc(dma, tx_ring_size, E1000_DESC_ALIGN, 0, PS_MEM_NORMAL);
    if (!drv->tx_ring) return -1;
    memset(drv->tx_ring, 0, tx_ring_size);
    drv->tx_ring_phys = ps_dma_pin(dma, drv->tx_ring, tx_ring_size);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        drv->rx_bufs[i] = ps_dma_alloc(dma, E1000_RX_BUF_SIZE, E1000_BUF_ALIGN, 0, PS_MEM_NORMAL);
        if (!drv->rx_bufs[i]) return -1;
        memset(drv->rx_bufs[i], 0, E1000_RX_BUF_SIZE);
        drv->rx_buf_phys[i] = ps_dma_pin(dma, drv->rx_bufs[i], E1000_RX_BUF_SIZE);
        drv->rx_ring[i].addr = drv->rx_buf_phys[i];
        drv->rx_ring[i].status = 0;
    }

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        drv->tx_bufs[i] = ps_dma_alloc(dma, E1000_TX_BUF_SIZE, E1000_BUF_ALIGN, 0, PS_MEM_NORMAL);
        if (!drv->tx_bufs[i]) return -1;
        memset(drv->tx_bufs[i], 0, E1000_TX_BUF_SIZE);
        drv->tx_buf_phys[i] = ps_dma_pin(dma, drv->tx_bufs[i], E1000_TX_BUF_SIZE);
        drv->tx_ring[i].addr = drv->tx_buf_phys[i];
        drv->tx_ring[i].status = E1000_TXD_STAT_DD;
    }

    DMB();
    printf("[%s] DMA allocated: %d RX + %d TX buffers\n", COMPONENT_NAME,
           E1000_NUM_RX_DESC, E1000_NUM_TX_DESC);
    return 0;
}

static int e1000_hw_init(struct e1000_driver *drv)
{
    /* Device reset */
    e1000_wr(drv, E1000_CTRL, E1000_CTRL_RST);
    int timeout = 100000;
    while ((e1000_rd(drv, E1000_CTRL) & E1000_CTRL_RST) && timeout > 0) timeout--;
    for (volatile int i = 0; i < 1000000; i++) {}

    /* Set Link Up, Full Duplex */
    e1000_wr(drv, E1000_CTRL, E1000_CTRL_SLU | E1000_CTRL_FD | E1000_CTRL_ASDE);
    for (volatile int i = 0; i < 100000; i++) {}

    /* Disable interrupts during setup */
    e1000_wr(drv, E1000_IMC, 0xFFFFFFFF);
    (void)e1000_rd(drv, E1000_ICR);

    /* MAC address */
    e1000_read_mac(drv);
    e1000_write_mac(drv);
    printf("[%s] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", COMPONENT_NAME,
           drv->mac_addr[0], drv->mac_addr[1], drv->mac_addr[2],
           drv->mac_addr[3], drv->mac_addr[4], drv->mac_addr[5]);

    /* Clear MTA */
    for (int i = 0; i < E1000_MTA_SIZE; i++)
        e1000_wr(drv, E1000_MTA + i * 4, 0);

    /* Setup RX */
    e1000_wr(drv, E1000_RCTL, 0);
    e1000_wr(drv, E1000_RDBAL, (uint32_t)(drv->rx_ring_phys & 0xFFFFFFFF));
    e1000_wr(drv, E1000_RDBAH, (uint32_t)(drv->rx_ring_phys >> 32));
    e1000_wr(drv, E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_wr(drv, E1000_RDH, 0);
    e1000_wr(drv, E1000_RDT, E1000_NUM_RX_DESC - 1);
    drv->rx_tail = 0;
    DMB();

    e1000_wr(drv, E1000_RCTL,
             E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC |
             E1000_RCTL_BSIZE_2048 | E1000_RCTL_UPE | E1000_RCTL_MPE);

    /* Setup TX */
    e1000_wr(drv, E1000_TDBAL, (uint32_t)(drv->tx_ring_phys & 0xFFFFFFFF));
    e1000_wr(drv, E1000_TDBAH, (uint32_t)(drv->tx_ring_phys >> 32));
    e1000_wr(drv, E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_wr(drv, E1000_TDH, 0);
    e1000_wr(drv, E1000_TDT, 0);
    drv->tx_tail = 0;
    drv->tx_head = 0;

    e1000_wr(drv, E1000_TCTL,
             E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);

    /* Enable RX interrupts */
    e1000_wr(drv, E1000_IMS,
             E1000_IMS_RXT0 | E1000_IMS_RXDMT0 | E1000_IMS_RXO | E1000_IMS_LSC);

    uint32_t status = e1000_rd(drv, E1000_STATUS);
    printf("[%s] E1000 init complete (link=%s)\n", COMPONENT_NAME,
           (status & E1000_STATUS_LU) ? "UP" : "DOWN");

    return 0;
}

static int e1000_tx(struct e1000_driver *drv, const void *data, uint16_t len)
{
    if (len == 0 || len > FRAME_MTU) return -1;

    uint32_t idx = drv->tx_tail;
    struct e1000_tx_desc *desc = &drv->tx_ring[idx];

    int timeout = 10000;
    while (!(desc->status & E1000_TXD_STAT_DD) && timeout > 0) timeout--;
    if (timeout == 0) return -1;

    memcpy(drv->tx_bufs[idx], data, len);
    DMB();

    desc->length = len;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    DMB();

    drv->tx_tail = (idx + 1) % E1000_NUM_TX_DESC;
    e1000_wr(drv, E1000_TDT, drv->tx_tail);
    drv->tx_pkts++;
    return 0;
}

/*
 * ============================================================
 *  lwIP Integration
 * ============================================================
 */

/* FPT-179 Stage 4 fix-8: Pre-populate peers' ARP caches via directed
 * gratuitous ARP REPLY. Each node broadcasts an ARP REPLY with
 * spa=self_ip, sha=self_mac, tpa=peer_ip. The receiver's lwIP sees
 * dipaddr==own_ip → for_us=TRUE → TRY_HARD flag → unconditionally adds
 * the (spa, sha) mapping to the cache. No resolution query needed.
 *
 * This kills the ARP storm (previously 13000+ arp_reqs/node with <0.2%
 * delivery through Xen's virtual-switch/tap bottleneck).
 *
 * Root cause evidence (Xen host tap stats):
 *   tap148.0 TX dropped=93295  (node 0)
 *   tap149.0 TX dropped=98487  (node 2)
 *   tap150.0 TX dropped=94858  (node 1)
 * Only ~170 packets per tap actually reach qemu-dm in 3 minutes. */
static void send_directed_garp(int peer_idx)
{
    uint8_t frame[42];
    /* Ethernet header */
    memset(frame, 0xff, 6);                 /* L2 dst = broadcast */
    memcpy(frame + 6, g_drv.mac_addr, 6);   /* L2 src = our MAC */
    frame[12] = 0x08; frame[13] = 0x06;     /* EtherType = ARP */
    /* ARP body */
    frame[14] = 0x00; frame[15] = 0x01;     /* htype = Ethernet */
    frame[16] = 0x08; frame[17] = 0x00;     /* ptype = IPv4 */
    frame[18] = 0x06;                        /* hlen = 6 */
    frame[19] = 0x04;                        /* plen = 4 */
    frame[20] = 0x00; frame[21] = 0x02;     /* op = REPLY */
    memcpy(frame + 22, g_drv.mac_addr, 6);  /* sha = our MAC */
    uint32_t ours = ip4_addr_get_u32(&self_ip_addr);
    memcpy(frame + 28, &ours, 4);           /* spa = our IP */
    memset(frame + 32, 0xff, 6);            /* tha = broadcast (unknown) */
    uint32_t them = ip4_addr_get_u32(&peer_addrs[peer_idx]);
    memcpy(frame + 38, &them, 4);           /* tpa = peer IP */
    e1000_tx(&g_drv, frame, 42);
}

static void send_directed_garp_all(void)
{
    for (int i = 0; i < NUM_PEERS; i++)
        send_directed_garp(i);
}

/* FPT-179 Stage 4 fix-7: L2 TX/RX packet counters by type, for
 * diagnosing the 3-node HELLO convergence failure. */
static uint32_t g_tx_arp_req = 0;
static uint32_t g_tx_arp_rep = 0;
static uint32_t g_tx_ip_unicast = 0;
static uint32_t g_tx_ip_mcast = 0;
static uint32_t g_rx_arp_req = 0;
static uint32_t g_rx_arp_rep = 0;
static uint32_t g_rx_ip_to_us = 0;
static uint32_t g_rx_ip_not_us = 0;
static uint32_t g_rx_bcast = 0;
static uint32_t g_rx_other = 0;

/* FPT-179 Stage-5: RX-path stage counters + per-peer attribution. One
 * counter per hop lets us locate exactly where inbound traffic disappears
 * (nic -> ipu -> udpc -> ctok -> ring). Per-peer attribution discriminates
 * routing bugs (H1) from crypto/filter drops. */
static uint32_t g_rx_nic_raw     = 0;  /* raw NIC RX, EOP+no err, post-length-check */
static uint32_t g_rx_udp_cb      = 0;  /* dtu_udp_recv_cb entries (port 7654) */
static uint32_t g_rx_udp_trunc   = 0;  /* UDP payload > ct_buf[600], silently clipped */
static uint32_t g_rx_ct_ok       = 0;  /* ct_decrypt returned >= 0 */
static uint32_t g_rx_ring_send   = 0;  /* vdtu_ring_send(net_in) returned 0 */
static uint32_t g_rx_ring_full   = 0;  /* vdtu_ring_send(net_in) returned != 0 */
static uint32_t g_rx_from_peer[NUM_PEERS] = {0};
static uint32_t g_tx_to_peer[NUM_PEERS]   = {0};

/* FPT-179 guest-starvation diagnostics. If XCP-ng drops packets at the tap
 * because our driver fails to refill the E1000 RX ring fast enough, these
 * counters will show it: rx_poll_empty should dominate in a healthy system
 * (ring is usually empty); rx_mpc_total growing means the HW signaled a
 * missed-packet (no descriptor available). Peak batch close to RX ring size
 * means we just caught the ring near full — at risk of overrun next time. */
static uint32_t g_rx_poll_count     = 0;  /* invocations of e1000_poll_rx_lwip */
static uint32_t g_rx_poll_descs     = 0;  /* total descriptors drained */
static uint32_t g_rx_poll_empty     = 0;  /* polls that saw ring empty at entry */
static uint32_t g_rx_poll_max_batch = 0;  /* peak descriptors drained in one call */
static uint32_t g_rx_mpc_total      = 0;  /* cumulative E1000 MPC (HW missed-packet) */
/* Loop-rate proxy: rdtsc delta between status prints. Combined with the
 * 2100 MHz XCP-ng TSC constant this gives us rough loops-per-second,
 * exposing whether the VM is CPU-starved at the seL4 vCPU level. */
static uint64_t g_last_status_tsc   = 0;

/* FPT-179 driver → lwIP handoff diagnostics. Checks whether every
 * descriptor drained from the E1000 RX ring actually makes it into
 * lwIP's input path. Invariant: g_rx_poll_descs == g_rx_eth_input_ok
 *                               + g_rx_eth_input_fail
 *                               + g_rx_pbuf_alloc_fail
 *                               + (frames with desc->errors or !EOP or bad len)
 * The last bucket is silently accounted; we're splitting out the three
 * paths where a valid frame is dropped AFTER reaching the ring. */
static uint32_t g_rx_eth_input_called = 0;  /* calls to g_netif.input */
static uint32_t g_rx_eth_input_fail   = 0;  /* g_netif.input returned != ERR_OK */
static uint32_t g_rx_pbuf_alloc_fail  = 0;  /* pbuf_alloc(PBUF_RAW, PBUF_RAM) -> NULL */

/* lwIP netif linkoutput: called when lwIP wants to send an Ethernet frame */
static err_t e1000_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (p->tot_len > FRAME_MTU) return ERR_BUF;

    /* Linearize if chained */
    uint8_t frame_buf[FRAME_MTU];
    pbuf_copy_partial(p, frame_buf, p->tot_len, 0);

    /* Classify and log first N L2 TX frames for diagnostics. */
    if (p->tot_len >= 14) {
        uint16_t eth_type = ((uint16_t)frame_buf[12] << 8) | frame_buf[13];
        int is_bcast = (frame_buf[0] == 0xff && frame_buf[1] == 0xff);
        if (eth_type == 0x0806 && p->tot_len >= 28+14) {  /* ARP */
            uint16_t op = ((uint16_t)frame_buf[14+6] << 8) | frame_buf[14+7];
            if (op == 1) g_tx_arp_req++;
            else if (op == 2) g_tx_arp_rep++;
        } else if (eth_type == 0x0800) {
            if (is_bcast) g_tx_ip_mcast++;
            else g_tx_ip_unicast++;
        }
        static int tx_log_count = 0;
        if (tx_log_count < 12) {
            printf("[%s] L2 TX #%d: dst=%02x:%02x:%02x:%02x:%02x:%02x type=0x%04x len=%u\n",
                   COMPONENT_NAME, tx_log_count,
                   frame_buf[0], frame_buf[1], frame_buf[2],
                   frame_buf[3], frame_buf[4], frame_buf[5],
                   eth_type, p->tot_len);
            if (eth_type == 0x0806 && p->tot_len >= 42) {
                /* ARP body offsets: op at 6, sha at 8, spa at 14, tha at 18, tpa at 24 */
                const uint8_t *arp = frame_buf + 14;
                printf("[%s] L2 TX #%d ARP op=%u spa=%u.%u.%u.%u tpa=%u.%u.%u.%u\n",
                       COMPONENT_NAME, tx_log_count,
                       ((uint16_t)arp[6]<<8)|arp[7],
                       arp[14], arp[15], arp[16], arp[17],
                       arp[24], arp[25], arp[26], arp[27]);
            } else if (eth_type == 0x0800 && p->tot_len >= 14+20) {
                const uint8_t *ip = frame_buf + 14;
                printf("[%s] L2 TX #%d IP dst=%u.%u.%u.%u proto=%u\n",
                       COMPONENT_NAME, tx_log_count,
                       ip[16], ip[17], ip[18], ip[19], ip[9]);
            }
            tx_log_count++;
        }
    }

    int rc = e1000_tx(&g_drv, frame_buf, (uint16_t)p->tot_len);
    return (rc == 0) ? ERR_OK : ERR_IF;
}

/* lwIP netif init callback */
static err_t e1000_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, g_drv.mac_addr, 6);
    netif->linkoutput = e1000_linkoutput;
    netif->output = etharp_output;
    return ERR_OK;
}

/*
 * Poll RX and feed frames to lwIP (replaces ring buffer dispatch)
 */
static void e1000_poll_rx_lwip(struct e1000_driver *drv)
{
    uint32_t batch = 0;
    g_rx_poll_count++;
    while (1) {
        uint32_t idx = drv->rx_tail;
        struct e1000_rx_desc *desc = &drv->rx_ring[idx];

        if (!(desc->status & E1000_RXD_STAT_DD)) break;
        batch++;

        if (!desc->errors && (desc->status & E1000_RXD_STAT_EOP)) {
            uint16_t len = desc->length;
            if (len >= 14 && len <= FRAME_MTU) {
                g_rx_nic_raw++;
                /* FPT-179 Stage 4 fix-7: classify/log L2 RX frames. */
                const uint8_t *frame = drv->rx_bufs[idx];
                uint16_t eth_type = ((uint16_t)frame[12] << 8) | frame[13];
                int is_bcast = (frame[0] == 0xff && frame[1] == 0xff);
                int is_to_us = (memcmp(frame, drv->mac_addr, 6) == 0);
                if (eth_type == 0x0806 && len >= 42) {
                    uint16_t op = ((uint16_t)frame[14+6] << 8) | frame[14+7];
                    if (op == 1) g_rx_arp_req++;
                    else if (op == 2) g_rx_arp_rep++;
                    /* FPT-179 Stage 4 fix-9: passive MAC learning from ANY
                     * incoming ARP frame. lwIP's etharp_input only caches
                     * for_us frames; we learn from all. Static entries
                     * never age out, so once learned, HELLOs flow forever. */
                    const uint8_t *arp = frame + 14;
                    ip4_addr_t spa_addr;
                    IP4_ADDR(&spa_addr, arp[14], arp[15], arp[16], arp[17]);
                    for (int pi = 0; pi < NUM_PEERS; pi++) {
                        if (ip4_addr_cmp(&spa_addr, &peer_addrs[pi])) {
                            struct eth_addr sha;
                            memcpy(&sha, arp + 8, 6);
                            err_t rc = etharp_add_static_entry(&spa_addr, &sha);
                            static int learned_log = 0;
                            if (learned_log < 6) {
                                printf("[%s] ARP LEARN: %u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x (rc=%d)\n",
                                       COMPONENT_NAME,
                                       arp[14], arp[15], arp[16], arp[17],
                                       arp[8], arp[9], arp[10], arp[11], arp[12], arp[13],
                                       rc);
                                learned_log++;
                            }
                            break;
                        }
                    }
                } else if (eth_type == 0x0800 && len >= 14+20) {
                    const uint8_t *ip = frame + 14;
                    ip4_addr_t dst; IP4_ADDR(&dst, ip[16], ip[17], ip[18], ip[19]);
                    if (ip4_addr_cmp(&dst, &self_ip_addr)) {
                        g_rx_ip_to_us++;
                        /* FPT-179 Stage-5: attribute inbound IP unicast to peer. */
                        ip4_addr_t src; IP4_ADDR(&src, ip[12], ip[13], ip[14], ip[15]);
                        for (int pi = 0; pi < NUM_PEERS; pi++) {
                            if (ip4_addr_cmp(&src, &peer_addrs[pi])) {
                                g_rx_from_peer[pi]++;
                                break;
                            }
                        }
                    }
                    else if (is_bcast) g_rx_bcast++;
                    else g_rx_ip_not_us++;
                } else {
                    g_rx_other++;
                }
                static int rx_log_count = 0;
                if (rx_log_count < 15) {
                    printf("[%s] L2 RX #%d: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%04x len=%u to_us=%d\n",
                           COMPONENT_NAME, rx_log_count,
                           frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                           frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                           eth_type, len, is_to_us);
                    if (eth_type == 0x0806 && len >= 42) {
                        const uint8_t *arp = frame + 14;
                        printf("[%s] L2 RX #%d ARP op=%u spa=%u.%u.%u.%u tpa=%u.%u.%u.%u sha=%02x:%02x:%02x:%02x:%02x:%02x\n",
                               COMPONENT_NAME, rx_log_count,
                               ((uint16_t)arp[6]<<8)|arp[7],
                               arp[14], arp[15], arp[16], arp[17],
                               arp[24], arp[25], arp[26], arp[27],
                               arp[8], arp[9], arp[10], arp[11], arp[12], arp[13]);
                    } else if (eth_type == 0x0800 && len >= 14+20) {
                        const uint8_t *ip = frame + 14;
                        printf("[%s] L2 RX #%d IP src=%u.%u.%u.%u dst=%u.%u.%u.%u proto=%u\n",
                               COMPONENT_NAME, rx_log_count,
                               ip[12], ip[13], ip[14], ip[15],
                               ip[16], ip[17], ip[18], ip[19], ip[9]);
                    }
                    rx_log_count++;
                }
                /* Create pbuf and pass to lwIP */
                struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
                if (p) {
                    memcpy(p->payload, drv->rx_bufs[idx], len);
                    g_rx_eth_input_called++;
                    if (g_netif.input(p, &g_netif) != ERR_OK) {
                        pbuf_free(p);
                        drv->rx_dropped++;
                        g_rx_eth_input_fail++;
                    } else {
                        drv->rx_pkts++;
                    }
                } else {
                    drv->rx_dropped++;
                    g_rx_pbuf_alloc_fail++;
                }
            }
        }

        desc->status = 0;
        desc->errors = 0;
        desc->length = 0;
        DMB();

        /* Advance RDT. Writing idx (the just-processed index) means
         * the newly-available descriptor appears one step behind in
         * the ring (circular invariant: always one "reserve slot"
         * between SW and HW). Writing idx+1 would make HEAD==RDT
         * after every packet and HW stops. */
        drv->rx_tail = (idx + 1) % E1000_NUM_RX_DESC;
        e1000_wr(drv, E1000_RDT, idx);
    }
    /* FPT-179 guest-starvation accounting. */
    if (batch == 0) {
        g_rx_poll_empty++;
    } else {
        g_rx_poll_descs += batch;
        if (batch > g_rx_poll_max_batch)
            g_rx_poll_max_batch = batch;
    }
}

/*
 * ============================================================
 *  DTU Message Transport (UDP)
 * ============================================================
 */

/*
 * Mutual HELLO exchange — UDP port 5000
 *
 * Protocol:
 *   HELLO     = "HELLO FROM NODE X (ip)"     — initial announcement
 *   HELLO_ACK = "HELLO_ACK FROM NODE X (ip)" — confirms peer's HELLO received
 *
 * State machine per node:
 *   HELLO_INIT     → send HELLO;     on recv HELLO → HELLO_GOT_PEER
 *   HELLO_GOT_PEER → send HELLO_ACK; on recv HELLO_ACK → HELLO_COMPLETE
 *   HELLO_COMPLETE → stop sending
 *
 * Future: HELLO becomes CERT_OFFER, HELLO_ACK becomes CERT_VERIFY (FPT-157).
 */
/* Identify which peer an incoming packet came from, by source IP.
 * Returns index into peer_addrs[] / peer_hello_phase[], or -1 if unknown. */
static int peer_index_of(const ip_addr_t *addr)
{
    if (!addr) return -1;
    const ip4_addr_t *src = ip_2_ip4(addr);
    for (int i = 0; i < NUM_PEERS; i++) {
        if (ip4_addr_cmp(src, &peer_addrs[i]))
            return i;
    }
    return -1;
}

/* FPT-179: reset all per-peer state when we observe a peer-reboot via
 * HELLO epoch change. Re-enters HELLO_INIT so the normal handshake runs
 * from scratch, clears CT replay window so the peer's fresh tx_seq=1
 * packets are accepted, and refreshes peer_kernel_id from HELLO payload
 * in case the peer's KERNEL_ID changed (e.g., ISO swap). */
static void peer_reboot_reset(int peer, uint32_t new_epoch, uint8_t new_kid)
{
    uint8_t old_kid = peer_kernel_id[peer];
    printf("[%s] PEER REBOOT: peer %d epoch %08x->%08x, kid %u->%u — reset\n",
           COMPONENT_NAME, peer,
           peer_epoch[peer], new_epoch,
           old_kid, new_kid);
    peer_hello_phase[peer] = HELLO_INIT;
    peer_hello_grace[peer] = 0;
    peer_epoch[peer] = new_epoch;
    g_peer_reboots_detected++;
    if (new_kid != old_kid) {
        /* Old kid may still have stale seq state that would reject any
         * late-arriving packets from the old instance. Clear both. */
        ct_reset_peer(old_kid);
        peer_kernel_id[peer] = new_kid;
    }
    ct_reset_peer(new_kid);
}

static void hello_udp_recv_cb(void *arg, struct udp_pcb *pcb,
                               struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)port;
    if (!p) return;

    char msg[128];
    uint16_t len = p->tot_len;
    if (len >= sizeof(msg)) len = sizeof(msg) - 1;
    pbuf_copy_partial(p, msg, len, 0);
    msg[len] = '\0';
    pbuf_free(p);

    int peer = peer_index_of(addr);
    /* Log every HELLO-port arrival at least once so we can prove the
     * packet reached lwIP → our callback. FPT-179 Stage 4 fix-5. */
    {
        static int hello_rx_logged = 0;
        if (hello_rx_logged < 6) {
            printf("[%s] HELLO RAW RX from %d.%d.%d.%d (peer_idx=%d, msg[0]=%c%c%c%c%c)\n",
                   COMPONENT_NAME,
                   ip4_addr1(ip_2_ip4(addr)), ip4_addr2(ip_2_ip4(addr)),
                   ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
                   peer,
                   len>0?msg[0]:'?', len>1?msg[1]:'?', len>2?msg[2]:'?',
                   len>3?msg[3]:'?', len>4?msg[4]:'?');
            hello_rx_logged++;
        }
    }
    if (peer < 0) {
        /* Unknown source IP — ignore. */
        return;
    }

    /* FPT-179: parse EPOCH and sender kid from HELLO payload. Payload
     * format is "<PREFIX> FROM NODE <kid> EPOCH <hex> (<ip>)". Missing
     * EPOCH (e.g., pre-fix peer) leaves rx_epoch=0 which disables the
     * reboot-detection path for that peer — backward compatible. */
    uint32_t rx_epoch = 0;
    uint32_t rx_kid_u32 = (uint32_t)peer_kernel_id[peer];  /* default = cached */
    {
        const char *ep = strstr(msg, "EPOCH ");
        if (ep) (void)sscanf(ep, "EPOCH %x", &rx_epoch);
        const char *np = strstr(msg, "NODE ");
        if (np) (void)sscanf(np, "NODE %u", &rx_kid_u32);
    }
    if (rx_epoch != 0) {
        if (peer_epoch[peer] == 0) {
            /* First HELLO ever from this peer — record epoch, no reset
             * needed (CT state for this kid is already pristine). */
            peer_epoch[peer] = rx_epoch;
        } else if (peer_epoch[peer] != rx_epoch) {
            peer_reboot_reset(peer, rx_epoch, (uint8_t)rx_kid_u32);
        }
    }

    int is_ack = (len >= 9 && msg[0] == 'H' && msg[5] == '_');

    if (is_ack) {
        /* Received HELLO_ACK from peer i — that pair is complete */
        if (peer_hello_phase[peer] < HELLO_COMPLETE) {
            printf("[%s] HELLO_ACK RX from %d.%d.%d.%d (peer %d) → peer COMPLETE\n",
                   COMPONENT_NAME,
                   ip4_addr1(ip_2_ip4(addr)), ip4_addr2(ip_2_ip4(addr)),
                   ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
                   peer);
            peer_hello_phase[peer] = HELLO_COMPLETE;
            peer_hello_grace[peer] = HELLO_GRACE_COUNT;
        }
        /* Even if already COMPLETE, refresh grace — the peer is telling us it
         * wants more HELLO_ACKs, presumably because its own transitions got
         * lost. This is the key fix for 3+ node convergence. */
        if (peer_hello_grace[peer] < HELLO_GRACE_COUNT)
            peer_hello_grace[peer] = HELLO_GRACE_COUNT;
    } else {
        /* Received HELLO from peer i — at minimum, advance to GOT_PEER */
        if (peer_hello_phase[peer] < HELLO_GOT_PEER) {
            printf("[%s] HELLO RX from %d.%d.%d.%d (peer %d) → sending ACKs\n",
                   COMPONENT_NAME,
                   ip4_addr1(ip_2_ip4(addr)), ip4_addr2(ip_2_ip4(addr)),
                   ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
                   peer);
            peer_hello_phase[peer] = HELLO_GOT_PEER;
        }
        /* If already COMPLETE, the peer is still sending HELLO — it likely
         * didn't see our HELLO_ACK yet. Refresh grace so we keep ACKing. */
        if (peer_hello_phase[peer] == HELLO_COMPLETE
            && peer_hello_grace[peer] < HELLO_GRACE_COUNT)
            peer_hello_grace[peer] = HELLO_GRACE_COUNT;
    }
}

/* Per-peer HELLO send. Each peer gets HELLO if we haven't heard from them
 * yet, or HELLO_ACK if we have (and remain in grace). Peers fully in
 * COMPLETE with grace=0 are skipped. */
static void send_hello(void)
{
    static uint32_t send_count[NUM_PEERS] = {0};  /* per-peer send counter */
    for (int i = 0; i < NUM_PEERS; i++) {
        enum hello_state st = peer_hello_phase[i];
        if (st == HELLO_COMPLETE && peer_hello_grace[i] <= 0)
            continue;

        const char *prefix = (st >= HELLO_GOT_PEER) ? "HELLO_ACK" : "HELLO";
        /* FPT-179: include EPOCH so peers can detect our reboot and reset
         * their per-peer state for us. Format extension is additive —
         * pre-fix receivers ignore the extra tokens. */
        char msg[96];
        int len = snprintf(msg, sizeof(msg), "%s FROM NODE %u EPOCH %08x (%s)",
                           prefix, my_node_id, g_my_epoch, DTUB_SELF_IP);

        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)len, PBUF_RAM);
        if (!p) continue;
        memcpy(p->payload, msg, len);

        ip_addr_t dest;
        ip_addr_copy_from_ip4(dest, peer_addrs[i]);

        err_t err = udp_sendto(g_hello_pcb, p, &dest, HELLO_UDP_PORT);
        pbuf_free(p);

        /* Log first 3 sends per peer and then every 200th, so we can tell
         * what's being sent without flooding the log. */
        send_count[i]++;
        if (send_count[i] <= 3 || (send_count[i] % 200) == 0) {
            printf("[%s] HELLO TX #%u to peer %d (%d.%d.%d.%d) %s err=%d state=%d grace=%d\n",
                   COMPONENT_NAME, send_count[i], i,
                   ip4_addr1(&peer_addrs[i]), ip4_addr2(&peer_addrs[i]),
                   ip4_addr3(&peer_addrs[i]), ip4_addr4(&peer_addrs[i]),
                   prefix, err, (int)st, peer_hello_grace[i]);
        }

        if (st == HELLO_COMPLETE && peer_hello_grace[i] > 0)
            peer_hello_grace[i]--;
    }
}

/* True iff all peer handshakes have reached HELLO_COMPLETE. */
static bool all_peers_complete(void)
{
    for (int i = 0; i < NUM_PEERS; i++) {
        if (peer_hello_phase[i] != HELLO_COMPLETE)
            return false;
    }
    return true;
}

/* True iff any peer still needs a HELLO or HELLO_ACK (incomplete OR in grace). */
static bool any_peer_pending(void)
{
    for (int i = 0; i < NUM_PEERS; i++) {
        if (peer_hello_phase[i] != HELLO_COMPLETE || peer_hello_grace[i] > 0)
            return true;
    }
    return false;
}

/*
 * UDP receive callback — a DTU message arrived from the remote node.
 * Write it to the net_inbound ring buffer for kernel consumption.
 */
static void dtu_udp_recv_cb(void *arg, struct udp_pcb *pcb,
                             struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)port;
    g_rx_udp_cb++;

    if (!p || p->tot_len < CT_OVERHEAD) {
        if (p) pbuf_free(p);
        return;
    }

    /* CryptoTransport: decrypt the entire UDP payload.
     * Wire format: [ct_header_t][ciphertext][tag]
     * Plaintext is the original DTU message: [vdtu_msg_header][payload]. */
    uint8_t ct_buf[600];
    uint16_t ct_len = p->tot_len;
    if (ct_len > sizeof(ct_buf)) {
        ct_len = sizeof(ct_buf);
        g_rx_udp_trunc++;
    }
    pbuf_copy_partial(p, ct_buf, ct_len, 0);
    pbuf_free(p);

    uint8_t decrypted[600];
    uint8_t sender_node;
    int pt_len = ct_decrypt(ct_buf, ct_len, decrypted, sizeof(decrypted),
                            &sender_node);
    if (pt_len < 0) {
        /* Auth failure, replay, or wrong receiver — drop silently */
        return;
    }
    g_rx_ct_ok++;

    if (pt_len < VDTU_HEADER_SIZE) {
        printf("[%s] NET RX: decrypted too short (%d)\n", COMPONENT_NAME, pt_len);
        return;
    }

    /* Extract DTU header from decrypted plaintext */
    struct vdtu_msg_header hdr;
    memcpy(&hdr, decrypted, VDTU_HEADER_SIZE);

    /* Extract payload */
    uint16_t payload_len = hdr.length;
    uint8_t *payload_buf = decrypted + VDTU_HEADER_SIZE;
    uint16_t avail = (uint16_t)(pt_len - VDTU_HEADER_SIZE);
    if (payload_len > avail)
        payload_len = avail;

    printf("[%s] NET RX: %d bytes from node %u (label=0x%lx)\n",
           COMPONENT_NAME, pt_len, sender_node,
           (unsigned long)hdr.label);

    /* Write to inbound ring buffer for kernel to consume */
    if (net_rings_ready) {
        int rc = vdtu_ring_send(&g_net_in_ring,
                                hdr.sender_core_id, hdr.sender_ep_id,
                                hdr.sender_vpe_id, hdr.reply_ep_id,
                                hdr.label, hdr.replylabel, hdr.flags,
                                payload_buf, payload_len);
        if (rc != 0) {
            g_rx_ring_full++;
            printf("[%s] NET RX: inbound ring full!\n", COMPONENT_NAME);
        } else {
            g_rx_ring_send++;
        }
    }
}

/*
 * RPC handler: SemperKernel calls this to send a DTU message to a remote node.
 * The kernel has already written the raw DTU message bytes into the dtu_out dataport.
 */
int net_net_send(int dest_node, int msg_len)
{
    if (msg_len <= 0 || msg_len > 1400) return -1;
    if (dest_node < 0 || dest_node >= NUM_PEERS) {
        printf("[%s] Invalid dest_node %d\n", COMPONENT_NAME, dest_node);
        return -1;
    }

    const uint8_t *msg_bytes = (const uint8_t *)dtu_out;

    /* Build UDP datagram */
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)msg_len, PBUF_RAM);
    if (!p) return -1;

    memcpy(p->payload, msg_bytes, msg_len);

    ip_addr_t dest_ip;
    ip_addr_copy_from_ip4(dest_ip, peer_addrs[dest_node]);

    err_t err = udp_sendto(g_udp_pcb, p, &dest_ip, DTU_UDP_PORT);
    pbuf_free(p);

    if (err != ERR_OK) {
        printf("[%s] UDP send failed: %d\n", COMPONENT_NAME, err);
        return -1;
    }

    printf("[%s] TX DTU msg to peer %d (%d bytes)\n",
           COMPONENT_NAME, dest_node, msg_len);
    return 0;
}

/*
 * ============================================================
 *  CAmkES Lifecycle
 * ============================================================
 */

/* IRQ handler — acks and polls RX, guarded against re-entry.
 *
 * Both the main loop and the IRQ handler call e1000_poll_rx_lwip.
 * IRQs fire asynchronously, so the two can race on rx_tail/rx_pkts
 * (bug 9 from FPT-176). We use a simple in-progress flag as a
 * recursion guard: the main loop sets it before polling and clears
 * it after; the IRQ handler skips polling if the flag is set.
 *
 * On QEMU the main loop handles RX fine without IRQ participation.
 * On XCP-ng (Xen HVM) the E1000 emulation apparently needs its IRQs
 * acknowledged through eth_irq_acknowledge() for receive to keep
 * working — without it, irq=0 rx=0 and the ring never fills.
 */
volatile int g_drv_rx_poll_busy = 0;

void eth_irq_handle(void)
{
    if (!driver_ready) {
        eth_irq_acknowledge();
        return;
    }
    g_drv.irq_count++;
    uint32_t icr = e1000_rd(&g_drv, E1000_ICR);

    if ((icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO))
        && !g_drv_rx_poll_busy) {
        g_drv_rx_poll_busy = 1;
        e1000_poll_rx_lwip(&g_drv);
        g_drv_rx_poll_busy = 0;
    }

    eth_irq_acknowledge();
}

void pre_init(void)
{
    printf("[%s] pre_init\n", COMPONENT_NAME);
}

void post_init(void)
{
    int error;
    ps_io_ops_t io_ops;

    printf("[%s] E1000 + lwIP UDP bridge\n", COMPONENT_NAME);

    /* PCI init */
    error = e1000_pci_init();
    if (error) return;

    /* CAmkES io_ops for DMA */
    error = camkes_io_ops(&io_ops);
    if (error) {
        printf("[%s] Failed to get io_ops: %d\n", COMPONENT_NAME, error);
        return;
    }

    /* Driver init */
    memset(&g_drv, 0, sizeof(g_drv));
    g_drv.mmio = (volatile void *)eth_mmio;

    error = e1000_alloc_dma(&g_drv, &io_ops.dma_manager);
    if (error) {
        printf("[%s] DMA alloc failed\n", COMPONENT_NAME);
        return;
    }

    error = e1000_hw_init(&g_drv);
    if (error) {
        printf("[%s] HW init failed\n", COMPONENT_NAME);
        return;
    }

    /* Node identity from compile-time constants */
    my_node_id = KERNEL_ID;
    parse_ip4(DTUB_SELF_IP, &self_ip_addr);
    parse_ip4(DTUB_PEER_IP_0, &peer_addrs[0]);
#if NUM_PEERS >= 2
    parse_ip4(DTUB_PEER_IP_1, &peer_addrs[1]);
#endif

    /* Build peer index → KERNEL_ID mapping from IP last octet convention.
     * XCP-ng: .10 = kernel 0, .11 = kernel 1, .12 = kernel 2 (offset 10).
     * QEMU:   .1  = kernel 0, .2  = kernel 1, .3  = kernel 2 (offset 1). */
    for (int i = 0; i < NUM_PEERS; i++) {
        uint8_t last = ip4_addr4(&peer_addrs[i]);
        peer_kernel_id[i] = (last >= 10) ? (last - 10) : (last - 1);
    }

#if NUM_PEERS >= 2
    printf("[%s] Node %u (self=%s, peer0=%s[k%u], peer1=%s[k%u])\n",
           COMPONENT_NAME, my_node_id, DTUB_SELF_IP,
           DTUB_PEER_IP_0, peer_kernel_id[0],
           DTUB_PEER_IP_1, peer_kernel_id[1]);
#else
    printf("[%s] Node %u (self=%s, peer0=%s[k%u])\n",
           COMPONENT_NAME, my_node_id, DTUB_SELF_IP,
           DTUB_PEER_IP_0, peer_kernel_id[0]);
#endif

    /* FPT-179: seed our session epoch from rdtsc so each boot gets a
     * distinct value even if KERNEL_ID and IP are identical to a prior
     * run. Peers detect our reboot by observing this value change in
     * our outbound HELLO. 0 is reserved as "unknown" sentinel. */
    {
        uint32_t hi, lo;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        g_my_epoch = lo ^ (hi << 1) ^ ((uint32_t)KERNEL_ID << 24) ^ 0x5A5A5A5Au;
        if (g_my_epoch == 0) g_my_epoch = 1;
        printf("[%s] session epoch = 0x%08x\n", COMPONENT_NAME, g_my_epoch);
    }

    /* CryptoTransport init — per-packet AEAD with static PSKs (FPT-155) */
    ct_init(my_node_id);

    /* lwIP init */
    lwip_init();

    ip4_addr_t netmask, gw;
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    netif_add(&g_netif, &self_ip_addr, &netmask, &gw, NULL, e1000_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    printf("[%s] lwIP UP: %s/24\n", COMPONENT_NAME, DTUB_SELF_IP);

    /* UDP PCB for DTU transport (port 7654) */
    g_udp_pcb = udp_new();
    if (!g_udp_pcb) {
        printf("[%s] Failed to create DTU UDP PCB\n", COMPONENT_NAME);
        return;
    }
    udp_bind(g_udp_pcb, IP_ANY_TYPE, DTU_UDP_PORT);
    udp_recv(g_udp_pcb, dtu_udp_recv_cb, NULL);

    /* UDP PCB for hello exchange (port 5000) */
    g_hello_pcb = udp_new();
    if (!g_hello_pcb) {
        printf("[%s] Failed to create hello UDP PCB\n", COMPONENT_NAME);
        return;
    }
    udp_bind(g_hello_pcb, IP_ANY_TYPE, HELLO_UDP_PORT);
    udp_recv(g_hello_pcb, hello_udp_recv_cb, NULL);

    printf("[%s] UDP: DTU port %d, Hello port %d\n",
           COMPONENT_NAME, DTU_UDP_PORT, HELLO_UDP_PORT);

    driver_ready = true;

    /* FPT-179 Stage 4 fix-9: announce ourselves and also learn peers.
     * 20 rounds at init (40 frames) maximises the chance at least one
     * gets through the lossy tap path. In combination with passive MAC
     * learning in e1000_poll_rx_lwip (calls etharp_add_static_entry on
     * any peer's ARP frame), this bootstraps peer caches from very few
     * successful packet deliveries. */
    for (int r = 0; r < 20; r++) {
        send_directed_garp_all();
    }
    printf("[%s] Sent %d directed gratuitous ARP rounds to %d peers at init\n",
           COMPONENT_NAME, 20, NUM_PEERS);

    /* FPT-179: one-shot diagnostics after HW init. */
    e1000_pci_debug_dump();
    e1000_reg_debug_dump(&g_drv, "post_init");

    /* Initialize network ring buffers (07e).
     * DTUBridge inits both rings; kernel attaches later in kernel_start().
     * post_init runs before any run(), so kernel_start() sees initialized rings. */
    vdtu_ring_init(&g_net_out_ring, (void *)net_outbound, 4, 512);
    vdtu_ring_init(&g_net_in_ring, (void *)net_inbound, 4, 512);
    net_rings_ready = true;
    printf("[%s] Net rings initialized (4 slots x 512B)\n", COMPONENT_NAME);

    printf("[%s] Ready\n", COMPONENT_NAME);
}

int run(void)
{
    printf("[%s] Entering main loop\n", COMPONENT_NAME);

    uint32_t loop_count = 0;
    int hello_sent = 0;
    int hello_send_attempts = 0;

    while (1) {
        bool did_work = false;

        /* Poll RX (supplement IRQ-driven receive). Set the busy flag
         * so the IRQ handler skips its own poll — prevents the race
         * on rx_tail / rx_pkts (bug 9 from FPT-176). */
        if (driver_ready) {
            uint32_t rx_before = g_drv.rx_pkts;
            g_drv_rx_poll_busy = 1;
            e1000_poll_rx_lwip(&g_drv);
            g_drv_rx_poll_busy = 0;
            if (g_drv.rx_pkts != rx_before) did_work = true;
        }

        /* Pump lwIP timers (ARP, etc.) */
        sys_check_timeouts();

        /* FPT-179 Stage 4 fix-9: aggressive gratuitous ARP while not
         * converged. Every 100K iters until all peers COMPLETE, plus
         * periodic steady-state (every 1M) after. */
        if (driver_ready) {
            bool converged = all_peers_complete();
            if (!converged && (loop_count % 100000) == 75000) {
                send_directed_garp_all();
            } else if (converged && (loop_count % 1000000) == 750000) {
                send_directed_garp_all();
            }
        }

        /* Per-peer mutual HELLO exchange (FPT-179 Stage 3).
         * send_hello() iterates peers and emits HELLO/HELLO_ACK per-peer,
         * decrementing each peer's grace when sent from COMPLETE. */
        if (driver_ready && any_peer_pending()
            && (loop_count % 100000) == 50000) {
            send_hello();
            hello_send_attempts++;
            hello_sent = 1;
        }

        /* Report hello exchange status once */
        if (hello_sent && !all_peers_complete() && loop_count == 800000) {
            printf("[%s] === HELLO EXCHANGE: not all peers yet (peer0=%d peer1=%d) ===\n",
                   COMPONENT_NAME,
                   (int)peer_hello_phase[0], (int)peer_hello_phase[1]);
        }
        if (all_peers_complete() && hello_sent) {
            printf("[%s] === HELLO EXCHANGE: COMPLETE (all %d peers) ===\n",
                   COMPONENT_NAME, NUM_PEERS);
            hello_sent = 0;  /* print once */
        }

        /* Poll outbound ring: kernel → network (07e)
         * FPT-179 Stage 4: route to the peer whose KERNEL_ID matches the
         * dest_kid encoded in the flags high-nibble by the kernel. If the
         * dest doesn't match any known peer (e.g. 0xF sentinel for pre-link
         * PING probes), fall back to peer 0. Encrypt outmsg directly — the
         * dest-kid bits ride along in flags on the wire; receivers only
         * test bits 0-1 and ignore higher bits. */
        if (net_rings_ready && !vdtu_ring_is_empty(&g_net_out_ring)) {
            const struct vdtu_message *outmsg = vdtu_ring_fetch(&g_net_out_ring);
            if (outmsg) {
                uint16_t plaintext_len = VDTU_HEADER_SIZE + outmsg->hdr.length;

                /* Decode dest kernel id, then look up which peer slot owns it. */
                uint8_t dest_kid = VDTU_FLAGS_GET_DEST(outmsg->hdr.flags);
                int peer_idx = 0;  /* default fallback */
                for (int pi = 0; pi < NUM_PEERS; pi++) {
                    if (peer_kernel_id[pi] == dest_kid) {
                        peer_idx = pi;
                        break;
                    }
                }
                /* FPT-179 Stage-5: per-peer TX attribution. Counts every
                 * outbound attempt, including the silent peer_idx=0 fallback
                 * taken when dest_kid matches no peer — asymmetry vs. actual
                 * traffic shape flags the fallback (routing bug H1). */
                if (peer_idx >= 0 && peer_idx < NUM_PEERS)
                    g_tx_to_peer[peer_idx]++;

                /* ct_buf sized for KRNLC max (2048) + VDTU_HEADER_SIZE + CT_OVERHEAD. */
                uint8_t ct_buf[2200];
                int ct_len = ct_encrypt(peer_kernel_id[peer_idx],
                                        (const uint8_t *)outmsg, plaintext_len,
                                        ct_buf, sizeof(ct_buf));
                if (ct_len > 0) {
                    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, ct_len, PBUF_RAM);
                    if (p) {
                        memcpy(p->payload, ct_buf, ct_len);
                        ip_addr_t dest_ip;
                        ip_addr_copy_from_ip4(dest_ip, peer_addrs[peer_idx]);
                        err_t err = udp_sendto(g_udp_pcb, p, &dest_ip, DTU_UDP_PORT);
                        pbuf_free(p);
                        printf("[%s] NET TX: %u->%d bytes to peer %d (kid=%u, label=0x%lx, err=%d)\n",
                               COMPONENT_NAME, plaintext_len, ct_len,
                               peer_idx, (unsigned)dest_kid,
                               (unsigned long)outmsg->hdr.label, err);
                    }
                } else {
                    printf("[%s] NET TX: encrypt failed for %u bytes (pt_len=%u ct_max=%zu)\n",
                           COMPONENT_NAME, plaintext_len, plaintext_len, sizeof(ct_buf));
                }
                vdtu_ring_ack(&g_net_out_ring);
                did_work = true;
            }
        }

        /* Periodic status */
        loop_count++;
        if ((loop_count % 1000000) == 0) {
            printf("[%s] irq=%u rx=%u tx=%u drop=%u hello=%s (p0=%d p1=%d)\n",
                   COMPONENT_NAME,
                   g_drv.irq_count, g_drv.rx_pkts,
                   g_drv.tx_pkts, g_drv.rx_dropped,
                   all_peers_complete() ? "YES" : "no",
                   (int)peer_hello_phase[0], (int)peer_hello_phase[1]);
            /* FPT-179 Stage 4 fix-7: L2/L3 packet class counters. */
            printf("[%s] L2/3: TX[arp_req=%u arp_rep=%u ip_uc=%u ip_mc=%u] RX[arp_req=%u arp_rep=%u ip_to_us=%u ip_not_us=%u bcast=%u other=%u]\n",
                   COMPONENT_NAME,
                   g_tx_arp_req, g_tx_arp_rep, g_tx_ip_unicast, g_tx_ip_mcast,
                   g_rx_arp_req, g_rx_arp_rep, g_rx_ip_to_us, g_rx_ip_not_us,
                   g_rx_bcast, g_rx_other);
            /* FPT-179 Stage-5: RX-path stage counters — locate where inbound
             * traffic disappears (nic -> ipu -> udpc -> ctok -> ring). */
            printf("[%s] RX-path: nic=%u ipu=%u udpc=%u trunc=%u ctok=%u ring=%u rfull=%u\n",
                   COMPONENT_NAME,
                   g_rx_nic_raw, g_rx_ip_to_us, g_rx_udp_cb, g_rx_udp_trunc,
                   g_rx_ct_ok, g_rx_ring_send, g_rx_ring_full);
            printf("[%s] CT-rej: rcvr=%u sndr=%u repl=%u auth=%u | "
                   "RX-per-peer: p0=%u p1=%u | TX-per-peer: p0=%u p1=%u | "
                   "reboots_detected=%u\n",
                   COMPONENT_NAME,
                   ct_get_rej_rcvr(), ct_get_rej_sender(),
                   ct_get_rej_replay(), ct_get_rej_auth(),
                   g_rx_from_peer[0], g_rx_from_peer[1],
                   g_tx_to_peer[0],   g_tx_to_peer[1],
                   g_peer_reboots_detected);
            /* FPT-179 guest-starvation diagnostic.
             * - polls/descs/empty/max_batch: does our driver service the RX
             *   ring often enough? Healthy = empty >> descs (ring usually
             *   idle when we poll). Starvation = descs close to polls and
             *   max_batch climbing toward E1000_NUM_RX_DESC = ring filling.
             * - hw_mpc: E1000 Missed Packet Count (HW-level dropped for no
             *   descriptor). Read-to-clear, so we accumulate into a total.
             *   MPC > 0 means our guest driver let the ring run empty and
             *   qemu-dm had to drop frames on the host side.
             * - ring_fill = RDH-RDT mod NUM, current descriptors awaiting us.
             * - tsc_delta: cycles between two status prints; with 2100 MHz
             *   XCP-ng TSC, loops_per_sec = 1M / (tsc_delta / 2.1e9). */
            if (driver_ready) {
                uint32_t mpc = e1000_rd(&g_drv, E1000_MPC);
                g_rx_mpc_total += mpc;
                uint32_t rdh = e1000_rd(&g_drv, E1000_RDH);
                uint32_t rdt = e1000_rd(&g_drv, E1000_RDT);
                uint32_t ring_fill = (rdh + E1000_NUM_RX_DESC - rdt - 1)
                                     % E1000_NUM_RX_DESC;
                uint32_t hi, lo;
                __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
                uint64_t now_tsc = ((uint64_t)hi << 32) | lo;
                uint64_t tsc_delta = g_last_status_tsc ?
                                     (now_tsc - g_last_status_tsc) : 0;
                g_last_status_tsc = now_tsc;
                printf("[%s] rx-drain: polls=%u descs=%u empty=%u max_batch=%u"
                       " hw_mpc=%u ring_fill=%u tsc_delta=%llu\n",
                       COMPONENT_NAME,
                       g_rx_poll_count, g_rx_poll_descs, g_rx_poll_empty,
                       g_rx_poll_max_batch, g_rx_mpc_total, ring_fill,
                       (unsigned long long)tsc_delta);
                /* FPT-179: driver → lwIP handoff breakdown. Proves whether
                 * every descriptor we drained actually makes it into lwIP.
                 * Invariant check: eth_in_call ≈ descs - pbuf_fail
                 * Success path: eth_in_ok = g_drv.rx_pkts. */
                printf("[%s] rx-handoff: eth_in_call=%u eth_in_ok=%u"
                       " eth_in_fail=%u pbuf_fail=%u nic_raw=%u\n",
                       COMPONENT_NAME,
                       g_rx_eth_input_called, g_drv.rx_pkts,
                       g_rx_eth_input_fail, g_rx_pbuf_alloc_fail,
                       g_rx_nic_raw);
            }
            /* FPT-179: periodic register dump — check if descriptor-ring state
             * advances (RDH moves on RX). If RDH stays at 0 and ICR=0 both
             * always, either IRQ path or MMIO mapping is broken. */
            if (driver_ready)
                e1000_reg_debug_dump(&g_drv, "periodic");
        }

        if (!did_work) seL4_Yield();
    }

    return 0;
}
