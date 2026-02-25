/*
 * e1000_hw.h - Intel 82540EM (e1000) Register Definitions
 *
 * Standalone driver for QEMU's -device e1000 which emulates the Intel 82540EM.
 * This uses legacy INTx interrupts (no MSI-X), avoiding seL4 interrupt issues.
 *
 * Based on Zephyr's eth_e1000_priv.h (Apache 2.0 License)
 * Reference: Intel 82540EM (e1000) PRM
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef E1000_HW_H
#define E1000_HW_H

#include <stdint.h>

/*
 * Register Offsets for Intel 82540EM
 *
 * Note: 82540EM uses different offsets than 82574 (e1000e).
 * Key differences from 82574:
 *   - No MSI-X support (legacy interrupts only)
 *   - Simpler interrupt handling (single ICR)
 *   - 256-byte aligned descriptor rings
 */

/* Device Control and Status */
#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */

/* Interrupt Registers */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read (auto-clear on read) */
#define E1000_ITR       0x00C4  /* Interrupt Throttling Rate */
#define E1000_ICS       0x00C8  /* Interrupt Cause Set */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set/Read */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */

/* Receive Control */
#define E1000_RCTL      0x0100  /* RX Control */

/* Transmit Control */
#define E1000_TCTL      0x0400  /* TX Control */

/* RX Descriptor Ring */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Address Low */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base Address High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Length (bytes, multiple of 128) */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */

/* TX Descriptor Ring */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Address Low */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base Address High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Length (bytes, multiple of 128) */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */

/* Receive Address (MAC address) */
#define E1000_RAL       0x5400  /* Receive Address Low */
#define E1000_RAH       0x5404  /* Receive Address High */

/* Multicast Table Array (128 entries × 4 bytes = 512 bytes) */
#define E1000_MTA       0x5200  /* Multicast Table Array base */
#define E1000_MTA_SIZE  128     /* Number of MTA entries */

/* EEPROM (for MAC address fallback) */
#define E1000_EERD      0x0014  /* EEPROM Read */

/* RX Descriptor Control Registers */
#define E1000_RDTR      0x2820  /* RX Delay Timer */
#define E1000_RXDCTL    0x2828  /* RX Descriptor Control */
#define E1000_RADV      0x282C  /* RX Interrupt Absolute Delay Timer */

/* TX Descriptor Control Registers */
#define E1000_TXDCTL    0x3828  /* TX Descriptor Control */
#define E1000_TADV      0x382C  /* TX Interrupt Absolute Delay Timer */
#define E1000_TIPG      0x0410  /* TX Inter Packet Gap */

/* Statistics Registers (useful for debugging RX issues) */
#define E1000_CRCERRS   0x4000  /* CRC Error Count */
#define E1000_ALGNERRC  0x4004  /* Alignment Error Count */
#define E1000_SYMERRS   0x4008  /* Symbol Error Count */
#define E1000_RXERRC    0x400C  /* RX Error Count */
#define E1000_MPC       0x4010  /* Missed Packets Count */
#define E1000_SCC       0x4014  /* Single Collision Count */
#define E1000_ECOL      0x4018  /* Excessive Collision Count */
#define E1000_MCC       0x401C  /* Multiple Collision Count */
#define E1000_LATECOL   0x4020  /* Late Collision Count */
#define E1000_COLC      0x4028  /* Collision Count */
#define E1000_DC        0x4030  /* Defer Count */
#define E1000_TNCRS     0x4034  /* TX No CRS */
#define E1000_SEC       0x4038  /* Sequence Error Count */
#define E1000_CEXTERR   0x403C  /* Carrier Extension Error Count */
#define E1000_RLEC      0x4040  /* Receive Length Error Count */
#define E1000_XONRXC    0x4048  /* XON Received Count */
#define E1000_XONTXC    0x404C  /* XON Transmitted Count */
#define E1000_XOFFRXC   0x4050  /* XOFF Received Count */
#define E1000_XOFFTXC   0x4054  /* XOFF Transmitted Count */
#define E1000_FCRUC     0x4058  /* FC Received Unsupported Count */
#define E1000_PRC64     0x405C  /* Packets RX (64 bytes) Count */
#define E1000_PRC127    0x4060  /* Packets RX (65-127 bytes) */
#define E1000_PRC255    0x4064  /* Packets RX (128-255 bytes) */
#define E1000_PRC511    0x4068  /* Packets RX (256-511 bytes) */
#define E1000_PRC1023   0x406C  /* Packets RX (512-1023 bytes) */
#define E1000_PRC1522   0x4070  /* Packets RX (1024-1522 bytes) */
#define E1000_GPRC      0x4074  /* Good Packets Received Count */
#define E1000_BPRC      0x4078  /* Broadcast Packets RX Count */
#define E1000_MPRC      0x407C  /* Multicast Packets RX Count */
#define E1000_GPTC      0x4080  /* Good Packets Transmitted Count */
#define E1000_GORCL     0x4088  /* Good Octets RX Count Low */
#define E1000_GORCH     0x408C  /* Good Octets RX Count High */
#define E1000_GOTCL     0x4090  /* Good Octets TX Count Low */
#define E1000_GOTCH     0x4094  /* Good Octets TX Count High */
#define E1000_RUC       0x40A4  /* Receive Undersize Count */
#define E1000_RFC       0x40A8  /* Receive Fragment Count */
#define E1000_ROC       0x40AC  /* Receive Oversize Count */
#define E1000_RNBC      0x40A0  /* Receive No Buffers Count */
#define E1000_RJC       0x40B0  /* Receive Jabber Count */
#define E1000_MGTPRC    0x40B4  /* Management Packets RX Count */
#define E1000_MGTPDC    0x40B8  /* Management Packets Dropped Count */
#define E1000_MGTPTC    0x40BC  /* Management Packets TX Count */
#define E1000_TORL      0x40C0  /* Total Octets RX Low */
#define E1000_TORH      0x40C4  /* Total Octets RX High */
#define E1000_TOTL      0x40C8  /* Total Octets TX Low */
#define E1000_TOTH      0x40CC  /* Total Octets TX High */
#define E1000_TPR       0x40D0  /* Total Packets Received */
#define E1000_TPT       0x40D4  /* Total Packets Transmitted */

/*
 * Control Register (CTRL) Bits
 */
#define E1000_CTRL_FD       (1 << 0)   /* Full Duplex */
#define E1000_CTRL_LRST     (1 << 3)   /* Link Reset */
#define E1000_CTRL_ASDE     (1 << 5)   /* Auto-Speed Detection Enable */
#define E1000_CTRL_SLU      (1 << 6)   /* Set Link Up */
#define E1000_CTRL_ILOS     (1 << 7)   /* Invert Loss-of-Signal */
#define E1000_CTRL_RST      (1 << 26)  /* Device Reset */
#define E1000_CTRL_VME      (1 << 30)  /* VLAN Mode Enable */
#define E1000_CTRL_PHY_RST  (1 << 31)  /* PHY Reset */

/*
 * Status Register (STATUS) Bits
 */
#define E1000_STATUS_FD     (1 << 0)   /* Full Duplex */
#define E1000_STATUS_LU     (1 << 1)   /* Link Up */
#define E1000_STATUS_TXOFF  (1 << 4)   /* Transmission Paused */
#define E1000_STATUS_SPEED_MASK  (3 << 6)
#define E1000_STATUS_SPEED_10    (0 << 6)
#define E1000_STATUS_SPEED_100   (1 << 6)
#define E1000_STATUS_SPEED_1000  (2 << 6)

/*
 * Interrupt Cause Read (ICR) Bits
 * Reading ICR clears all bits (auto-clear)
 */
#define E1000_ICR_TXDW      (1 << 0)   /* Transmit Descriptor Written Back */
#define E1000_ICR_TXQE      (1 << 1)   /* Transmit Queue Empty */
#define E1000_ICR_LSC       (1 << 2)   /* Link Status Change */
#define E1000_ICR_RXSEQ     (1 << 3)   /* RX Sequence Error */
#define E1000_ICR_RXDMT0    (1 << 4)   /* RX Descriptor Minimum Threshold */
#define E1000_ICR_RXO       (1 << 6)   /* RX FIFO Overrun */
#define E1000_ICR_RXT0      (1 << 7)   /* RX Timer Interrupt */

/*
 * Interrupt Mask Set (IMS) Bits
 * Same bit positions as ICR
 */
#define E1000_IMS_TXDW      (1 << 0)
#define E1000_IMS_TXQE      (1 << 1)
#define E1000_IMS_LSC       (1 << 2)
#define E1000_IMS_RXSEQ     (1 << 3)
#define E1000_IMS_RXDMT0    (1 << 4)
#define E1000_IMS_RXO       (1 << 6)
#define E1000_IMS_RXT0      (1 << 7)

/*
 * Receive Control (RCTL) Bits
 */
#define E1000_RCTL_EN       (1 << 1)   /* Receiver Enable */
#define E1000_RCTL_SBP      (1 << 2)   /* Store Bad Packets */
#define E1000_RCTL_UPE      (1 << 3)   /* Unicast Promiscuous Enable */
#define E1000_RCTL_MPE      (1 << 4)   /* Multicast Promiscuous Enable */
#define E1000_RCTL_LPE      (1 << 5)   /* Long Packet Enable */
#define E1000_RCTL_LBM_MASK (3 << 6)   /* Loopback Mode */
#define E1000_RCTL_LBM_NO   (0 << 6)   /* No Loopback */
#define E1000_RCTL_RDMTS_HALF  (0 << 8)  /* RX Desc Min Threshold: 1/2 */
#define E1000_RCTL_RDMTS_QRTR  (1 << 8)  /* RX Desc Min Threshold: 1/4 */
#define E1000_RCTL_RDMTS_EIGTH (2 << 8)  /* RX Desc Min Threshold: 1/8 */
#define E1000_RCTL_MO_SHIFT    12        /* Multicast Offset */
#define E1000_RCTL_BAM      (1 << 15)  /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_MASK  (3 << 16)
#define E1000_RCTL_BSIZE_2048  (0 << 16) /* Buffer Size 2048 */
#define E1000_RCTL_BSIZE_1024  (1 << 16) /* Buffer Size 1024 */
#define E1000_RCTL_BSIZE_512   (2 << 16) /* Buffer Size 512 */
#define E1000_RCTL_BSIZE_256   (3 << 16) /* Buffer Size 256 */
#define E1000_RCTL_VFE      (1 << 18)  /* VLAN Filter Enable */
#define E1000_RCTL_BSEX     (1 << 25)  /* Buffer Size Extension */
#define E1000_RCTL_SECRC    (1 << 26)  /* Strip Ethernet CRC */

/*
 * Transmit Control (TCTL) Bits
 */
#define E1000_TCTL_EN       (1 << 1)   /* Transmit Enable */
#define E1000_TCTL_PSP      (1 << 3)   /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT 4          /* Collision Threshold */
#define E1000_TCTL_COLD_SHIFT 12       /* Collision Distance */
#define E1000_TCTL_SWXOFF   (1 << 22)  /* Software XOFF Transmission */

/* Standard TCTL values for full-duplex gigabit */
#define E1000_TCTL_CT       (0x0F << E1000_TCTL_CT_SHIFT)   /* Collision Threshold */
#define E1000_TCTL_COLD     (0x40 << E1000_TCTL_COLD_SHIFT) /* Collision Distance (FD) */

/*
 * Receive Address High (RAH) Bits
 */
#define E1000_RAH_AV        (1 << 31)  /* Address Valid */

/*
 * EEPROM Read (EERD) Bits
 */
#define E1000_EERD_START    (1 << 0)   /* Start Read */
#define E1000_EERD_DONE     (1 << 4)   /* Read Done */
#define E1000_EERD_ADDR_SHIFT 8        /* Address Shift */
#define E1000_EERD_DATA_SHIFT 16       /* Data Shift */

/*
 * Legacy RX Descriptor
 *
 * 16 bytes, must be 16-byte aligned
 * Hardware writes length, status, errors, special on receive
 */
struct e1000_rx_desc {
    uint64_t addr;      /* Buffer address (physical) */
    uint16_t length;    /* Received frame length */
    uint16_t checksum;  /* Packet checksum */
    uint8_t  status;    /* Descriptor status */
    uint8_t  errors;    /* Descriptor errors */
    uint16_t special;   /* VLAN tag */
} __attribute__((packed));

/* RX Descriptor Status Bits */
#define E1000_RXD_STAT_DD   (1 << 0)   /* Descriptor Done */
#define E1000_RXD_STAT_EOP  (1 << 1)   /* End of Packet */
#define E1000_RXD_STAT_IXSM (1 << 2)   /* Ignore Checksum */
#define E1000_RXD_STAT_VP   (1 << 3)   /* VLAN Packet */
#define E1000_RXD_STAT_TCPCS (1 << 5)  /* TCP Checksum Calculated */
#define E1000_RXD_STAT_IPCS (1 << 6)   /* IP Checksum Calculated */
#define E1000_RXD_STAT_PIF  (1 << 7)   /* Passed In-exact Filter */

/* RX Descriptor Error Bits */
#define E1000_RXD_ERR_CE    (1 << 0)   /* CRC Error */
#define E1000_RXD_ERR_SE    (1 << 1)   /* Symbol Error */
#define E1000_RXD_ERR_SEQ   (1 << 2)   /* Sequence Error */
#define E1000_RXD_ERR_RXE   (1 << 7)   /* RX Data Error */

/*
 * Legacy TX Descriptor
 *
 * 16 bytes, must be 16-byte aligned
 * Software sets cmd, hardware updates status on completion
 */
struct e1000_tx_desc {
    uint64_t addr;      /* Buffer address (physical) */
    uint16_t length;    /* Frame length */
    uint8_t  cso;       /* Checksum Offset */
    uint8_t  cmd;       /* Command field */
    uint8_t  status;    /* Status field */
    uint8_t  css;       /* Checksum Start */
    uint16_t special;   /* VLAN tag */
} __attribute__((packed));

/* TX Descriptor Command Bits */
#define E1000_TXD_CMD_EOP   (1 << 0)   /* End of Packet */
#define E1000_TXD_CMD_IFCS  (1 << 1)   /* Insert FCS (CRC) */
#define E1000_TXD_CMD_IC    (1 << 2)   /* Insert Checksum */
#define E1000_TXD_CMD_RS    (1 << 3)   /* Report Status */
#define E1000_TXD_CMD_RPS   (1 << 4)   /* Report Packet Sent */
#define E1000_TXD_CMD_DEXT  (1 << 5)   /* Descriptor Extension (context) */
#define E1000_TXD_CMD_VLE   (1 << 6)   /* VLAN Packet Enable */
#define E1000_TXD_CMD_IDE   (1 << 7)   /* Interrupt Delay Enable */

/* TX Descriptor Status Bits */
#define E1000_TXD_STAT_DD   (1 << 0)   /* Descriptor Done */
#define E1000_TXD_STAT_EC   (1 << 1)   /* Excess Collisions */
#define E1000_TXD_STAT_LC   (1 << 2)   /* Late Collision */
#define E1000_TXD_STAT_TU   (1 << 3)   /* Transmit Underrun */

/*
 * Driver Configuration Constants
 */
#define E1000_NUM_RX_DESC   64          /* Number of RX descriptors (power of 2) */
#define E1000_NUM_TX_DESC   64          /* Number of TX descriptors (power of 2) */
#define E1000_RX_BUF_SIZE   2048        /* RX buffer size */
#define E1000_TX_BUF_SIZE   2048        /* TX buffer size */

#define E1000_DESC_ALIGN    128         /* Descriptor ring alignment (82540EM) */
#define E1000_BUF_ALIGN     16          /* Buffer alignment */

/*
 * Memory barrier for x86
 */
#define E1000_MFENCE()  __asm__ volatile("mfence" ::: "memory")
#define E1000_SFENCE()  __asm__ volatile("sfence" ::: "memory")
#define E1000_LFENCE()  __asm__ volatile("lfence" ::: "memory")

/*
 * Register access macros
 *
 * Note: These operate on the MMIO base pointer provided by CAmkES
 */
static inline uint32_t e1000_read_reg(volatile void *base, uint32_t reg)
{
    return *(volatile uint32_t *)((uintptr_t)base + reg);
}

static inline void e1000_write_reg(volatile void *base, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)base + reg) = val;
}

#endif /* E1000_HW_H */
