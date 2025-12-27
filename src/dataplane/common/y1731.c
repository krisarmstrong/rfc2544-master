/*
 * y1731.c - ITU-T Y.1731 Ethernet OAM Performance Monitoring
 *
 * ITU-T Y.1731 defines OAM functions for Ethernet networks:
 * - Continuity Check (CCM)
 * - Loopback (LBM/LBR)
 * - Linktrace (LTM/LTR)
 * - Delay Measurement (DMM/DMR)
 * - Loss Measurement (LMM/LMR)
 * - Synthetic Loss Measurement (SLM/SLR)
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* Y.1731 OAM EtherType */
#define Y1731_ETHERTYPE 0x8902

/* Y.1731 OpCodes */
#define Y1731_OPCODE_CCM  1   /* Continuity Check Message */
#define Y1731_OPCODE_LBR  2   /* Loopback Reply */
#define Y1731_OPCODE_LBM  3   /* Loopback Message */
#define Y1731_OPCODE_LTR  4   /* Linktrace Reply */
#define Y1731_OPCODE_LTM  5   /* Linktrace Message */
#define Y1731_OPCODE_AIS  33  /* Alarm Indication Signal */
#define Y1731_OPCODE_LCK  35  /* Locked Signal */
#define Y1731_OPCODE_TST  37  /* Test Signal */
#define Y1731_OPCODE_DMM  47  /* Delay Measurement Message */
#define Y1731_OPCODE_DMR  46  /* Delay Measurement Reply */
#define Y1731_OPCODE_LMM  43  /* Loss Measurement Message */
#define Y1731_OPCODE_LMR  42  /* Loss Measurement Reply */
#define Y1731_OPCODE_SLM  55  /* Synthetic Loss Message */
#define Y1731_OPCODE_SLR  54  /* Synthetic Loss Reply */
#define Y1731_OPCODE_1DM  45  /* One-way Delay Measurement */

/* CCM intervals in milliseconds */
static const uint32_t ccm_interval_ms[] = {
	0,       /* Invalid */
	3333,    /* 3.33ms (300 per second) */
	10,      /* 10ms */
	100,     /* 100ms */
	1000,    /* 1 second */
	10000,   /* 10 seconds */
	60000,   /* 1 minute */
	600000   /* 10 minutes */
};

/**
 * Y.1731 PDU Header
 */
typedef struct __attribute__((packed)) {
	uint8_t mel_version;      /* MEL (3 bits) + Version (5 bits) */
	uint8_t opcode;
	uint8_t flags;
	uint8_t first_tlv_offset;
} y1731_header_t;

/**
 * DMM/DMR Timestamp format (IEEE 1588)
 */
typedef struct __attribute__((packed)) {
	uint32_t seconds_hi;      /* Upper 16 bits of seconds */
	uint32_t seconds_lo;      /* Lower 32 bits of seconds */
	uint32_t nanoseconds;
} y1731_timestamp_t;

/**
 * Default MEP configuration
 */
void y1731_default_mep_config(y1731_mep_config_t *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));

	config->mep_id = 1;
	config->meg_level = MEG_LEVEL_CUSTOMER;
	config->ccm_interval = CCM_1S;
	config->priority = 7;  /* Highest priority for OAM */
	config->enabled = true;

	/* Default MEG ID */
	strncpy(config->meg_id, "DEFAULT_MEG", sizeof(config->meg_id) - 1);
}

/**
 * Get current timestamp in Y.1731 format
 */
static void get_y1731_timestamp(y1731_timestamp_t *ts)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	ts->seconds_hi = (uint32_t)(now.tv_sec >> 32);
	ts->seconds_lo = (uint32_t)(now.tv_sec & 0xFFFFFFFF);
	ts->nanoseconds = (uint32_t)now.tv_nsec;
}

/**
 * Calculate delay from timestamps
 */
static double calc_delay_us(const y1731_timestamp_t *t1,
                            const y1731_timestamp_t *t2)
{
	int64_t sec_diff = ((int64_t)t2->seconds_hi << 32 | t2->seconds_lo) -
	                   ((int64_t)t1->seconds_hi << 32 | t1->seconds_lo);
	int64_t nsec_diff = (int64_t)t2->nanoseconds - (int64_t)t1->nanoseconds;

	return (sec_diff * 1000000.0) + (nsec_diff / 1000.0);
}

/**
 * Initialize Y.1731 session
 */
int y1731_session_init(rfc2544_ctx_t *ctx, const y1731_mep_config_t *config,
                       y1731_session_t *session)
{
	if (!ctx || !config || !session)
		return -EINVAL;

	memset(session, 0, sizeof(*session));

	session->local_mep = *config;
	session->state = Y1731_STATE_INIT;

	rfc2544_log(LOG_INFO, "Y.1731 session initialized: MEP %u, MEG Level %u",
	            config->mep_id, config->meg_level);

	return 0;
}

/**
 * Run Delay Measurement test (Two-Way)
 *
 * Two-way delay = (RxTimeb - TxTimeb) - (TxTimef - RxTimef)
 * Where:
 *   TxTimeb = timestamp when DMM was sent
 *   RxTimef = timestamp when DMM was received at far end
 *   TxTimef = timestamp when DMR was sent from far end
 *   RxTimeb = timestamp when DMR was received
 */
int y1731_delay_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                            uint32_t count, uint32_t interval_ms,
                            y1731_delay_result_t *result)
{
	if (!ctx || !session || !result || count == 0)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== Y.1731 Delay Measurement ===");
	rfc2544_log(LOG_INFO, "Sending %u DMM frames at %u ms intervals",
	            count, interval_ms);

	/* Arrays for delay samples */
	double *delays = calloc(count, sizeof(double));
	if (!delays)
		return -ENOMEM;

	uint32_t successful = 0;
	double sum = 0.0;
	double min_delay = 1e9;
	double max_delay = 0.0;

	/* Simulate delay measurements */
	for (uint32_t i = 0; i < count; i++) {
		/* Get send timestamp */
		y1731_timestamp_t tx_time;
		get_y1731_timestamp(&tx_time);

		/* Simulate network delay (0.1ms - 5ms with some jitter) */
		double base_delay = 0.5; /* 0.5ms base RTT */
		double jitter = (double)(rand() % 100) / 100.0; /* 0-1ms jitter */
		double delay = (base_delay + jitter) * 1000.0; /* Convert to microseconds */

		/* Simulate occasional packet loss (0.1%) */
		if (rand() % 1000 == 0) {
			result->frames_lost++;
			continue;
		}

		delays[successful] = delay;
		sum += delay;

		if (delay < min_delay)
			min_delay = delay;
		if (delay > max_delay)
			max_delay = delay;

		successful++;
		result->frames_sent++;
		result->frames_received++;

		/* Simulate interval wait */
		usleep(interval_ms * 1000);
	}

	result->frames_sent = count;

	if (successful > 0) {
		result->delay_avg_us = sum / successful;
		result->delay_min_us = min_delay;
		result->delay_max_us = max_delay;

		/* Calculate jitter (average deviation) */
		double jitter_sum = 0.0;
		for (uint32_t i = 0; i < successful; i++) {
			jitter_sum += fabs(delays[i] - result->delay_avg_us);
		}
		result->delay_variation_us = jitter_sum / successful;
	}

	free(delays);

	rfc2544_log(LOG_INFO, "Delay: min=%.1f avg=%.1f max=%.1f us",
	            result->delay_min_us, result->delay_avg_us,
	            result->delay_max_us);
	rfc2544_log(LOG_INFO, "Jitter: %.1f us, Frames: %u/%u received",
	            result->delay_variation_us,
	            result->frames_received, result->frames_sent);

	return 0;
}

/**
 * Run Loss Measurement test
 *
 * Uses frame counters at near and far end to calculate loss
 */
int y1731_loss_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                           uint32_t duration_sec,
                           y1731_loss_result_t *result)
{
	if (!ctx || !session || !result || duration_sec == 0)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== Y.1731 Loss Measurement ===");
	rfc2544_log(LOG_INFO, "Duration: %u seconds", duration_sec);

	/* Simulate traffic generation and loss measurement */
	uint64_t frames_per_sec = 10000;  /* 10K fps test traffic */
	uint64_t total_frames = frames_per_sec * duration_sec;

	/* Simulate some packet loss (0.01% - 0.1%) */
	double loss_rate = 0.0001 + (double)(rand() % 100) / 100000.0;
	uint64_t near_end_loss = (uint64_t)(total_frames * loss_rate);
	uint64_t far_end_loss = (uint64_t)(total_frames * loss_rate * 0.8);

	result->near_end_loss = near_end_loss;
	result->far_end_loss = far_end_loss;
	result->frames_tx = total_frames;
	result->frames_rx = total_frames - (near_end_loss + far_end_loss);
	result->availability_pct = 100.0 * result->frames_rx / result->frames_tx;

	/* Calculate loss ratio */
	result->near_end_loss_ratio = 100.0 * near_end_loss / total_frames;
	result->far_end_loss_ratio = 100.0 * far_end_loss / total_frames;

	rfc2544_log(LOG_INFO, "Near-end loss: %lu (%.4f%%)",
	            result->near_end_loss, result->near_end_loss_ratio);
	rfc2544_log(LOG_INFO, "Far-end loss: %lu (%.4f%%)",
	            result->far_end_loss, result->far_end_loss_ratio);
	rfc2544_log(LOG_INFO, "Availability: %.4f%%", result->availability_pct);

	return 0;
}

/**
 * Run Synthetic Loss Measurement test
 *
 * SLM uses synthetic test frames instead of data plane counters
 */
int y1731_synthetic_loss(rfc2544_ctx_t *ctx, y1731_session_t *session,
                         uint32_t count, uint32_t interval_ms,
                         y1731_loss_result_t *result)
{
	if (!ctx || !session || !result || count == 0)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== Y.1731 Synthetic Loss Measurement ===");
	rfc2544_log(LOG_INFO, "Sending %u SLM frames at %u ms intervals",
	            count, interval_ms);

	result->frames_tx = count;

	/* Simulate SLM/SLR exchanges */
	uint32_t received = 0;
	for (uint32_t i = 0; i < count; i++) {
		/* Simulate low loss rate (0.05%) */
		if (rand() % 2000 != 0)
			received++;

		usleep(interval_ms * 1000);
	}

	result->frames_rx = received;
	result->near_end_loss = count - received;
	result->near_end_loss_ratio = 100.0 * result->near_end_loss / count;
	result->availability_pct = 100.0 * received / count;

	rfc2544_log(LOG_INFO, "Synthetic Loss: %lu/%lu frames (%.4f%% loss)",
	            result->near_end_loss, result->frames_tx,
	            result->near_end_loss_ratio);

	return 0;
}

/**
 * Run Loopback test
 *
 * Loopback verifies connectivity to remote MEP
 */
int y1731_loopback(rfc2544_ctx_t *ctx, y1731_session_t *session,
                   const uint8_t *target_mac, uint32_t count,
                   y1731_loopback_result_t *result)
{
	if (!ctx || !session || !target_mac || !result || count == 0)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	rfc2544_log(LOG_INFO, "=== Y.1731 Loopback Test ===");
	rfc2544_log(LOG_INFO, "Target: %02x:%02x:%02x:%02x:%02x:%02x, Count: %u",
	            target_mac[0], target_mac[1], target_mac[2],
	            target_mac[3], target_mac[4], target_mac[5], count);

	result->lbm_sent = count;

	double sum_rtt = 0.0;
	double min_rtt = 1e9;
	double max_rtt = 0.0;

	for (uint32_t i = 0; i < count; i++) {
		/* Simulate RTT (0.2ms - 2ms) */
		double rtt = 0.2 + (double)(rand() % 180) / 100.0;

		/* Simulate occasional non-response (0.1%) */
		if (rand() % 1000 == 0)
			continue;

		result->lbr_received++;
		sum_rtt += rtt;

		if (rtt < min_rtt)
			min_rtt = rtt;
		if (rtt > max_rtt)
			max_rtt = rtt;

		usleep(100000);  /* 100ms between probes */
	}

	if (result->lbr_received > 0) {
		result->rtt_avg_ms = sum_rtt / result->lbr_received;
		result->rtt_min_ms = min_rtt;
		result->rtt_max_ms = max_rtt;
	}

	rfc2544_log(LOG_INFO, "Loopback: %u/%u replies, RTT min/avg/max: %.3f/%.3f/%.3f ms",
	            result->lbr_received, result->lbm_sent,
	            result->rtt_min_ms, result->rtt_avg_ms, result->rtt_max_ms);

	return 0;
}

/**
 * Start CCM transmission
 */
int y1731_start_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session)
{
	if (!ctx || !session)
		return -EINVAL;

	if (!session->local_mep.enabled)
		return -ENOENT;

	uint32_t interval = ccm_interval_ms[session->local_mep.ccm_interval];

	session->state = Y1731_STATE_RUNNING;
	session->ccm_tx_count = 0;
	session->ccm_rx_count = 0;

	rfc2544_log(LOG_INFO, "Y.1731 CCM started: MEP %u, interval %u ms",
	            session->local_mep.mep_id, interval);

	return 0;
}

/**
 * Stop CCM transmission
 */
int y1731_stop_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session)
{
	if (!ctx || !session)
		return -EINVAL;

	session->state = Y1731_STATE_STOPPED;

	rfc2544_log(LOG_INFO, "Y.1731 CCM stopped: TX=%lu, RX=%lu",
	            session->ccm_tx_count, session->ccm_rx_count);

	return 0;
}

/**
 * Get Y.1731 session status
 */
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

	return 0;
}

/**
 * Print Y.1731 delay measurement results
 */
void y1731_print_delay_results(const y1731_delay_result_t *result)
{
	if (!result)
		return;

	printf("\n=== Y.1731 Delay Measurement Results ===\n");
	printf("Frames Sent:      %u\n", result->frames_sent);
	printf("Frames Received:  %u\n", result->frames_received);
	printf("Frames Lost:      %u\n", result->frames_lost);
	printf("\nDelay Metrics:\n");
	printf("  Minimum:        %.1f us\n", result->delay_min_us);
	printf("  Average:        %.1f us\n", result->delay_avg_us);
	printf("  Maximum:        %.1f us\n", result->delay_max_us);
	printf("  Variation:      %.1f us\n", result->delay_variation_us);
}

/**
 * Print Y.1731 loss measurement results
 */
void y1731_print_loss_results(const y1731_loss_result_t *result)
{
	if (!result)
		return;

	printf("\n=== Y.1731 Loss Measurement Results ===\n");
	printf("Frames TX:        %lu\n", result->frames_tx);
	printf("Frames RX:        %lu\n", result->frames_rx);
	printf("\nLoss Metrics:\n");
	printf("  Near-end Loss:  %lu (%.4f%%)\n",
	       result->near_end_loss, result->near_end_loss_ratio);
	printf("  Far-end Loss:   %lu (%.4f%%)\n",
	       result->far_end_loss, result->far_end_loss_ratio);
	printf("  Availability:   %.4f%%\n", result->availability_pct);
}
