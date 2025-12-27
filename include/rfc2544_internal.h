/*
 * rfc2544_internal.h - Internal structures for RFC 2544 implementation
 *
 * This file contains the internal struct definitions that are shared
 * between core.c and y1564.c but not exposed to the public API.
 */

#ifndef RFC2544_INTERNAL_H
#define RFC2544_INTERNAL_H

#include "rfc2544.h"
#include "platform_config.h"
#include <pthread.h>

/* Forward declarations */
typedef struct platform_ops platform_ops_t;

/* Per-worker context (for multi-queue support) */
typedef struct {
	int worker_id;
	int queue_id;
	void *pctx; /* Platform-specific context */

	/* Stats */
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_errors;
	uint64_t rx_errors;
} worker_ctx_t;

/* Main test context structure */
struct rfc2544_ctx {
	/* Configuration */
	rfc2544_config_t config;

	/* State */
	test_state_t state;
	volatile bool cancel_requested;

	/* Platform */
	const platform_ops_t *platform;
	worker_ctx_t *workers;
	int num_workers;

	/* Interface info */
	char interface[64];
	uint64_t line_rate;
	uint8_t local_mac[6];
	uint8_t remote_mac[6];
	uint32_t local_ip;
	uint32_t remote_ip;

	/* Timing */
	struct timespec start_time;
	struct timespec end_time;

	/* Results storage */
	throughput_result_t throughput_results[8]; /* 7 standard + 1 jumbo */
	uint32_t throughput_count;
	latency_result_t latency_results[80]; /* 10 load levels x 8 sizes */
	uint32_t latency_count;
	frame_loss_point_t loss_results[100]; /* Up to 100 load points */
	uint32_t loss_count;
	burst_result_t burst_results[8];
	uint32_t burst_count;

	/* Callbacks */
	progress_callback_t progress_cb;

	/* Sequence tracking */
	uint32_t next_seq_num;
	pthread_mutex_t seq_lock;

	/* Latency tracking */
	uint64_t *latency_samples;
	uint32_t latency_sample_count;
	uint32_t latency_sample_capacity;
	pthread_mutex_t latency_lock;
};

/* Logging function (implemented in core.c) */
void rfc2544_log(log_level_t level, const char *fmt, ...);

#endif /* RFC2544_INTERNAL_H */
