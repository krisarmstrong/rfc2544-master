/*
 * dpdk_platform.c - DPDK platform implementation for RFC2544 Test Master
 *
 * Line-rate packet I/O using DPDK (Data Plane Development Kit).
 * Requires DPDK libraries and a bound NIC.
 *
 * Performance target: 100+ Gbps (true line-rate)
 */

#include "rfc2544.h"
#include "platform_config.h"

#if HAVE_DPDK

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* DPDK configuration */
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define BURST_SIZE 64

#define RX_RING_SIZE 4096
#define TX_RING_SIZE 4096

/* Platform context */
typedef struct {
	uint16_t port_id;
	uint16_t queue_id;
	struct rte_mempool *mbuf_pool;
	struct rte_mbuf *rx_mbufs[BURST_SIZE];
	struct rte_mbuf *tx_mbufs[BURST_SIZE];
	bool is_primary;
	uint8_t port_mac[6];
} platform_ctx_t;

/* Shared state (initialized by worker 0) */
static struct {
	bool initialized;
	uint16_t port_id;
	struct rte_mempool *mbuf_pool;
	uint16_t num_rx_queues;
	uint16_t num_tx_queues;
} dpdk_shared = {0};

static pthread_mutex_t dpdk_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * DPDK Initialization
 * ============================================================================ */

static int dpdk_init_eal(rfc2544_ctx_t *ctx)
{
	/* Build EAL arguments */
	char *argv[32];
	int argc = 0;

	argv[argc++] = "rfc2544";
	argv[argc++] = "-l";
	argv[argc++] = "0-1"; /* Default: use cores 0-1 */
	argv[argc++] = "--proc-type";
	argv[argc++] = "primary";

	/* Add user-provided DPDK args if any */
	if (ctx->config.dpdk_args && strlen(ctx->config.dpdk_args) > 0) {
		/* Parse space-separated args - simplified */
		char *args_copy = strdup(ctx->config.dpdk_args);
		char *token = strtok(args_copy, " ");
		while (token && argc < 30) {
			argv[argc++] = strdup(token);
			token = strtok(NULL, " ");
		}
		free(args_copy);
	}

	/* Initialize EAL */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		fprintf(stderr, "[dpdk] EAL init failed: %s\n", rte_strerror(rte_errno));
		return -1;
	}

	return 0;
}

static int dpdk_init_port(uint16_t port_id, uint16_t num_queues, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = {
	    .rxmode =
	        {
	            .mq_mode = RTE_ETH_MQ_RX_RSS,
	        },
	    .txmode =
	        {
	            .mq_mode = RTE_ETH_MQ_TX_NONE,
	        },
	    .rx_adv_conf =
	        {
	            .rss_conf =
	                {
	                    .rss_key = NULL,
	                    .rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP,
	                },
	        },
	};

	struct rte_eth_dev_info dev_info;
	int ret;

	/* Get device info */
	ret = rte_eth_dev_info_get(port_id, &dev_info);
	if (ret != 0) {
		fprintf(stderr, "[dpdk] Failed to get device info: %s\n", rte_strerror(-ret));
		return ret;
	}

	/* Adjust for device capabilities */
	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
	}

	/* Configure port */
	ret = rte_eth_dev_configure(port_id, num_queues, num_queues, &port_conf);
	if (ret != 0) {
		fprintf(stderr, "[dpdk] Failed to configure port: %s\n", rte_strerror(-ret));
		return ret;
	}

	/* Adjust ring sizes */
	uint16_t rx_size = RX_RING_SIZE;
	uint16_t tx_size = TX_RING_SIZE;
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_size, &tx_size);
	if (ret != 0) {
		fprintf(stderr, "[dpdk] Failed to adjust descriptors: %s\n", rte_strerror(-ret));
		return ret;
	}

	/* Setup RX queues */
	for (uint16_t q = 0; q < num_queues; q++) {
		ret = rte_eth_rx_queue_setup(port_id, q, rx_size,
		                             rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
		if (ret < 0) {
			fprintf(stderr, "[dpdk] Failed to setup RX queue %u: %s\n", q,
			        rte_strerror(-ret));
			return ret;
		}
	}

	/* Setup TX queues */
	for (uint16_t q = 0; q < num_queues; q++) {
		ret = rte_eth_tx_queue_setup(port_id, q, tx_size,
		                             rte_eth_dev_socket_id(port_id), NULL);
		if (ret < 0) {
			fprintf(stderr, "[dpdk] Failed to setup TX queue %u: %s\n", q,
			        rte_strerror(-ret));
			return ret;
		}
	}

	/* Enable promiscuous mode */
	ret = rte_eth_promiscuous_enable(port_id);
	if (ret != 0) {
		fprintf(stderr, "[dpdk] Warning: Failed to enable promiscuous mode\n");
	}

	/* Start port */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		fprintf(stderr, "[dpdk] Failed to start port: %s\n", rte_strerror(-ret));
		return ret;
	}

	return 0;
}

/* ============================================================================
 * Platform Operations
 * ============================================================================ */

static int dpdk_init(rfc2544_ctx_t *ctx, worker_ctx_t *wctx)
{
	platform_ctx_t *pctx = calloc(1, sizeof(platform_ctx_t));
	if (!pctx)
		return -ENOMEM;

	int ret = 0;

	pthread_mutex_lock(&dpdk_init_lock);

	if (wctx->worker_id == 0 && !dpdk_shared.initialized) {
		/* Primary worker - initialize EAL and port */
		pctx->is_primary = true;

		/* Initialize EAL */
		ret = dpdk_init_eal(ctx);
		if (ret < 0) {
			pthread_mutex_unlock(&dpdk_init_lock);
			free(pctx);
			return ret;
		}

		/* Check available ports */
		uint16_t nb_ports = rte_eth_dev_count_avail();
		if (nb_ports == 0) {
			fprintf(stderr, "[dpdk] No available ports. Is the NIC bound to DPDK?\n");
			pthread_mutex_unlock(&dpdk_init_lock);
			free(pctx);
			return -ENODEV;
		}

		/* Use first available port */
		dpdk_shared.port_id = 0;
		dpdk_shared.num_rx_queues = 1;
		dpdk_shared.num_tx_queues = 1;

		/* Create mempool */
		dpdk_shared.mbuf_pool = rte_pktmbuf_pool_create(
		    "MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
		    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

		if (!dpdk_shared.mbuf_pool) {
			fprintf(stderr, "[dpdk] Failed to create mempool: %s\n",
			        rte_strerror(rte_errno));
			pthread_mutex_unlock(&dpdk_init_lock);
			free(pctx);
			return -ENOMEM;
		}

		/* Initialize port */
		ret = dpdk_init_port(dpdk_shared.port_id, dpdk_shared.num_rx_queues,
		                     dpdk_shared.mbuf_pool);
		if (ret < 0) {
			rte_mempool_free(dpdk_shared.mbuf_pool);
			pthread_mutex_unlock(&dpdk_init_lock);
			free(pctx);
			return ret;
		}

		dpdk_shared.initialized = true;

		/* Get MAC address */
		struct rte_ether_addr mac;
		ret = rte_eth_macaddr_get(dpdk_shared.port_id, &mac);
		if (ret == 0) {
			memcpy(pctx->port_mac, mac.addr_bytes, 6);
			memcpy(ctx->local_mac, mac.addr_bytes, 6);
		}

		fprintf(stderr, "[dpdk] Initialized port %u with %u queues\n",
		        dpdk_shared.port_id, dpdk_shared.num_rx_queues);
	}

	pthread_mutex_unlock(&dpdk_init_lock);

	/* All workers attach to shared resources */
	pctx->port_id = dpdk_shared.port_id;
	pctx->queue_id = wctx->queue_id;
	pctx->mbuf_pool = dpdk_shared.mbuf_pool;

	wctx->pctx = pctx;

	return 0;
}

static void dpdk_cleanup(worker_ctx_t *wctx)
{
	if (!wctx || !wctx->pctx)
		return;

	platform_ctx_t *pctx = wctx->pctx;

	pthread_mutex_lock(&dpdk_init_lock);

	if (pctx->is_primary && dpdk_shared.initialized) {
		rte_eth_dev_stop(dpdk_shared.port_id);
		rte_eth_dev_close(dpdk_shared.port_id);
		rte_mempool_free(dpdk_shared.mbuf_pool);
		rte_eal_cleanup();
		dpdk_shared.initialized = false;
	}

	pthread_mutex_unlock(&dpdk_init_lock);

	free(pctx);
	wctx->pctx = NULL;
}

static int dpdk_send_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	if (!wctx || !wctx->pctx || !pkts || count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int sent = 0;

	/* Allocate mbufs */
	struct rte_mbuf *mbufs[BURST_SIZE];
	int to_send = (count > BURST_SIZE) ? BURST_SIZE : count;

	int allocated = rte_pktmbuf_alloc_bulk(pctx->mbuf_pool, mbufs, to_send);
	if (allocated != 0) {
		wctx->tx_errors++;
		return 0;
	}

	/* Copy packets to mbufs */
	for (int i = 0; i < to_send; i++) {
		char *data = rte_pktmbuf_append(mbufs[i], pkts[i].len);
		if (!data) {
			rte_pktmbuf_free(mbufs[i]);
			mbufs[i] = NULL;
			continue;
		}
		memcpy(data, pkts[i].data, pkts[i].len);
	}

	/* Transmit */
	uint16_t nb_tx = rte_eth_tx_burst(pctx->port_id, pctx->queue_id, mbufs, to_send);

	/* Free unsent mbufs */
	for (uint16_t i = nb_tx; i < to_send; i++) {
		if (mbufs[i])
			rte_pktmbuf_free(mbufs[i]);
	}

	sent = nb_tx;
	wctx->tx_packets += sent;
	for (int i = 0; i < sent; i++) {
		wctx->tx_bytes += pkts[i].len;
	}

	return sent;
}

static int dpdk_recv_batch(worker_ctx_t *wctx, packet_t *pkts, int max_count)
{
	if (!wctx || !wctx->pctx || !pkts || max_count <= 0)
		return -EINVAL;

	platform_ctx_t *pctx = wctx->pctx;
	int received = 0;

	struct rte_mbuf *mbufs[BURST_SIZE];
	int to_recv = (max_count > BURST_SIZE) ? BURST_SIZE : max_count;

	/* Receive packets */
	uint16_t nb_rx = rte_eth_rx_burst(pctx->port_id, pctx->queue_id, mbufs, to_recv);
	if (nb_rx == 0)
		return 0;

	/* Get timestamp */
	uint64_t now_ns = rte_get_timer_cycles() * 1000000000ULL / rte_get_timer_hz();

	/* Copy to packet structures */
	for (uint16_t i = 0; i < nb_rx; i++) {
		uint32_t len = rte_pktmbuf_data_len(mbufs[i]);
		pkts[received].data = malloc(len);
		if (!pkts[received].data) {
			rte_pktmbuf_free(mbufs[i]);
			continue;
		}

		memcpy(pkts[received].data, rte_pktmbuf_mtod(mbufs[i], void *), len);
		pkts[received].len = len;
		pkts[received].timestamp = now_ns;
		pkts[received].platform_data = mbufs[i];

		received++;
		wctx->rx_packets++;
		wctx->rx_bytes += len;
	}

	return received;
}

static void dpdk_release_batch(worker_ctx_t *wctx, packet_t *pkts, int count)
{
	(void)wctx;

	for (int i = 0; i < count; i++) {
		if (pkts[i].platform_data) {
			rte_pktmbuf_free((struct rte_mbuf *)pkts[i].platform_data);
		}
		free(pkts[i].data);
		pkts[i].data = NULL;
		pkts[i].platform_data = NULL;
	}
}

static uint64_t dpdk_get_timestamp(worker_ctx_t *wctx, packet_t *pkt)
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
} dpdk_ops = {
    .name = "DPDK",
    .init = dpdk_init,
    .cleanup = dpdk_cleanup,
    .send_batch = dpdk_send_batch,
    .recv_batch = dpdk_recv_batch,
    .release_batch = dpdk_release_batch,
    .get_tx_timestamp = dpdk_get_timestamp,
    .get_rx_timestamp = dpdk_get_timestamp,
};

const void *get_dpdk_platform_ops(void)
{
	return &dpdk_ops;
}

#endif /* HAVE_DPDK */
