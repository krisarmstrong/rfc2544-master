/*
 * pacing.c - Rate Control and Packet Pacing
 *
 * Implements software-based rate limiting for accurate RFC 2544 testing.
 * Uses high-resolution timing to achieve precise packet rates.
 */

#include "rfc2544.h"
#include "platform_config.h"

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Pacing Context
 * ============================================================================ */

struct pacing_ctx {
	/* Target rate */
	uint64_t line_rate_bps;   /* Original line rate (for rate changes) */
	uint64_t target_pps;      /* Target packets per second */
	uint64_t target_bps;      /* Target bits per second */
	uint32_t frame_size;      /* Frame size for rate calculation */

	/* Timing */
	uint64_t interval_ns;     /* Nanoseconds between packets */
	uint64_t next_tx_ns;      /* Next allowed TX time */
	uint64_t start_ns;        /* Start timestamp */

	/* Burst control */
	uint32_t batch_size;      /* Packets per batch */
	uint32_t batch_interval_ns; /* Time per batch */

	/* Statistics */
	uint64_t packets_sent;
	uint64_t bytes_sent;
	uint64_t pacing_delays;   /* Number of times we had to wait */
	uint64_t overruns;        /* Number of times we fell behind */

	/* Mode */
	bool enabled;
	bool use_busy_wait;       /* Use busy-wait for precision */
};
typedef struct pacing_ctx pacing_ctx_t;

/* ============================================================================
 * High-Resolution Timing
 * ============================================================================ */

static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

/* Busy-wait until target time (high precision but CPU intensive) */
static inline void busy_wait_until(uint64_t target_ns)
{
	while (get_time_ns() < target_ns) {
		/* Spin - could add pause instruction for power efficiency */
		__asm__ volatile("" ::: "memory");
	}
}

/* Sleep-based wait (lower precision but CPU friendly) */
static inline void sleep_wait_until(uint64_t target_ns)
{
	uint64_t now = get_time_ns();
	if (now >= target_ns)
		return;

	uint64_t delta = target_ns - now;

	/* Sleep for most of the time, then busy-wait for precision */
	if (delta > 50000) { /* > 50us */
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = delta - 10000; /* Sleep, leaving 10us margin */
		nanosleep(&ts, NULL);
	}

	/* Busy-wait for final precision */
	busy_wait_until(target_ns);
}

/* ============================================================================
 * Pacing API
 * ============================================================================ */

/**
 * Create pacing context
 *
 * @param line_rate_bps Line rate in bits per second
 * @param frame_size Frame size in bytes
 * @param rate_pct Target rate as percentage of line rate
 * @return Pacing context, or NULL on error
 */
pacing_ctx_t *pacing_create(uint64_t line_rate_bps, uint32_t frame_size, double rate_pct)
{
	pacing_ctx_t *ctx = calloc(1, sizeof(pacing_ctx_t));
	if (!ctx)
		return NULL;

	ctx->line_rate_bps = line_rate_bps; /* Store for rate changes */
	ctx->frame_size = frame_size;
	ctx->enabled = true;
	ctx->use_busy_wait = false; /* Default to sleep-based for CPU efficiency */
	ctx->batch_size = 1;

	/* Calculate wire size (frame + preamble + IFG) */
	uint32_t wire_size = frame_size + 20; /* 8 preamble + 12 IFG */

	/* Calculate target rates */
	ctx->target_bps = (uint64_t)(line_rate_bps * rate_pct / 100.0);
	ctx->target_pps = ctx->target_bps / (wire_size * 8);

	/* Calculate inter-packet interval */
	if (ctx->target_pps > 0) {
		ctx->interval_ns = NS_PER_SEC / ctx->target_pps;
	} else {
		ctx->interval_ns = NS_PER_SEC; /* 1 pps minimum */
	}

	/* Initialize timing */
	ctx->start_ns = get_time_ns();
	ctx->next_tx_ns = ctx->start_ns;

	return ctx;
}

/**
 * Update target rate
 *
 * @param ctx Pacing context
 * @param rate_pct New rate as percentage of line rate
 */
void pacing_set_rate(pacing_ctx_t *ctx, double rate_pct)
{
	if (!ctx || rate_pct <= 0.0 || rate_pct > 100.0)
		return;

	uint32_t wire_size = ctx->frame_size + 20;

	/* Recalculate target rates from stored line rate */
	ctx->target_bps = (uint64_t)(ctx->line_rate_bps * rate_pct / 100.0);
	ctx->target_pps = ctx->target_bps / (wire_size * 8);

	if (ctx->target_pps > 0) {
		ctx->interval_ns = NS_PER_SEC / ctx->target_pps;
	} else {
		ctx->interval_ns = NS_PER_SEC; /* 1 pps minimum */
	}
}

/**
 * Set batch size for bulk transmission
 *
 * @param ctx Pacing context
 * @param batch_size Number of packets per batch
 */
void pacing_set_batch_size(pacing_ctx_t *ctx, uint32_t batch_size)
{
	if (!ctx || batch_size == 0)
		return;

	ctx->batch_size = batch_size;
	ctx->batch_interval_ns = ctx->interval_ns * batch_size;
}

/**
 * Enable/disable busy-wait mode
 *
 * @param ctx Pacing context
 * @param enable true for busy-wait (high precision), false for sleep-based
 */
void pacing_set_busy_wait(pacing_ctx_t *ctx, bool enable)
{
	if (ctx)
		ctx->use_busy_wait = enable;
}

/**
 * Wait until it's time to send the next packet
 *
 * @param ctx Pacing context
 * @return Current timestamp in nanoseconds
 */
uint64_t pacing_wait(pacing_ctx_t *ctx)
{
	if (!ctx || !ctx->enabled)
		return get_time_ns();

	uint64_t now = get_time_ns();

	if (now < ctx->next_tx_ns) {
		/* Need to wait */
		ctx->pacing_delays++;
		if (ctx->use_busy_wait) {
			busy_wait_until(ctx->next_tx_ns);
		} else {
			sleep_wait_until(ctx->next_tx_ns);
		}
	} else if (now > ctx->next_tx_ns + ctx->interval_ns * 10) {
		/* Fell behind by more than 10 packets - reset */
		ctx->overruns++;
		ctx->next_tx_ns = now;
	}

	/* Update next TX time */
	ctx->next_tx_ns += ctx->interval_ns;

	return get_time_ns();
}

/**
 * Wait until it's time to send the next batch
 *
 * @param ctx Pacing context
 * @param batch_size Number of packets in batch
 * @return Current timestamp in nanoseconds
 */
uint64_t pacing_wait_batch(pacing_ctx_t *ctx, uint32_t batch_size)
{
	if (!ctx || !ctx->enabled)
		return get_time_ns();

	uint64_t now = get_time_ns();
	uint64_t batch_interval = ctx->interval_ns * batch_size;

	if (now < ctx->next_tx_ns) {
		ctx->pacing_delays++;
		if (ctx->use_busy_wait) {
			busy_wait_until(ctx->next_tx_ns);
		} else {
			sleep_wait_until(ctx->next_tx_ns);
		}
	} else if (now > ctx->next_tx_ns + batch_interval * 10) {
		ctx->overruns++;
		ctx->next_tx_ns = now;
	}

	ctx->next_tx_ns += batch_interval;

	return get_time_ns();
}

/**
 * Record that packets were sent (for statistics)
 *
 * @param ctx Pacing context
 * @param packets Number of packets sent
 * @param bytes Number of bytes sent
 */
void pacing_record_tx(pacing_ctx_t *ctx, uint32_t packets, uint32_t bytes)
{
	if (!ctx)
		return;

	ctx->packets_sent += packets;
	ctx->bytes_sent += bytes;
}

/**
 * Get current achieved rate
 *
 * @param ctx Pacing context
 * @param pps Output: packets per second
 * @param mbps Output: megabits per second
 */
void pacing_get_rate(const pacing_ctx_t *ctx, double *pps, double *mbps)
{
	if (!ctx) {
		if (pps)
			*pps = 0;
		if (mbps)
			*mbps = 0;
		return;
	}

	uint64_t now = get_time_ns();
	double elapsed = (double)(now - ctx->start_ns) / NS_PER_SEC;

	if (elapsed > 0) {
		if (pps)
			*pps = ctx->packets_sent / elapsed;
		if (mbps)
			*mbps = (ctx->bytes_sent * 8.0) / (elapsed * 1e6);
	}
}

/**
 * Get pacing statistics
 *
 * @param ctx Pacing context
 * @param delays Output: number of pacing delays
 * @param overruns Output: number of overruns (fell behind)
 */
void pacing_get_stats(const pacing_ctx_t *ctx, uint64_t *delays, uint64_t *overruns)
{
	if (!ctx)
		return;

	if (delays)
		*delays = ctx->pacing_delays;
	if (overruns)
		*overruns = ctx->overruns;
}

/**
 * Reset pacing context for new test
 *
 * @param ctx Pacing context
 */
void pacing_reset(pacing_ctx_t *ctx)
{
	if (!ctx)
		return;

	ctx->start_ns = get_time_ns();
	ctx->next_tx_ns = ctx->start_ns;
	ctx->packets_sent = 0;
	ctx->bytes_sent = 0;
	ctx->pacing_delays = 0;
	ctx->overruns = 0;
}

/**
 * Destroy pacing context
 *
 * @param ctx Pacing context
 */
void pacing_destroy(pacing_ctx_t *ctx)
{
	free(ctx);
}

/* ============================================================================
 * Rate Calculator
 * ============================================================================ */

/**
 * Calculate maximum theoretical packet rate
 *
 * @param line_rate_bps Line rate in bits per second
 * @param frame_size Frame size in bytes
 * @return Maximum packets per second
 */
uint64_t calc_max_pps(uint64_t line_rate_bps, uint32_t frame_size)
{
	/* Wire size = frame + preamble (8) + SFD (in preamble) + IFG (12) */
	uint32_t wire_size = frame_size + 20;
	return line_rate_bps / (wire_size * 8);
}

/**
 * Calculate line rate utilization
 *
 * @param achieved_pps Achieved packets per second
 * @param frame_size Frame size in bytes
 * @param line_rate_bps Line rate in bits per second
 * @return Utilization as percentage (0-100)
 */
double calc_utilization(uint64_t achieved_pps, uint32_t frame_size, uint64_t line_rate_bps)
{
	/* Protect against division by zero */
	if (line_rate_bps == 0)
		return 0.0;

	uint32_t wire_size = frame_size + 20;
	uint64_t achieved_bps = achieved_pps * wire_size * 8;
	return 100.0 * achieved_bps / line_rate_bps;
}

/* ============================================================================
 * Trial Timer
 * ============================================================================ */

struct trial_timer {
	uint64_t start_ns;
	uint64_t duration_ns;
	uint64_t warmup_ns;
	bool in_warmup;
	bool expired;
};
typedef struct trial_timer trial_timer_t;

/**
 * Create trial timer
 *
 * @param duration_sec Trial duration in seconds
 * @param warmup_sec Warmup period in seconds
 * @return Timer, or NULL on error
 */
trial_timer_t *trial_timer_create(uint32_t duration_sec, uint32_t warmup_sec)
{
	trial_timer_t *timer = calloc(1, sizeof(trial_timer_t));
	if (!timer)
		return NULL;

	timer->duration_ns = (uint64_t)duration_sec * NS_PER_SEC;
	timer->warmup_ns = (uint64_t)warmup_sec * NS_PER_SEC;
	timer->in_warmup = (warmup_sec > 0);
	timer->expired = false;

	return timer;
}

/**
 * Start the timer
 *
 * @param timer Trial timer
 */
void trial_timer_start(trial_timer_t *timer)
{
	if (!timer)
		return;

	timer->start_ns = get_time_ns();
	timer->in_warmup = (timer->warmup_ns > 0);
	timer->expired = false;
}

/**
 * Check timer status
 *
 * @param timer Trial timer
 * @return true if trial time has expired
 */
bool trial_timer_expired(trial_timer_t *timer)
{
	if (!timer || timer->expired)
		return true;

	uint64_t elapsed = get_time_ns() - timer->start_ns;

	/* Check if warmup period ended */
	if (timer->in_warmup && elapsed >= timer->warmup_ns) {
		timer->in_warmup = false;
	}

	/* Check if trial expired */
	if (elapsed >= timer->warmup_ns + timer->duration_ns) {
		timer->expired = true;
		return true;
	}

	return false;
}

/**
 * Check if still in warmup period
 *
 * @param timer Trial timer
 * @return true if in warmup period
 */
bool trial_timer_in_warmup(const trial_timer_t *timer)
{
	return timer ? timer->in_warmup : false;
}

/**
 * Get elapsed time in seconds
 *
 * @param timer Trial timer
 * @return Elapsed seconds (excluding warmup)
 */
double trial_timer_elapsed(const trial_timer_t *timer)
{
	if (!timer)
		return 0;

	uint64_t elapsed = get_time_ns() - timer->start_ns;

	if (elapsed <= timer->warmup_ns) {
		return 0;
	}

	return (double)(elapsed - timer->warmup_ns) / NS_PER_SEC;
}

/**
 * Destroy timer
 *
 * @param timer Trial timer
 */
void trial_timer_destroy(trial_timer_t *timer)
{
	free(timer);
}
