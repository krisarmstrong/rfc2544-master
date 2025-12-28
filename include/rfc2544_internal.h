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

/* ============================================================================
 * Trial Execution (shared by all test implementations)
 * ============================================================================ */

/* Trial result structure */
typedef struct {
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t bytes_sent;
	double loss_pct;
	double elapsed_sec;
	double achieved_pps;
	double achieved_mbps;
	latency_stats_t latency;
} trial_result_t;

/**
 * Run a single trial at the specified rate
 * This is the core packet I/O function used by all tests
 *
 * @param ctx Test context
 * @param frame_size Frame size in bytes
 * @param rate_pct Target rate as percentage of line rate
 * @param duration_sec Trial duration in seconds
 * @param warmup_sec Warmup period in seconds
 * @param result Output trial result
 * @return 0 on success, negative on error
 */
int run_trial(rfc2544_ctx_t *ctx, uint32_t frame_size, double rate_pct,
              uint32_t duration_sec, uint32_t warmup_sec, trial_result_t *result);

/**
 * Run a trial with custom signature (for Y.1564, Y.1731, etc.)
 *
 * @param ctx Test context
 * @param frame_size Frame size in bytes
 * @param rate_pct Target rate as percentage of line rate
 * @param duration_sec Trial duration in seconds
 * @param warmup_sec Warmup period in seconds
 * @param signature 7-byte packet signature (e.g., "Y.1564 ", "Y.1731 ")
 * @param stream_id Stream/service ID for multi-service tests
 * @param result Output trial result
 * @return 0 on success, negative on error
 */
int run_trial_custom(rfc2544_ctx_t *ctx, uint32_t frame_size, double rate_pct,
                     uint32_t duration_sec, uint32_t warmup_sec,
                     const char *signature, uint32_t stream_id,
                     trial_result_t *result);

/* Calculate max PPS for given line rate and frame size */
uint64_t calc_max_pps(uint64_t line_rate_bps, uint32_t frame_size);

/* Report progress to callback */
void report_progress(rfc2544_ctx_t *ctx, const char *message, double pct);

#endif /* RFC2544_INTERNAL_H */
