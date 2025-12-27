/*
 * multiport.c - Multi-Port RFC 2544 Testing Implementation
 *
 * Enables simultaneous testing across multiple network interfaces
 * for aggregated throughput measurement and multi-path testing.
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

/* Per-port test thread data */
typedef struct {
	rfc2544_ctx_t *ctx;
	port_config_t *port;
	throughput_result_t result;
	int status;
	int port_index;
} port_thread_data_t;

/**
 * Port test thread function
 */
static void *port_thread_func(void *arg)
{
	port_thread_data_t *data = (port_thread_data_t *)arg;

	rfc2544_log(LOG_INFO, "Port %d (%s): Starting throughput test",
	            data->port_index, data->port->interface);

	/* Run throughput test on this port */
	uint32_t count;
	data->status = rfc2544_throughput_test(data->ctx, data->ctx->config.frame_size,
	                                        &data->result, &count);

	if (data->status == 0) {
		rfc2544_log(LOG_INFO, "Port %d (%s): %.2f Mbps",
		            data->port_index, data->port->interface,
		            data->result.max_rate_mbps);
	} else {
		rfc2544_log(LOG_ERROR, "Port %d (%s): Test failed (%d)",
		            data->port_index, data->port->interface, data->status);
	}

	return NULL;
}

/**
 * Initialize multi-port test context
 */
int rfc2544_multiport_init(rfc2544_ctx_t *ctx, const multiport_config_t *config)
{
	if (!ctx || !config)
		return -EINVAL;

	if (config->port_count == 0 || config->port_count > MAX_TEST_PORTS)
		return -EINVAL;

	/* Store multi-port configuration */
	memcpy(&ctx->config.multiport, config, sizeof(multiport_config_t));

	rfc2544_log(LOG_INFO, "Multi-port test initialized with %u ports:",
	            config->port_count);

	for (uint32_t i = 0; i < config->port_count; i++) {
		if (config->ports[i].enabled) {
			rfc2544_log(LOG_INFO, "  Port %u: %s (rate %.1f%%)",
			            i, config->ports[i].interface, config->ports[i].rate_pct);
		}
	}

	return 0;
}

/**
 * Run multi-port throughput test
 */
int rfc2544_multiport_throughput(rfc2544_ctx_t *ctx, throughput_result_t *results)
{
	if (!ctx || !results)
		return -EINVAL;

	const multiport_config_t *config = &ctx->config.multiport;

	if (config->port_count == 0)
		return -EINVAL;

	rfc2544_log(LOG_INFO, "Starting multi-port throughput test on %u ports",
	            config->port_count);

	/* Allocate per-port contexts and thread data */
	pthread_t threads[MAX_TEST_PORTS];
	port_thread_data_t thread_data[MAX_TEST_PORTS];
	rfc2544_ctx_t *port_contexts[MAX_TEST_PORTS];
	int active_ports = 0;

	memset(thread_data, 0, sizeof(thread_data));
	memset(port_contexts, 0, sizeof(port_contexts));

	/* Initialize context for each enabled port */
	for (uint32_t i = 0; i < config->port_count; i++) {
		if (!config->ports[i].enabled)
			continue;

		/* Create separate context for this port */
		int ret = rfc2544_init(&port_contexts[i], config->ports[i].interface);
		if (ret < 0) {
			rfc2544_log(LOG_ERROR, "Failed to init port %u (%s): %d",
			            i, config->ports[i].interface, ret);
			continue;
		}

		/* Copy base configuration */
		memcpy(&port_contexts[i]->config, &ctx->config, sizeof(rfc2544_config_t));

		/* Set port-specific parameters */
		strncpy(port_contexts[i]->config.interface, config->ports[i].interface,
		        sizeof(port_contexts[i]->config.interface) - 1);

		/* Setup thread data */
		thread_data[i].ctx = port_contexts[i];
		thread_data[i].port = (port_config_t *)&config->ports[i];
		thread_data[i].port_index = i;

		active_ports++;
	}

	if (active_ports == 0) {
		rfc2544_log(LOG_ERROR, "No active ports for multi-port test");
		return -EINVAL;
	}

	/* Start all port threads */
	for (uint32_t i = 0; i < config->port_count; i++) {
		if (!port_contexts[i])
			continue;

		int ret = pthread_create(&threads[i], NULL, port_thread_func, &thread_data[i]);
		if (ret != 0) {
			rfc2544_log(LOG_ERROR, "Failed to create thread for port %u: %d", i, ret);
			thread_data[i].status = -ret;
		}
	}

	/* Wait for all threads to complete */
	for (uint32_t i = 0; i < config->port_count; i++) {
		if (!port_contexts[i])
			continue;

		pthread_join(threads[i], NULL);

		/* Copy result */
		memcpy(&results[i], &thread_data[i].result, sizeof(throughput_result_t));
	}

	/* Calculate aggregate statistics */
	double total_throughput = 0.0;
	uint64_t total_tx = 0;
	uint64_t total_rx = 0;
	int successful_ports = 0;

	for (uint32_t i = 0; i < config->port_count; i++) {
		if (!port_contexts[i] || thread_data[i].status < 0)
			continue;

		total_throughput += results[i].max_rate_mbps;
		total_tx += results[i].frames_tested;
		total_rx += results[i].frames_tested;
		successful_ports++;
	}

	rfc2544_log(LOG_INFO, "Multi-port test complete:");
	rfc2544_log(LOG_INFO, "  Successful ports: %d/%d", successful_ports, active_ports);
	rfc2544_log(LOG_INFO, "  Aggregate throughput: %.2f Mbps", total_throughput);
	rfc2544_log(LOG_INFO, "  Total TX: %lu, RX: %lu", total_tx, total_rx);

	/* Cleanup port contexts */
	for (uint32_t i = 0; i < config->port_count; i++) {
		if (port_contexts[i]) {
			rfc2544_cleanup(port_contexts[i]);
		}
	}

	return successful_ports > 0 ? 0 : -1;
}
