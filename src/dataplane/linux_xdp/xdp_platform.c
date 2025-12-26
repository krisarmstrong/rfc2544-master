/*
 * xdp_platform.c - AF_XDP platform implementation for RFC2544 Test Master
 *
 * High-performance packet I/O using AF_XDP (eXpress Data Path).
 * Requires Linux kernel 5.4+ for best performance.
 *
 * Performance target: 10-40 Gbps
 */

#include "rfc2544.h"
#include "platform_config.h"

#if HAVE_AF_XDP

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

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

/* XDP configuration */
#define NUM_FRAMES 4096
#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define BATCH_SIZE 64
#define INVALID_UMEM_FRAME UINT64_MAX

/* UMEM frame tracking */
typedef struct {
	uint64_t *frames;
	uint32_t head;
	uint32_t tail;
	uint32_t size;
} frame_allocator_t;

/* Platform context */
typedef struct {
	/* XDP socket */
	struct xsk_socket *xsk;
	struct xsk_umem *umem;
	struct xsk_ring_prod fill_ring;
	struct xsk_ring_cons comp_ring;
	struct xsk_ring_prod tx_ring;
	struct xsk_ring_cons rx_ring;

	/* UMEM */
	void *umem_area;
	size_t umem_size;
	frame_allocator_t frame_alloc;

	/* Interface info */
	int if_index;
	uint8_t if_mac[6];
	int xsk_fd;

	/* BPF program (optional, for multi-queue) */
	struct bpf_object *bpf_obj;
	int bpf_prog_fd;

	/* Statistics */
	uint64_t tx_wakeups;
	uint64_t rx_wakeups;
} platform_ctx_t;

/* ============================================================================
 * Frame Allocator
 * ============================================================================ */

static int frame_alloc_init(frame_allocator_t *alloc, uint32_t num_frames)
{
	alloc->frames = calloc(num_frames, sizeof(uint64_t));
	if (!alloc->frames)
		return -ENOMEM;

	alloc->size = num_frames;
	alloc->head = 0;
	alloc->tail = num_frames;

	/* Initialize with frame addresses */
	for (uint32_t i = 0; i < num_frames; i++) {
		alloc->frames[i] = i * FRAME_SIZE;
	}

	return 0;
}

static void frame_alloc_cleanup(frame_allocator_t *alloc)
{
	free(alloc->frames);
	alloc->frames = NULL;
}

static uint64_t frame_alloc_get(frame_allocator_t *alloc)
{
	if (alloc->head == alloc->tail)
		return INVALID_UMEM_FRAME;

	uint64_t frame = alloc->frames[alloc->head];
	alloc->head = (alloc->head + 1) % alloc->size;
	return frame;
}

static void frame_alloc_put(frame_allocator_t *alloc, uint64_t frame)
{
	alloc->frames[alloc->tail] = frame;
	alloc->tail = (alloc->tail + 1) % alloc->size;
}

/* ============================================================================
 * XDP Platform Operations
 * ============================================================================ */

static int xdp_init(rfc2544_ctx_t *ctx, worker_ctx_t *wctx)
{
	platform_ctx_t *pctx = calloc(1, sizeof(platform_ctx_t));
	if (!pctx)
		return -ENOMEM;

	int ret;

	/* Get interface index */
	pctx->if_index = if_nametoindex(ctx->config.interface);
	if (pctx->if_index == 0) {
		fprintf(stderr, "[xdp] Failed to get interface index for %s\n",
		        ctx->config.interface);
		free(pctx);
		return -ENODEV;
	}

	/* Increase RLIMIT_MEMLOCK for UMEM */
	struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
	if (setrlimit(RLIMIT_MEMLOCK, &rlim)) {
		fprintf(stderr, "[xdp] Warning: Failed to increase RLIMIT_MEMLOCK\n");
	}

	/* Allocate UMEM */
	pctx->umem_size = NUM_FRAMES * FRAME_SIZE;
	pctx->umem_area = mmap(NULL, pctx->umem_size, PROT_READ | PROT_WRITE,
	                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (pctx->umem_area == MAP_FAILED) {
		/* Fall back to regular pages */
		pctx->umem_area = mmap(NULL, pctx->umem_size, PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pctx->umem_area == MAP_FAILED) {
			fprintf(stderr, "[xdp] Failed to allocate UMEM\n");
			free(pctx);
			return -ENOMEM;
		}
	}

	/* Initialize frame allocator */
	ret = frame_alloc_init(&pctx->frame_alloc, NUM_FRAMES);
	if (ret < 0) {
		munmap(pctx->umem_area, pctx->umem_size);
		free(pctx);
		return ret;
	}

	/* Create UMEM */
	struct xsk_umem_config umem_cfg = {
	    .fill_size = NUM_FRAMES / 2,
	    .comp_size = NUM_FRAMES / 2,
	    .frame_size = FRAME_SIZE,
	    .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
	    .flags = 0,
	};

	ret = xsk_umem__create(&pctx->umem, pctx->umem_area, pctx->umem_size,
	                       &pctx->fill_ring, &pctx->comp_ring, &umem_cfg);
	if (ret) {
		fprintf(stderr, "[xdp] Failed to create UMEM: %s\n", strerror(-ret));
		frame_alloc_cleanup(&pctx->frame_alloc);
		munmap(pctx->umem_area, pctx->umem_size);
		free(pctx);
		return ret;
	}

	/* Create XDP socket */
	struct xsk_socket_config xsk_cfg = {
	    .rx_size = NUM_FRAMES / 2,
	    .tx_size = NUM_FRAMES / 2,
	    .xdp_flags = XDP_FLAGS_DRV_MODE, /* Try native mode first */
	    .bind_flags = XDP_USE_NEED_WAKEUP,
	    .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
	};

	ret = xsk_socket__create(&pctx->xsk, ctx->config.interface, wctx->queue_id,
	                         pctx->umem, &pctx->rx_ring, &pctx->tx_ring, &xsk_cfg);

	if (ret) {
		/* Fall back to SKB mode */
		xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
		ret = xsk_socket__create(&pctx->xsk, ctx->config.interface, wctx->queue_id,
		                         pctx->umem, &pctx->rx_ring, &pctx->tx_ring, &xsk_cfg);
		if (ret) {
			fprintf(stderr, "[xdp] Failed to create XDP socket: %s\n", strerror(-ret));
			xsk_umem__delete(pctx->umem);
			frame_alloc_cleanup(&pctx->frame_alloc);
			munmap(pctx->umem_area, pctx->umem_size);
			free(pctx);
			return ret;
		}
		fprintf(stderr, "[xdp] Using SKB mode (lower performance)\n");
	}

	pctx->xsk_fd = xsk_socket__fd(pctx->xsk);

	/* Populate fill ring */
	uint32_t idx;
	ret = xsk_ring_prod__reserve(&pctx->fill_ring, NUM_FRAMES / 2, &idx);
	if (ret != NUM_FRAMES / 2) {
		fprintf(stderr, "[xdp] Failed to populate fill ring\n");
		xsk_socket__delete(pctx->xsk);
		xsk_umem__delete(pctx->umem);
		frame_alloc_cleanup(&pctx->frame_alloc);
		munmap(pctx->umem_area, pctx->umem_size);
		free(pctx);
		return -ENOSPC;
	}

	for (uint32_t i = 0; i < NUM_FRAMES / 2; i++) {
		*xsk_ring_prod__fill_addr(&pctx->fill_ring, idx++) = frame_alloc_get(&pctx->frame_alloc);
	}
	xsk_ring_prod__submit(&pctx->fill_ring, NUM_FRAMES / 2);

	/* Get interface MAC */
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock >= 0) {
		struct ifreq ifr;
		strncpy(ifr.ifr_name, ctx->config.interface, IFNAMSIZ - 1);
		if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
			memcpy(pctx->if_mac, ifr.ifr_hwaddr.sa_data, 6);
			memcpy(ctx->local_mac, pctx->if_mac, 6);
		}
		close(sock);
	}

	wctx->pctx = pctx;

	fprintf(stderr, "[xdp] Initialized on %s queue %d (fd=%d)\n",
	        ctx->config.interface, wctx->queue_id, pctx->xsk_fd);

	return 0;
}

static void xdp_cleanup(worker_ctx_t *wctx)
{
	if (!wctx || !wctx->pctx)
		return;

	platform_ctx_t *pctx = wctx->pctx;

	if (pctx->xsk)
		xsk_socket__delete(pctx->xsk);
	if (pctx->umem)
		xsk_umem__delete(pctx->umem);
	if (pctx->umem_area)
		munmap(pctx->umem_area, pctx->umem_size);

	frame_alloc_cleanup(&pctx->frame_alloc);
	free(pctx);
	wctx->pctx = NULL;
}

static int xdp_send_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	if (!wctx || !wctx->pctx || !pkts || count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int sent = 0;

	/* Complete any pending TX */
	uint32_t idx_comp;
	unsigned int completed = xsk_ring_cons__peek(&pctx->comp_ring, BATCH_SIZE, &idx_comp);
	if (completed > 0) {
		for (unsigned int i = 0; i < completed; i++) {
			uint64_t addr = *xsk_ring_cons__comp_addr(&pctx->comp_ring, idx_comp++);
			frame_alloc_put(&pctx->frame_alloc, addr);
		}
		xsk_ring_cons__release(&pctx->comp_ring, completed);
	}

	/* Reserve TX slots */
	uint32_t idx_tx;
	unsigned int reserved = xsk_ring_prod__reserve(&pctx->tx_ring, count, &idx_tx);
	if (reserved == 0) {
		/* TX ring full, need wakeup */
		if (xsk_ring_prod__needs_wakeup(&pctx->tx_ring)) {
			sendto(pctx->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
			pctx->tx_wakeups++;
		}
		return 0;
	}

	/* Copy packets to UMEM and fill TX descriptors */
	for (unsigned int i = 0; i < reserved && sent < count; i++) {
		uint64_t addr = frame_alloc_get(&pctx->frame_alloc);
		if (addr == INVALID_UMEM_FRAME)
			break;

		/* Copy packet data to UMEM frame */
		uint8_t *frame = (uint8_t *)pctx->umem_area + addr;
		memcpy(frame, pkts[sent].data, pkts[sent].len);

		/* Fill TX descriptor */
		struct xdp_desc *desc = xsk_ring_prod__tx_desc(&pctx->tx_ring, idx_tx++);
		desc->addr = addr;
		desc->len = pkts[sent].len;

		sent++;
		wctx->tx_packets++;
		wctx->tx_bytes += pkts[sent - 1].len;
	}

	xsk_ring_prod__submit(&pctx->tx_ring, sent);

	/* Kick TX if needed */
	if (xsk_ring_prod__needs_wakeup(&pctx->tx_ring)) {
		sendto(pctx->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
		pctx->tx_wakeups++;
	}

	return sent;
}

static int xdp_recv_batch(worker_ctx_t *wctx, packet_t *pkts, int max_count)
{
	if (!wctx || !wctx->pctx || !pkts || max_count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int received = 0;

	/* Check if we need to wakeup for RX */
	if (xsk_ring_prod__needs_wakeup(&pctx->fill_ring)) {
		struct pollfd fds = {.fd = pctx->xsk_fd, .events = POLLIN};
		poll(&fds, 1, 0); /* Non-blocking poll */
		pctx->rx_wakeups++;
	}

	/* Peek RX ring */
	uint32_t idx_rx;
	unsigned int rcvd = xsk_ring_cons__peek(&pctx->rx_ring, max_count, &idx_rx);
	if (rcvd == 0)
		return 0;

	/* Get timestamp */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	/* Process received packets */
	for (unsigned int i = 0; i < rcvd; i++) {
		const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&pctx->rx_ring, idx_rx++);
		uint8_t *frame = (uint8_t *)pctx->umem_area + desc->addr;

		/* Allocate and copy packet */
		pkts[received].data = malloc(desc->len);
		if (!pkts[received].data)
			break;

		memcpy(pkts[received].data, frame, desc->len);
		pkts[received].len = desc->len;
		pkts[received].timestamp = now_ns;
		pkts[received].platform_data = (void *)(uintptr_t)desc->addr;

		received++;
		wctx->rx_packets++;
		wctx->rx_bytes += desc->len;
	}

	xsk_ring_cons__release(&pctx->rx_ring, rcvd);

	/* Refill fill ring */
	uint32_t idx_fill;
	unsigned int slots = xsk_ring_prod__reserve(&pctx->fill_ring, rcvd, &idx_fill);
	for (unsigned int i = 0; i < slots; i++) {
		*xsk_ring_prod__fill_addr(&pctx->fill_ring, idx_fill++) =
		    frame_alloc_get(&pctx->frame_alloc);
	}
	xsk_ring_prod__submit(&pctx->fill_ring, slots);

	return received;
}

static void xdp_release_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	if (!wctx || !wctx->pctx)
		return;

	platform_ctx_t *pctx = wctx->pctx;

	for (int i = 0; i < count; i++) {
		if (pkts[i].platform_data) {
			frame_alloc_put(&pctx->frame_alloc, (uint64_t)(uintptr_t)pkts[i].platform_data);
		}
		free(pkts[i].data);
		pkts[i].data = NULL;
		pkts[i].platform_data = NULL;
	}
}

static uint64_t xdp_get_timestamp(worker_ctx_t *wctx, packet_t *pkt)
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
} xdp_ops = {
    .name = "AF_XDP",
    .init = xdp_init,
    .cleanup = xdp_cleanup,
    .send_batch = xdp_send_batch,
    .recv_batch = xdp_recv_batch,
    .release_batch = xdp_release_batch,
    .get_tx_timestamp = xdp_get_timestamp,
    .get_rx_timestamp = xdp_get_timestamp,
};

const void *get_xdp_platform_ops(void)
{
	return &xdp_ops;
}

#endif /* HAVE_AF_XDP */
