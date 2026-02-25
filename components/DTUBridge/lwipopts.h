/*
 * lwipopts.h — lwIP configuration for DTUBridge (UDP-only)
 *
 * Minimal lwIP config for DTU message transport over UDP.
 * Based on http_gateway_x86 E1000Driver config, with TCP disabled.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ---------- Core / OS ---------- */
#define NO_SYS                      1       /* No OS threading */
#define LWIP_TIMERS                 1       /* Enable timers for ARP */
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0       /* Disable netconn API */
#define LWIP_SOCKET                 0       /* Disable socket API */
#define LWIP_RAND                   rand

/* ---------- Memory ---------- */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (256 * 1024)  /* 256 KiB (UDP only) */
#define MEM_LIBC_MALLOC             0

/* ---------- Pool sizes ---------- */
#define MEMP_NUM_PBUF               32
#define PBUF_POOL_SIZE              64
#define PBUF_POOL_BUFSIZE           2048

/* TCP disabled */
#define MEMP_NUM_TCP_PCB            0
#define MEMP_NUM_TCP_PCB_LISTEN     0
#define MEMP_NUM_TCP_SEG            0

/* UDP pools */
#define MEMP_NUM_UDP_PCB            4

/* Other pools */
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_ARP_QUEUE          16
#define MEMP_NUM_NETBUF             0
#define MEMP_NUM_NETCONN            0

/* ---------- Protocol configuration ---------- */
#define LWIP_ARP                    1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1       /* Enable ping for testing */
#define LWIP_UDP                    1
#define LWIP_TCP                    0       /* TCP disabled — UDP only */
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    0

/* ---------- Checksum ---------- */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            0
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          0

/* ---------- Stats / Debug ---------- */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* ---------- Netif ---------- */
#define LWIP_NETIF_STATUS_CALLBACK  1

#endif /* LWIPOPTS_H */
