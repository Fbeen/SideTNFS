#ifndef LWIPOPTS_H_
#define LWIPOPTS_H_

// ─── lwIP configuration for SideTNFS ─────────────────────────────────────────
// Bare-metal (no RTOS). DHCP for IP. UDP only (TNFS uses UDP port 16384).

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Memory
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define PBUF_POOL_SIZE              12

// Protocols
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    0
#define LWIP_DNS                    0
#define LWIP_RAW                    0

// Callbacks used by the CYW43 driver
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// DHCP: skip ARP check for speed
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Checksum algorithm
#define LWIP_CHKSUM_ALGORITHM       3

// Stats — disabled to save RAM
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0

// Debug — all off
#define LWIP_DEBUG                  0

#endif // LWIPOPTS_H_
