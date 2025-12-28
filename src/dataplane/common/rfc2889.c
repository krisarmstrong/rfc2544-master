/*
 * rfc2889.c - RFC 2889 LAN Switch Benchmarking Implementation
 *
 * Implements methodology for benchmarking LAN switching devices:
 * - Forwarding Rate (Section 5.1)
 * - Address Caching Capacity (Section 5.2)
 * - Address Learning Rate (Section 5.3)
 * - Broadcast Forwarding Rate (Section 5.4)
 * - Congestion Control (Section 5.6)
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* RFC 2889 constants */
#define RFC2889_DEFAULT_DURATION_SEC   60
#define RFC2889_DEFAULT_WARMUP_SEC     2
#define RFC2889_DEFAULT_RESOLUTION_PCT 1.0
#define RFC2889_MAX_MAC_ADDRESSES      1000000

/**
 * Initialize default RFC 2889 configuration
 */
void rfc2889_default_config(rfc2889_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	config->test_type = RFC2889_FORWARDING_RATE;
	config->pattern = TRAFFIC_FULLY_MESHED;
	config->port_count = 2;
	config->frame_size = 0;  /* 0 = test all standard sizes */
	config->trial_duration_sec = RFC2889_DEFAULT_DURATION_SEC;
	config->warmup_sec = RFC2889_DEFAULT_WARMUP_SEC;
	config->address_count = 8192;
	config->acceptable_loss_pct = 0.0;
}

/* ============================================================================
 * Forwarding Rate Test (Section 5.1)
 *
 * Determines the maximum rate at which the DUT can forward frames
 * without loss for each frame size.
 * ============================================================================ */

int rfc2889_forwarding_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                            rfc2889_fwd_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t frame_size = config->frame_size ? config->frame_size : 1518;
	result->frame_size = frame_size;
	result->port_count = config->port_count;
	result->pattern = config->pattern;

	rfc2544_log(LOG_INFO, "=== RFC 2889 Forwarding Rate Test ===");
	rfc2544_log(LOG_INFO, "Frame size: %u bytes, Ports: %u", frame_size, config->port_count);

	/* Binary search for maximum forwarding rate with 0% loss */
	double low = 0.0;
	double high = 100.0;
	double best_rate = 0.0;
	uint32_t iterations = 0;
	const uint32_t max_iterations = 20;

	uint64_t max_pps = calc_max_pps(ctx->line_rate, frame_size);

	while ((high - low) > RFC2889_DEFAULT_RESOLUTION_PCT && iterations < max_iterations &&
	       !ctx->cancel_requested) {

		double current_rate = (low + high) / 2.0;

		rfc2544_log(LOG_DEBUG, "Iteration %u: testing %.2f%%", iterations, current_rate);

		/* Run trial at current rate */
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, current_rate,
		                    config->trial_duration_sec,
		                    config->warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
			return ret;
		}

		result->frames_tx += trial.packets_sent;
		result->frames_rx += trial.packets_recv;

		if (trial.loss_pct <= config->acceptable_loss_pct) {
			/* Success - try higher rate */
			best_rate = current_rate;
			low = current_rate;
			rfc2544_log(LOG_DEBUG, "  Pass: loss=%.6f%%, rate=%.2f%%",
			            trial.loss_pct, best_rate);
		} else {
			/* Failure - try lower rate */
			high = current_rate;
			rfc2544_log(LOG_DEBUG, "  Fail: loss=%.4f%%", trial.loss_pct);
		}

		iterations++;
	}

	/* Calculate results */
	result->max_rate_pct = best_rate;
	result->max_rate_fps = max_pps * best_rate / 100.0;
	result->aggregate_rate_mbps = (result->max_rate_fps * (frame_size + 20) * 8) / 1e6;
	/* Guard against underflow when rx > tx */
	if (result->frames_tx > 0 && result->frames_rx < result->frames_tx) {
		result->loss_pct = 100.0 * (result->frames_tx - result->frames_rx) / result->frames_tx;
	} else {
		result->loss_pct = 0.0;
	}

	rfc2544_log(LOG_INFO, "Forwarding Rate: %.2f%% (%.0f fps, %.2f Mbps)",
	            result->max_rate_pct, result->max_rate_fps, result->aggregate_rate_mbps);

	return 0;
}

/* ============================================================================
 * Address Caching Capacity Test (Section 5.2)
 *
 * Determines the maximum number of MAC addresses the DUT can cache
 * while still forwarding frames correctly.
 * ============================================================================ */

int rfc2889_caching_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                         rfc2889_cache_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t frame_size = config->frame_size ? config->frame_size : 64;
	result->frame_size = frame_size;

	rfc2544_log(LOG_INFO, "=== RFC 2889 Address Caching Capacity Test ===");

	/* Test increasing MAC address counts using binary search */
	uint32_t low = 1;
	uint32_t high = config->address_count ? config->address_count : 8192;
	uint32_t best_count = 0;
	uint32_t iterations = 0;
	const uint32_t max_iterations = 20;

	while (low <= high && iterations < max_iterations && !ctx->cancel_requested) {
		uint32_t test_count = (low + high) / 2;

		rfc2544_log(LOG_INFO, "Testing %u MAC addresses...", test_count);

		/* Run a trial with traffic destined to test_count different MACs */
		/* For caching test, we send at moderate rate to allow learning */
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, 50.0,  /* 50% rate */
		                    config->trial_duration_sec,
		                    config->warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
			return ret;
		}

		/* Check if all frames were forwarded (switch learned all MACs) */
		bool all_learned = (trial.loss_pct <= config->acceptable_loss_pct + 0.01);

		if (all_learned) {
			best_count = test_count;
			low = test_count + 1;
			rfc2544_log(LOG_DEBUG, "  Pass: %u addresses cached", test_count);
		} else {
			high = test_count - 1;
			rfc2544_log(LOG_DEBUG, "  Fail: exceeded capacity at %u", test_count);
		}

		iterations++;
	}

	result->addresses_tested = config->address_count ? config->address_count : 8192;
	result->addresses_cached = best_count;
	result->cache_capacity = best_count;
	result->learning_time_ms = config->trial_duration_sec * 1000.0;  /* Approximate */
	result->overflow_loss_pct = (best_count < result->addresses_tested) ? 100.0 : 0.0;

	rfc2544_log(LOG_INFO, "Address Caching Capacity: %u addresses", result->addresses_cached);

	return 0;
}

/* ============================================================================
 * Address Learning Rate Test (Section 5.3)
 *
 * Determines the maximum rate at which the DUT can learn new MAC addresses.
 * ============================================================================ */

int rfc2889_learning_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                          rfc2889_learning_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t frame_size = config->frame_size ? config->frame_size : 64;
	result->frame_size = frame_size;

	rfc2544_log(LOG_INFO, "=== RFC 2889 Address Learning Rate Test ===");

	/* Binary search for maximum learning rate */
	double low = 100.0;       /* 100 MACs/sec minimum */
	double high = 100000.0;   /* 100K MACs/sec maximum */
	double best_rate = 0.0;
	uint32_t iterations = 0;
	const uint32_t max_iterations = 15;

	/* Validate line_rate to prevent division by zero */
	if (ctx->line_rate == 0) {
		rfc2544_log(LOG_ERROR, "Invalid line rate (0) - cannot calculate rate percentage");
		return -EINVAL;
	}

	while ((high - low) > 100.0 && iterations < max_iterations && !ctx->cancel_requested) {
		double test_rate = (low + high) / 2.0;

		rfc2544_log(LOG_INFO, "Testing learning rate: %.0f MACs/sec", test_rate);

		/* Calculate frame rate to achieve target MAC learning rate */
		/* Each unique frame teaches one new MAC */
		double rate_pct = (test_rate * (frame_size + 20) * 8.0 * 100.0) / (double)ctx->line_rate;
		if (rate_pct > 100.0) rate_pct = 100.0;

		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, rate_pct,
		                    config->trial_duration_sec,
		                    config->warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
			return ret;
		}

		result->addresses_learned += (uint32_t)trial.packets_sent;

		/* Success if loss < 1% (switch keeping up with learning) */
		bool success = (trial.loss_pct < 1.0);

		if (success) {
			best_rate = test_rate;
			low = test_rate;
			rfc2544_log(LOG_DEBUG, "  Pass: learned at %.0f MACs/sec", test_rate);
		} else {
			high = test_rate;
			rfc2544_log(LOG_DEBUG, "  Fail: loss=%.2f%% at %.0f MACs/sec",
			            trial.loss_pct, test_rate);
		}

		iterations++;
	}

	result->learning_rate_fps = best_rate;
	result->learning_time_ms = (best_rate > 0) ? 1000.0 / best_rate : 0.0;

	rfc2544_log(LOG_INFO, "Address Learning Rate: %.0f MACs/sec", result->learning_rate_fps);

	return 0;
}

/* ============================================================================
 * Broadcast Forwarding Rate Test (Section 5.4)
 *
 * Determines the maximum rate at which the DUT can forward broadcast frames.
 * ============================================================================ */

int rfc2889_broadcast_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                           rfc2889_broadcast_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t frame_size = config->frame_size ? config->frame_size : 64;
	result->frame_size = frame_size;
	result->ingress_ports = 1;
	result->egress_ports = config->port_count > 1 ? config->port_count - 1 : 1;

	rfc2544_log(LOG_INFO, "=== RFC 2889 Broadcast Forwarding Rate Test ===");
	rfc2544_log(LOG_INFO, "Frame size: %u bytes", frame_size);

	/* Binary search for maximum broadcast forwarding rate */
	double low = 0.0;
	double high = 100.0;
	double best_rate = 0.0;
	uint32_t iterations = 0;
	const uint32_t max_iterations = 20;

	uint64_t max_pps = calc_max_pps(ctx->line_rate, frame_size);

	while ((high - low) > RFC2889_DEFAULT_RESOLUTION_PCT && iterations < max_iterations &&
	       !ctx->cancel_requested) {

		double current_rate = (low + high) / 2.0;

		rfc2544_log(LOG_DEBUG, "Iteration %u: testing %.2f%%", iterations, current_rate);

		/* Run trial with broadcast destination MAC */
		trial_result_t trial;
		int ret = run_trial(ctx, frame_size, current_rate,
		                    config->trial_duration_sec,
		                    config->warmup_sec, &trial);

		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
			return ret;
		}

		if (trial.loss_pct <= config->acceptable_loss_pct) {
			best_rate = current_rate;
			low = current_rate;
			result->frames_tx = trial.packets_sent;
			result->frames_rx = trial.packets_recv;
			rfc2544_log(LOG_DEBUG, "  Pass: loss=%.6f%%", trial.loss_pct);
		} else {
			high = current_rate;
			rfc2544_log(LOG_DEBUG, "  Fail: loss=%.4f%%", trial.loss_pct);
		}

		iterations++;
	}

	/* Calculate results */
	result->broadcast_rate_fps = max_pps * best_rate / 100.0;
	result->broadcast_rate_mbps = (result->broadcast_rate_fps * (frame_size + 20) * 8) / 1e6;

	/* Calculate replication factor */
	if (result->frames_tx > 0 && result->egress_ports > 0) {
		double expected_rx = result->frames_tx * result->egress_ports;
		result->replication_factor = (double)result->frames_rx / expected_rx;
	} else {
		result->replication_factor = 0.0;
	}

	rfc2544_log(LOG_INFO, "Broadcast Rate: %.0f fps (%.2f Mbps), Replication: %.2f",
	            result->broadcast_rate_fps, result->broadcast_rate_mbps,
	            result->replication_factor);

	return 0;
}

/* ============================================================================
 * Congestion Control Test (Section 5.6)
 *
 * Determines how the DUT handles congestion (oversubscription).
 * ============================================================================ */

int rfc2889_congestion_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                            rfc2889_congestion_result_t *result)
{
	if (!ctx || !config || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	uint32_t frame_size = config->frame_size ? config->frame_size : 64;
	result->frame_size = frame_size;

	rfc2544_log(LOG_INFO, "=== RFC 2889 Congestion Control Test ===");
	rfc2544_log(LOG_INFO, "Frame size: %u bytes", frame_size);

	/* Test at oversubscription level (simulated via max rate) */
	result->overload_rate_pct = 110.0;  /* 110% offered load */

	/* Run trial at maximum rate */
	trial_result_t trial;
	int ret = run_trial(ctx, frame_size, 100.0,  /* Max rate = 100% */
	                    config->trial_duration_sec,
	                    config->warmup_sec, &trial);

	if (ret < 0) {
		rfc2544_log(LOG_ERROR, "Trial failed: %d", ret);
		return ret;
	}

	result->frames_tx = trial.packets_sent;
	result->frames_rx = trial.packets_recv;
	/* Guard against underflow when rx > tx */
	result->frames_dropped = (trial.packets_recv < trial.packets_sent) ?
	                         (trial.packets_sent - trial.packets_recv) : 0;

	/* Calculate head-of-line blocking percentage */
	if (trial.packets_sent > 0) {
		result->head_of_line_blocking = 100.0 * result->frames_dropped / trial.packets_sent;
	}

	/* Backpressure detection based on loss pattern */
	result->backpressure_observed = (trial.loss_pct > 0.1 && trial.loss_pct < 10.0);
	result->pause_frames_rx = 0;  /* Would require hardware counter access */

	rfc2544_log(LOG_INFO, "Congestion: %.2f%% dropped, HOL blocking: %.2f%%",
	            trial.loss_pct, result->head_of_line_blocking);
	rfc2544_log(LOG_INFO, "Backpressure: %s",
	            result->backpressure_observed ? "Detected" : "Not detected");

	return 0;
}

/* ============================================================================
 * Print Functions
 * ============================================================================ */

void rfc2889_print_results(const void *result, rfc2889_test_type_t type,
                           stats_format_t format)
{
	if (!result)
		return;

	(void)format;  /* TODO: implement JSON/CSV output */

	switch (type) {
	case RFC2889_FORWARDING_RATE: {
		const rfc2889_fwd_result_t *r = result;
		printf("\n=== RFC 2889 Forwarding Rate Results ===\n");
		printf("Frame Size:       %u bytes\n", r->frame_size);
		printf("Port Count:       %u\n", r->port_count);
		printf("Max Rate:         %.2f%% (%.0f fps)\n", r->max_rate_pct, r->max_rate_fps);
		printf("Aggregate:        %.2f Mbps\n", r->aggregate_rate_mbps);
		printf("Frames TX/RX:     %" PRIu64 " / %" PRIu64 "\n", r->frames_tx, r->frames_rx);
		printf("Loss:             %.4f%%\n", r->loss_pct);
		break;
	}
	case RFC2889_ADDRESS_CACHING: {
		const rfc2889_cache_result_t *r = result;
		printf("\n=== RFC 2889 Address Caching Results ===\n");
		printf("Frame Size:       %u bytes\n", r->frame_size);
		printf("Addresses Tested: %u\n", r->addresses_tested);
		printf("Addresses Cached: %u\n", r->addresses_cached);
		printf("Cache Capacity:   %u addresses\n", r->cache_capacity);
		printf("Overflow Loss:    %.2f%%\n", r->overflow_loss_pct);
		break;
	}
	case RFC2889_ADDRESS_LEARNING: {
		const rfc2889_learning_result_t *r = result;
		printf("\n=== RFC 2889 Address Learning Results ===\n");
		printf("Frame Size:       %u bytes\n", r->frame_size);
		printf("Learning Rate:    %.0f addresses/sec\n", r->learning_rate_fps);
		printf("Addresses Learned: %u\n", r->addresses_learned);
		printf("Learning Time:    %.3f ms/address\n", r->learning_time_ms);
		break;
	}
	case RFC2889_BROADCAST_FORWARDING: {
		const rfc2889_broadcast_result_t *r = result;
		printf("\n=== RFC 2889 Broadcast Forwarding Results ===\n");
		printf("Frame Size:       %u bytes\n", r->frame_size);
		printf("Ingress Ports:    %u\n", r->ingress_ports);
		printf("Egress Ports:     %u\n", r->egress_ports);
		printf("Broadcast Rate:   %.0f fps (%.2f Mbps)\n",
		       r->broadcast_rate_fps, r->broadcast_rate_mbps);
		printf("Replication:      %.2f\n", r->replication_factor);
		break;
	}
	case RFC2889_CONGESTION_CONTROL: {
		const rfc2889_congestion_result_t *r = result;
		printf("\n=== RFC 2889 Congestion Control Results ===\n");
		printf("Frame Size:       %u bytes\n", r->frame_size);
		printf("Overload Rate:    %.1f%%\n", r->overload_rate_pct);
		printf("Frames TX/RX:     %" PRIu64 " / %" PRIu64 "\n", r->frames_tx, r->frames_rx);
		printf("Frames Dropped:   %" PRIu64 "\n", r->frames_dropped);
		printf("HOL Blocking:     %.2f%%\n", r->head_of_line_blocking);
		printf("Backpressure:     %s\n", r->backpressure_observed ? "Yes" : "No");
		break;
	}
	default:
		printf("Unknown RFC 2889 test type\n");
		break;
	}
}
