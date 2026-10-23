// lwIP mínimo para sys-GRID0: sin SO, sin sockets/netconn, solo IPv4 + TCP (API raw).
// Presupuesto de memoria estático: heap 24 KB + pools de memp/pbuf (ver TCP_PLAN.md).
#pragma once
#include <stdint.h>
#define NO_SYS 1
#define LWIP_TIMERS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_ICMP 0
#define LWIP_RAW 0
#define LWIP_UDP 0
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_TCP 1
#define LWIP_CALLBACK_API 1
#define LWIP_STATS 0
#define LWIP_NETIF_LOOPBACK 0
#define IP_FORWARD 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_GEN_IP 1
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 8
#ifdef ZTNX_PERF_BASELINE
/* Valores de la versión anterior (sys-GRID0-tcp.pasv.nsp); solo para comparar en tests/test_perf. */
#define MEM_SIZE (16 * 1024)
#else
/* Cada segmento en vuelo ocupa un pbuf RAM de ~1.42 KB: 8 por conexión de datos = ~11.4 KB. */
#define MEM_SIZE (28 * 1024)
#endif
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_PCB_LISTEN 4
#ifdef ZTNX_PERF_BASELINE
#define MEMP_NUM_TCP_SEG 12
#else
#define MEMP_NUM_TCP_SEG 24
#endif
#define MEMP_NUM_PBUF 8
#ifdef ZTNX_PERF_BASELINE
#define PBUF_POOL_SIZE 10
#else
/* Ventana de recepción de 8 segmentos (cada uno ocupa un pbuf del pool) + holgura de entrada. */
#define PBUF_POOL_SIZE 14
#endif
#define PBUF_POOL_BUFSIZE 1536
#define TCP_MSS 1360
#ifdef ZTNX_PERF_BASELINE
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN 12
#define TCP_QUEUE_OOSEQ 0
#else
/* Con RTT de 40-100 ms (Wi-Fi), 4 segmentos limitan a ~55-140 KB/s; 8 duplican el techo. */
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN 20
/* Segmentos fuera de orden: ZeroTier puede reordenar y cada pérdida costaba un RTO de >=1 s. */
#define TCP_QUEUE_OOSEQ 1
/* Timers de 100 ms (por defecto 250): ACK retardado y RTO con granularidad de 200 ms (antes 500). */
#define TCP_TMR_INTERVAL 100
#endif
#define LWIP_TCP_SACK_OUT 0
// Keepalive: un cliente FTP que desaparece no debe ocupar para siempre una de las 4 ranuras.
#define LWIP_TCP_KEEPALIVE 1
#define TCP_KEEPIDLE_DEFAULT 120000UL
#define TCP_KEEPINTVL_DEFAULT 15000UL
#define TCP_KEEPCNT_DEFAULT 4U
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0
#ifdef __cplusplus
extern "C" {
#endif
uint32_t ztnx_lwip_now(void);
uint32_t ztnx_lwip_rand(void);
#ifdef __cplusplus
}
#endif
#define LWIP_RAND() ztnx_lwip_rand()
