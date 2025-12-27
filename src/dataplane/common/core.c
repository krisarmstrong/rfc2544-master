/*
 * core.c - RFC 2544 Test Master Core Implementation
 *
 * This file implements the main test orchestration logic:
 * - Context management
 * - Platform abstraction
 * - Test execution coordination
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"
#include "platform_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if PLATFORM_LINUX
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

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

/* Forward declarations for y1564.c */
int y1564_config_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                      y1564_config_result_t *result);
int y1564_perf_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                    uint32_t duration_sec, y1564_perf_result_t *result);
int y1564_multi_service_test(rfc2544_ctx_t *ctx, const y1564_service_t *services,
                             uint32_t service_count, y1564_config_result_t *config_results,
                             y1564_perf_result_t *perf_results);
void y1564_print_results(const y1564_config_result_t *config_results,
                         const y1564_perf_result_t *perf_results, uint32_t service_count,
                         stats_format_t format);

/* struct rfc2544_ctx is defined in rfc2544_internal.h */

/* Global log level */
static log_level_t g_log_level = LOG_INFO;

/* ============================================================================
 * Logging
 * ============================================================================ */

void rfc2544_set_log_level(log_level_t level)
{
	g_log_level = level;
}

void rfc2544_log(log_level_t level, const char *fmt, ...)
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
#else
	(void)ctx; /* Silence unused parameter warning when DPDK disabled */
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
#if PLATFORM_LINUX
	/* Try sysfs first - most reliable on modern Linux */
	char path[256];
	snprintf(path, sizeof(path), "/sys/class/net/%s/speed", interface);

	FILE *f = fopen(path, "r");
	if (f) {
		int speed_mbps = 0;
		if (fscanf(f, "%d", &speed_mbps) == 1 && speed_mbps > 0) {
			fclose(f);
			rfc2544_log(LOG_DEBUG, "Interface %s speed: %d Mbps (from sysfs)", interface, speed_mbps);
			return (uint64_t)speed_mbps * 1000000ULL;
		}
		fclose(f);
	}

	/* Try ethtool ioctl as fallback */
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock >= 0) {
		struct ifreq ifr;
		struct ethtool_cmd ecmd;

		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);

		ecmd.cmd = ETHTOOL_GSET;
		ifr.ifr_data = (void *)&ecmd;

		if (ioctl(sock, SIOCETHTOOL, &ifr) == 0) {
			uint32_t speed = ethtool_cmd_speed(&ecmd);
			close(sock);
			if (speed != (uint32_t)-1 && speed > 0) {
				rfc2544_log(LOG_DEBUG, "Interface %s speed: %u Mbps (from ethtool)", interface, speed);
				return (uint64_t)speed * 1000000ULL;
			}
		}
		close(sock);
	}
#endif

	/* Default to 10 Gbps if detection fails */
	rfc2544_log(LOG_WARN, "Could not detect interface speed for %s, assuming 10 Gbps", interface);
	return 10000000000ULL;
}

static uint64_t get_timestamp_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

/* ============================================================================
 * Context Accessor Functions (for y1564.c and other modules)
 * ============================================================================ */

const platform_ops_t *rfc2544_get_platform(const rfc2544_ctx_t *ctx)
{
	return ctx ? ctx->platform : NULL;
}

worker_ctx_t *rfc2544_get_worker(rfc2544_ctx_t *ctx, int index)
{
	if (!ctx || !ctx->workers || index < 0 || index >= ctx->num_workers)
		return NULL;
	return &ctx->workers[index];
}

uint64_t rfc2544_get_line_rate_ctx(const rfc2544_ctx_t *ctx)
{
	return ctx ? ctx->line_rate : 0;
}

void rfc2544_get_macs(const rfc2544_ctx_t *ctx, uint8_t *src_mac, uint8_t *dst_mac)
{
	if (!ctx)
		return;
	if (src_mac)
		memcpy(src_mac, ctx->local_mac, 6);
	if (dst_mac)
		memcpy(dst_mac, ctx->remote_mac, 6);
}

void rfc2544_get_ips(const rfc2544_ctx_t *ctx, uint32_t *src_ip, uint32_t *dst_ip)
{
	if (!ctx)
		return;
	if (src_ip)
		*src_ip = ctx->local_ip;
	if (dst_ip)
		*dst_ip = ctx->remote_ip;
}

bool rfc2544_is_cancelled(const rfc2544_ctx_t *ctx)
{
	return ctx ? ctx->cancel_requested : true;
}

void rfc2544_log_internal(log_level_t level, const char *fmt, ...)
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
		pthread_mutex_destroy(&ctx->seq_lock);
		pthread_mutex_destroy(&ctx->latency_lock);
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
		/* Wait for completion with timeout (max 10 seconds) */
		int timeout_count = 0;
		while (ctx->state == STATE_RUNNING && timeout_count < 1000) {
			usleep(10000);
			timeout_count++;
		}
		if (ctx->state == STATE_RUNNING) {
			rfc2544_log(LOG_ERROR, "Cleanup timeout waiting for test to stop");
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
			/* Cleanup already-initialized workers */
			for (int j = 0; j < i; j++) {
				ctx->platform->cleanup(&ctx->workers[j]);
			}
			free(ctx->workers);
			ctx->workers = NULL;
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

	case TEST_Y1564_CONFIG:
		report_progress(ctx, "Starting Y.1564 Configuration test", 0);
		{
			const y1564_config_t *y1564_cfg = &ctx->config.y1564;
			for (uint32_t i = 0; i < y1564_cfg->service_count && !ctx->cancel_requested; i++) {
				if (!y1564_cfg->services[i].enabled)
					continue;
				y1564_config_result_t config_result;
				ret = y1564_config_test(ctx, &y1564_cfg->services[i], &config_result);
				if (ret < 0)
					break;
			}
		}
		break;

	case TEST_Y1564_PERF:
		report_progress(ctx, "Starting Y.1564 Performance test", 0);
		{
			const y1564_config_t *y1564_cfg = &ctx->config.y1564;
			for (uint32_t i = 0; i < y1564_cfg->service_count && !ctx->cancel_requested; i++) {
				if (!y1564_cfg->services[i].enabled)
					continue;
				y1564_perf_result_t perf_result;
				ret = y1564_perf_test(ctx, &y1564_cfg->services[i],
				                      y1564_cfg->perf_duration_sec, &perf_result);
				if (ret < 0)
					break;
			}
		}
		break;

	case TEST_Y1564_FULL:
		report_progress(ctx, "Starting Y.1564 Full test suite", 0);
		{
			const y1564_config_t *y1564_cfg = &ctx->config.y1564;
			y1564_config_result_t config_results[Y1564_MAX_SERVICES];
			y1564_perf_result_t perf_results[Y1564_MAX_SERVICES];
			memset(config_results, 0, sizeof(config_results));
			memset(perf_results, 0, sizeof(perf_results));

			ret = y1564_multi_service_test(ctx, y1564_cfg->services, y1564_cfg->service_count,
			                               config_results, perf_results);

			if (ret == 0) {
				y1564_print_results(config_results, perf_results, y1564_cfg->service_count,
			                    ctx->config.output_format);
			}
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
 * Trial Execution Helper
 * ============================================================================ */

/* Trial result */
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
 *
 * @param ctx Test context
 * @param frame_size Frame size in bytes
 * @param rate_pct Target rate as percentage of line rate
 * @param duration_sec Trial duration in seconds
 * @param warmup_sec Warmup period in seconds
 * @param result Output trial result
 * @return 0 on success, negative on error
 */
static int run_trial(rfc2544_ctx_t *ctx, uint32_t frame_size, double rate_pct,
                     uint32_t duration_sec, uint32_t warmup_sec, trial_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	worker_ctx_t *wctx = &ctx->workers[0];

	/* Create packet template */
	uint8_t *pkt_buffer = malloc(frame_size);
	if (!pkt_buffer)
		return -ENOMEM;

	/* Default addresses - in real use, would be configured */
	uint8_t src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
	uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
	uint32_t src_ip = htonl(0x0A000001); /* 10.0.0.1 */
	uint32_t dst_ip = htonl(0x0A000002); /* 10.0.0.2 */

	/* Use configured MAC if available */
	if (ctx->local_mac[0] || ctx->local_mac[1] || ctx->local_mac[2]) {
		memcpy(src_mac, ctx->local_mac, 6);
	}
	if (ctx->remote_mac[0] || ctx->remote_mac[1] || ctx->remote_mac[2]) {
		memcpy(dst_mac, ctx->remote_mac, 6);
	}

	rfc2544_payload_t *payload = rfc2544_create_packet_template(
	    pkt_buffer, frame_size, src_mac, dst_mac, src_ip, dst_ip, 12345, 3842, 0);

	if (!payload) {
		free(pkt_buffer);
		return -EINVAL;
	}

	/* Create pacing context */
	pacing_ctx_t *pacer = pacing_create(ctx->line_rate, frame_size, rate_pct);
	if (!pacer) {
		free(pkt_buffer);
		return -ENOMEM;
	}

	/* Create trial timer */
	trial_timer_t *timer = trial_timer_create(duration_sec, warmup_sec);
	if (!timer) {
		pacing_destroy(pacer);
		free(pkt_buffer);
		return -ENOMEM;
	}

	/* Create sequence tracker (use uint64_t to avoid overflow at high rates) */
	uint64_t expected_packets = (uint64_t)(calc_max_pps(ctx->line_rate, frame_size) *
	                                       rate_pct / 100.0 * duration_sec);
	seq_tracker_t *tracker = rfc2544_seq_tracker_create(expected_packets + 1000);
	if (!tracker) {
		trial_timer_destroy(timer);
		pacing_destroy(pacer);
		free(pkt_buffer);
		return -ENOMEM;
	}

	/* Prepare TX packet */
	packet_t tx_pkt;
	tx_pkt.data = pkt_buffer;
	tx_pkt.len = frame_size;

	/* RX buffer */
	packet_t rx_pkts[64];
	memset(rx_pkts, 0, sizeof(rx_pkts));

	/* Latency samples */
	uint64_t *latency_samples = NULL;
	uint32_t latency_count = 0;
	uint32_t latency_capacity = 10000;
	if (ctx->config.measure_latency) {
		latency_samples = malloc(latency_capacity * sizeof(uint64_t));
	}

	/* Start trial */
	uint32_t seq_num = 0;
	uint64_t packets_sent = 0;
	uint64_t packets_recv = 0;
	uint64_t bytes_sent = 0;
	bool in_measurement = false;

	trial_timer_start(timer);
	pacing_reset(pacer);

	rfc2544_log(LOG_DEBUG, "Trial started: rate=%.2f%%, duration=%us, warmup=%us",
	            rate_pct, duration_sec, warmup_sec);

	while (!trial_timer_expired(timer) && !ctx->cancel_requested) {
		/* Check if we've exited warmup */
		if (!in_measurement && !trial_timer_in_warmup(timer)) {
			in_measurement = true;
			/* Reset counters at start of measurement */
			seq_num = 0;
			packets_sent = 0;
			packets_recv = 0;
			bytes_sent = 0;
			pacing_reset(pacer);
		}

		/* TX: Send packet at paced rate */
		uint64_t tx_ts = pacing_wait(pacer);
		rfc2544_stamp_packet(payload, seq_num, tx_ts);
		tx_pkt.timestamp = tx_ts;
		tx_pkt.seq_num = seq_num;

		int sent = ctx->platform->send_batch(wctx, &tx_pkt, 1);
		if (sent > 0 && in_measurement) {
			packets_sent++;
			bytes_sent += frame_size;
			seq_num++;
			pacing_record_tx(pacer, 1, frame_size);
		}

		/* RX: Check for returned packets (non-blocking) */
		int recv_count = ctx->platform->recv_batch(wctx, rx_pkts, 64);
		for (int i = 0; i < recv_count; i++) {
			if (rfc2544_is_valid_response(rx_pkts[i].data, rx_pkts[i].len)) {
				uint32_t rx_seq = rfc2544_get_seq_num(rx_pkts[i].data, rx_pkts[i].len);

				if (in_measurement) {
					rfc2544_seq_tracker_record(tracker, rx_seq);
					packets_recv++;

					/* Record latency if enabled */
					if (latency_samples && latency_count < latency_capacity) {
						uint64_t tx_ts_pkt = rfc2544_get_tx_timestamp(
						    rx_pkts[i].data, rx_pkts[i].len);
						uint64_t latency = rx_pkts[i].timestamp - tx_ts_pkt;
						latency_samples[latency_count++] = latency;
					}
				}
			}
		}

		/* Release RX packets */
		if (recv_count > 0) {
			ctx->platform->release_batch(wctx, rx_pkts, recv_count);
		}
	}

	/* Wait a bit for straggler packets */
	for (int i = 0; i < 10 && !ctx->cancel_requested; i++) {
		usleep(10000); /* 10ms */
		int recv_count = ctx->platform->recv_batch(wctx, rx_pkts, 64);
		for (int j = 0; j < recv_count; j++) {
			if (rfc2544_is_valid_response(rx_pkts[j].data, rx_pkts[j].len)) {
				uint32_t rx_seq = rfc2544_get_seq_num(rx_pkts[j].data, rx_pkts[j].len);
				rfc2544_seq_tracker_record(tracker, rx_seq);
				packets_recv++;
			}
		}
		if (recv_count > 0) {
			ctx->platform->release_batch(wctx, rx_pkts, recv_count);
		}
	}

	/* Calculate results */
	double elapsed = trial_timer_elapsed(timer);
	result->packets_sent = packets_sent;
	result->packets_recv = packets_recv;
	result->bytes_sent = bytes_sent;
	result->elapsed_sec = elapsed;

	if (packets_sent > 0) {
		result->loss_pct = 100.0 * (packets_sent - packets_recv) / packets_sent;
	} else {
		result->loss_pct = 0.0;
	}

	if (elapsed > 0) {
		result->achieved_pps = packets_sent / elapsed;
		result->achieved_mbps = (bytes_sent * 8.0) / (elapsed * 1e6);
	}

	/* Calculate latency stats */
	if (latency_samples && latency_count > 0) {
		rfc2544_calc_latency_stats(latency_samples, latency_count, &result->latency);
	}

	rfc2544_log(LOG_DEBUG, "Trial complete: sent=%lu, recv=%lu, loss=%.4f%%",
	            packets_sent, packets_recv, result->loss_pct);

	/* Cleanup */
	free(latency_samples);
	rfc2544_seq_tracker_destroy(tracker);
	trial_timer_destroy(timer);
	pacing_destroy(pacer);
	free(pkt_buffer);

	return 0;
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
	uint64_t total_frames = 0;

	while ((high - low) > ctx->config.resolution_pct &&
	       iterations < ctx->config.max_iterations && !ctx->cancel_requested) {

		double current_rate = (low + high) / 2.0;

		rfc2544_log(LOG_DEBUG, "Iteration %u: testing %.2f%%", iterations, current_rate);

		/* Run trial at current rate */
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, current_rate,
		                    ctx->config.trial_duration_sec,
		                    ctx->config.warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
			return ret;
		}

		total_frames += trial.packets_sent;

		if (trial.loss_pct <= ctx->config.acceptable_loss) {
			/* Success - try higher rate */
			best_rate = current_rate;
			low = current_rate;
			rfc2544_log(LOG_DEBUG, "  Pass: loss=%.4f%%, new best=%.2f%%",
			            trial.loss_pct, best_rate);

			/* Store latency from best rate */
			result->latency = trial.latency;
		} else {
			/* Failure - try lower rate */
			high = current_rate;
			rfc2544_log(LOG_DEBUG, "  Fail: loss=%.4f%%, reducing rate", trial.loss_pct);
		}

		iterations++;
	}

	/* Store result */
	result->frame_size = frame_size;
	result->max_rate_pct = best_rate;
	result->max_rate_mbps = (ctx->line_rate * best_rate / 100.0) / 1e6;
	result->max_rate_pps = (uint64_t)(max_pps * best_rate / 100.0);
	result->iterations = iterations;
	result->frames_tested = total_frames;

	rfc2544_log(LOG_INFO, "Throughput result: %.2f%% (%.2f Mbps, %lu pps)",
	            result->max_rate_pct, result->max_rate_mbps, result->max_rate_pps);

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

	/* Enable latency measurement for this trial */
	bool orig_measure = ctx->config.measure_latency;
	ctx->config.measure_latency = true;

	/* Run trial at specified load */
	trial_result_t trial;
	int ret = run_trial(ctx, frame_size, load_pct,
	                    ctx->config.trial_duration_sec,
	                    ctx->config.warmup_sec, &trial);

	ctx->config.measure_latency = orig_measure;

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Latency trial failed: %d", ret);
		return ret;
	}

	result->frame_size = frame_size;
	result->offered_rate_pct = load_pct;
	result->latency = trial.latency;

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
		rfc2544_log(LOG_DEBUG, "Testing at %.1f%% load", rate);

		/* Run trial at this rate */
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, rate,
		                    ctx->config.trial_duration_sec,
		                    ctx->config.warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Frame loss trial failed: %d", ret);
			return ret;
		}

		results[count].offered_rate_pct = rate;
		results[count].actual_rate_mbps = trial.achieved_mbps;
		results[count].frames_sent = trial.packets_sent;
		results[count].frames_recv = trial.packets_recv;
		results[count].loss_pct = trial.loss_pct;

		rfc2544_log(LOG_DEBUG, "  Result: sent=%lu, recv=%lu, loss=%.4f%%",
		            trial.packets_sent, trial.packets_recv, trial.loss_pct);

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

	/* Back-to-back test: send bursts of increasing size at line rate
	 * until we find the maximum burst that can be received without loss.
	 *
	 * Per RFC 2544, we send a burst, wait for all frames to return,
	 * then try a larger burst. Binary search would be more efficient
	 * but linear search is more traditional for this test.
	 */

	uint64_t max_burst = 0;
	uint64_t current_burst = ctx->config.initial_burst;
	uint32_t trials_passed = 0;

	/* Calculate max theoretical burst based on memory */
	uint64_t max_possible = 1000000; /* Cap at 1M frames */

	while (current_burst <= max_possible && !ctx->cancel_requested) {
		bool all_passed = true;

		/* Run multiple trials at this burst size */
		for (uint32_t trial = 0; trial < ctx->config.burst_trials && !ctx->cancel_requested;
		     trial++) {

			/* Run burst trial at 100% rate for very short duration */
			trial_result_t trial_result;
			uint32_t burst_duration_ms = (current_burst * 1000) /
			                             calc_max_pps(ctx->line_rate, frame_size);
			if (burst_duration_ms < 1)
				burst_duration_ms = 1;

			/* Use trial helper with short duration */
			int ret = run_trial(ctx, frame_size, 100.0,
			                    burst_duration_ms / 1000 + 1, 0, &trial_result);

			if (ret < 0) {
				rfc2544_log(LOG_ERROR, "Burst trial failed: %d", ret);
				return ret;
			}

			if (trial_result.loss_pct > 0) {
				all_passed = false;
				break;
			}
		}

		if (all_passed) {
			max_burst = current_burst;
			trials_passed++;
			current_burst *= 2; /* Double burst size */
		} else {
			/* Found limit - could do binary search refinement here */
			break;
		}
	}

	result->frame_size = frame_size;
	result->max_burst = max_burst;
	result->burst_duration = (double)max_burst * 1e6 / calc_max_pps(ctx->line_rate, frame_size);
	result->trials = trials_passed;

	rfc2544_log(LOG_INFO, "Back-to-back result: max_burst=%lu frames (%.1f us)",
	            result->max_burst, result->burst_duration);

	return 0;
}

/* ============================================================================
 * System Recovery Test (Section 26.5)
 * ============================================================================ */

int rfc2544_system_recovery_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                 double throughput_pct, uint32_t overload_sec,
                                 recovery_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "System recovery test: frame_size=%u, throughput=%.2f%%",
	            frame_size, throughput_pct);

	memset(result, 0, sizeof(*result));
	result->frame_size = frame_size;
	result->overload_rate_pct = throughput_pct * 1.1; /* 110% of throughput */
	result->recovery_rate_pct = throughput_pct * 0.5; /* 50% of throughput */
	result->overload_sec = overload_sec;

	/*
	 * RFC 2544 Section 26.5 - System Recovery:
	 * 1. Send at 110% of determined throughput for overload_sec seconds
	 * 2. Drop to 50% of throughput
	 * 3. Measure time until frame loss reaches zero
	 */

	/* Phase 1: Overload */
	rfc2544_log(LOG_INFO, "Phase 1: Sending at %.1f%% for %u seconds (overload)",
	            result->overload_rate_pct, overload_sec);

	trial_result_t overload_trial;
	int ret = run_trial(ctx, frame_size, result->overload_rate_pct, overload_sec, 0, &overload_trial);
	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Overload phase failed: %d", ret);
		return ret;
	}

	/* Phase 2: Recovery - send at 50% and measure time to zero loss */
	rfc2544_log(LOG_INFO, "Phase 2: Dropping to %.1f%% and measuring recovery time",
	            result->recovery_rate_pct);

	uint64_t recovery_start = get_timestamp_ns();
	uint64_t frames_lost = 0;
	bool recovered = false;
	uint32_t check_interval_ms = 100; /* Check every 100ms */
	uint32_t max_recovery_sec = 60;   /* Max 60 seconds to recover */

	for (uint32_t i = 0; i < (max_recovery_sec * 1000 / check_interval_ms); i++) {
		if (ctx->cancel_requested)
			break;

		trial_result_t recovery_trial;
		/* Run short trial at recovery rate */
		ret = run_trial(ctx, frame_size, result->recovery_rate_pct, 1, 0, &recovery_trial);
		if (ret < 0)
			break;

		if (recovery_trial.loss_pct <= 0.001) { /* Effectively zero loss */
			recovered = true;
			result->recovery_time_ms = (get_timestamp_ns() - recovery_start) / 1e6;
			break;
		}

		frames_lost += (recovery_trial.packets_sent - recovery_trial.packets_recv);
		usleep(check_interval_ms * 1000);
	}

	result->frames_lost = frames_lost;
	result->trials = 1;

	if (recovered) {
		rfc2544_log(LOG_INFO, "System recovery result: %.2f ms, %lu frames lost",
		            result->recovery_time_ms, result->frames_lost);
	} else {
		rfc2544_log(LOG_WARN, "System did not recover within %u seconds", max_recovery_sec);
		result->recovery_time_ms = -1.0; /* Did not recover */
	}

	return 0;
}

/* ============================================================================
 * Reset Test (Section 26.6)
 * ============================================================================ */

int rfc2544_reset_test(rfc2544_ctx_t *ctx, uint32_t frame_size, reset_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Reset test: frame_size=%u", frame_size);
	rfc2544_log(LOG_WARN, "NOTE: Reset test requires external reset trigger");

	memset(result, 0, sizeof(*result));
	result->frame_size = frame_size;
	result->manual_reset = true;

	/*
	 * RFC 2544 Section 26.6 - Reset:
	 * This test measures the time for a device to resume forwarding
	 * after various reset conditions (power, software, hardware).
	 *
	 * The test procedure is:
	 * 1. Send continuous traffic at throughput rate
	 * 2. Trigger reset (manually or via external mechanism)
	 * 3. Measure time until frames start being forwarded again
	 *
	 * NOTE: This implementation waits for reset trigger via user input
	 * or external signal. For automated testing, integrate with power
	 * control or management interface.
	 */

	rfc2544_log(LOG_INFO, "Starting background traffic at throughput rate");
	rfc2544_log(LOG_INFO, "Trigger device reset when ready...");

	/* Send continuous traffic and monitor for interruption */
	uint64_t monitor_start = get_timestamp_ns();
	(void)monitor_start; /* Used for potential future enhancements */
	uint64_t first_loss_time = 0;
	uint64_t recovery_time = 0;
	uint64_t frames_lost = 0;
	bool loss_detected = false;
	bool recovered = false;

	uint32_t max_wait_sec = 300; /* 5 minutes max wait for reset */

	for (uint32_t sec = 0; sec < max_wait_sec && !ctx->cancel_requested; sec++) {
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, 100.0, 1, 0, &trial);
		if (ret < 0) {
			continue;
		}

		if (trial.loss_pct > 0.1) { /* Significant loss detected */
			if (!loss_detected) {
				loss_detected = true;
				first_loss_time = get_timestamp_ns();
				rfc2544_log(LOG_INFO, "Reset detected - loss started");
			}
			frames_lost += (trial.packets_sent - trial.packets_recv);
		} else if (loss_detected && trial.loss_pct <= 0.001) {
			/* Recovery after loss */
			recovery_time = get_timestamp_ns();
			recovered = true;
			rfc2544_log(LOG_INFO, "Forwarding resumed");
			break;
		}
	}

	if (loss_detected && recovered) {
		result->reset_time_ms = (recovery_time - first_loss_time) / 1e6;
		result->frames_lost = frames_lost;
		result->trials = 1;

		rfc2544_log(LOG_INFO, "Reset test result: %.2f ms reset time, %lu frames lost",
		            result->reset_time_ms, result->frames_lost);
	} else if (!loss_detected) {
		rfc2544_log(LOG_WARN, "No reset detected within %u seconds", max_wait_sec);
		result->reset_time_ms = -1.0;
	} else {
		rfc2544_log(LOG_WARN, "Reset detected but device did not recover");
		result->reset_time_ms = -1.0;
	}

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
			printf("%-10u %11.2f%% %12.2f %15.0f %10u\n", r->frame_size, r->max_rate_pct,
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
			printf("%11.1f%% %15llu %15llu %11.4f%%\n", r->offered_rate_pct,
			       (unsigned long long)r->frames_sent,
			       (unsigned long long)r->frames_recv, r->loss_pct);
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
			printf("%-10u %15llu %15.1f %10u\n", r->frame_size,
			       (unsigned long long)r->max_burst, r->burst_duration, r->trials);
		}
		printf("\n");
	}

	printf("=================================================================\n");
}
