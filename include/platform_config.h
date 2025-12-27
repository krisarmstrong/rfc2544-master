/*
 * platform_config.h - Platform detection for RFC2544 Test Master
 *
 * Detects available packet I/O mechanisms:
 * - AF_XDP (Linux, high performance)
 * - AF_PACKET (Linux, fallback)
 * - DPDK (Linux, line-rate)
 */

#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

#ifdef __linux__
#define PLATFORM_LINUX 1
#define PLATFORM_NAME "linux"
#else
#define PLATFORM_LINUX 0
#endif

#ifdef __APPLE__
#define PLATFORM_MACOS 1
#define PLATFORM_NAME "macos"
#else
#define PLATFORM_MACOS 0
#endif

/* ============================================================================
 * Linux: AF_XDP Detection (requires kernel 5.4+ for good performance)
 * ============================================================================ */

#ifdef __linux__
#ifdef __has_include
#if __has_include(<linux/if_xdp.h>)
#define HAVE_AF_XDP 1
#else
#define HAVE_AF_XDP 0
#endif
#else
/* Fallback for older compilers */
#define HAVE_AF_XDP 0
#endif
#else
#define HAVE_AF_XDP 0
#endif

/* ============================================================================
 * Linux: DPDK Detection
 * ============================================================================ */

#ifdef __linux__
#ifdef __has_include
#if __has_include(<rte_eal.h>)
#define HAVE_DPDK 1
#else
#define HAVE_DPDK 0
#endif
#else
#define HAVE_DPDK 0
#endif
#else
#define HAVE_DPDK 0
#endif

/* ============================================================================
 * Hardware Timestamping Detection
 * ============================================================================ */

#ifdef __linux__
#ifdef __has_include
#if __has_include(<linux/net_tstamp.h>)
#define HAVE_HW_TIMESTAMP 1
#else
#define HAVE_HW_TIMESTAMP 0
#endif
#else
#define HAVE_HW_TIMESTAMP 0
#endif
#else
#define HAVE_HW_TIMESTAMP 0
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Default batch sizes */
#define DEFAULT_BATCH_SIZE 64
#define DEFAULT_RX_RING_SIZE 4096
#define DEFAULT_TX_RING_SIZE 4096

/* XDP-specific */
#define XDP_FRAME_SIZE 4096
#define XDP_HEADROOM 256

/* Timing */
#define NS_PER_SEC 1000000000ULL
#define US_PER_SEC 1000000ULL
#define MS_PER_SEC 1000ULL

/* Ethernet */
#define ETH_HEADER_LEN 14
#define ETH_FCS_LEN 4
#define ETH_MIN_FRAME 64
#define ETH_MAX_FRAME 1518
#define ETH_JUMBO_FRAME 9000

/* IP/UDP */
#define IP_HEADER_LEN 20
#define UDP_HEADER_LEN 8
#define MIN_PAYLOAD_SIZE 18 /* Minimum to avoid runt frames */

/* Protocol numbers - only define if not already available from system headers */
#if !defined(__linux__) && !defined(IPPROTO_UDP)
#define IPPROTO_UDP 17
#endif
#if !defined(__linux__) && !defined(ETH_P_IP)
#define ETH_P_IP 0x0800
#endif

/* ============================================================================
 * Performance Targets
 * ============================================================================
 *
 * Platform         Target Rate    Use Case
 * --------         -----------    --------
 * AF_PACKET        ~100 Mbps      Testing, development
 * AF_XDP           ~40 Gbps       Production (10G-40G)
 * DPDK             100+ Gbps      Line-rate (100G+)
 */

#endif /* PLATFORM_CONFIG_H */
