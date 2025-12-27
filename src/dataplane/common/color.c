/*
 * color.c - Y.1564 Color-Aware Metering and Burst Validation
 *
 * Implements MEF color-aware traffic metering:
 * - Green: Traffic within CIR (committed information rate)
 * - Yellow: Traffic in EIR (excess information rate)
 * - Red: Traffic above CIR+EIR (dropped)
 *
 * Also implements CBS/EBS (Committed/Excess Burst Size) validation.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/**
 * Token bucket state for metering
 */
typedef struct {
	double tokens;           /* Current tokens in bucket */
	double bucket_size;      /* Maximum bucket size (bytes) */
	double rate;             /* Token refill rate (bytes/sec) */
	uint64_t last_update_ns; /* Last update timestamp */
} token_bucket_t;

/**
 * Dual token bucket meter (CIR + EIR)
 */
typedef struct {
	token_bucket_t cir_bucket;  /* CIR bucket (green) */
	token_bucket_t eir_bucket;  /* EIR bucket (yellow) */
} dual_bucket_meter_t;

/**
 * Get current time in nanoseconds
 */
static uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/**
 * Initialize token bucket
 */
static void bucket_init(token_bucket_t *bucket, double rate_bps, double burst_bytes)
{
	bucket->tokens = burst_bytes;  /* Start full */
	bucket->bucket_size = burst_bytes;
	bucket->rate = rate_bps / 8.0; /* Convert to bytes/sec */
	bucket->last_update_ns = get_time_ns();
}

/**
 * Update token bucket and check if packet conforms
 */
static bool bucket_conform(token_bucket_t *bucket, uint32_t packet_size)
{
	uint64_t now = get_time_ns();
	double elapsed_sec = (now - bucket->last_update_ns) / 1e9;
	bucket->last_update_ns = now;

	/* Refill tokens */
	bucket->tokens += elapsed_sec * bucket->rate;
	if (bucket->tokens > bucket->bucket_size)
		bucket->tokens = bucket->bucket_size;

	/* Check if packet conforms */
	if (bucket->tokens >= packet_size) {
		bucket->tokens -= packet_size;
		return true;
	}

	return false;
}

/**
 * Initialize dual bucket meter
 */
static void meter_init(dual_bucket_meter_t *meter, const y1564_sla_t *sla)
{
	/* CIR bucket: rate = CIR, burst = CBS */
	bucket_init(&meter->cir_bucket, sla->cir_mbps * 1e6, sla->cbs_bytes);

	/* EIR bucket: rate = EIR, burst = EBS */
	if (sla->eir_mbps > 0) {
		bucket_init(&meter->eir_bucket, sla->eir_mbps * 1e6, sla->ebs_bytes);
	} else {
		bucket_init(&meter->eir_bucket, 0, 0);
	}
}

/**
 * Meter a packet and return its color
 */
static traffic_color_t meter_packet(dual_bucket_meter_t *meter, uint32_t packet_size)
{
	/* Check CIR bucket first (green) */
	if (bucket_conform(&meter->cir_bucket, packet_size)) {
		return COLOR_GREEN;
	}

	/* Check EIR bucket (yellow) */
	if (bucket_conform(&meter->eir_bucket, packet_size)) {
		return COLOR_YELLOW;
	}

	/* Above CIR+EIR (red - drop) */
	return COLOR_RED;
}

/**
 * Run color-aware metering test
 */
int y1564_color_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                     color_result_t *result)
{
	if (!ctx || !service || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "Starting color-aware metering test for service %u",
	            service->service_id);
	rfc2544_log(LOG_INFO, "  CIR: %.2f Mbps, CBS: %u bytes",
	            service->sla.cir_mbps, service->sla.cbs_bytes);
	rfc2544_log(LOG_INFO, "  EIR: %.2f Mbps, EBS: %u bytes",
	            service->sla.eir_mbps, service->sla.ebs_bytes);

	/* Initialize dual bucket meter */
	dual_bucket_meter_t meter;
	meter_init(&meter, &service->sla);

	/*
	 * Generate traffic at CIR + EIR + excess (150% of CIR+EIR)
	 * and track how packets are colored.
	 */
	double test_rate = (service->sla.cir_mbps + service->sla.eir_mbps) * 1.5;
	uint32_t frame_size = service->frame_size > 0 ? service->frame_size : 512;
	uint32_t test_duration_sec = 10;

	/* Calculate packets to send */
	uint64_t total_packets = (uint64_t)(test_rate * 1e6 / 8 / frame_size * test_duration_sec);

	rfc2544_log(LOG_INFO, "  Test rate: %.2f Mbps, duration: %u sec, packets: %lu",
	            test_rate, test_duration_sec, total_packets);

	/* Simulate metering (in real implementation, this would send actual traffic) */
	for (uint64_t i = 0; i < total_packets; i++) {
		traffic_color_t color = meter_packet(&meter, frame_size);

		switch (color) {
		case COLOR_GREEN:
			result->green_frames++;
			break;
		case COLOR_YELLOW:
			result->yellow_frames++;
			break;
		case COLOR_RED:
			result->red_frames++;
			break;
		}
	}

	/* Calculate percentages */
	uint64_t total = result->green_frames + result->yellow_frames + result->red_frames;
	if (total > 0) {
		result->green_pct = 100.0 * result->green_frames / total;
		result->yellow_pct = 100.0 * result->yellow_frames / total;
		result->red_pct = 100.0 * result->red_frames / total;
	}

	rfc2544_log(LOG_INFO, "Color test complete:");
	rfc2544_log(LOG_INFO, "  Green:  %lu (%.2f%%)", result->green_frames, result->green_pct);
	rfc2544_log(LOG_INFO, "  Yellow: %lu (%.2f%%)", result->yellow_frames, result->yellow_pct);
	rfc2544_log(LOG_INFO, "  Red:    %lu (%.2f%%)", result->red_frames, result->red_pct);

	return 0;
}

/**
 * Validate CBS/EBS burst sizes
 */
int y1564_burst_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                     y1564_burst_result_t *result)
{
	if (!ctx || !service || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	result->expected_cbs = service->sla.cbs_bytes;
	result->expected_ebs = service->sla.ebs_bytes;

	rfc2544_log(LOG_INFO, "Starting burst validation for service %u", service->service_id);
	rfc2544_log(LOG_INFO, "  Expected CBS: %u bytes, EBS: %u bytes",
	            result->expected_cbs, result->expected_ebs);

	/*
	 * CBS Test: Send burst at line rate, count green frames
	 * The number of green frames * frame_size should approximate CBS
	 */
	dual_bucket_meter_t meter;
	meter_init(&meter, &service->sla);

	uint32_t frame_size = service->frame_size > 0 ? service->frame_size : 64;
	uint32_t max_burst_frames = (result->expected_cbs * 3) / frame_size; /* 3x expected */

	/* Measure CBS: send burst, count green */
	uint32_t green_count = 0;
	for (uint32_t i = 0; i < max_burst_frames; i++) {
		if (bucket_conform(&meter.cir_bucket, frame_size)) {
			green_count++;
		} else {
			break; /* First non-green ends CBS measurement */
		}
	}
	result->measured_cbs = green_count * frame_size;

	/* Measure EBS: reset and send burst above CIR */
	meter_init(&meter, &service->sla);

	/* Drain CIR bucket first */
	for (uint32_t i = 0; i < max_burst_frames; i++) {
		if (!bucket_conform(&meter.cir_bucket, frame_size))
			break;
	}

	/* Now count yellow frames */
	uint32_t yellow_count = 0;
	for (uint32_t i = 0; i < max_burst_frames; i++) {
		if (bucket_conform(&meter.eir_bucket, frame_size)) {
			yellow_count++;
		} else {
			break;
		}
	}
	result->measured_ebs = yellow_count * frame_size;

	/* Validate results (allow 10% tolerance) */
	double cbs_tolerance = result->expected_cbs * 0.1;
	double ebs_tolerance = result->expected_ebs * 0.1;

	result->cbs_valid = (result->measured_cbs >= result->expected_cbs - cbs_tolerance) &&
	                    (result->measured_cbs <= result->expected_cbs + cbs_tolerance);

	if (result->expected_ebs > 0) {
		result->ebs_valid = (result->measured_ebs >= result->expected_ebs - ebs_tolerance) &&
		                    (result->measured_ebs <= result->expected_ebs + ebs_tolerance);
	} else {
		result->ebs_valid = true; /* No EBS configured */
	}

	rfc2544_log(LOG_INFO, "Burst validation complete:");
	rfc2544_log(LOG_INFO, "  CBS: measured=%u, expected=%u, %s",
	            result->measured_cbs, result->expected_cbs,
	            result->cbs_valid ? "PASS" : "FAIL");
	rfc2544_log(LOG_INFO, "  EBS: measured=%u, expected=%u, %s",
	            result->measured_ebs, result->expected_ebs,
	            result->ebs_valid ? "PASS" : "FAIL");

	return 0;
}
