/*
 * y1564.c - ITU-T Y.1564 (EtherSAM) Test Implementation
 *
 * This file implements the ITU-T Y.1564 Ethernet Service Activation Methodology:
 * - Service Configuration Test (step test at 25%, 50%, 75%, 100% CIR)
 * - Service Performance Test (sustained traffic at CIR for extended duration)
 * - Multi-service simultaneous testing (up to 8 services)
 *
 * Y.1564 tests services against SLA parameters (CIR, EIR, FD, FDV, FLR)
 * rather than raw throughput like RFC 2544.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"
#include "platform_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Internal packet structure (matches core.c) */
typedef struct {
	uint8_t *data;
	uint32_t len;
	uint64_t timestamp;
	uint32_t seq_num;
	void *platform_data;
} packet_t;

/* Platform operations interface (matches core.c) */
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

/* Y.1564 payload structure (matches packet.c) */
typedef struct __attribute__((packed)) {
	uint8_t signature[Y1564_SIG_LEN];
	uint32_t seq_num;
	uint64_t timestamp;
	uint32_t service_id;
	uint8_t flags;
} y1564_payload_t;

/* Forward declarations from packet.c */
y1564_payload_t *y1564_create_packet_template(uint8_t *buffer, uint32_t frame_size,
                                               const uint8_t *src_mac, const uint8_t *dst_mac,
                                               uint32_t src_ip, uint32_t dst_ip,
                                               uint16_t src_port, uint16_t dst_port,
                                               uint32_t service_id, uint8_t dscp);
void y1564_stamp_packet(y1564_payload_t *payload, uint32_t seq_num, uint64_t timestamp_ns);
bool y1564_is_valid_response(const uint8_t *data, uint32_t len);
uint32_t y1564_get_seq_num(const uint8_t *data, uint32_t len);
uint64_t y1564_get_tx_timestamp(const uint8_t *data, uint32_t len);
uint32_t y1564_get_service_id(const uint8_t *data, uint32_t len);

/* Forward declarations from pacing.c */
typedef struct pacing_ctx pacing_ctx_t;
typedef struct trial_timer trial_timer_t;
typedef struct seq_tracker seq_tracker_t;

pacing_ctx_t *pacing_create(uint64_t line_rate_bps, uint32_t frame_size, double rate_pct);
void pacing_set_rate(pacing_ctx_t *ctx, double rate_pct);
uint64_t pacing_wait(pacing_ctx_t *ctx);
void pacing_record_tx(pacing_ctx_t *ctx, uint32_t packets, uint32_t bytes);
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
void rfc2544_seq_tracker_destroy(seq_tracker_t *tracker);

uint64_t calc_max_pps(uint64_t line_rate_bps, uint32_t frame_size);

/* External context access (defined in core.c) */
extern const platform_ops_t *rfc2544_get_platform(const rfc2544_ctx_t *ctx);
extern worker_ctx_t *rfc2544_get_worker(rfc2544_ctx_t *ctx, int index);
extern uint64_t rfc2544_get_line_rate_ctx(const rfc2544_ctx_t *ctx);
extern void rfc2544_get_macs(const rfc2544_ctx_t *ctx, uint8_t *src_mac, uint8_t *dst_mac);
extern void rfc2544_get_ips(const rfc2544_ctx_t *ctx, uint32_t *src_ip, uint32_t *dst_ip);
extern bool rfc2544_is_cancelled(const rfc2544_ctx_t *ctx);

/* Logging (from core.c) */
extern void rfc2544_log_internal(int level, const char *fmt, ...);
#define y1564_log(level, ...) rfc2544_log_internal(level, "[Y.1564] " __VA_ARGS__)

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static uint64_t __attribute__((unused)) get_timestamp_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

/**
 * Calculate packets per second for a given rate in Mbps
 */
static uint64_t __attribute__((unused)) rate_to_pps(double rate_mbps, uint32_t frame_size)
{
	/* Wire size includes preamble (8) + IFG (12) = 20 bytes */
	uint32_t wire_size = frame_size + 20;
	uint64_t bits_per_packet = wire_size * 8;
	uint64_t bits_per_sec = (uint64_t)(rate_mbps * 1e6);
	return bits_per_sec / bits_per_packet;
}

/**
 * Calculate achieved rate in Mbps from packet count
 */
static double calc_rate_mbps(uint64_t packets, uint32_t frame_size, double elapsed_sec)
{
	if (elapsed_sec <= 0)
		return 0.0;
	uint64_t bytes = packets * frame_size;
	return (bytes * 8.0) / (elapsed_sec * 1e6);
}

/**
 * Compare latencies for qsort
 */
static int compare_latency(const void *a, const void *b)
{
	uint64_t la = *(const uint64_t *)a;
	uint64_t lb = *(const uint64_t *)b;
	if (la < lb)
		return -1;
	if (la > lb)
		return 1;
	return 0;
}

/**
 * Calculate latency statistics from samples
 */
static void calc_latency_stats(const uint64_t *samples, uint32_t count, double *avg_ms,
                               double *min_ms, double *max_ms, double *jitter_ms)
{
	/* Sanity check: limit to 10M samples (80MB allocation) */
	if (count == 0 || count > 10000000) {
		*avg_ms = 0.0;
		*min_ms = 0.0;
		*max_ms = 0.0;
		*jitter_ms = 0.0;
		return;
	}

	/* Make a copy for sorting */
	uint64_t *sorted = malloc(count * sizeof(uint64_t));
	if (!sorted) {
		*avg_ms = 0.0;
		*min_ms = 0.0;
		*max_ms = 0.0;
		*jitter_ms = 0.0;
		return;
	}
	memcpy(sorted, samples, count * sizeof(uint64_t));
	qsort(sorted, count, sizeof(uint64_t), compare_latency);

	/* Calculate stats */
	uint64_t sum = 0;
	for (uint32_t i = 0; i < count; i++) {
		sum += sorted[i];
	}

	*avg_ms = (sum / (double)count) / 1e6;
	*min_ms = sorted[0] / 1e6;
	*max_ms = sorted[count - 1] / 1e6;

	/* FDV = max - min (simplified definition per Y.1564) */
	*jitter_ms = (*max_ms - *min_ms);

	free(sorted);
}

/* ============================================================================
 * Default Configuration
 * ============================================================================ */

void y1564_default_sla(y1564_sla_t *sla)
{
	if (!sla)
		return;

	memset(sla, 0, sizeof(*sla));
	sla->cir_mbps = 100.0;           /* 100 Mbps default CIR */
	sla->eir_mbps = 0.0;             /* No EIR by default */
	sla->cbs_bytes = 12000;          /* 12KB committed burst */
	sla->ebs_bytes = 0;              /* No excess burst */
	sla->fd_threshold_ms = 10.0;     /* 10ms frame delay threshold */
	sla->fdv_threshold_ms = 5.0;     /* 5ms jitter threshold */
	sla->flr_threshold_pct = 0.01;   /* 0.01% frame loss threshold */
}

void y1564_default_config(y1564_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	/* Default step percentages per ITU-T Y.1564 */
	config->config_steps[0] = 25.0;
	config->config_steps[1] = 50.0;
	config->config_steps[2] = 75.0;
	config->config_steps[3] = 100.0;

	/* Default durations */
	config->step_duration_sec = 60;        /* 1 minute per step */
	config->perf_duration_sec = 15 * 60;   /* 15 minutes performance test */

	/* Default to running both tests */
	config->run_config_test = true;
	config->run_perf_test = true;

	/* No services configured by default */
	config->service_count = 0;

	/* Initialize default SLA for all service slots */
	for (int i = 0; i < Y1564_MAX_SERVICES; i++) {
		config->services[i].service_id = i + 1;
		snprintf(config->services[i].service_name, sizeof(config->services[i].service_name),
		         "Service%d", i + 1);
		y1564_default_sla(&config->services[i].sla);
		config->services[i].frame_size = 512;
		config->services[i].cos = 0;  /* Best effort by default */
		config->services[i].enabled = false;
	}
}

/* ============================================================================
 * Y.1564 Step Trial Execution
 * ============================================================================ */

/**
 * Y.1564 step trial result (internal)
 */
typedef struct {
	uint64_t frames_tx;
	uint64_t frames_rx;
	double elapsed_sec;
	double achieved_mbps;
	double flr_pct;
	double fd_avg_ms;
	double fd_min_ms;
	double fd_max_ms;
	double fdv_ms;
} y1564_trial_t;

/**
 * Run a single Y.1564 step trial
 *
 * @param ctx          Test context
 * @param service      Service configuration
 * @param rate_mbps    Target rate in Mbps
 * @param duration_sec Trial duration
 * @param warmup_sec   Warmup period
 * @param result       Output trial result
 * @return 0 on success, negative on error
 */
static int y1564_run_step(rfc2544_ctx_t *ctx, const y1564_service_t *service, double rate_mbps,
                          uint32_t duration_sec, uint32_t warmup_sec, y1564_trial_t *result)
{
	if (!ctx || !service || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	/* Get platform and worker */
	const platform_ops_t *platform = rfc2544_get_platform(ctx);
	worker_ctx_t *wctx = rfc2544_get_worker(ctx, 0);
	if (!platform || !wctx)
		return -EINVAL;

	uint64_t line_rate = rfc2544_get_line_rate_ctx(ctx);
	uint32_t frame_size = service->frame_size;

	/* Create packet template */
	uint8_t *pkt_buffer = malloc(frame_size);
	if (!pkt_buffer)
		return -ENOMEM;

	/* Get MAC and IP addresses */
	uint8_t src_mac[6], dst_mac[6];
	uint32_t src_ip, dst_ip;
	rfc2544_get_macs(ctx, src_mac, dst_mac);
	rfc2544_get_ips(ctx, &src_ip, &dst_ip);

	/* Create Y.1564 packet with service DSCP marking */
	y1564_payload_t *payload = y1564_create_packet_template(pkt_buffer, frame_size, src_mac,
	                                                        dst_mac, src_ip, dst_ip, 12345, 3842,
	                                                        service->service_id, service->cos);
	if (!payload) {
		free(pkt_buffer);
		return -EINVAL;
	}

	/* Calculate target rate as percentage of line rate */
	double rate_pct = (rate_mbps * 1e6 * 100.0) / line_rate;
	if (rate_pct > 100.0)
		rate_pct = 100.0;

	/* Create pacing context */
	pacing_ctx_t *pacer = pacing_create(line_rate, frame_size, rate_pct);
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

	/* Allocate latency sample buffer */
	uint32_t latency_capacity = 100000;
	uint64_t *latency_samples = malloc(latency_capacity * sizeof(uint64_t));
	if (!latency_samples) {
		trial_timer_destroy(timer);
		pacing_destroy(pacer);
		free(pkt_buffer);
		return -ENOMEM;
	}
	uint32_t latency_count = 0;

	/* Prepare TX packet */
	packet_t tx_pkt;
	tx_pkt.data = pkt_buffer;
	tx_pkt.len = frame_size;

	/* RX buffer */
	packet_t rx_pkts[64];
	memset(rx_pkts, 0, sizeof(rx_pkts));

	/* Start trial */
	uint32_t seq_num = 0;
	uint64_t frames_tx = 0;
	uint64_t frames_rx = 0;
	bool in_measurement = false;

	trial_timer_start(timer);
	pacing_reset(pacer);

	y1564_log(LOG_DEBUG, "Step started: service=%u, rate=%.2f Mbps, duration=%us",
	          service->service_id, rate_mbps, duration_sec);

	while (!trial_timer_expired(timer) && !rfc2544_is_cancelled(ctx)) {
		/* Check warmup completion */
		if (!in_measurement && !trial_timer_in_warmup(timer)) {
			in_measurement = true;
			seq_num = 0;
			frames_tx = 0;
			frames_rx = 0;
			latency_count = 0;
			pacing_reset(pacer);
		}

		/* TX: Send packet at paced rate */
		uint64_t tx_ts = pacing_wait(pacer);
		y1564_stamp_packet(payload, seq_num, tx_ts);
		tx_pkt.timestamp = tx_ts;
		tx_pkt.seq_num = seq_num;

		int sent = platform->send_batch(wctx, &tx_pkt, 1);
		if (sent > 0 && in_measurement) {
			frames_tx++;
			seq_num++;
			pacing_record_tx(pacer, 1, frame_size);
		}

		/* RX: Check for returned packets */
		int recv_count = platform->recv_batch(wctx, rx_pkts, 64);
		for (int i = 0; i < recv_count; i++) {
			if (y1564_is_valid_response(rx_pkts[i].data, rx_pkts[i].len)) {
				uint32_t rx_service = y1564_get_service_id(rx_pkts[i].data, rx_pkts[i].len);

				/* Only count packets for this service */
				if (rx_service == service->service_id && in_measurement) {
					frames_rx++;

					/* Record latency */
					if (latency_count < latency_capacity) {
						uint64_t tx_ts_pkt =
						    y1564_get_tx_timestamp(rx_pkts[i].data, rx_pkts[i].len);
						uint64_t latency = rx_pkts[i].timestamp - tx_ts_pkt;
						latency_samples[latency_count++] = latency;
					}
				}
			}
		}

		if (recv_count > 0) {
			platform->release_batch(wctx, rx_pkts, recv_count);
		}
	}

	/* Wait for straggler packets */
	for (int i = 0; i < 10 && !rfc2544_is_cancelled(ctx); i++) {
		usleep(10000);
		int recv_count = platform->recv_batch(wctx, rx_pkts, 64);
		for (int j = 0; j < recv_count; j++) {
			if (y1564_is_valid_response(rx_pkts[j].data, rx_pkts[j].len)) {
				uint32_t rx_service = y1564_get_service_id(rx_pkts[j].data, rx_pkts[j].len);
				if (rx_service == service->service_id) {
					frames_rx++;
					if (latency_count < latency_capacity) {
						uint64_t tx_ts_pkt =
						    y1564_get_tx_timestamp(rx_pkts[j].data, rx_pkts[j].len);
						uint64_t latency = rx_pkts[j].timestamp - tx_ts_pkt;
						latency_samples[latency_count++] = latency;
					}
				}
			}
		}
		if (recv_count > 0) {
			platform->release_batch(wctx, rx_pkts, recv_count);
		}
	}

	/* Calculate results */
	double elapsed = trial_timer_elapsed(timer);
	result->frames_tx = frames_tx;
	result->frames_rx = frames_rx;
	result->elapsed_sec = elapsed;
	result->achieved_mbps = calc_rate_mbps(frames_tx, frame_size, elapsed);

	if (frames_tx > 0) {
		result->flr_pct = 100.0 * (frames_tx - frames_rx) / frames_tx;
	}

	/* Calculate latency stats */
	calc_latency_stats(latency_samples, latency_count, &result->fd_avg_ms, &result->fd_min_ms,
	                   &result->fd_max_ms, &result->fdv_ms);

	y1564_log(LOG_DEBUG, "Step complete: tx=%lu, rx=%lu, FLR=%.4f%%, FD=%.2fms, FDV=%.2fms",
	          frames_tx, frames_rx, result->flr_pct, result->fd_avg_ms, result->fdv_ms);

	/* Cleanup */
	free(latency_samples);
	trial_timer_destroy(timer);
	pacing_destroy(pacer);
	free(pkt_buffer);

	return 0;
}

/* ============================================================================
 * Service Configuration Test
 * ============================================================================ */

int y1564_config_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                      y1564_config_result_t *result)
{
	if (!ctx || !service || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	result->service_id = service->service_id;

	/* Get Y.1564 config from context */
	const y1564_config_t *y1564_cfg = &ctx->config.y1564;
	uint32_t step_duration = y1564_cfg->step_duration_sec;
	uint32_t warmup_sec = 2;  /* 2 second warmup per step */

	y1564_log(LOG_INFO, "Service Configuration Test: service=%u (%s), CIR=%.2f Mbps",
	          service->service_id, service->service_name, service->sla.cir_mbps);

	bool all_steps_pass = true;

	/* Run each step */
	for (int step = 0; step < Y1564_CONFIG_STEPS; step++) {
		double step_pct = y1564_cfg->config_steps[step];
		double step_rate = service->sla.cir_mbps * step_pct / 100.0;

		y1564_log(LOG_INFO, "  Step %d: %.0f%% CIR (%.2f Mbps)", step + 1, step_pct, step_rate);

		/* Run the step trial */
		y1564_trial_t trial;
		int ret = y1564_run_step(ctx, service, step_rate, step_duration, warmup_sec, &trial);

		if (ret < 0) {
			y1564_log(LOG_ERROR, "Step %d failed: %d", step + 1, ret);
			return ret;
		}

		/* Check for cancellation */
		if (rfc2544_is_cancelled(ctx)) {
			return -ECANCELED;
		}

		/* Store step result */
		y1564_step_result_t *sr = &result->steps[step];
		sr->step = step + 1;
		sr->offered_rate_pct = step_pct;
		sr->achieved_rate_mbps = trial.achieved_mbps;
		sr->frames_tx = trial.frames_tx;
		sr->frames_rx = trial.frames_rx;
		sr->flr_pct = trial.flr_pct;
		sr->fd_avg_ms = trial.fd_avg_ms;
		sr->fd_min_ms = trial.fd_min_ms;
		sr->fd_max_ms = trial.fd_max_ms;
		sr->fdv_ms = trial.fdv_ms;

		/* Evaluate pass/fail against SLA thresholds */
		sr->flr_pass = (trial.flr_pct <= service->sla.flr_threshold_pct);
		sr->fd_pass = (trial.fd_avg_ms <= service->sla.fd_threshold_ms);
		sr->fdv_pass = (trial.fdv_ms <= service->sla.fdv_threshold_ms);
		sr->step_pass = sr->flr_pass && sr->fd_pass && sr->fdv_pass;

		if (!sr->step_pass) {
			all_steps_pass = false;
		}

		y1564_log(LOG_INFO, "    Result: FLR=%.4f%% (%s), FD=%.2fms (%s), FDV=%.2fms (%s) -> %s",
		          sr->flr_pct, sr->flr_pass ? "PASS" : "FAIL", sr->fd_avg_ms,
		          sr->fd_pass ? "PASS" : "FAIL", sr->fdv_ms, sr->fdv_pass ? "PASS" : "FAIL",
		          sr->step_pass ? "PASS" : "FAIL");
	}

	result->service_pass = all_steps_pass;

	y1564_log(LOG_INFO, "Service Configuration Test %s: service=%u (%s)",
	          result->service_pass ? "PASSED" : "FAILED", service->service_id,
	          service->service_name);

	return 0;
}

/* ============================================================================
 * Service Performance Test
 * ============================================================================ */

int y1564_perf_test(rfc2544_ctx_t *ctx, const y1564_service_t *service, uint32_t duration_sec,
                    y1564_perf_result_t *result)
{
	if (!ctx || !service || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	result->service_id = service->service_id;
	result->duration_sec = duration_sec;

	uint32_t warmup_sec = 5;  /* 5 second warmup for performance test */

	y1564_log(LOG_INFO, "Service Performance Test: service=%u (%s), CIR=%.2f Mbps, duration=%um",
	          service->service_id, service->service_name, service->sla.cir_mbps,
	          duration_sec / 60);

	/* Run performance trial at full CIR */
	y1564_trial_t trial;
	int ret = y1564_run_step(ctx, service, service->sla.cir_mbps, duration_sec, warmup_sec, &trial);

	if (ret < 0) {
		y1564_log(LOG_ERROR, "Performance test failed: %d", ret);
		return ret;
	}

	if (rfc2544_is_cancelled(ctx)) {
		return -ECANCELED;
	}

	/* Store results */
	result->frames_tx = trial.frames_tx;
	result->frames_rx = trial.frames_rx;
	result->flr_pct = trial.flr_pct;
	result->fd_avg_ms = trial.fd_avg_ms;
	result->fd_min_ms = trial.fd_min_ms;
	result->fd_max_ms = trial.fd_max_ms;
	result->fdv_ms = trial.fdv_ms;

	/* Evaluate pass/fail */
	result->flr_pass = (trial.flr_pct <= service->sla.flr_threshold_pct);
	result->fd_pass = (trial.fd_avg_ms <= service->sla.fd_threshold_ms);
	result->fdv_pass = (trial.fdv_ms <= service->sla.fdv_threshold_ms);
	result->service_pass = result->flr_pass && result->fd_pass && result->fdv_pass;

	y1564_log(LOG_INFO, "Service Performance Test %s: FLR=%.4f%% (%s), FD=%.2fms (%s), "
	                    "FDV=%.2fms (%s)",
	          result->service_pass ? "PASSED" : "FAILED", result->flr_pct,
	          result->flr_pass ? "PASS" : "FAIL", result->fd_avg_ms,
	          result->fd_pass ? "PASS" : "FAIL", result->fdv_ms,
	          result->fdv_pass ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Multi-Service Test
 * ============================================================================ */

int y1564_multi_service_test(rfc2544_ctx_t *ctx, const y1564_service_t *services,
                             uint32_t service_count, y1564_config_result_t *config_results,
                             y1564_perf_result_t *perf_results)
{
	if (!ctx || !services || service_count == 0)
		return -EINVAL;

	if (service_count > Y1564_MAX_SERVICES) {
		y1564_log(LOG_ERROR, "Too many services: %u (max %d)", service_count, Y1564_MAX_SERVICES);
		return -EINVAL;
	}

	const y1564_config_t *y1564_cfg = &ctx->config.y1564;
	int ret = 0;

	y1564_log(LOG_INFO, "=================================================================");
	y1564_log(LOG_INFO, "ITU-T Y.1564 Multi-Service Test");
	y1564_log(LOG_INFO, "=================================================================");
	y1564_log(LOG_INFO, "Services: %u", service_count);

	/* Phase 1: Service Configuration Tests */
	if (y1564_cfg->run_config_test && config_results) {
		y1564_log(LOG_INFO, "");
		y1564_log(LOG_INFO, "-----------------------------------------------------------------");
		y1564_log(LOG_INFO, "Phase 1: Service Configuration Tests");
		y1564_log(LOG_INFO, "-----------------------------------------------------------------");

		for (uint32_t i = 0; i < service_count && !rfc2544_is_cancelled(ctx); i++) {
			const y1564_service_t *svc = &services[i];
			if (!svc->enabled)
				continue;

			ret = y1564_config_test(ctx, svc, &config_results[i]);
			if (ret < 0 && ret != -ECANCELED) {
				y1564_log(LOG_ERROR, "Config test failed for service %u: %d", svc->service_id,
				          ret);
				return ret;
			}
		}
	}

	/* Phase 2: Service Performance Tests */
	if (y1564_cfg->run_perf_test && perf_results && !rfc2544_is_cancelled(ctx)) {
		y1564_log(LOG_INFO, "");
		y1564_log(LOG_INFO, "-----------------------------------------------------------------");
		y1564_log(LOG_INFO, "Phase 2: Service Performance Tests");
		y1564_log(LOG_INFO, "-----------------------------------------------------------------");

		uint32_t perf_duration = y1564_cfg->perf_duration_sec;

		for (uint32_t i = 0; i < service_count && !rfc2544_is_cancelled(ctx); i++) {
			const y1564_service_t *svc = &services[i];
			if (!svc->enabled)
				continue;

			ret = y1564_perf_test(ctx, svc, perf_duration, &perf_results[i]);
			if (ret < 0 && ret != -ECANCELED) {
				y1564_log(LOG_ERROR, "Perf test failed for service %u: %d", svc->service_id,
				          ret);
				return ret;
			}
		}
	}

	if (rfc2544_is_cancelled(ctx)) {
		y1564_log(LOG_WARN, "Test cancelled by user");
		return -ECANCELED;
	}

	y1564_log(LOG_INFO, "");
	y1564_log(LOG_INFO, "=================================================================");
	y1564_log(LOG_INFO, "Y.1564 Test Complete");
	y1564_log(LOG_INFO, "=================================================================");

	return 0;
}

/* ============================================================================
 * Results Printing
 * ============================================================================ */

void y1564_print_results(const y1564_config_result_t *config_results,
                         const y1564_perf_result_t *perf_results, uint32_t service_count,
                         stats_format_t format)
{
	/* JSON format */
	if (format == STATS_FORMAT_JSON) {
		printf("{\"type\":\"y1564\",\"service_count\":%u,\"config_results\":[", service_count);
		if (config_results) {
			for (uint32_t s = 0; s < service_count; s++) {
				const y1564_config_result_t *cr = &config_results[s];
				if (s > 0) printf(",");
				printf("{\"service_id\":%u,\"service_pass\":%s,\"steps\":[",
				       cr->service_id, cr->service_pass ? "true" : "false");
				for (int i = 0; i < Y1564_CONFIG_STEPS; i++) {
					const y1564_step_result_t *sr = &cr->steps[i];
					if (i > 0) printf(",");
					printf("{\"step\":%u,\"offered_rate_pct\":%.1f,\"achieved_rate_mbps\":%.2f,"
					       "\"frames_tx\":%" PRIu64 ",\"frames_rx\":%" PRIu64 ",\"flr_pct\":%.4f,"
					       "\"fd_avg_ms\":%.2f,\"fd_min_ms\":%.2f,\"fd_max_ms\":%.2f,"
					       "\"fdv_ms\":%.2f,\"flr_pass\":%s,\"fd_pass\":%s,\"fdv_pass\":%s,"
					       "\"step_pass\":%s}",
					       sr->step, sr->offered_rate_pct, sr->achieved_rate_mbps,
					       sr->frames_tx, sr->frames_rx, sr->flr_pct,
					       sr->fd_avg_ms, sr->fd_min_ms, sr->fd_max_ms, sr->fdv_ms,
					       sr->flr_pass ? "true" : "false",
					       sr->fd_pass ? "true" : "false",
					       sr->fdv_pass ? "true" : "false",
					       sr->step_pass ? "true" : "false");
				}
				printf("]}");
			}
		}
		printf("],\"perf_results\":[");
		if (perf_results) {
			for (uint32_t s = 0; s < service_count; s++) {
				const y1564_perf_result_t *pr = &perf_results[s];
				if (s > 0) printf(",");
				printf("{\"service_id\":%u,\"duration_sec\":%u,\"frames_tx\":%" PRIu64 ","
				       "\"frames_rx\":%" PRIu64 ",\"flr_pct\":%.4f,\"fd_avg_ms\":%.2f,"
				       "\"fd_min_ms\":%.2f,\"fd_max_ms\":%.2f,\"fdv_ms\":%.2f,"
				       "\"flr_pass\":%s,\"fd_pass\":%s,\"fdv_pass\":%s,\"service_pass\":%s}",
				       pr->service_id, pr->duration_sec, pr->frames_tx, pr->frames_rx,
				       pr->flr_pct, pr->fd_avg_ms, pr->fd_min_ms, pr->fd_max_ms, pr->fdv_ms,
				       pr->flr_pass ? "true" : "false",
				       pr->fd_pass ? "true" : "false",
				       pr->fdv_pass ? "true" : "false",
				       pr->service_pass ? "true" : "false");
			}
		}
		printf("]}\n");
		return;
	}

	/* CSV format */
	if (format == STATS_FORMAT_CSV) {
		printf("service_id,test_phase,step,offered_pct,achieved_mbps,flr_pct,fd_ms,fdv_ms,result\n");
		if (config_results) {
			for (uint32_t s = 0; s < service_count; s++) {
				const y1564_config_result_t *cr = &config_results[s];
				for (int i = 0; i < Y1564_CONFIG_STEPS; i++) {
					const y1564_step_result_t *sr = &cr->steps[i];
					printf("%u,config,%u,%.0f,%.2f,%.4f,%.2f,%.2f,%s\n",
					       cr->service_id, sr->step, sr->offered_rate_pct,
					       sr->achieved_rate_mbps, sr->flr_pct, sr->fd_avg_ms,
					       sr->fdv_ms, sr->step_pass ? "PASS" : "FAIL");
				}
			}
		}
		if (perf_results) {
			for (uint32_t s = 0; s < service_count; s++) {
				const y1564_perf_result_t *pr = &perf_results[s];
				double rate_mbps = pr->duration_sec > 0
				    ? (double)pr->frames_tx * 8 / pr->duration_sec / 1e6
				    : 0.0;
				printf("%u,perf,0,100,%.2f,%.4f,%.2f,%.2f,%s\n",
				       pr->service_id, rate_mbps,
				       pr->flr_pct, pr->fd_avg_ms, pr->fdv_ms,
				       pr->service_pass ? "PASS" : "FAIL");
			}
		}
		return;
	}

	/* Default: TEXT format */
	printf("\n");
	printf("=================================================================\n");
	printf("ITU-T Y.1564 Test Results\n");
	printf("=================================================================\n");

	/* Service Configuration Test Results */
	if (config_results) {
		printf("\nService Configuration Test Results\n");
		printf("-----------------------------------------------------------------\n");

		for (uint32_t s = 0; s < service_count; s++) {
			const y1564_config_result_t *cr = &config_results[s];

			printf("\nService %u: %s\n", cr->service_id,
			       cr->service_pass ? "PASS" : "FAIL");
			printf("%-6s %8s %12s %15s %12s %10s %10s %10s %8s\n", "Step", "% CIR", "Rate (Mbps)",
			       "Frames TX", "FLR (%)", "FD (ms)", "FDV (ms)", "Status", "Result");
			printf("-----------------------------------------------------------------\n");

			for (int i = 0; i < Y1564_CONFIG_STEPS; i++) {
				const y1564_step_result_t *sr = &cr->steps[i];
				printf("%-6u %7.0f%% %12.2f %15" PRIu64 " %11.4f%% %10.2f %10.2f %10s %8s\n", sr->step,
				       sr->offered_rate_pct, sr->achieved_rate_mbps, sr->frames_tx,
				       sr->flr_pct, sr->fd_avg_ms, sr->fdv_ms,
				       sr->step_pass ? "PASS" : "FAIL",
				       (sr->flr_pass && sr->fd_pass && sr->fdv_pass) ? "OK" : "FAIL");
			}
		}
	}

	/* Service Performance Test Results */
	if (perf_results) {
		printf("\nService Performance Test Results\n");
		printf("-----------------------------------------------------------------\n");
		printf("%-10s %12s %15s %12s %10s %10s %8s\n", "Service", "Duration", "Frames TX",
		       "FLR (%)", "FD (ms)", "FDV (ms)", "Result");
		printf("-----------------------------------------------------------------\n");

		for (uint32_t s = 0; s < service_count; s++) {
			const y1564_perf_result_t *pr = &perf_results[s];
			printf("%-10u %10um %15" PRIu64 " %11.4f%% %10.2f %10.2f %8s\n", pr->service_id,
			       pr->duration_sec / 60, pr->frames_tx, pr->flr_pct, pr->fd_avg_ms,
			       pr->fdv_ms, pr->service_pass ? "PASS" : "FAIL");
		}
	}

	printf("\n");
	printf("=================================================================\n");

	/* Summary */
	bool all_pass = true;
	if (config_results) {
		for (uint32_t s = 0; s < service_count; s++) {
			if (!config_results[s].service_pass) {
				all_pass = false;
				break;
			}
		}
	}
	if (perf_results && all_pass) {
		for (uint32_t s = 0; s < service_count; s++) {
			if (!perf_results[s].service_pass) {
				all_pass = false;
				break;
			}
		}
	}

	printf("Overall Result: %s\n", all_pass ? "ALL SERVICES PASSED" : "ONE OR MORE SERVICES FAILED");
	printf("=================================================================\n");
}
