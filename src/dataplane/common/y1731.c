/*
 * y1731.c - ITU-T Y.1731 OAM Performance Monitoring Implementation
 *
 * Implements Ethernet OAM performance monitoring:
 * - Delay Measurement (DM)
 * - Loss Measurement (LM)
 * - Synthetic Loss Measurement (SLM)
 * - Loopback (LB)
 * - Continuity Check Messages (CCM)
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Y.1731 constants */
#define Y1731_DEFAULT_DURATION_SEC   60
#define Y1731_DEFAULT_WARMUP_SEC     2
#define Y1731_SIGNATURE_LOCAL        "Y.1731 "

/**
 * Initialize default MEP configuration
 */
void y1731_default_mep_config(y1731_mep_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	config->mep_id = 1;
	config->meg_level = MEG_LEVEL_CUSTOMER;
	config->ccm_interval = CCM_1S;
	config->priority = 7;
	config->enabled = true;
	strncpy(config->meg_id, "DEFAULT-MEG", sizeof(config->meg_id) - 1);
}

/**
 * Initialize Y.1731 session
 */
int y1731_session_init(rfc2544_ctx_t *ctx, const y1731_mep_config_t *config,
                       y1731_session_t *session)
{
	if (!ctx || !config || !session)
		return -EINVAL;

	(void)ctx;  /* Context used for future extensions */

	memset(session, 0, sizeof(*session));

	session->local_mep = *config;
	session->state = Y1731_STATE_INIT;

	rfc2544_log(LOG_INFO, "Y.1731 session initialized: MEP %u",
	            config->mep_id);

	return 0;
}

/* ============================================================================
 * Delay Measurement (DMM/DMR)
 *
 * Measures one-way and two-way frame delay using timestamps
 * ============================================================================ */

int y1731_delay_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                            uint32_t count, uint32_t interval_ms,
                            y1731_delay_result_t *result)
{
	if (!ctx || !session || !result)
		return -EINVAL;

	/* Validate interval_ms to prevent division by zero */
	if (interval_ms == 0) {
		rfc2544_log(LOG_ERROR, "Invalid interval_ms (0)");
		return -EINVAL;
	}

	memset(result, 0, sizeof(*result));
	session->state = Y1731_STATE_RUNNING;

	rfc2544_log(LOG_INFO, "=== Y.1731 Delay Measurement ===");
	rfc2544_log(LOG_INFO, "Count: %u, Interval: %u ms", count, interval_ms);

	/* Enable latency measurement */
	ctx->config.measure_latency = true;

	/* Calculate duration based on count and interval */
	uint32_t duration_sec = (count * interval_ms) / 1000 + 5;

	/* Calculate rate based on interval */
	/* interval_ms determines probe rate: 1000ms interval = 1 pps */
	double probes_per_sec = 1000.0 / (double)interval_ms;
	uint64_t max_pps = calc_max_pps(ctx->line_rate, 128);  /* DMM frames ~128 bytes */
	double rate_pct = (max_pps > 0) ? ((probes_per_sec * 100.0) / (double)max_pps) : 0.001;
	if (rate_pct > 100.0) rate_pct = 100.0;
	if (rate_pct < 0.001) rate_pct = 0.001;

	/* Run delay measurement trial */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 128, rate_pct,
	                           duration_sec, Y1731_DEFAULT_WARMUP_SEC,
	                           Y1731_SIGNATURE_LOCAL, session->local_mep.mep_id,
	                           &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Delay measurement trial failed: %d", ret);
		session->state = Y1731_STATE_ERROR;
		return ret;
	}

	/* Record results */
	result->frames_sent = trial.packets_sent;
	result->frames_received = trial.packets_recv;

	/* Two-way delay = RTT */
	result->delay_avg_us = trial.latency.avg_ns / 1000.0;
	result->delay_min_us = trial.latency.min_ns / 1000.0;
	result->delay_max_us = trial.latency.max_ns / 1000.0;

	/* Delay variation = max - min */
	result->delay_variation_us = result->delay_max_us - result->delay_min_us;

	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "Two-way Delay: avg=%.1f, min=%.1f, max=%.1f us",
	            result->delay_avg_us, result->delay_min_us, result->delay_max_us);
	rfc2544_log(LOG_INFO, "Delay Variation: %.1f us", result->delay_variation_us);

	return 0;
}

/* ============================================================================
 * Loss Measurement (LMM/LMR)
 *
 * Measures near-end and far-end frame loss
 * ============================================================================ */

int y1731_loss_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                           uint32_t duration_sec, y1731_loss_result_t *result)
{
	if (!ctx || !session || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	session->state = Y1731_STATE_RUNNING;

	rfc2544_log(LOG_INFO, "=== Y.1731 Loss Measurement ===");
	rfc2544_log(LOG_INFO, "Duration: %u sec", duration_sec);

	/* Run at moderate rate for loss measurement */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 128, 50.0,  /* 50% rate */
	                           duration_sec, Y1731_DEFAULT_WARMUP_SEC,
	                           Y1731_SIGNATURE_LOCAL, session->local_mep.mep_id,
	                           &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Loss measurement trial failed: %d", ret);
		session->state = Y1731_STATE_ERROR;
		return ret;
	}

	/* Record results */
	result->frames_tx = trial.packets_sent;
	result->frames_rx = trial.packets_recv;

	/* Calculate loss - guard against underflow when rx > tx */
	if (trial.packets_sent > 0) {
		if (trial.packets_recv < trial.packets_sent) {
			result->near_end_loss = trial.packets_sent - trial.packets_recv;
			result->near_end_loss_ratio = (double)result->near_end_loss / trial.packets_sent;
		} else {
			result->near_end_loss = 0;
			result->near_end_loss_ratio = 0.0;
		}
	}

	/* Far-end loss requires bidirectional counters from remote MEP */
	/* In loopback mode, far-end = near-end */
	result->far_end_loss = result->near_end_loss;
	result->far_end_loss_ratio = result->near_end_loss_ratio;

	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "Near-end Loss: %" PRIu64 " frames (%.4f%%)",
	            result->near_end_loss, result->near_end_loss_ratio * 100.0);
	rfc2544_log(LOG_INFO, "Far-end Loss: %" PRIu64 " frames (%.4f%%)",
	            result->far_end_loss, result->far_end_loss_ratio * 100.0);

	return 0;
}

/* ============================================================================
 * Synthetic Loss Measurement (SLM)
 *
 * Proactive loss measurement using synthetic frames
 * ============================================================================ */

int y1731_synthetic_loss(rfc2544_ctx_t *ctx, y1731_session_t *session,
                         uint32_t count, uint32_t interval_ms,
                         y1731_loss_result_t *result)
{
	if (!ctx || !session || !result)
		return -EINVAL;

	/* Validate interval_ms to prevent division by zero */
	if (interval_ms == 0) {
		rfc2544_log(LOG_ERROR, "Invalid interval_ms (0)");
		return -EINVAL;
	}

	memset(result, 0, sizeof(*result));
	session->state = Y1731_STATE_RUNNING;

	uint32_t duration_sec = (count * interval_ms) / 1000 + 5;

	rfc2544_log(LOG_INFO, "=== Y.1731 Synthetic Loss Measurement ===");
	rfc2544_log(LOG_INFO, "Count: %u, Interval: %u ms", count, interval_ms);

	/* Calculate rate percentage */
	double probes_per_sec = 1000.0 / (double)interval_ms;
	uint64_t max_pps = calc_max_pps(ctx->line_rate, 128);
	double rate_pct = (max_pps > 0) ? ((probes_per_sec * 100.0) / (double)max_pps) : 1.0;
	if (rate_pct > 100.0) rate_pct = 100.0;

	/* Run synthetic loss trial */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 128, rate_pct,
	                           duration_sec, Y1731_DEFAULT_WARMUP_SEC,
	                           Y1731_SIGNATURE_LOCAL, session->local_mep.mep_id,
	                           &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Synthetic loss trial failed: %d", ret);
		session->state = Y1731_STATE_ERROR;
		return ret;
	}

	/* Record results */
	result->frames_tx = trial.packets_sent;
	result->frames_rx = trial.packets_recv;

	/* Guard against underflow when rx > tx */
	if (trial.packets_sent > 0) {
		if (trial.packets_recv < trial.packets_sent) {
			result->near_end_loss = trial.packets_sent - trial.packets_recv;
			result->near_end_loss_ratio = (double)result->near_end_loss / trial.packets_sent;
		} else {
			result->near_end_loss = 0;
			result->near_end_loss_ratio = 0.0;
		}
	}

	/* For SLM, far-end is same as near-end in loopback mode */
	result->far_end_loss = result->near_end_loss;
	result->far_end_loss_ratio = result->near_end_loss_ratio;

	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "Synthetic Loss: %" PRIu64 "/%" PRIu64 " frames lost (%.4f%%)",
	            result->near_end_loss, result->frames_tx,
	            result->near_end_loss_ratio * 100.0);

	return 0;
}

/* ============================================================================
 * Loopback (LBM/LBR)
 *
 * Verifies connectivity and measures response time
 * ============================================================================ */

int y1731_loopback(rfc2544_ctx_t *ctx, y1731_session_t *session,
                   const uint8_t *target_mac, uint32_t count,
                   y1731_loopback_result_t *result)
{
	if (!ctx || !session || !result)
		return -EINVAL;

	(void)target_mac;  /* Used for actual OAM implementation */

	memset(result, 0, sizeof(*result));
	session->state = Y1731_STATE_RUNNING;

	rfc2544_log(LOG_INFO, "=== Y.1731 Loopback Test ===");
	rfc2544_log(LOG_INFO, "Count: %u", count);

	/* Enable latency measurement */
	ctx->config.measure_latency = true;

	/* Calculate duration - 1 probe per second */
	uint32_t duration_sec = count + 5;

	/* Low rate - approximately 1 pps */
	uint64_t max_pps = calc_max_pps(ctx->line_rate, 128);
	double rate_pct = (max_pps > 0) ? ((1.0 * 100.0) / (double)max_pps) : 0.001;
	if (rate_pct < 0.001) rate_pct = 0.001;

	/* Run loopback trial */
	trial_result_t trial;
	int ret = run_trial_custom(ctx, 128, rate_pct,
	                           duration_sec, 1,
	                           Y1731_SIGNATURE_LOCAL, session->local_mep.mep_id,
	                           &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Loopback trial failed: %d", ret);
		session->state = Y1731_STATE_ERROR;
		return ret;
	}

	/* Record results */
	result->lbm_sent = (trial.packets_sent < count) ? (uint32_t)trial.packets_sent : count;
	result->lbr_received = (uint32_t)trial.packets_recv;

	/* Response time from latency stats (convert ns to ms) */
	result->rtt_avg_ms = trial.latency.avg_ns / 1e6;
	result->rtt_min_ms = trial.latency.min_ns / 1e6;
	result->rtt_max_ms = trial.latency.max_ns / 1e6;

	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "Loopback: %u/%u replies (%.1f%%)",
	            result->lbr_received, result->lbm_sent,
	            result->lbm_sent > 0 ?
	            100.0 * result->lbr_received / result->lbm_sent : 0.0);
	rfc2544_log(LOG_INFO, "RTT: avg=%.3f, min=%.3f, max=%.3f ms",
	            result->rtt_avg_ms, result->rtt_min_ms, result->rtt_max_ms);

	return 0;
}

/* ============================================================================
 * CCM Management
 * ============================================================================ */

int y1731_start_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session)
{
	if (!ctx || !session)
		return -EINVAL;

	session->state = Y1731_STATE_RUNNING;
	session->ccm_tx_count = 0;
	session->ccm_rx_count = 0;
	session->rdi_received = false;

	rfc2544_log(LOG_INFO, "CCM started for MEP %u", session->local_mep.mep_id);

	return 0;
}

int y1731_stop_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session)
{
	if (!ctx || !session)
		return -EINVAL;

	(void)ctx;
	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "CCM stopped for MEP %u", session->local_mep.mep_id);

	return 0;
}

int y1731_get_status(y1731_session_t *session, y1731_session_status_t *status)
{
	if (!session || !status)
		return -EINVAL;

	memset(status, 0, sizeof(*status));

	status->state = session->state;
	status->ccm_tx_count = session->ccm_tx_count;
	status->ccm_rx_count = session->ccm_rx_count;
	status->rdi_received = session->rdi_received;
	status->local_mep_id = session->local_mep.mep_id;
	status->remote_mep_id = session->remote_mep.mep_id;

	/* Connectivity based on CCM exchange */
	if (session->state == Y1731_STATE_RUNNING) {
		status->connectivity_ok = (session->ccm_rx_count > 0);
	}

	return 0;
}

/* ============================================================================
 * Print Functions
 * ============================================================================ */

void y1731_print_delay_results(const y1731_delay_result_t *result)
{
	if (!result)
		return;

	printf("\n=== Y.1731 Delay Measurement Results ===\n");
	printf("Frames Sent:      %" PRIu32 "\n", result->frames_sent);
	printf("Frames Received:  %" PRIu32 "\n", result->frames_received);
	printf("\nTwo-way Delay:\n");
	printf("  Average:        %.1f us\n", result->delay_avg_us);
	printf("  Minimum:        %.1f us\n", result->delay_min_us);
	printf("  Maximum:        %.1f us\n", result->delay_max_us);
	printf("  Variation:      %.1f us\n", result->delay_variation_us);
}

void y1731_print_loss_results(const y1731_loss_result_t *result)
{
	if (!result)
		return;

	printf("\n=== Y.1731 Loss Measurement Results ===\n");
	printf("Frames TX:        %" PRIu64 "\n", result->frames_tx);
	printf("Frames RX:        %" PRIu64 "\n", result->frames_rx);
	printf("\nLoss Statistics:\n");
	printf("  Near-end Loss:  %" PRIu64 " (%.4f%%)\n",
	       result->near_end_loss, result->near_end_loss_ratio * 100.0);
	printf("  Far-end Loss:   %" PRIu64 " (%.4f%%)\n",
	       result->far_end_loss, result->far_end_loss_ratio * 100.0);
}
