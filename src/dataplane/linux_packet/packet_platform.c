/*
 * packet_platform.c - AF_PACKET platform implementation
 *
 * Fallback Linux implementation using AF_PACKET raw sockets.
 * Lower performance than AF_XDP but works on all Linux kernels.
 */

#include "rfc2544.h"
#include "platform_config.h"

#if PLATFORM_LINUX

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Forward declarations */
typedef struct rfc2544_ctx rfc2544_ctx_t;
typedef struct {
	int worker_id;
	int queue_id;
	void *pctx;
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_errors;
	uint64_t rx_errors;
} worker_ctx_t;

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
} platform_ctx_t;

#define BUFFER_SIZE 65536

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
	strncpy(ifr.ifr_name, ctx->config.interface, IFNAMSIZ - 1);
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

	fprintf(stderr, "[packet] Initialized on %s (ifindex=%d, MAC=%02x:%02x:%02x:%02x:%02x:%02x)\n",
	        ctx->config.interface, pctx->if_index, pctx->if_mac[0], pctx->if_mac[1],
	        pctx->if_mac[2], pctx->if_mac[3], pctx->if_mac[4], pctx->if_mac[5]);

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

	for (int i = 0; i < max_count; i++) {
		struct sockaddr_ll from;
		socklen_t fromlen = sizeof(from);

		ssize_t ret = recvfrom(pctx->sock_fd, pctx->rx_buffer, pctx->buffer_size, 0,
		                       (struct sockaddr *)&from, &fromlen);

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

		/* Get timestamp */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);

		/* Fill packet structure */
		pkts[received].data = malloc(ret);
		if (pkts[received].data) {
			memcpy(pkts[received].data, pctx->rx_buffer, ret);
			pkts[received].len = ret;
			pkts[received].timestamp = ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
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
