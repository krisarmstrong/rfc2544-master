/*
 * packet_platform.c - AF_PACKET platform implementation
 *
 * Fallback Linux implementation using AF_PACKET raw sockets.
 * Lower performance than AF_XDP but works on all Linux kernels.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"
#include "platform_config.h"

#if PLATFORM_LINUX

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* worker_ctx_t and rfc2544_ctx_t are defined in rfc2544_internal.h */

typedef struct {
	uint8_t *data;
	uint32_t len;
	uint64_t timestamp;
	uint32_t seq_num;
	void *platform_data;
} packet_t;

/* Platform context */
typedef struct {
	int sock_fd;                /* Raw socket file descriptor */
	int if_index;               /* Interface index */
	uint8_t if_mac[6];          /* Interface MAC address */
	struct sockaddr_ll addr;    /* Socket address */

	/* Packet buffers */
	uint8_t *rx_buffer;
	uint8_t *tx_buffer;
	size_t buffer_size;

	/* TPACKET v3 ring (if available) */
	void *rx_ring;
	void *tx_ring;
	size_t ring_size;

	/* Hardware timestamping */
	bool hw_timestamp_enabled;  /* HW timestamping available */
	bool hw_timestamp_tx;       /* TX hardware timestamps */
	bool hw_timestamp_rx;       /* RX hardware timestamps */
} platform_ctx_t;

#define BUFFER_SIZE 65536

/* Control message buffer for timestamps */
#define CMSG_BUFFER_SIZE 256

/* ============================================================================
 * Hardware Timestamping Setup
 * ============================================================================ */

/**
 * Enable hardware timestamping on the NIC
 * Returns 0 on success, negative on error
 */
static int enable_hw_timestamping(platform_ctx_t *pctx, const char *ifname)
{
	struct ifreq ifr;
	struct hwtstamp_config hwconfig;

	memset(&ifr, 0, sizeof(ifr));
	memset(&hwconfig, 0, sizeof(hwconfig));

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0'; /* Ensure null-termination */

	/* Request hardware TX and RX timestamps */
	hwconfig.tx_type = HWTSTAMP_TX_ON;
	hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;

	ifr.ifr_data = (void *)&hwconfig;

	if (ioctl(pctx->sock_fd, SIOCSHWTSTAMP, &ifr) < 0) {
		/* Hardware timestamping not supported - fall back to software */
		fprintf(stderr, "[packet] HW timestamping not available: %s (using software timestamps)\n",
		        strerror(errno));
		return -1;
	}

	/* Enable socket-level timestamping */
	int timestamping_flags = SOF_TIMESTAMPING_RX_HARDWARE |
	                         SOF_TIMESTAMPING_TX_HARDWARE |
	                         SOF_TIMESTAMPING_RAW_HARDWARE |
	                         SOF_TIMESTAMPING_SOFTWARE |
	                         SOF_TIMESTAMPING_RX_SOFTWARE |
	                         SOF_TIMESTAMPING_TX_SOFTWARE;

	if (setsockopt(pctx->sock_fd, SOL_SOCKET, SO_TIMESTAMPING,
	               &timestamping_flags, sizeof(timestamping_flags)) < 0) {
		fprintf(stderr, "[packet] SO_TIMESTAMPING failed: %s\n", strerror(errno));
		return -1;
	}

	pctx->hw_timestamp_enabled = true;
	pctx->hw_timestamp_tx = (hwconfig.tx_type == HWTSTAMP_TX_ON);
	pctx->hw_timestamp_rx = (hwconfig.rx_filter != HWTSTAMP_FILTER_NONE);

	fprintf(stderr, "[packet] Hardware timestamping enabled (TX=%s, RX=%s)\n",
	        pctx->hw_timestamp_tx ? "yes" : "no",
	        pctx->hw_timestamp_rx ? "yes" : "no");

	return 0;
}

/**
 * Extract timestamp from socket control messages
 * Returns timestamp in nanoseconds, or software timestamp if HW not available
 */
static uint64_t extract_timestamp(struct msghdr *msg, bool prefer_hw)
{
	struct cmsghdr *cmsg;
	uint64_t hw_timestamp = 0;
	uint64_t sw_timestamp = 0;

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
			struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);

			/* ts[0] = software timestamp
			 * ts[1] = deprecated (hw_timestamp transformed)
			 * ts[2] = raw hardware timestamp */
			sw_timestamp = (uint64_t)ts[0].tv_sec * 1000000000ULL + ts[0].tv_nsec;
			hw_timestamp = (uint64_t)ts[2].tv_sec * 1000000000ULL + ts[2].tv_nsec;
		}
	}

	/* Prefer hardware timestamp if available and requested */
	if (prefer_hw && hw_timestamp > 0) {
		return hw_timestamp;
	}

	/* Fall back to software timestamp */
	if (sw_timestamp > 0) {
		return sw_timestamp;
	}

	/* Last resort: get current time */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ============================================================================
 * Platform Operations
 * ============================================================================ */

static int packet_init(rfc2544_ctx_t *ctx, worker_ctx_t *wctx)
{
	platform_ctx_t *pctx = calloc(1, sizeof(platform_ctx_t));
	if (!pctx) {
		return -ENOMEM;
	}

	/* Get interface index */
	pctx->if_index = if_nametoindex(ctx->config.interface);
	if (pctx->if_index == 0) {
		fprintf(stderr, "Failed to get interface index for %s\n", ctx->config.interface);
		free(pctx);
		return -ENODEV;
	}

	/* Create raw socket */
	pctx->sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (pctx->sock_fd < 0) {
		perror("socket");
		free(pctx);
		return -errno;
	}

	/* Bind to interface */
	memset(&pctx->addr, 0, sizeof(pctx->addr));
	pctx->addr.sll_family = AF_PACKET;
	pctx->addr.sll_protocol = htons(ETH_P_ALL);
	pctx->addr.sll_ifindex = pctx->if_index;

	if (bind(pctx->sock_fd, (struct sockaddr *)&pctx->addr, sizeof(pctx->addr)) < 0) {
		perror("bind");
		close(pctx->sock_fd);
		free(pctx);
		return -errno;
	}

	/* Get interface MAC address */
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ctx->config.interface, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0'; /* Ensure null-termination */
	if (ioctl(pctx->sock_fd, SIOCGIFHWADDR, &ifr) < 0) {
		perror("ioctl SIOCGIFHWADDR");
		close(pctx->sock_fd);
		free(pctx);
		return -errno;
	}
	memcpy(pctx->if_mac, ifr.ifr_hwaddr.sa_data, 6);

	/* Set promiscuous mode */
	struct packet_mreq mr;
	memset(&mr, 0, sizeof(mr));
	mr.mr_ifindex = pctx->if_index;
	mr.mr_type = PACKET_MR_PROMISC;
	if (setsockopt(pctx->sock_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
		perror("setsockopt PACKET_ADD_MEMBERSHIP");
		/* Continue anyway - might work without promisc */
	}

	/* Allocate buffers */
	pctx->buffer_size = BUFFER_SIZE;
	pctx->rx_buffer = malloc(pctx->buffer_size);
	pctx->tx_buffer = malloc(pctx->buffer_size);
	if (!pctx->rx_buffer || !pctx->tx_buffer) {
		free(pctx->rx_buffer);
		free(pctx->tx_buffer);
		close(pctx->sock_fd);
		free(pctx);
		return -ENOMEM;
	}

	/* Store context */
	memcpy(ctx->local_mac, pctx->if_mac, 6);
	wctx->pctx = pctx;

	/* Try to enable hardware timestamping if requested */
	if (ctx->config.hw_timestamp) {
		enable_hw_timestamping(pctx, ctx->config.interface);
	}

	fprintf(stderr, "[packet] Initialized on %s (ifindex=%d, MAC=%02x:%02x:%02x:%02x:%02x:%02x, HW-TS=%s)\n",
	        ctx->config.interface, pctx->if_index, pctx->if_mac[0], pctx->if_mac[1],
	        pctx->if_mac[2], pctx->if_mac[3], pctx->if_mac[4], pctx->if_mac[5],
	        pctx->hw_timestamp_enabled ? "enabled" : "disabled");

	return 0;
}

static void packet_cleanup(worker_ctx_t *wctx)
{
	if (!wctx || !wctx->pctx)
		return;

	platform_ctx_t *pctx = wctx->pctx;

	if (pctx->sock_fd >= 0) {
		close(pctx->sock_fd);
	}

	free(pctx->rx_buffer);
	free(pctx->tx_buffer);
	free(pctx);
	wctx->pctx = NULL;
}

static int packet_send_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	if (!wctx || !wctx->pctx || !pkts || count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int sent = 0;

	for (int i = 0; i < count; i++) {
		ssize_t ret = sendto(pctx->sock_fd, pkts[i].data, pkts[i].len, 0,
		                     (struct sockaddr *)&pctx->addr, sizeof(pctx->addr));
		if (ret < 0) {
			wctx->tx_errors++;
			continue;
		}
		sent++;
		wctx->tx_packets++;
		wctx->tx_bytes += pkts[i].len;
	}

	return sent;
}

static int packet_recv_batch(worker_ctx_t *wctx, packet_t *pkts, int max_count)
{
	if (!wctx || !wctx->pctx || !pkts || max_count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int received = 0;

	/* Non-blocking receive with timeout */
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 1000; /* 1ms timeout */
	setsockopt(pctx->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Control message buffer for timestamps */
	char cmsg_buf[CMSG_BUFFER_SIZE];

	for (int i = 0; i < max_count; i++) {
		struct sockaddr_ll from;
		struct iovec iov;
		struct msghdr msg;
		ssize_t ret;

		/* Setup message header for recvmsg (to receive timestamps) */
		iov.iov_base = pctx->rx_buffer;
		iov.iov_len = pctx->buffer_size;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &from;
		msg.msg_namelen = sizeof(from);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cmsg_buf;
		msg.msg_controllen = sizeof(cmsg_buf);

		ret = recvmsg(pctx->sock_fd, &msg, 0);

		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break; /* No more packets */
			}
			wctx->rx_errors++;
			continue;
		}

		/* Skip outgoing packets */
		if (from.sll_pkttype == PACKET_OUTGOING) {
			continue;
		}

		/* Get timestamp (prefer HW if available) */
		uint64_t timestamp;
		if (pctx->hw_timestamp_enabled) {
			timestamp = extract_timestamp(&msg, pctx->hw_timestamp_rx);
		} else {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
		}

		/* Fill packet structure */
		pkts[received].data = malloc(ret);
		if (pkts[received].data) {
			memcpy(pkts[received].data, pctx->rx_buffer, ret);
			pkts[received].len = ret;
			pkts[received].timestamp = timestamp;
			received++;
			wctx->rx_packets++;
			wctx->rx_bytes += ret;
		}
	}

	return received;
}

static void packet_release_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	(void)wctx;
	for (int i = 0; i < count; i++) {
		free(pkts[i].data);
		pkts[i].data = NULL;
	}
}

static uint64_t packet_get_tx_timestamp(worker_ctx_t *wctx, packet_t *pkt)
{
	(void)wctx;
	return pkt->timestamp;
}

static uint64_t packet_get_rx_timestamp(worker_ctx_t *wctx, packet_t *pkt)
{
	(void)wctx;
	return pkt->timestamp;
}

/* Platform ops structure */
static const struct {
	const char *name;
	int (*init)(rfc2544_ctx_t *ctx, worker_ctx_t *wctx);
	void (*cleanup)(worker_ctx_t *wctx);
	int (*send_batch)(worker_ctx_t *wctx, packet_t *pkts, int count);
	int (*recv_batch)(worker_ctx_t *wctx, packet_t *pkts, int max_count);
	void (*release_batch)(worker_ctx_t *wctx, packet_t *pkts, int count);
	uint64_t (*get_tx_timestamp)(worker_ctx_t *wctx, packet_t *pkt);
	uint64_t (*get_rx_timestamp)(worker_ctx_t *wctx, packet_t *pkt);
} packet_ops = {
    .name = "AF_PACKET",
    .init = packet_init,
    .cleanup = packet_cleanup,
    .send_batch = packet_send_batch,
    .recv_batch = packet_recv_batch,
    .release_batch = packet_release_batch,
    .get_tx_timestamp = packet_get_tx_timestamp,
    .get_rx_timestamp = packet_get_rx_timestamp,
};

const void *get_packet_platform_ops(void)
{
	return &packet_ops;
}

#endif /* PLATFORM_LINUX */
