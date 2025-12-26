/*
 * core.c - RFC 2544 Test Master Core Implementation
 *
 * This file implements the main test orchestration logic:
 * - Context management
 * - Platform abstraction
 * - Test execution coordination
 */

#include "rfc2544.h"
#include "platform_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Forward declarations for platform ops */
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

/* Internal packet structure */
typedef struct {
	uint8_t *data;
	uint32_t len;
	uint64_t timestamp; /* TX or RX timestamp in nanoseconds */
	uint32_t seq_num;
	void *platform_data;
} packet_t;

/* Platform operations interface */
struct platform_ops {
	const char *name;
	int (*init)(rfc2544_ctx_t *ctx, worker_ctx_t *wctx);
	void (*cleanup)(worker_ctx_t *wctx);
	int (*send_batch)(worker_ctx_t *wctx, packet_t *pkts, int count);
	int (*recv_batch)(worker_ctx_t *wctx, packet_t *pkts, int max_count);
	void (*release_batch)(worker_ctx_t *wctx, packet_t *pkts, int count);
	uint64_t (*get_tx_timestamp)(worker_ctx_t *wctx, packet_t *pkt);
	uint64_t (*get_rx_timestamp)(worker_ctx_t *wctx, packet_t *pkt);
};

/* Forward declarations for packet.c */
typedef struct __attribute__((packed)) {
	uint8_t signature[RFC2544_SIG_LEN];
	uint32_t seq_num;
	uint64_t timestamp;
	uint32_t stream_id;
	uint8_t flags;
} rfc2544_payload_t;

rfc2544_payload_t *rfc2544_create_packet_template(uint8_t *buffer, uint32_t frame_size,
                                                   const uint8_t *src_mac, const uint8_t *dst_mac,
                                                   uint32_t src_ip, uint32_t dst_ip,
                                                   uint16_t src_port, uint16_t dst_port,
                                                   uint32_t stream_id);
void rfc2544_stamp_packet(rfc2544_payload_t *payload, uint32_t seq_num, uint64_t timestamp_ns);
bool rfc2544_is_valid_response(const uint8_t *data, uint32_t len);
uint32_t rfc2544_get_seq_num(const uint8_t *data, uint32_t len);
uint64_t rfc2544_get_tx_timestamp(const uint8_t *data, uint32_t len);
void rfc2544_calc_latency_stats(const uint64_t *samples, uint32_t count, latency_stats_t *stats);

/* Forward declarations for pacing.c */
typedef struct pacing_ctx pacing_ctx_t;
typedef struct trial_timer trial_timer_t;
typedef struct seq_tracker seq_tracker_t;

pacing_ctx_t *pacing_create(uint64_t line_rate_bps, uint32_t frame_size, double rate_pct);
void pacing_set_rate(pacing_ctx_t *ctx, double rate_pct);
void pacing_set_batch_size(pacing_ctx_t *ctx, uint32_t batch_size);
void pacing_set_busy_wait(pacing_ctx_t *ctx, bool enable);
uint64_t pacing_wait(pacing_ctx_t *ctx);
uint64_t pacing_wait_batch(pacing_ctx_t *ctx, uint32_t batch_size);
void pacing_record_tx(pacing_ctx_t *ctx, uint32_t packets, uint32_t bytes);
void pacing_get_rate(const pacing_ctx_t *ctx, double *pps, double *mbps);
void pacing_reset(pacing_ctx_t *ctx);
void pacing_destroy(pacing_ctx_t *ctx);

trial_timer_t *trial_timer_create(uint32_t duration_sec, uint32_t warmup_sec);
void trial_timer_start(trial_timer_t *timer);
bool trial_timer_expired(trial_timer_t *timer);
bool trial_timer_in_warmup(const trial_timer_t *timer);
double trial_timer_elapsed(const trial_timer_t *timer);
void trial_timer_destroy(trial_timer_t *timer);

seq_tracker_t *rfc2544_seq_tracker_create(uint32_t capacity);
void rfc2544_seq_tracker_record(seq_tracker_t *tracker, uint32_t seq_num);
void rfc2544_seq_tracker_stats(const seq_tracker_t *tracker, uint32_t expected, uint32_t *received,
                               uint32_t *lost, double *loss_pct);
void rfc2544_seq_tracker_destroy(seq_tracker_t *tracker);

uint64_t calc_max_pps(uint64_t line_rate_bps, uint32_t frame_size);

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

/* Global log level */
static log_level_t g_log_level = LOG_INFO;

/* ============================================================================
 * Logging
 * ============================================================================ */

void rfc2544_set_log_level(log_level_t level)
{
	g_log_level = level;
}

static void rfc2544_log(log_level_t level, const char *fmt, ...)
{
	if (level > g_log_level)
		return;

	const char *level_str[] = {"ERROR", "WARN", "INFO", "DEBUG"};
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	fprintf(stderr, "[%ld.%03ld] [%s] ", ts.tv_sec, ts.tv_nsec / 1000000, level_str[level]);

	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");
}

/* ============================================================================
 * Platform Selection
 * ============================================================================ */

#if HAVE_AF_XDP
extern const platform_ops_t *get_xdp_platform_ops(void);
#endif

#if PLATFORM_LINUX
extern const platform_ops_t *get_packet_platform_ops(void);
#endif

#if HAVE_DPDK
extern const platform_ops_t *get_dpdk_platform_ops(void);
#endif

static const platform_ops_t *select_platform(rfc2544_ctx_t *ctx)
{
#if HAVE_DPDK
	if (ctx->config.use_dpdk) {
		rfc2544_log(LOG_INFO, "Platform: DPDK (line-rate mode)");
		return get_dpdk_platform_ops();
	}
#endif

#if HAVE_AF_XDP
	rfc2544_log(LOG_INFO, "Platform: AF_XDP (high performance)");
	return get_xdp_platform_ops();
#elif PLATFORM_LINUX
	rfc2544_log(LOG_INFO, "Platform: AF_PACKET (fallback)");
	return get_packet_platform_ops();
#else
	rfc2544_log(LOG_ERROR, "No supported platform available");
	return NULL;
#endif
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

void rfc2544_default_config(rfc2544_config_t *config)
{
	memset(config, 0, sizeof(*config));

	/* Defaults */
	config->test_type = TEST_THROUGHPUT;
	config->frame_size = 0; /* All standard sizes */
	config->include_jumbo = false;
	config->trial_duration_sec = 60;
	config->warmup_sec = 2;

	/* Throughput test */
	config->initial_rate_pct = 100.0;
	config->resolution_pct = 0.1;
	config->max_iterations = 20;
	config->acceptable_loss = 0.0;

	/* Latency test */
	config->latency_samples = 1000;
	config->latency_load_count = 10;
	for (int i = 0; i < 10; i++) {
		config->latency_load_pct[i] = (i + 1) * 10.0; /* 10%, 20%, ..., 100% */
	}

	/* Frame loss test */
	config->loss_start_pct = 100.0;
	config->loss_end_pct = 10.0;
	config->loss_step_pct = 10.0;

	/* Back-to-back test */
	config->initial_burst = 2;
	config->burst_trials = 50;

	/* Hardware timestamping */
	config->hw_timestamp = true;

	/* Output */
	config->output_format = STATS_FORMAT_TEXT;
	config->verbose = false;

	/* Rate control */
	config->use_pacing = true;
	config->batch_size = DEFAULT_BATCH_SIZE;
}

uint64_t rfc2544_calc_pps(uint64_t line_rate, uint32_t frame_size)
{
	/* Ethernet overhead: preamble (8) + IFG (12) = 20 bytes */
	uint32_t wire_size = frame_size + 20;
	uint64_t bits_per_packet = wire_size * 8;
	return line_rate / bits_per_packet;
}

uint64_t rfc2544_get_line_rate(const char *interface)
{
	/* TODO: Query interface speed via ethtool/sysfs */
	(void)interface;
	return 10000000000ULL; /* Default 10 Gbps */
}

static uint64_t get_timestamp_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

/* ============================================================================
 * Core API Implementation
 * ============================================================================ */

int rfc2544_init(rfc2544_ctx_t **ctx_out, const char *interface)
{
	rfc2544_ctx_t *ctx = calloc(1, sizeof(rfc2544_ctx_t));
	if (!ctx) {
		rfc2544_log(LOG_ERROR, "Failed to allocate context");
		return -ENOMEM;
	}

	strncpy(ctx->interface, interface, sizeof(ctx->interface) - 1);
	strncpy(ctx->config.interface, interface, sizeof(ctx->config.interface) - 1);

	/* Initialize defaults */
	rfc2544_default_config(&ctx->config);

	/* Get line rate */
	ctx->line_rate = rfc2544_get_line_rate(interface);
	ctx->config.line_rate = ctx->line_rate;

	/* Initialize locks */
	pthread_mutex_init(&ctx->seq_lock, NULL);
	pthread_mutex_init(&ctx->latency_lock, NULL);

	/* Allocate latency sample buffer */
	ctx->latency_sample_capacity = 100000;
	ctx->latency_samples = malloc(ctx->latency_sample_capacity * sizeof(uint64_t));
	if (!ctx->latency_samples) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->state = STATE_IDLE;
	*ctx_out = ctx;

	rfc2544_log(LOG_INFO, "RFC2544 Test Master v%d.%d.%d initialized", RFC2544_VERSION_MAJOR,
	            RFC2544_VERSION_MINOR, RFC2544_VERSION_PATCH);
	rfc2544_log(LOG_INFO, "Interface: %s, Line rate: %.2f Gbps", interface,
	            ctx->line_rate / 1e9);

	return 0;
}

int rfc2544_configure(rfc2544_ctx_t *ctx, const rfc2544_config_t *config)
{
	if (!ctx || !config)
		return -EINVAL;

	if (ctx->state == STATE_RUNNING) {
		rfc2544_log(LOG_ERROR, "Cannot configure while test is running");
		return -EBUSY;
	}

	memcpy(&ctx->config, config, sizeof(rfc2544_config_t));

	/* Validate */
	if (config->trial_duration_sec < 1) {
		rfc2544_log(LOG_WARN, "Trial duration too short, using 1 second");
		ctx->config.trial_duration_sec = 1;
	}

	if (config->resolution_pct < 0.01) {
		rfc2544_log(LOG_WARN, "Resolution too fine, using 0.01%%");
		ctx->config.resolution_pct = 0.01;
	}

	return 0;
}

void rfc2544_set_progress_callback(rfc2544_ctx_t *ctx, progress_callback_t callback)
{
	if (ctx)
		ctx->progress_cb = callback;
}

test_state_t rfc2544_get_state(const rfc2544_ctx_t *ctx)
{
	return ctx ? ctx->state : STATE_IDLE;
}

void rfc2544_cancel(rfc2544_ctx_t *ctx)
{
	if (ctx) {
		ctx->cancel_requested = true;
		rfc2544_log(LOG_INFO, "Cancellation requested");
	}
}

void rfc2544_cleanup(rfc2544_ctx_t *ctx)
{
	if (!ctx)
		return;

	/* Cancel if running */
	if (ctx->state == STATE_RUNNING) {
		rfc2544_cancel(ctx);
		/* Wait for completion */
		while (ctx->state == STATE_RUNNING) {
			usleep(10000);
		}
	}

	/* Cleanup platform */
	if (ctx->platform && ctx->workers) {
		for (int i = 0; i < ctx->num_workers; i++) {
			ctx->platform->cleanup(&ctx->workers[i]);
		}
		free(ctx->workers);
	}

	/* Free resources */
	free(ctx->latency_samples);
	pthread_mutex_destroy(&ctx->seq_lock);
	pthread_mutex_destroy(&ctx->latency_lock);
	free(ctx);

	rfc2544_log(LOG_INFO, "Cleanup complete");
}

/* ============================================================================
 * Test Execution
 * ============================================================================ */

static void report_progress(rfc2544_ctx_t *ctx, const char *message, double pct)
{
	if (ctx->progress_cb) {
		ctx->progress_cb(ctx, message, pct);
	}
	if (ctx->config.verbose) {
		rfc2544_log(LOG_INFO, "[%.1f%%] %s", pct, message);
	}
}

int rfc2544_run(rfc2544_ctx_t *ctx)
{
	if (!ctx)
		return -EINVAL;

	if (ctx->state == STATE_RUNNING) {
		rfc2544_log(LOG_ERROR, "Test already running");
		return -EBUSY;
	}

	/* Select platform */
	ctx->platform = select_platform(ctx);
	if (!ctx->platform) {
		ctx->state = STATE_FAILED;
		return -ENOTSUP;
	}

	/* Allocate workers (single worker for now) */
	ctx->num_workers = 1;
	ctx->workers = calloc(ctx->num_workers, sizeof(worker_ctx_t));
	if (!ctx->workers) {
		ctx->state = STATE_FAILED;
		return -ENOMEM;
	}

	/* Initialize platform */
	for (int i = 0; i < ctx->num_workers; i++) {
		ctx->workers[i].worker_id = i;
		ctx->workers[i].queue_id = i;
		if (ctx->platform->init(ctx, &ctx->workers[i]) < 0) {
			rfc2544_log(LOG_ERROR, "Failed to initialize platform");
			ctx->state = STATE_FAILED;
			return -EIO;
		}
	}

	ctx->state = STATE_RUNNING;
	ctx->cancel_requested = false;
	clock_gettime(CLOCK_MONOTONIC, &ctx->start_time);

	int ret = 0;

	/* Get frame sizes to test */
	uint32_t frame_sizes[8];
	int num_sizes = 0;

	if (ctx->config.frame_size > 0) {
		/* Specific size */
		frame_sizes[num_sizes++] = ctx->config.frame_size;
	} else {
		/* Standard sizes */
		uint32_t std_sizes[] = RFC2544_FRAME_SIZES;
		for (int i = 0; i < RFC2544_FRAME_SIZE_COUNT; i++) {
			frame_sizes[num_sizes++] = std_sizes[i];
		}
		if (ctx->config.include_jumbo) {
			frame_sizes[num_sizes++] = FRAME_SIZE_9000;
		}
	}

	/* Run appropriate test */
	switch (ctx->config.test_type) {
	case TEST_THROUGHPUT:
		report_progress(ctx, "Starting throughput test", 0);
		for (int i = 0; i < num_sizes && !ctx->cancel_requested; i++) {
			double pct = (i * 100.0) / num_sizes;
			char msg[128];
			snprintf(msg, sizeof(msg), "Testing frame size %u", frame_sizes[i]);
			report_progress(ctx, msg, pct);

			ret = rfc2544_throughput_test(ctx, frame_sizes[i],
			                              &ctx->throughput_results[ctx->throughput_count],
			                              NULL);
			if (ret < 0)
				break;
			ctx->throughput_count++;
		}
		break;

	case TEST_LATENCY:
		report_progress(ctx, "Starting latency test", 0);
		for (int i = 0; i < num_sizes && !ctx->cancel_requested; i++) {
			for (uint32_t j = 0;
			     j < ctx->config.latency_load_count && !ctx->cancel_requested; j++) {
				ret = rfc2544_latency_test(ctx, frame_sizes[i],
				                           ctx->config.latency_load_pct[j],
				                           &ctx->latency_results[ctx->latency_count]);
				if (ret < 0)
					break;
				ctx->latency_count++;
			}
		}
		break;

	case TEST_FRAME_LOSS:
		report_progress(ctx, "Starting frame loss test", 0);
		for (int i = 0; i < num_sizes && !ctx->cancel_requested; i++) {
			uint32_t count = 0;
			ret = rfc2544_frame_loss_test(ctx, frame_sizes[i],
			                              &ctx->loss_results[ctx->loss_count], &count);
			if (ret < 0)
				break;
			ctx->loss_count += count;
		}
		break;

	case TEST_BACK_TO_BACK:
		report_progress(ctx, "Starting back-to-back test", 0);
		for (int i = 0; i < num_sizes && !ctx->cancel_requested; i++) {
			ret = rfc2544_back_to_back_test(ctx, frame_sizes[i],
			                                &ctx->burst_results[ctx->burst_count]);
			if (ret < 0)
				break;
			ctx->burst_count++;
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	clock_gettime(CLOCK_MONOTONIC, &ctx->end_time);

	if (ctx->cancel_requested) {
		ctx->state = STATE_CANCELLED;
		rfc2544_log(LOG_INFO, "Test cancelled");
	} else if (ret < 0) {
		ctx->state = STATE_FAILED;
		rfc2544_log(LOG_ERROR, "Test failed with error %d", ret);
	} else {
		ctx->state = STATE_COMPLETED;
		report_progress(ctx, "Test completed", 100);
	}

	return ret;
}

/* ============================================================================
 * Throughput Test (Section 26.1)
 * ============================================================================ */

int rfc2544_throughput_test(rfc2544_ctx_t *ctx, uint32_t frame_size, throughput_result_t *result,
                            uint32_t *result_count)
{
	if (!ctx || !result)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Throughput test: frame_size=%u", frame_size);

	/* Calculate max theoretical PPS */
	uint64_t max_pps = rfc2544_calc_pps(ctx->line_rate, frame_size);
	rfc2544_log(LOG_DEBUG, "Max theoretical rate: %lu pps", max_pps);

	/* Binary search for max throughput with 0% loss */
	double low = 0.0;
	double high = ctx->config.initial_rate_pct;
	double best_rate = 0.0;
	uint32_t iterations = 0;

	while ((high - low) > ctx->config.resolution_pct &&
	       iterations < ctx->config.max_iterations && !ctx->cancel_requested) {

		double current_rate = (low + high) / 2.0;
		uint64_t target_pps = (uint64_t)(max_pps * current_rate / 100.0);

		rfc2544_log(LOG_DEBUG, "Iteration %u: testing %.2f%% (%lu pps)", iterations,
		            current_rate, target_pps);

		/* Run trial at current rate */
		/* TODO: Implement actual packet transmission and reception */
		uint64_t sent = 0, received = 0;

		/* Placeholder: simulate trial */
		usleep(100000); /* 100ms for testing */
		sent = target_pps * ctx->config.trial_duration_sec;
		received = sent; /* Simulate 0% loss for now */

		double loss_pct = (sent > 0) ? (100.0 * (sent - received) / sent) : 100.0;

		if (loss_pct <= ctx->config.acceptable_loss) {
			/* Success - try higher rate */
			best_rate = current_rate;
			low = current_rate;
			rfc2544_log(LOG_DEBUG, "  Pass: loss=%.4f%%, new best=%.2f%%", loss_pct,
			            best_rate);
		} else {
			/* Failure - try lower rate */
			high = current_rate;
			rfc2544_log(LOG_DEBUG, "  Fail: loss=%.4f%%, reducing rate", loss_pct);
		}

		iterations++;
	}

	/* Store result */
	result->frame_size = frame_size;
	result->max_rate_pct = best_rate;
	result->max_rate_mbps = (ctx->line_rate * best_rate / 100.0) / 1e6;
	result->max_rate_pps = (uint64_t)(max_pps * best_rate / 100.0);
	result->iterations = iterations;

	rfc2544_log(LOG_INFO, "Throughput result: %.2f%% (%.2f Mbps, %lu pps)", result->max_rate_pct,
	            result->max_rate_mbps, result->max_rate_pps);

	if (result_count)
		*result_count = 1;

	return 0;
}

/* ============================================================================
 * Latency Test (Section 26.2)
 * ============================================================================ */

int rfc2544_latency_test(rfc2544_ctx_t *ctx, uint32_t frame_size, double load_pct,
                         latency_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Latency test: frame_size=%u, load=%.1f%%", frame_size, load_pct);

	/* TODO: Implement actual latency measurement */
	/* For now, return placeholder values */

	result->frame_size = frame_size;
	result->offered_rate_pct = load_pct;
	result->latency.count = ctx->config.latency_samples;
	result->latency.min_ns = 100000;            /* 100 us */
	result->latency.max_ns = 500000;            /* 500 us */
	result->latency.avg_ns = 200000;            /* 200 us */
	result->latency.jitter_ns = 50000;          /* 50 us */

	rfc2544_log(LOG_INFO, "Latency result: min=%.1f us, avg=%.1f us, max=%.1f us",
	            result->latency.min_ns / 1000.0, result->latency.avg_ns / 1000.0,
	            result->latency.max_ns / 1000.0);

	return 0;
}

/* ============================================================================
 * Frame Loss Test (Section 26.3)
 * ============================================================================ */

int rfc2544_frame_loss_test(rfc2544_ctx_t *ctx, uint32_t frame_size, frame_loss_point_t *results,
                            uint32_t *result_count)
{
	if (!ctx || !results || !result_count)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Frame loss test: frame_size=%u", frame_size);

	uint32_t count = 0;
	double rate = ctx->config.loss_start_pct;

	while (rate >= ctx->config.loss_end_pct && !ctx->cancel_requested) {
		/* TODO: Implement actual test */
		results[count].offered_rate_pct = rate;
		results[count].actual_rate_mbps = (ctx->line_rate * rate / 100.0) / 1e6;
		results[count].frames_sent = 1000000;
		results[count].frames_recv = 1000000;
		results[count].loss_pct = 0.0;

		count++;
		rate -= ctx->config.loss_step_pct;
	}

	*result_count = count;
	return 0;
}

/* ============================================================================
 * Back-to-Back Test (Section 26.4)
 * ============================================================================ */

int rfc2544_back_to_back_test(rfc2544_ctx_t *ctx, uint32_t frame_size, burst_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Back-to-back test: frame_size=%u", frame_size);

	/* TODO: Implement actual burst testing */
	result->frame_size = frame_size;
	result->max_burst = 1000;
	result->burst_duration = 100.0; /* 100 us */
	result->trials = ctx->config.burst_trials;

	rfc2544_log(LOG_INFO, "Back-to-back result: max_burst=%lu frames", result->max_burst);

	return 0;
}

/* ============================================================================
 * Results Printing
 * ============================================================================ */

void rfc2544_print_results(const rfc2544_ctx_t *ctx)
{
	if (!ctx)
		return;

	printf("\n");
	printf("=================================================================\n");
	printf("RFC 2544 Test Results\n");
	printf("=================================================================\n");
	printf("Interface: %s\n", ctx->interface);
	printf("Line rate: %.2f Gbps\n", ctx->line_rate / 1e9);
	printf("\n");

	/* Throughput results */
	if (ctx->throughput_count > 0) {
		printf("Throughput Test Results (Section 26.1)\n");
		printf("-----------------------------------------------------------------\n");
		printf("%-10s %12s %12s %15s %10s\n", "Frame", "Rate", "Rate", "Rate",
		       "Iterations");
		printf("%-10s %12s %12s %15s %10s\n", "Size", "(%)", "(Mbps)", "(pps)", "");
		printf("-----------------------------------------------------------------\n");
		for (uint32_t i = 0; i < ctx->throughput_count; i++) {
			const throughput_result_t *r = &ctx->throughput_results[i];
			printf("%-10u %11.2f%% %12.2f %15lu %10u\n", r->frame_size, r->max_rate_pct,
			       r->max_rate_mbps, r->max_rate_pps, r->iterations);
		}
		printf("\n");
	}

	/* Latency results */
	if (ctx->latency_count > 0) {
		printf("Latency Test Results (Section 26.2)\n");
		printf("-----------------------------------------------------------------\n");
		printf("%-10s %10s %12s %12s %12s\n", "Frame", "Load", "Min", "Avg", "Max");
		printf("%-10s %10s %12s %12s %12s\n", "Size", "(%)", "(us)", "(us)", "(us)");
		printf("-----------------------------------------------------------------\n");
		for (uint32_t i = 0; i < ctx->latency_count; i++) {
			const latency_result_t *r = &ctx->latency_results[i];
			printf("%-10u %9.1f%% %12.1f %12.1f %12.1f\n", r->frame_size,
			       r->offered_rate_pct, r->latency.min_ns / 1000.0,
			       r->latency.avg_ns / 1000.0, r->latency.max_ns / 1000.0);
		}
		printf("\n");
	}

	/* Frame loss results */
	if (ctx->loss_count > 0) {
		printf("Frame Loss Test Results (Section 26.3)\n");
		printf("-----------------------------------------------------------------\n");
		printf("%-12s %15s %15s %12s\n", "Offered", "Frames", "Frames", "Loss");
		printf("%-12s %15s %15s %12s\n", "Load (%)", "Sent", "Received", "(%)");
		printf("-----------------------------------------------------------------\n");
		for (uint32_t i = 0; i < ctx->loss_count; i++) {
			const frame_loss_point_t *r = &ctx->loss_results[i];
			printf("%11.1f%% %15lu %15lu %11.4f%%\n", r->offered_rate_pct, r->frames_sent,
			       r->frames_recv, r->loss_pct);
		}
		printf("\n");
	}

	/* Back-to-back results */
	if (ctx->burst_count > 0) {
		printf("Back-to-Back Test Results (Section 26.4)\n");
		printf("-----------------------------------------------------------------\n");
		printf("%-10s %15s %15s %10s\n", "Frame", "Max Burst", "Duration", "Trials");
		printf("%-10s %15s %15s %10s\n", "Size", "(frames)", "(us)", "");
		printf("-----------------------------------------------------------------\n");
		for (uint32_t i = 0; i < ctx->burst_count; i++) {
			const burst_result_t *r = &ctx->burst_results[i];
			printf("%-10u %15lu %15.1f %10u\n", r->frame_size, r->max_burst,
			       r->burst_duration, r->trials);
		}
		printf("\n");
	}

	printf("=================================================================\n");
}
