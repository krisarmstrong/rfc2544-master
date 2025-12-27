/*
 * bidir.c - Bidirectional RFC 2544 Testing Implementation
 *
 * Implements bidirectional throughput testing where traffic flows
 * in both directions simultaneously.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

/* Thread data for reverse direction */
typedef struct {
	rfc2544_ctx_t *ctx;
	double rate_pct;
	throughput_result_t result;
	int status;
} bidir_thread_data_t;

/**
 * Reverse direction thread function
 */
static void *reverse_thread_func(void *arg)
{
	bidir_thread_data_t *data = (bidir_thread_data_t *)arg;

	/* Run throughput test in reverse direction */
	uint32_t count;
	data->status = rfc2544_throughput_test(data->ctx, data->ctx->config.frame_size,
	                                        &data->result, &count);

	return NULL;
}

/**
 * Run bidirectional throughput test
 */
int rfc2544_bidir_throughput(rfc2544_ctx_t *ctx, bidir_mode_t mode,
                             double reverse_rate, bidir_result_t *result)
{
	if (!ctx || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	if (mode == BIDIR_NONE) {
		/* Unidirectional - just run normal test */
		uint32_t count;
		int ret = rfc2544_throughput_test(ctx, ctx->config.frame_size, &result->tx_result, &count);
		if (ret < 0)
			return ret;

		result->aggregate_mbps = result->tx_result.max_rate_mbps;
		return 0;
	}

	rfc2544_log(LOG_INFO, "Starting bidirectional throughput test (mode=%s)",
	            mode == BIDIR_SYMMETRIC ? "symmetric" : "asymmetric");

	/* For symmetric mode, use same rate both directions */
	if (mode == BIDIR_SYMMETRIC) {
		reverse_rate = 100.0; /* Same as forward */
	}

	/*
	 * Create a separate context for reverse direction
	 * In a real implementation, this would configure the remote end
	 * to generate traffic back to us.
	 *
	 * For now, we simulate by running two threads with the same context,
	 * which tests the local NIC's bidirectional capacity.
	 */
	bidir_thread_data_t reverse_data = {
		.ctx = ctx,
		.rate_pct = reverse_rate,
		.status = 0
	};

	pthread_t reverse_thread;
	int ret = pthread_create(&reverse_thread, NULL, reverse_thread_func, &reverse_data);
	if (ret != 0) {
		rfc2544_log(LOG_ERROR, "Failed to create reverse thread: %d", ret);
		return -ret;
	}

	/* Run forward direction in main thread */
	uint32_t count;
	ret = rfc2544_throughput_test(ctx, ctx->config.frame_size, &result->tx_result, &count);
	if (ret < 0) {
		rfc2544_log(LOG_WARN, "Forward direction test failed: %d", ret);
	}

	/* Wait for reverse direction to complete */
	pthread_join(reverse_thread, NULL);

	if (reverse_data.status < 0) {
		rfc2544_log(LOG_WARN, "Reverse direction test failed: %d", reverse_data.status);
	}

	/* Copy reverse results */
	memcpy(&result->rx_result, &reverse_data.result, sizeof(throughput_result_t));

	/* Calculate aggregate throughput */
	result->aggregate_mbps = result->tx_result.max_rate_mbps +
	                         result->rx_result.max_rate_mbps;

	rfc2544_log(LOG_INFO, "Bidirectional test complete:");
	rfc2544_log(LOG_INFO, "  TX: %.2f Mbps", result->tx_result.max_rate_mbps);
	rfc2544_log(LOG_INFO, "  RX: %.2f Mbps", result->rx_result.max_rate_mbps);
	rfc2544_log(LOG_INFO, "  Aggregate: %.2f Mbps", result->aggregate_mbps);

	return (ret < 0 || reverse_data.status < 0) ? -1 : 0;
}
