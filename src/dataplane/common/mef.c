/*
 * mef.c - MEF 48/49 Carrier Ethernet Performance Testing
 *
 * MEF 48: Carrier Ethernet Service Activation Testing (CESA)
 * MEF 49: Performance Objectives (EPO)
 *
 * Implements Service OAM (SOAM) testing for:
 * - Service Configuration (SC) tests
 * - Service Performance (SP) tests
 * - SLA validation against MEF performance objectives
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

/* MEF constants */
#define MEF_DEFAULT_DURATION_SEC     60
#define MEF_DEFAULT_WARMUP_SEC       2
#define MEF_MAX_STEPS                4
/* MEF_SIGNATURE is defined in rfc2544.h */

/**
 * Initialize default bandwidth profile
 */
void mef_default_bandwidth_profile(mef_bandwidth_profile_t *profile)
{
	if (!profile)
		return;

	memset(profile, 0, sizeof(*profile));

	profile->cir_kbps = 100000;      /* 100 Mbps CIR */
	profile->cbs_bytes = 12000;       /* 12KB CBS (8 frames) */
	profile->eir_kbps = 0;            /* No EIR by default */
	profile->ebs_bytes = 0;
	profile->color_mode = false;      /* Color-blind */
	profile->coupling_flag = false;
}

/**
 * Initialize default SLA parameters
 */
void mef_default_sla(mef_sla_t *sla)
{
	if (!sla)
		return;

	memset(sla, 0, sizeof(*sla));

	sla->fd_threshold_us = 10000;     /* 10ms max delay (in us) */
	sla->fdv_threshold_us = 5000;     /* 5ms max jitter (in us) */
	sla->flr_threshold_pct = 0.1;     /* 0.1% max loss */
	sla->availability_pct = 99.99;    /* 99.99% availability */
	sla->mttr_minutes = 60;           /* 1 hour MTTR */
	sla->mtbf_hours = 8760;           /* 1 year MTBF */
}

/**
 * Initialize default MEF test configuration
 */
void mef_default_config(mef_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	config->service_type = MEF_EPL;
	config->cos = MEF_COS_HIGH;
	strncpy(config->service_id, "DEFAULT", sizeof(config->service_id) - 1);
	mef_default_bandwidth_profile(&config->bw_profile);
	mef_default_sla(&config->sla);

	config->frame_sizes[0] = 64;
	config->frame_sizes[1] = 512;
	config->frame_sizes[2] = 1518;
	config->num_frame_sizes = 3;

	config->config_test_duration_sec = 60;
	config->perf_test_duration_min = 15;  /* 15 minutes per MEF */
}

/* ============================================================================
 * Service Configuration Test (MEF 48 SC Test)
 *
 * Step test at 25%, 50%, 75%, 100% of CIR
 * ============================================================================ */

int mef_config_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                    mef_config_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	strncpy(result->service_id, config->service_id, sizeof(result->service_id) - 1);

	rfc2544_log(LOG_INFO, "=== MEF 48 Service Configuration Test ===");
	rfc2544_log(LOG_INFO, "Service: %s, CIR: %u kbps", config->service_id,
	            config->bw_profile.cir_kbps);

	/* Test at each step (25%, 50%, 75%, 100% of CIR) */
	uint32_t steps[] = {25, 50, 75, 100};
	uint32_t num_steps = sizeof(steps) / sizeof(steps[0]);
	bool all_passed = true;

	/* Validate line_rate to prevent division by zero */
	if (ctx->line_rate == 0) {
		rfc2544_log(LOG_ERROR, "Invalid line rate (0) - cannot calculate rate percentage");
		return -EINVAL;
	}

	for (uint32_t s = 0; s < num_steps && !ctx->cancel_requested; s++) {
		uint32_t step_cir_kbps = (config->bw_profile.cir_kbps * steps[s]) / 100;

		rfc2544_log(LOG_INFO, "Step %u: Testing at %u%% CIR (%u kbps)",
		            s + 1, steps[s], step_cir_kbps);

		mef_step_result_t *step = &result->steps[s];
		step->step_pct = steps[s];
		step->offered_rate_kbps = step_cir_kbps;
		step->passed = true;

		/* Calculate rate percentage for trial */
		double rate_pct = (step_cir_kbps * 1000.0 * 100.0) / (double)ctx->line_rate;
		if (rate_pct > 100.0) rate_pct = 100.0;

		/* Test primary frame size (1518 bytes for throughput) */
		uint32_t frame_size = 1518;

		/* Enable latency measurement */
		ctx->config.measure_latency = true;

		/* Run trial at this step */
		trial_result_t trial;
		int ret = run_trial_custom(ctx, frame_size, rate_pct,
		                           config->config_test_duration_sec,
		                           MEF_DEFAULT_WARMUP_SEC,
		                           MEF_SIGNATURE, s + 1, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Step %u trial failed: %d", s + 1, ret);
			return ret;
		}

		/* Record results */
		step->achieved_rate_kbps = (uint32_t)(trial.achieved_mbps * 1000.0);
		step->frames_tx = trial.packets_sent;
		step->frames_rx = trial.packets_recv;

		/* Calculate metrics (latency in microseconds) */
		step->fd_us = trial.latency.avg_ns / 1000.0;
		step->fd_min_us = trial.latency.min_ns / 1000.0;
		step->fd_max_us = trial.latency.max_ns / 1000.0;
		step->fdv_us = step->fd_max_us - step->fd_min_us;
		step->flr_pct = trial.loss_pct;

		/* Check against SLA thresholds */
		if (step->fd_us > config->sla.fd_threshold_us ||
		    step->fdv_us > config->sla.fdv_threshold_us ||
		    step->flr_pct > config->sla.flr_threshold_pct) {
			step->passed = false;
			all_passed = false;
		}

		rfc2544_log(LOG_INFO, "  Achieved: %u kbps, FD: %.1f us, FDV: %.1f us, FLR: %.4f%% - %s",
		            step->achieved_rate_kbps, step->fd_us, step->fdv_us, step->flr_pct,
		            step->passed ? "PASS" : "FAIL");
	}

	result->num_steps = num_steps;
	result->overall_passed = all_passed;

	rfc2544_log(LOG_INFO, "Configuration Test: %s",
	            result->overall_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Service Performance Test (MEF 48 SP Test)
 *
 * Long-duration test at CIR (typically 15+ minutes)
 * ============================================================================ */

int mef_perf_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                  mef_perf_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	strncpy(result->service_id, config->service_id, sizeof(result->service_id) - 1);

	/* Convert minutes to seconds */
	uint32_t duration_sec = config->perf_test_duration_min * 60;
	result->duration_sec = duration_sec;

	rfc2544_log(LOG_INFO, "=== MEF 48 Service Performance Test ===");
	rfc2544_log(LOG_INFO, "Service: %s, Duration: %u min (%u sec)",
	            config->service_id, config->perf_test_duration_min, duration_sec);

	/* Validate line_rate to prevent division by zero */
	if (ctx->line_rate == 0) {
		rfc2544_log(LOG_ERROR, "Invalid line rate (0) - cannot calculate rate percentage");
		return -EINVAL;
	}

	/* Calculate rate for CIR */
	double rate_pct = (config->bw_profile.cir_kbps * 1000.0 * 100.0) / (double)ctx->line_rate;
	if (rate_pct > 100.0) rate_pct = 100.0;

	/* Enable latency measurement */
	ctx->config.measure_latency = true;

	/* Run long-duration trial */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 1518, rate_pct,
	                           duration_sec,
	                           MEF_DEFAULT_WARMUP_SEC,
	                           MEF_SIGNATURE, 0, &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Performance trial failed: %d", ret);
		return ret;
	}

	/* Record results */
	result->frames_tx = trial.packets_sent;
	result->frames_rx = trial.packets_recv;
	result->throughput_kbps = (uint32_t)(trial.achieved_mbps * 1000.0);

	/* Delay in microseconds */
	result->fd_avg_us = trial.latency.avg_ns / 1000.0;
	result->fd_min_us = trial.latency.min_ns / 1000.0;
	result->fd_max_us = trial.latency.max_ns / 1000.0;
	result->fdv_us = result->fd_max_us - result->fd_min_us;
	result->flr_pct = trial.loss_pct;

	/* Calculate availability */
	/* Availability = time with service / total time * 100 */
	/* Approximate: if loss < threshold, service was available */
	if (trial.loss_pct <= config->sla.flr_threshold_pct) {
		result->availability_pct = 100.0;
	} else {
		result->availability_pct = 100.0 - trial.loss_pct;
	}

	/* Check SLA compliance for each parameter */
	result->fd_passed = (result->fd_avg_us <= config->sla.fd_threshold_us);
	result->fdv_passed = (result->fdv_us <= config->sla.fdv_threshold_us);
	result->flr_passed = (result->flr_pct <= config->sla.flr_threshold_pct);
	result->avail_passed = (result->availability_pct >= config->sla.availability_pct);

	/* Overall pass requires all SLA parameters to be met */
	result->overall_passed = result->fd_passed && result->fdv_passed &&
	                         result->flr_passed && result->avail_passed;

	rfc2544_log(LOG_INFO, "Performance Results:");
	rfc2544_log(LOG_INFO, "  Throughput: %u kbps", result->throughput_kbps);
	rfc2544_log(LOG_INFO, "  FD: avg=%.1f, min=%.1f, max=%.1f us",
	            result->fd_avg_us, result->fd_min_us, result->fd_max_us);
	rfc2544_log(LOG_INFO, "  FDV: %.1f us, FLR: %.4f%%", result->fdv_us, result->flr_pct);
	rfc2544_log(LOG_INFO, "  Availability: %.4f%%", result->availability_pct);
	rfc2544_log(LOG_INFO, "Result: %s", result->overall_passed ? "PASS" : "FAIL");

	return 0;
}

/* ============================================================================
 * Full MEF Test (Configuration + Performance)
 * ============================================================================ */

int mef_full_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                  mef_config_result_t *config_result,
                  mef_perf_result_t *perf_result)
{
	if (!ctx || !config || !config_result || !perf_result)
		return -EINVAL;

	/* Run configuration test first */
	int ret = mef_config_test(ctx, config, config_result);
	if (ret < 0)
		return ret;

	/* Only run performance test if configuration test passed */
	if (!config_result->overall_passed) {
		rfc2544_log(LOG_WARN, "Configuration test failed - skipping performance test");
		return 0;
	}

	/* Run performance test */
	return mef_perf_test(ctx, config, perf_result);
}

/* ============================================================================
 * SLA Validation
 * ============================================================================ */

int mef_validate_sla(const mef_perf_result_t *result, const mef_sla_t *sla,
                     mef_sla_report_t *report)
{
	if (!result || !sla || !report)
		return -EINVAL;

	memset(report, 0, sizeof(*report));

	/* Record thresholds */
	report->fd_threshold_us = sla->fd_threshold_us;
	report->fdv_threshold_us = sla->fdv_threshold_us;
	report->flr_threshold_pct = sla->flr_threshold_pct;
	report->avail_threshold_pct = sla->availability_pct;

	/* Record measured values */
	report->fd_measured_us = result->fd_avg_us;
	report->fdv_measured_us = result->fdv_us;
	report->flr_measured_pct = result->flr_pct;
	report->avail_measured_pct = result->availability_pct;

	/* Check each SLA parameter */
	report->fd_compliant = (result->fd_avg_us <= sla->fd_threshold_us);
	report->fdv_compliant = (result->fdv_us <= sla->fdv_threshold_us);
	report->flr_compliant = (result->flr_pct <= sla->flr_threshold_pct);
	report->avail_compliant = (result->availability_pct >= sla->availability_pct);

	/* Record margins (in same units as thresholds) */
	report->fd_margin_us = sla->fd_threshold_us - result->fd_avg_us;
	report->fdv_margin_us = sla->fdv_threshold_us - result->fdv_us;
	report->flr_margin_pct = sla->flr_threshold_pct - result->flr_pct;
	report->avail_margin_pct = result->availability_pct - sla->availability_pct;

	/* Overall compliance */
	report->overall_compliant = report->fd_compliant && report->fdv_compliant &&
	                            report->flr_compliant && report->avail_compliant;

	return 0;
}

/* ============================================================================
 * Print Functions
 * ============================================================================ */

void mef_print_config_results(const mef_config_result_t *result)
{
	if (!result)
		return;

	printf("\n=== MEF 48 Configuration Test Results ===\n");
	printf("Service ID: %s\n", result->service_id);
	printf("Overall: %s\n\n", result->overall_passed ? "PASS" : "FAIL");

	for (uint32_t i = 0; i < result->num_steps; i++) {
		const mef_step_result_t *step = &result->steps[i];
		printf("Step %u (%u%% CIR):\n", i + 1, step->step_pct);
		printf("  Offered:  %u kbps\n", step->offered_rate_kbps);
		printf("  Achieved: %u kbps\n", step->achieved_rate_kbps);
		printf("  FD:       %.1f us (min=%.1f, max=%.1f)\n",
		       step->fd_us, step->fd_min_us, step->fd_max_us);
		printf("  FDV:      %.1f us\n", step->fdv_us);
		printf("  FLR:      %.4f%%\n", step->flr_pct);
		printf("  Result:   %s\n\n", step->passed ? "PASS" : "FAIL");
	}
}

void mef_print_perf_results(const mef_perf_result_t *result)
{
	if (!result)
		return;

	printf("\n=== MEF 48 Performance Test Results ===\n");
	printf("Service ID: %s\n", result->service_id);
	printf("Duration: %u sec\n", result->duration_sec);
	printf("\nThroughput:\n");
	printf("  Achieved Rate:    %u kbps\n", result->throughput_kbps);
	printf("  Frames TX/RX:     %" PRIu64 " / %" PRIu64 "\n",
	       result->frames_tx, result->frames_rx);
	printf("\nLatency:\n");
	printf("  Frame Delay:      avg=%.1f, min=%.1f, max=%.1f us\n",
	       result->fd_avg_us, result->fd_min_us, result->fd_max_us);
	printf("  Delay Variation:  %.1f us\n", result->fdv_us);
	printf("\nLoss & Availability:\n");
	printf("  Frame Loss Ratio: %.4f%%\n", result->flr_pct);
	printf("  Availability:     %.4f%%\n", result->availability_pct);
	printf("\nSLA Checks:\n");
	printf("  FD:    %s\n", result->fd_passed ? "PASS" : "FAIL");
	printf("  FDV:   %s\n", result->fdv_passed ? "PASS" : "FAIL");
	printf("  FLR:   %s\n", result->flr_passed ? "PASS" : "FAIL");
	printf("  Avail: %s\n", result->avail_passed ? "PASS" : "FAIL");
	printf("\nOverall: %s\n", result->overall_passed ? "PASS" : "FAIL");
}

void mef_print_results(const mef_config_result_t *config_result,
                       const mef_perf_result_t *perf_result,
                       stats_format_t format)
{
	(void)format;  /* TODO: implement JSON/CSV output */

	if (config_result)
		mef_print_config_results(config_result);

	if (perf_result)
		mef_print_perf_results(perf_result);
}

void mef_print_sla_report(const mef_sla_report_t *report)
{
	if (!report)
		return;

	printf("\n=== MEF SLA Compliance Report ===\n");
	printf("Frame Delay:     %s (threshold: %.1f us, measured: %.1f us, margin: %.1f us)\n",
	       report->fd_compliant ? "COMPLIANT" : "NON-COMPLIANT",
	       report->fd_threshold_us, report->fd_measured_us, report->fd_margin_us);
	printf("Delay Variation: %s (threshold: %.1f us, measured: %.1f us, margin: %.1f us)\n",
	       report->fdv_compliant ? "COMPLIANT" : "NON-COMPLIANT",
	       report->fdv_threshold_us, report->fdv_measured_us, report->fdv_margin_us);
	printf("Frame Loss:      %s (threshold: %.4f%%, measured: %.4f%%, margin: %.4f%%)\n",
	       report->flr_compliant ? "COMPLIANT" : "NON-COMPLIANT",
	       report->flr_threshold_pct, report->flr_measured_pct, report->flr_margin_pct);
	printf("Availability:    %s (threshold: %.4f%%, measured: %.4f%%, margin: %.4f%%)\n",
	       report->avail_compliant ? "COMPLIANT" : "NON-COMPLIANT",
	       report->avail_threshold_pct, report->avail_measured_pct, report->avail_margin_pct);
	printf("\nOverall SLA: %s\n",
	       report->overall_compliant ? "COMPLIANT" : "NON-COMPLIANT");
}
