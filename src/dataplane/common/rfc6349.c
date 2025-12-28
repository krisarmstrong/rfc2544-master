/*
 * rfc6349.c - RFC 6349 TCP Throughput Testing Implementation
 *
 * Framework for TCP Throughput Testing:
 * - Path MTU Discovery
 * - RTT Measurement
 * - Bandwidth-Delay Product calculation
 * - TCP Efficiency analysis
 * - Buffer Delay percentage
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* RFC 6349 constants */
#define RFC6349_DEFAULT_DURATION_SEC    30
#define RFC6349_DEFAULT_WARMUP_SEC      2
#define RFC6349_MIN_RTT_SAMPLES         20
#define RFC6349_DEFAULT_MSS             1460

/**
 * Initialize default RFC 6349 configuration
 */
void rfc6349_default_config(rfc6349_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	config->target_rate_mbps = 0;        /* 0 = auto-detect */
	config->min_rtt_ms = 0.1;            /* 0.1ms minimum */
	config->max_rtt_ms = 1000.0;         /* 1 second maximum */
	config->rwnd_size = 65535;           /* Default TCP window */
	config->test_duration_sec = RFC6349_DEFAULT_DURATION_SEC;
	config->parallel_streams = 1;
	config->mss = RFC6349_DEFAULT_MSS;
	config->mode = TCP_SINGLE_STREAM;
}

/* ============================================================================
 * Path Analysis
 *
 * Measures RTT and calculates Bandwidth-Delay Product
 * ============================================================================ */

int rfc6349_path_test(rfc2544_ctx_t *ctx, const rfc6349_config_t *config,
                      tcp_path_info_t *path)
{
	if (!ctx || !config || !path)
		return -EINVAL;

	memset(path, 0, sizeof(*path));

	rfc2544_log(LOG_INFO, "=== RFC 6349 Path Analysis ===");

	/* Run a short trial to measure RTT via latency measurement */
	ctx->config.measure_latency = true;

	trial_result_t trial;
	int ret = run_trial(ctx, config->mss + 40,  /* MSS + TCP/IP headers */
	                    10.0,  /* Low rate for RTT measurement */
	                    5,     /* 5 second trial */
	                    1,     /* 1 second warmup */
	                    &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Path analysis trial failed: %d", ret);
		return ret;
	}

	/* Extract RTT from latency measurements */
	/* Note: In a reflector setup, latency = RTT */
	path->rtt_min_ms = trial.latency.min_ns / 1e6;
	path->rtt_avg_ms = trial.latency.avg_ns / 1e6;
	path->rtt_max_ms = trial.latency.max_ns / 1e6;

	/* Ensure minimum RTT is reasonable */
	if (path->rtt_min_ms < 0.001)
		path->rtt_min_ms = 0.1;  /* Minimum 100us */
	if (path->rtt_avg_ms < 0.001)
		path->rtt_avg_ms = path->rtt_min_ms;
	if (path->rtt_max_ms < path->rtt_avg_ms)
		path->rtt_max_ms = path->rtt_avg_ms * 2;

	/* Path MTU and MSS from config */
	path->path_mtu = 1500;  /* Standard MTU */
	path->mss = config->mss;

	/* Calculate line rate in Mbps */
	double line_rate_mbps = ctx->line_rate / 1e6;

	/* Calculate Bandwidth-Delay Product */
	/* BDP = Bandwidth (bits/sec) * RTT (sec) / 8 */
	path->bdp_bytes = (uint64_t)((line_rate_mbps * 1e6) * (path->rtt_avg_ms / 1000.0) / 8.0);

	rfc2544_log(LOG_INFO, "RTT: min=%.3f, avg=%.3f, max=%.3f ms",
	            path->rtt_min_ms, path->rtt_avg_ms, path->rtt_max_ms);
	rfc2544_log(LOG_INFO, "BDP: %" PRIu64 " bytes", path->bdp_bytes);

	return 0;
}

/* ============================================================================
 * Throughput Test
 *
 * Measures achieved TCP throughput vs theoretical maximum
 * ============================================================================ */

int rfc6349_throughput_test(rfc2544_ctx_t *ctx, const rfc6349_config_t *config,
                            rfc6349_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== RFC 6349 Throughput Test ===");

	/* First run path analysis to get RTT and BDP */
	tcp_path_info_t path;
	int ret = rfc6349_path_test(ctx, config, &path);
	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Path test failed: %d", ret);
		return ret;
	}

	/* Calculate theoretical maximum throughput */
	/* Limited by either line rate or BDP/RTT */
	double line_rate_mbps = ctx->line_rate / 1e6;
	double bdp_limited_mbps = (path.bdp_bytes * 8.0) / (path.rtt_avg_ms / 1000.0) / 1e6;

	result->theoretical_rate_mbps = (line_rate_mbps < bdp_limited_mbps) ?
	                                line_rate_mbps : bdp_limited_mbps;

	rfc2544_log(LOG_INFO, "Theoretical max: %.2f Mbps (line: %.2f, BDP-limited: %.2f)",
	            result->theoretical_rate_mbps, line_rate_mbps, bdp_limited_mbps);

	/* Run throughput trial at maximum rate */
	trial_result_t trial;
	ret = run_trial(ctx, config->mss + 40,
	                100.0,  /* Full rate */
	                config->test_duration_sec,
	                RFC6349_DEFAULT_WARMUP_SEC,
	                &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Throughput trial failed: %d", ret);
		return ret;
	}

	/* Record achieved throughput */
	result->achieved_rate_mbps = trial.achieved_mbps;
	result->bytes_transferred = trial.bytes_sent;
	result->test_duration_ms = (uint32_t)(trial.elapsed_sec * 1000);

	/* Copy path info */
	result->rtt_min_ms = path.rtt_min_ms;
	result->rtt_avg_ms = path.rtt_avg_ms;
	result->rtt_max_ms = path.rtt_max_ms;
	result->bdp_bytes = path.bdp_bytes;
	result->rwnd_used = config->rwnd_size;

	/* Calculate TCP Efficiency */
	/* TCP Efficiency = (Transmitted Bytes - Retransmitted Bytes) / Transmitted Bytes */
	/* In our test, loss represents "retransmissions" */
	result->retransmissions = (uint64_t)(trial.packets_sent * trial.loss_pct / 100.0);
	if (trial.packets_sent > 0) {
		result->tcp_efficiency = 100.0 * (1.0 - trial.loss_pct / 100.0);
	} else {
		result->tcp_efficiency = 0.0;
	}

	/* Calculate Buffer Delay Percentage */
	/* Buffer Delay % = (Average RTT - Baseline RTT) / Baseline RTT * 100 */
	if (path.rtt_min_ms > 0) {
		result->buffer_delay_pct = 100.0 * (path.rtt_avg_ms - path.rtt_min_ms) / path.rtt_min_ms;
	}

	/* Calculate Transfer Time Ratio */
	/* TTR = Actual Transfer Time / Ideal Transfer Time */
	double ideal_time_sec = (result->bytes_transferred * 8.0) /
	                        (result->theoretical_rate_mbps * 1e6);
	if (ideal_time_sec > 0) {
		result->transfer_time_ratio = trial.elapsed_sec / ideal_time_sec;
	} else {
		result->transfer_time_ratio = 1.0;
	}

	/* Determine pass/fail */
	/* Pass if achieved >= 90% of theoretical and TCP efficiency >= 95% */
	double throughput_ratio = (result->theoretical_rate_mbps > 0.0) ?
	                          (result->achieved_rate_mbps / result->theoretical_rate_mbps) : 0.0;
	result->passed = (throughput_ratio >= 0.90 && result->tcp_efficiency >= 95.0);

	rfc2544_log(LOG_INFO, "Achieved: %.2f Mbps (%.1f%% of theoretical)",
	            result->achieved_rate_mbps, throughput_ratio * 100.0);
	rfc2544_log(LOG_INFO, "TCP Efficiency: %.2f%%", result->tcp_efficiency);
	rfc2544_log(LOG_INFO, "Buffer Delay: %.2f%%", result->buffer_delay_pct);
	rfc2544_log(LOG_INFO, "Result: %s", result->passed ? "PASS" : "FAIL");

	return 0;
}

/**
 * Calculate theoretical TCP throughput
 */
double rfc6349_theoretical_throughput(double bandwidth_mbps, double rtt_ms,
                                      double loss_pct, uint32_t mss)
{
	/* Validate inputs to prevent division by zero/invalid results */
	if (bandwidth_mbps <= 0.0)
		return 0.0;
	if (rtt_ms <= 0.0 || mss == 0)
		return bandwidth_mbps;
	/* Very low loss (< 0.0001% = 1e-6) means essentially lossless - return line rate */
	if (loss_pct <= 0.0001)
		return bandwidth_mbps;

	/* Mathis formula: Throughput = (MSS / RTT) * (C / sqrt(loss)) */
	/* where C is typically 1.22 for standard TCP */
	double c = 1.22;
	double loss_ratio = loss_pct / 100.0;
	/* Protect against sqrt of very small numbers causing huge results */
	double sqrt_loss = sqrt(loss_ratio);
	if (sqrt_loss < 1e-6)
		sqrt_loss = 1e-6;
	double max_throughput = (mss * 8.0 / (rtt_ms / 1000.0)) * (c / sqrt_loss) / 1e6;

	return (max_throughput < bandwidth_mbps) ? max_throughput : bandwidth_mbps;
}

/* ============================================================================
 * Print Results
 * ============================================================================ */

void rfc6349_print_results(const rfc6349_result_t *result, stats_format_t format)
{
	if (!result)
		return;

	(void)format;  /* TODO: implement JSON/CSV output */

	printf("\n=== RFC 6349 TCP Throughput Results ===\n");
	printf("Throughput:           %.2f Mbps\n", result->achieved_rate_mbps);
	printf("Theoretical Max:      %.2f Mbps\n", result->theoretical_rate_mbps);
	double efficiency_pct = (result->theoretical_rate_mbps > 0.0) ?
	                        (100.0 * result->achieved_rate_mbps / result->theoretical_rate_mbps) : 0.0;
	printf("Efficiency:           %.1f%%\n", efficiency_pct);
	printf("\nTCP Metrics:\n");
	printf("  TCP Efficiency:     %.2f%%\n", result->tcp_efficiency);
	printf("  Buffer Delay:       %.2f%%\n", result->buffer_delay_pct);
	printf("  Transfer Time Ratio: %.3f\n", result->transfer_time_ratio);
	printf("\nPath Metrics:\n");
	printf("  RTT (min/avg/max):  %.3f / %.3f / %.3f ms\n",
	       result->rtt_min_ms, result->rtt_avg_ms, result->rtt_max_ms);
	printf("  BDP:                %" PRIu64 " bytes\n", result->bdp_bytes);
	printf("  RWND Used:          %u bytes\n", result->rwnd_used);
	printf("\nTransfer Stats:\n");
	printf("  Bytes Transferred:  %" PRIu64 "\n", result->bytes_transferred);
	printf("  Retransmissions:    %" PRIu64 "\n", result->retransmissions);
	printf("  Duration:           %u ms\n", result->test_duration_ms);
	printf("\nResult: %s\n", result->passed ? "PASS" : "FAIL");
}
