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

#define COMPONENT_NAME "DTUBridge"

/* Node identity — set at build time via -DNODE_ID=N */
#ifndef NODE_ID
#define NODE_ID 0
#endif

/* DTU transport UDP port */
#define DTU_UDP_PORT   7654

/* Network configuration */
#if NODE_ID == 0
#define MY_IP_ADDR     "10.0.0.1"
#define MY_IP_A 10
#define MY_IP_B 0
#define MY_IP_C 0
#define MY_IP_D 1
#define PEER_IP_D 2
#else
#define MY_IP_ADDR     "10.0.0.2"
#define MY_IP_A 10
#define MY_IP_B 0
#define MY_IP_C 0
#define MY_IP_D 2
#define PEER_IP_D 1
#endif

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

#define E1000_PCI_BUS       0
#define E1000_PCI_DEV       2
#define E1000_PCI_FUN       0

#define E1000_BAR0_ADDR     0xfeb80000

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
    printf("[%s] Initializing PCI for e1000...\n", COMPONENT_NAME);

    uint16_t vendor = pci_cfg_read16(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_VENDOR_ID);
    uint16_t device = pci_cfg_read16(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_DEVICE_ID);

    printf("[%s] PCI %d:%d.%d: vendor=0x%04x device=0x%04x\n",
           COMPONENT_NAME, E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, vendor, device);

    if (vendor != E1000_VENDOR_ID || device != E1000_DEVICE_ID) {
        printf("[%s] ERROR: Expected 82540EM (8086:100E), got %04x:%04x\n",
               COMPONENT_NAME, vendor, device);
        return -1;
    }

    uint32_t bar0 = pci_cfg_read32(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_BAR0);
    if ((bar0 & ~0xF) == 0 || bar0 == 0xFFFFFFFF) {
        pci_cfg_write32(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_BAR0, E1000_BAR0_ADDR);
    }

    uint16_t cmd = pci_cfg_read16(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_cfg_write16(E1000_PCI_BUS, E1000_PCI_DEV, E1000_PCI_FUN, PCI_COMMAND, cmd);

    printf("[%s] PCI initialized\n", COMPONENT_NAME);
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
static struct udp_pcb *g_udp_pcb;

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

/* lwIP netif linkoutput: called when lwIP wants to send an Ethernet frame */
static err_t e1000_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (p->tot_len > FRAME_MTU) return ERR_BUF;

    /* Linearize if chained */
    uint8_t frame_buf[FRAME_MTU];
    pbuf_copy_partial(p, frame_buf, p->tot_len, 0);

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
    while (1) {
        uint32_t idx = drv->rx_tail;
        struct e1000_rx_desc *desc = &drv->rx_ring[idx];

        if (!(desc->status & E1000_RXD_STAT_DD)) break;

        if (!desc->errors && (desc->status & E1000_RXD_STAT_EOP)) {
            uint16_t len = desc->length;
            if (len >= 14 && len <= FRAME_MTU) {
                /* Create pbuf and pass to lwIP */
                struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
                if (p) {
                    memcpy(p->payload, drv->rx_bufs[idx], len);
                    if (g_netif.input(p, &g_netif) != ERR_OK) {
                        pbuf_free(p);
                        drv->rx_dropped++;
                    } else {
                        drv->rx_pkts++;
                    }
                } else {
                    drv->rx_dropped++;
                }
            }
        }

        desc->status = 0;
        desc->errors = 0;
        desc->length = 0;
        DMB();

        drv->rx_tail = (idx + 1) % E1000_NUM_RX_DESC;
        e1000_wr(drv, E1000_RDT, idx);
    }
}

/*
 * ============================================================
 *  DTU Message Transport (UDP)
 * ============================================================
 */

/*
 * UDP receive callback — a DTU message arrived from the remote node.
 * Copy it to the dtu_in dataport and signal SemperKernel.
 */
static void dtu_udp_recv_cb(void *arg, struct udp_pcb *pcb,
                             struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)port;

    if (!p || p->tot_len < VDTU_HEADER_SIZE) {
        if (p) pbuf_free(p);
        return;
    }

    /* Copy UDP payload (raw DTU message) to dtu_in dataport.
     * First 2 bytes: message length (uint16_t), then message body. */
    volatile uint8_t *dst = (volatile uint8_t *)dtu_in;
    uint16_t msg_len = (uint16_t)p->tot_len;

    /* Write length header */
    dst[0] = (uint8_t)(msg_len & 0xFF);
    dst[1] = (uint8_t)((msg_len >> 8) & 0xFF);

    /* Write message body */
    pbuf_copy_partial(p, (void *)(dst + 2), msg_len, 0);
    pbuf_free(p);

    printf("[%s] RX DTU msg from %d.%d.%d.%d:%u (%u bytes)\n",
           COMPONENT_NAME,
           ip4_addr1(ip_2_ip4(addr)), ip4_addr2(ip_2_ip4(addr)),
           ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
           port, msg_len);

    /* Signal SemperKernel that a DTU message arrived */
    net_msg_ready_emit();
}

/*
 * RPC handler: SemperKernel calls this to send a DTU message to a remote node.
 * The kernel has already written the raw DTU message bytes into the dtu_out dataport.
 */
int net_net_send(int dest_node, int msg_len)
{
    if (msg_len <= 0 || msg_len > 1400) return -1;

    const uint8_t *msg_bytes = (const uint8_t *)dtu_out;

    /* Build UDP datagram */
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)msg_len, PBUF_RAM);
    if (!p) return -1;

    memcpy(p->payload, msg_bytes, msg_len);

    ip_addr_t dest_ip;
    IP4_ADDR(ip_2_ip4(&dest_ip), MY_IP_A, MY_IP_B, MY_IP_C,
             (dest_node == 0) ? 1 : 2);

    err_t err = udp_sendto(g_udp_pcb, p, &dest_ip, DTU_UDP_PORT);
    pbuf_free(p);

    if (err != ERR_OK) {
        printf("[%s] UDP send failed: %d\n", COMPONENT_NAME, err);
        return -1;
    }

    printf("[%s] TX DTU msg to node %d (%d bytes)\n",
           COMPONENT_NAME, dest_node, msg_len);
    return 0;
}

/*
 * ============================================================
 *  CAmkES Lifecycle
 * ============================================================
 */

/* IRQ handler */
void eth_irq_handle(void)
{
    if (!driver_ready) {
        eth_irq_acknowledge();
        return;
    }

    g_drv.irq_count++;
    uint32_t icr = e1000_rd(&g_drv, E1000_ICR);

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        e1000_poll_rx_lwip(&g_drv);
    }

    eth_irq_acknowledge();
}

void pre_init(void)
{
    printf("[%s] pre_init (node_id=%d)\n", COMPONENT_NAME, NODE_ID);
}

void post_init(void)
{
    int error;
    ps_io_ops_t io_ops;

    printf("[%s] Node %d — E1000 + lwIP UDP bridge\n", COMPONENT_NAME, NODE_ID);

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

    /* lwIP init */
    lwip_init();

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, MY_IP_A, MY_IP_B, MY_IP_C, MY_IP_D);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    netif_add(&g_netif, &ipaddr, &netmask, &gw, NULL, e1000_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    printf("[%s] lwIP UP: %s/24\n", COMPONENT_NAME, MY_IP_ADDR);

    /* UDP PCB for DTU transport */
    g_udp_pcb = udp_new();
    if (!g_udp_pcb) {
        printf("[%s] Failed to create UDP PCB\n", COMPONENT_NAME);
        return;
    }
    udp_bind(g_udp_pcb, IP_ANY_TYPE, DTU_UDP_PORT);
    udp_recv(g_udp_pcb, dtu_udp_recv_cb, NULL);

    printf("[%s] UDP bound to port %d\n", COMPONENT_NAME, DTU_UDP_PORT);

    driver_ready = true;
    printf("[%s] Ready\n", COMPONENT_NAME);
}

int run(void)
{
    printf("[%s] Entering main loop\n", COMPONENT_NAME);

    uint32_t loop_count = 0;

    while (1) {
        bool did_work = false;

        /* Poll RX (supplement IRQ-driven receive) */
        if (driver_ready) {
            uint32_t rx_before = g_drv.rx_pkts;
            e1000_poll_rx_lwip(&g_drv);
            if (g_drv.rx_pkts != rx_before) did_work = true;
        }

        /* Pump lwIP timers (ARP, etc.) */
        sys_check_timeouts();

        /* Periodic status */
        loop_count++;
        if ((loop_count % 1000000) == 0) {
            printf("[%s] irq=%u rx=%u tx=%u drop=%u\n",
                   COMPONENT_NAME,
                   g_drv.irq_count, g_drv.rx_pkts,
                   g_drv.tx_pkts, g_drv.rx_dropped);
        }

        if (!did_work) seL4_Yield();
    }

    return 0;
}
