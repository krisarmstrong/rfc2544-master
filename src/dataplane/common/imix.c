/*
 * imix.c - IMIX (Internet Mix) Traffic Profile Implementation
 *
 * Implements IMIX profiles for realistic Internet traffic simulation.
 * IMIX uses weighted distributions of frame sizes instead of fixed sizes.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <string.h>

/* Predefined IMIX profiles */

/* Simple IMIX: 7:4:1 ratio (58.33% small, 33.33% medium, 8.33% large) */
static const imix_entry_t imix_simple[] = {
	{64, 58.33},
	{570, 33.33},
	{1518, 8.34}
};

/* Cisco IMIX: Similar to simple but with 594 byte medium frames */
static const imix_entry_t imix_cisco[] = {
	{64, 58.33},
	{594, 33.33},
	{1518, 8.34}
};

/* Tolly IMIX: Broader distribution */
static const imix_entry_t imix_tolly[] = {
	{64, 55.0},
	{78, 5.0},
	{576, 17.0},
	{1500, 23.0}
};

/* IPSec IMIX: Larger frames typical of encrypted traffic */
static const imix_entry_t imix_ipsec[] = {
	{90, 30.0},    /* Small encrypted packets */
	{594, 40.0},   /* Medium encrypted */
	{1418, 30.0}   /* Large encrypted (1500 - IPSec overhead) */
};

/**
 * Get predefined IMIX profile configuration
 */
void imix_get_profile(imix_profile_t profile, imix_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));
	config->profile = profile;

	switch (profile) {
	case IMIX_SIMPLE:
		config->entry_count = 3;
		memcpy(config->entries, imix_simple, sizeof(imix_simple));
		break;

	case IMIX_CISCO:
		config->entry_count = 3;
		memcpy(config->entries, imix_cisco, sizeof(imix_cisco));
		break;

	case IMIX_TOLLY:
		config->entry_count = 4;
		memcpy(config->entries, imix_tolly, sizeof(imix_tolly));
		break;

	case IMIX_IPSEC:
		config->entry_count = 3;
		memcpy(config->entries, imix_ipsec, sizeof(imix_ipsec));
		break;

	case IMIX_CUSTOM:
	case IMIX_NONE:
	default:
		/* Custom or none - leave empty for user to fill */
		break;
	}
}

/**
 * Calculate weighted average frame size for IMIX profile
 */
double imix_avg_frame_size(const imix_config_t *config)
{
	if (!config || config->entry_count == 0)
		return 0.0;

	double total_weight = 0.0;
	double weighted_sum = 0.0;

	for (uint32_t i = 0; i < config->entry_count; i++) {
		weighted_sum += config->entries[i].frame_size * config->entries[i].weight;
		total_weight += config->entries[i].weight;
	}

	if (total_weight == 0.0)
		return 0.0;

	return weighted_sum / total_weight;
}

/**
 * Run IMIX throughput test
 *
 * This test runs throughput measurements with the specified IMIX profile,
 * distributing traffic across frame sizes according to their weights.
 */
int rfc2544_imix_throughput(rfc2544_ctx_t *ctx, const imix_config_t *imix_config,
                            imix_result_t *result)
{
	if (!ctx || !imix_config || !result)
		return -EINVAL;

	if (imix_config->entry_count == 0)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	/* Calculate average frame size */
	result->avg_frame_size = imix_avg_frame_size(imix_config);

	/* Run throughput test for each frame size in the profile */
	double total_weight = 0.0;
	for (uint32_t i = 0; i < imix_config->entry_count; i++) {
		total_weight += imix_config->entries[i].weight;
	}

	if (total_weight == 0.0)
		return -EINVAL;

	/* Aggregate results from all frame sizes */
	throughput_result_t per_size_result;
	double weighted_throughput = 0.0;
	double weighted_latency = 0.0;
	double weighted_jitter = 0.0;
	double min_latency = 1e9;
	double max_latency = 0.0;

	for (uint32_t i = 0; i < imix_config->entry_count; i++) {
		const imix_entry_t *entry = &imix_config->entries[i];
		double weight_fraction = entry->weight / total_weight;

		/* Configure context for this frame size */
		ctx->config.frame_size = entry->frame_size;

		/* Run throughput test for this size */
		uint32_t result_count;
		int ret = rfc2544_throughput_test(ctx, entry->frame_size, &per_size_result, &result_count);
		if (ret < 0) {
			rfc2544_log(LOG_WARN, "IMIX: Failed frame size %u: %d",
			            entry->frame_size, ret);
			continue;
		}

		/* Accumulate weighted results */
		weighted_throughput += per_size_result.max_rate_mbps * weight_fraction;
		weighted_latency += (per_size_result.latency.avg_ns / 1000.0) * weight_fraction;
		weighted_jitter += (per_size_result.latency.jitter_ns / 1000.0) * weight_fraction;

		if (per_size_result.latency.min_ns / 1000.0 < min_latency)
			min_latency = per_size_result.latency.min_ns / 1000.0;
		if (per_size_result.latency.max_ns / 1000.0 > max_latency)
			max_latency = per_size_result.latency.max_ns / 1000.0;

		/* Weighted frame counts */
		result->total_frames_tx += (uint64_t)(per_size_result.frames_tested * weight_fraction);
		result->total_frames_rx += (uint64_t)(per_size_result.frames_tested * weight_fraction);
	}

	/* Populate result */
	result->throughput_mbps = weighted_throughput;
	result->latency_avg_ms = weighted_latency / 1000.0;
	result->latency_min_ms = min_latency / 1000.0;
	result->latency_max_ms = max_latency / 1000.0;
	result->jitter_ms = weighted_jitter / 1000.0;

	/* Calculate frame rate and loss */
	if (result->avg_frame_size > 0) {
		result->frame_rate_fps = (result->throughput_mbps * 1e6) /
		                         (result->avg_frame_size * 8);
	}

	if (result->total_frames_tx > 0) {
		result->loss_pct = 100.0 *
		                   (double)(result->total_frames_tx - result->total_frames_rx) /
		                   result->total_frames_tx;
	}

	rfc2544_log(LOG_INFO, "IMIX Test Complete: %.2f Mbps, avg frame %.0f bytes, %.4f%% loss",
	            result->throughput_mbps, result->avg_frame_size, result->loss_pct);

	return 0;
}
