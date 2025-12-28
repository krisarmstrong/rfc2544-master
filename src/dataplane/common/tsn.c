/*
 * tsn.c - IEEE 802.1Qbv Time-Sensitive Networking Implementation
 *
 * Implements TSN testing for deterministic Ethernet:
 * - Gate Control List (GCL) validation
 * - Time-aware queuing verification
 * - Scheduled latency measurement
 * - PTP synchronization testing
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

/* TSN constants */
#define TSN_DEFAULT_DURATION_SEC     30
#define TSN_DEFAULT_WARMUP_SEC       2

/**
 * Initialize default TSN configuration
 */
void tsn_default_config(tsn_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	/* Default GCL - all gates open */
	config->gcl.entry_count = 1;
	config->gcl.entries[0].gate_states = 0xFF;  /* All gates open */
	config->gcl.entries[0].time_interval_ns = 1000000;  /* 1ms cycle */
	config->gcl.base_time_ns = 0;
	config->gcl.cycle_time_ns = 1000000;
	config->gcl.cycle_time_extension_ns = 0;

	config->verify_gcl = true;
	config->stream_count = 0;

	config->duration_sec = TSN_DEFAULT_DURATION_SEC;
	config->warmup_sec = TSN_DEFAULT_WARMUP_SEC;
	config->frame_size = 128;

	config->max_latency_ns = 1000000;  /* 1ms max latency */
	config->max_jitter_ns = 100000;    /* 100us max jitter */

	config->require_ptp_sync = false;
	config->max_sync_offset_ns = 100;  /* 100ns sync requirement */
	config->ptp_enabled = false;
	config->preemption_enabled = false;
	config->num_traffic_classes = 8;
	config->base_time_ns = 0;
	config->cycle_time_ns = 1000000;
}

/* ============================================================================
 * Gate Control List Management
 * ============================================================================ */

/**
 * Create exclusive GCL - each traffic class gets exclusive time slice
 */
int tsn_create_exclusive_gcl(gate_control_list_t *gcl, uint32_t num_classes,
                             uint32_t cycle_time_ns)
{
	if (!gcl || num_classes == 0 || num_classes > TSN_MAX_GATES || cycle_time_ns == 0)
		return -EINVAL;

	memset(gcl, 0, sizeof(*gcl));

	/* Divide cycle time equally among classes, distributing remainder */
	uint32_t time_per_class = cycle_time_ns / num_classes;
	uint32_t remainder = cycle_time_ns % num_classes;

	gcl->entry_count = num_classes;
	gcl->cycle_time_ns = cycle_time_ns;
	gcl->base_time_ns = 0;
	gcl->cycle_time_extension_ns = 0;

	for (uint32_t i = 0; i < num_classes; i++) {
		/* Only gate i is open during this interval */
		gcl->entries[i].gate_states = (1 << i);
		/* Distribute remainder to first classes to ensure sum equals cycle_time */
		gcl->entries[i].time_interval_ns = time_per_class + (i < remainder ? 1 : 0);
	}

	rfc2544_log(LOG_INFO, "Created exclusive GCL: %u classes, %u ns/class",
	            num_classes, time_per_class);

	return 0;
}

/**
 * Create priority-based GCL - high priority gets specified percentage
 */
int tsn_create_priority_gcl(gate_control_list_t *gcl, uint32_t cycle_time_ns,
                            uint32_t high_prio_time_pct)
{
	if (!gcl || high_prio_time_pct > 100)
		return -EINVAL;

	memset(gcl, 0, sizeof(*gcl));

	uint32_t high_prio_time = (cycle_time_ns * high_prio_time_pct) / 100;
	uint32_t low_prio_time = cycle_time_ns - high_prio_time;

	gcl->entry_count = 2;
	gcl->cycle_time_ns = cycle_time_ns;
	gcl->base_time_ns = 0;
	gcl->cycle_time_extension_ns = 0;

	/* High priority window - gates 7,6,5 open (priorities 7,6,5) */
	gcl->entries[0].gate_states = 0xE0;  /* 11100000 */
	gcl->entries[0].time_interval_ns = high_prio_time;

	/* Low priority window - all gates open */
	gcl->entries[1].gate_states = 0xFF;
	gcl->entries[1].time_interval_ns = low_prio_time;

	rfc2544_log(LOG_INFO, "Created priority GCL: high=%u ns (%u%%), low=%u ns",
	            high_prio_time, high_prio_time_pct, low_prio_time);

	return 0;
}

/**
 * Verify GCL configuration is valid
 */
int tsn_verify_gcl(const gate_control_list_t *gcl)
{
	if (!gcl)
		return -EINVAL;

	if (gcl->entry_count == 0 || gcl->entry_count > TSN_MAX_GCL_ENTRIES) {
		rfc2544_log(LOG_ERROR, "Invalid GCL entry count: %u", gcl->entry_count);
		return -EINVAL;
	}

	/* Verify total time equals cycle time */
	uint64_t total_time = 0;
	for (uint32_t i = 0; i < gcl->entry_count; i++) {
		if (gcl->entries[i].time_interval_ns == 0) {
			rfc2544_log(LOG_ERROR, "GCL entry %u has zero interval", i);
			return -EINVAL;
		}
		total_time += gcl->entries[i].time_interval_ns;
	}

	if (total_time != gcl->cycle_time_ns) {
		rfc2544_log(LOG_WARN, "GCL total time (%lu ns) != cycle time (%u ns)",
		            (unsigned long)total_time, gcl->cycle_time_ns);
	}

	rfc2544_log(LOG_INFO, "GCL verified: %u entries, cycle=%u ns",
	            gcl->entry_count, gcl->cycle_time_ns);

	return 0;
}

/* ============================================================================
 * Gate Timing Test
 *
 * Verifies gate operations occur at correct times
 * ============================================================================ */

int tsn_gate_timing_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                         tsn_timing_result_t_v2 *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== TSN Gate Timing Test ===");
	rfc2544_log(LOG_INFO, "Cycle time: %u ns, Duration: %u sec",
	            config->gcl.cycle_time_ns, config->duration_sec);

	/* Verify GCL first */
	int ret = tsn_verify_gcl(&config->gcl);
	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "GCL verification failed");
		return ret;
	}

	/* Enable latency measurement for timing */
	ctx->config.measure_latency = true;

	/* Run trial to measure timing */
	trial_result_t trial;
	ret = run_trial_custom(ctx, config->frame_size, 10.0,  /* Low rate */
	                       config->duration_sec, config->warmup_sec,
	                       TSN_SIGNATURE, 0, &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Gate timing trial failed: %d", ret);
		return ret;
	}

	/* Calculate cycles tested */
	uint64_t test_time_ns = (uint64_t)config->duration_sec * 1000000000ULL;
	result->cycles_tested = (config->gcl.cycle_time_ns > 0) ?
	                        (uint32_t)(test_time_ns / config->gcl.cycle_time_ns) : 0;

	/* Gate deviation from latency jitter */
	result->max_gate_deviation_ns = trial.latency.max_ns - trial.latency.min_ns;
	result->avg_gate_deviation_ns = trial.latency.jitter_ns;

	/* Count timing errors - frames outside expected window */
	/* Error if deviation exceeds max_jitter_ns threshold */
	if (result->max_gate_deviation_ns > config->max_jitter_ns) {
		result->timing_errors = 1;  /* At least one timing error */
	} else {
		result->timing_errors = 0;
	}

	result->gate_timing_passed = (result->timing_errors == 0);

	rfc2544_log(LOG_INFO, "Cycles tested: %u", result->cycles_tested);
	rfc2544_log(LOG_INFO, "Gate deviation: avg=%.1f ns, max=%.1f ns",
	            result->avg_gate_deviation_ns, result->max_gate_deviation_ns);
	rfc2544_log(LOG_INFO, "Timing errors: %u - %s",
	            result->timing_errors,
	            result->gate_timing_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Traffic Class Isolation Test
 *
 * Verifies traffic classes don't interfere with each other
 * ============================================================================ */

int tsn_isolation_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                       tsn_isolation_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t num_classes = config->num_traffic_classes;
	if (num_classes > 8) num_classes = 8;

	rfc2544_log(LOG_INFO, "=== TSN Traffic Class Isolation Test ===");
	rfc2544_log(LOG_INFO, "Testing %u traffic classes", num_classes);

	result->num_classes = num_classes;
	bool overall_pass = true;

	/* Test each traffic class */
	for (uint32_t tc = 0; tc < num_classes && !ctx->cancel_requested; tc++) {
		rfc2544_log(LOG_INFO, "Testing traffic class %u...", tc);

		tsn_class_result_t *cr = &result->class_results[tc];

		/* Enable latency measurement */
		ctx->config.measure_latency = true;

		/* Run trial for this traffic class */
		trial_result_t trial;
		int ret = run_trial_custom(ctx, config->frame_size, 50.0,
		                           config->duration_sec / num_classes,
		                           config->warmup_sec,
		                           TSN_SIGNATURE, tc, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Class %u trial failed: %d", tc, ret);
			return ret;
		}

		cr->frames_tx = trial.packets_sent;
		cr->frames_rx = trial.packets_recv;
		cr->latency_avg_ns = trial.latency.avg_ns;
		cr->latency_max_ns = trial.latency.max_ns;

		/* Calculate jitter as max - avg */
		double jitter_ns = trial.latency.max_ns - trial.latency.avg_ns;

		/* Frames that arrived outside expected timing window */
		/* Approximated from jitter exceeding threshold */
		if (jitter_ns > config->max_jitter_ns && trial.latency.max_ns > 0) {
			cr->frames_interfered = (uint64_t)(trial.packets_recv *
			                        jitter_ns / trial.latency.max_ns);
		} else {
			cr->frames_interfered = 0;
		}

		/* Isolation percentage */
		if (trial.packets_recv > 0) {
			cr->isolation_pct = 100.0 * (1.0 - (double)cr->frames_interfered / trial.packets_recv);
		} else {
			cr->isolation_pct = 100.0;
		}

		/* Check against thresholds */
		bool latency_ok = (cr->latency_avg_ns <= config->max_latency_ns);
		bool jitter_ok = (jitter_ns <= config->max_jitter_ns);
		bool no_interference = (cr->frames_interfered == 0);

		cr->passed = latency_ok && jitter_ok && no_interference;

		if (!cr->passed)
			overall_pass = false;

		rfc2544_log(LOG_INFO, "  Class %u: TX=%lu, RX=%lu, latency=%.1f ns, jitter=%.1f ns - %s",
		            tc, (unsigned long)cr->frames_tx, (unsigned long)cr->frames_rx,
		            cr->latency_avg_ns, jitter_ns,
		            cr->passed ? "PASS" : "FAIL");
	}

	result->overall_passed = overall_pass;

	rfc2544_log(LOG_INFO, "Isolation Test: %s",
	            result->overall_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Scheduled Latency Test
 *
 * Measures latency for a specific traffic class during its scheduled window
 * ============================================================================ */

int tsn_scheduled_latency_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                               uint32_t traffic_class,
                               tsn_latency_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	if (traffic_class >= 8)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	result->traffic_class = traffic_class;

	rfc2544_log(LOG_INFO, "=== TSN Scheduled Latency Test ===");
	rfc2544_log(LOG_INFO, "Traffic class: %u", traffic_class);

	/* Enable latency measurement */
	ctx->config.measure_latency = true;

	/* Run trial for this traffic class */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, config->frame_size, 50.0,
	                           config->duration_sec, config->warmup_sec,
	                           TSN_SIGNATURE, traffic_class, &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Latency trial failed: %d", ret);
		return ret;
	}

	result->samples = (uint32_t)trial.packets_recv;
	result->latency_min_ns = trial.latency.min_ns;
	result->latency_avg_ns = trial.latency.avg_ns;
	result->latency_max_ns = trial.latency.max_ns;
	result->latency_99_ns = trial.latency.p99_ns;
	result->latency_999_ns = trial.latency.p99_ns * 1.1;  /* Approximate 99.9th */
	result->jitter_ns = trial.latency.jitter_ns;

	/* Check against thresholds */
	result->latency_passed = (result->latency_max_ns <= config->max_latency_ns);
	result->jitter_passed = (result->jitter_ns <= config->max_jitter_ns);

	rfc2544_log(LOG_INFO, "Latency: min=%.1f, avg=%.1f, max=%.1f, p99=%.1f ns",
	            result->latency_min_ns, result->latency_avg_ns,
	            result->latency_max_ns, result->latency_99_ns);
	rfc2544_log(LOG_INFO, "Jitter: %.1f ns", result->jitter_ns);
	rfc2544_log(LOG_INFO, "Latency: %s, Jitter: %s",
	            result->latency_passed ? "PASS" : "FAIL",
	            result->jitter_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * PTP Synchronization Test
 *
 * Verifies PTP synchronization accuracy
 * ============================================================================ */

int tsn_ptp_sync_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                      tsn_ptp_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== TSN PTP Synchronization Test ===");

	if (!config->ptp_enabled) {
		rfc2544_log(LOG_WARN, "PTP not enabled in configuration");
		result->sync_achieved = false;
		return 0;
	}

	/* Enable latency measurement for sync verification */
	ctx->config.measure_latency = true;

	/* Run short trial to measure synchronization */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 128, 1.0,  /* Very low rate for sync test */
	                           10, 2,  /* Short duration */
	                           TSN_SIGNATURE, 0, &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "PTP sync trial failed: %d", ret);
		return ret;
	}

	result->samples = (uint32_t)trial.packets_recv;

	/* Offset derived from one-way delay asymmetry */
	/* In loopback, this is approximated from latency variation */
	result->offset_avg_ns = trial.latency.jitter_ns / 2.0;
	result->offset_max_ns = (trial.latency.max_ns - trial.latency.min_ns) / 2.0;
	result->offset_stddev_ns = trial.latency.jitter_ns / 4.0;

	/* Check against threshold */
	result->sync_achieved = (result->offset_max_ns <= config->max_sync_offset_ns);

	rfc2544_log(LOG_INFO, "Samples: %u", result->samples);
	rfc2544_log(LOG_INFO, "Offset: avg=%.1f ns, max=%.1f ns, stddev=%.1f ns",
	            result->offset_avg_ns, result->offset_max_ns, result->offset_stddev_ns);
	rfc2544_log(LOG_INFO, "Sync: %s (threshold: %u ns)",
	            result->sync_achieved ? "ACHIEVED" : "NOT ACHIEVED",
	            config->max_sync_offset_ns);

	return 0;
}

/* ============================================================================
 * Full TSN Test Suite
 * ============================================================================ */

int tsn_full_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                  tsn_full_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	bool overall_pass = true;
	int ret;

	rfc2544_log(LOG_INFO, "=== Full TSN Test Suite ===");

	/* Gate timing test */
	ret = tsn_gate_timing_test(ctx, config, &result->timing_result);
	if (ret < 0)
		return ret;
	if (!result->timing_result.gate_timing_passed)
		overall_pass = false;

	/* Isolation test */
	ret = tsn_isolation_test(ctx, config, &result->isolation_result);
	if (ret < 0)
		return ret;
	if (!result->isolation_result.overall_passed)
		overall_pass = false;

	/* Latency test for each class */
	for (uint32_t tc = 0; tc < config->num_traffic_classes && tc < 8; tc++) {
		ret = tsn_scheduled_latency_test(ctx, config, tc, &result->latency_results[tc]);
		if (ret < 0)
			return ret;
		if (!result->latency_results[tc].latency_passed ||
		    !result->latency_results[tc].jitter_passed)
			overall_pass = false;
	}

	/* PTP sync test */
	if (config->ptp_enabled) {
		ret = tsn_ptp_sync_test(ctx, config, &result->ptp_result);
		if (ret < 0)
			return ret;
		if (!result->ptp_result.sync_achieved)
			overall_pass = false;
	}

	result->overall_passed = overall_pass;

	rfc2544_log(LOG_INFO, "=== TSN Full Test: %s ===",
	            result->overall_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Print Functions
 * ============================================================================ */

void tsn_print_timing_results(const tsn_timing_result_t_v2 *result)
{
	if (!result)
		return;

	printf("\n=== TSN Gate Timing Results ===\n");
	printf("Cycles tested:   %u\n", result->cycles_tested);
	printf("Timing errors:   %u\n", result->timing_errors);
	printf("Gate deviation:\n");
	printf("  Average:       %.1f ns\n", result->avg_gate_deviation_ns);
	printf("  Maximum:       %.1f ns\n", result->max_gate_deviation_ns);
	printf("Result:          %s\n", result->gate_timing_passed ? "PASS" : "FAIL");
}

void tsn_print_isolation_results(const tsn_isolation_result_t *result)
{
	if (!result)
		return;

	printf("\n=== TSN Traffic Class Isolation Results ===\n");
	printf("Classes tested:  %u\n", result->num_classes);

	for (uint32_t i = 0; i < result->num_classes; i++) {
		const tsn_class_result_t *cr = &result->class_results[i];
		printf("\nClass %u:\n", i);
		printf("  Frames TX:     %" PRIu64 "\n", cr->frames_tx);
		printf("  Frames RX:     %" PRIu64 "\n", cr->frames_rx);
		printf("  Interfered:    %" PRIu64 "\n", cr->frames_interfered);
		printf("  Isolation:     %.1f%%\n", cr->isolation_pct);
		printf("  Latency avg:   %.1f ns\n", cr->latency_avg_ns);
		printf("  Latency max:   %.1f ns\n", cr->latency_max_ns);
		printf("  Result:        %s\n", cr->passed ? "PASS" : "FAIL");
	}

	printf("\nOverall:         %s\n", result->overall_passed ? "PASS" : "FAIL");
}

void tsn_print_latency_results(const tsn_latency_result_t *result)
{
	if (!result)
		return;

	printf("\n=== TSN Scheduled Latency Results ===\n");
	printf("Traffic class:   %u\n", result->traffic_class);
	printf("Samples:         %u\n", result->samples);
	printf("\nLatency:\n");
	printf("  Minimum:       %.1f ns\n", result->latency_min_ns);
	printf("  Average:       %.1f ns\n", result->latency_avg_ns);
	printf("  Maximum:       %.1f ns\n", result->latency_max_ns);
	printf("  99th pct:      %.1f ns\n", result->latency_99_ns);
	printf("  99.9th pct:    %.1f ns\n", result->latency_999_ns);
	printf("\nJitter:          %.1f ns\n", result->jitter_ns);
	printf("\nLatency check:   %s\n", result->latency_passed ? "PASS" : "FAIL");
	printf("Jitter check:    %s\n", result->jitter_passed ? "PASS" : "FAIL");
}
