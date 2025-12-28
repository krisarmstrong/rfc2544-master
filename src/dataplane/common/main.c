/*
 * main.c - RFC 2544 Test Master CLI
 *
 * Command-line interface for RFC 2544 network benchmarking.
 */

#include "rfc2544.h"
#include "platform_config.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool g_running = true;
static rfc2544_ctx_t *g_ctx = NULL;

void signal_handler(int sig)
{
	(void)sig;
	g_running = false;
	if (g_ctx) {
		rfc2544_cancel(g_ctx);
	}
}

void progress_callback(const rfc2544_ctx_t *ctx, const char *message, double pct)
{
	(void)ctx;
	printf("\r[%5.1f%%] %-60s", pct, message);
	fflush(stdout);
}

void print_usage(const char *prog)
{
	fprintf(stderr, "RFC 2544 Network Benchmark Test Master v%d.%d.%d\n\n",
	        RFC2544_VERSION_MAJOR, RFC2544_VERSION_MINOR, RFC2544_VERSION_PATCH);
	fprintf(stderr, "Usage: %s <interface> [options]\n\n", prog);

	fprintf(stderr, "Test Selection:\n");
	fprintf(stderr, "  -t, --test TYPE     Test type: throughput, latency, loss, burst\n");
	fprintf(stderr, "                        throughput = RFC2544.26.1 (default)\n");
	fprintf(stderr, "                        latency    = RFC2544.26.2\n");
	fprintf(stderr, "                        loss       = RFC2544.26.3\n");
	fprintf(stderr, "                        burst      = RFC2544.26.4 (back-to-back)\n");

	fprintf(stderr, "\nFrame Size Options:\n");
	fprintf(stderr, "  -s, --size SIZE     Specific frame size (default: all standard)\n");
	fprintf(stderr, "  --jumbo             Include 9000 byte jumbo frames\n");
	fprintf(stderr, "  Standard sizes: 64, 128, 256, 512, 1024, 1280, 1518\n");

	fprintf(stderr, "\nTiming Options:\n");
	fprintf(stderr, "  -d, --duration SEC  Trial duration in seconds (default: 60)\n");
	fprintf(stderr, "  --warmup SEC        Warmup period in seconds (default: 2)\n");

	fprintf(stderr, "\nThroughput Test Options:\n");
	fprintf(stderr, "  --resolution PCT    Binary search resolution %% (default: 0.1)\n");
	fprintf(stderr, "  --max-iter N        Max binary search iterations (default: 20)\n");
	fprintf(stderr, "  --loss-tolerance    Acceptable frame loss %% (default: 0.0)\n");

	fprintf(stderr, "\nLatency Test Options:\n");
	fprintf(stderr, "  --samples N         Latency samples per trial (default: 1000)\n");

	fprintf(stderr, "\nOutput Options:\n");
	fprintf(stderr, "  -v, --verbose       Enable verbose logging\n");
	fprintf(stderr, "  --json              Output results in JSON format\n");
	fprintf(stderr, "  --csv               Output results in CSV format\n");

#if HAVE_DPDK
	fprintf(stderr, "\nDPDK Options (line-rate mode):\n");
	fprintf(stderr, "  --dpdk              Use DPDK for packet I/O\n");
	fprintf(stderr, "  --dpdk-args ARGS    Pass arguments to DPDK EAL\n");
#endif

	fprintf(stderr, "\nPlatform Options:\n");
	fprintf(stderr, "  --force-packet      Force AF_PACKET (for veth/testing)\n");

	fprintf(stderr, "\nGeneral:\n");
	fprintf(stderr, "  -h, --help          Show this help message\n");

	fprintf(stderr, "\nExamples:\n");
	fprintf(stderr, "  %s eth0 -t throughput          # Throughput test on eth0\n", prog);
	fprintf(stderr, "  %s eth0 -t latency -s 1518     # Latency test with 1518 byte frames\n",
	        prog);
	fprintf(stderr, "  %s eth0 -t loss --json         # Frame loss test with JSON output\n",
	        prog);
	fprintf(stderr, "  %s eth0 -t burst --jumbo       # Back-to-back test including jumbo\n",
	        prog);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	/* Check for help first */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	const char *interface = argv[1];
	rfc2544_config_t config;
	rfc2544_default_config(&config);
	strncpy(config.interface, interface, sizeof(config.interface) - 1);

	/* Parse options */
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			i++;
			if (strcmp(argv[i], "throughput") == 0) {
				config.test_type = TEST_THROUGHPUT;
			} else if (strcmp(argv[i], "latency") == 0) {
				config.test_type = TEST_LATENCY;
			} else if (strcmp(argv[i], "loss") == 0) {
				config.test_type = TEST_FRAME_LOSS;
			} else if (strcmp(argv[i], "burst") == 0) {
				config.test_type = TEST_BACK_TO_BACK;
			} else {
				fprintf(stderr, "Unknown test type: %s\n", argv[i]);
				return 1;
			}
		} else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.frame_size = atoi(argv[++i]);
			if (config.frame_size < 64 || config.frame_size > 9000) {
				fprintf(stderr, "Invalid frame size: %u (64-9000)\n", config.frame_size);
				return 1;
			}
		} else if (strcmp(argv[i], "--jumbo") == 0) {
			config.include_jumbo = true;
		} else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.trial_duration_sec = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--warmup") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.warmup_sec = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--resolution") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.resolution_pct = atof(argv[++i]);
		} else if (strcmp(argv[i], "--max-iter") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.max_iterations = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--loss-tolerance") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.acceptable_loss = atof(argv[++i]);
		} else if (strcmp(argv[i], "--samples") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.latency_samples = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			config.verbose = true;
			rfc2544_set_log_level(LOG_DEBUG);
		} else if (strcmp(argv[i], "--json") == 0) {
			config.output_format = STATS_FORMAT_JSON;
		} else if (strcmp(argv[i], "--csv") == 0) {
			config.output_format = STATS_FORMAT_CSV;
#if HAVE_DPDK
		} else if (strcmp(argv[i], "--dpdk") == 0) {
			config.use_dpdk = true;
		} else if (strcmp(argv[i], "--dpdk-args") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Missing value for %s\n", argv[i]);
				return 1;
			}
			config.dpdk_args = argv[++i];
#endif
		} else if (strcmp(argv[i], "--force-packet") == 0) {
			config.force_packet = true;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			print_usage(argv[0]);
			return 1;
		}
	}

	/* Setup signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Initialize context */
	if (rfc2544_init(&g_ctx, interface) < 0) {
		fprintf(stderr, "Failed to initialize RFC2544 context\n");
		return 1;
	}

	/* Configure */
	if (rfc2544_configure(g_ctx, &config) < 0) {
		fprintf(stderr, "Failed to configure test\n");
		rfc2544_cleanup(g_ctx);
		return 1;
	}

	/* Set progress callback for text output */
	if (config.output_format == STATS_FORMAT_TEXT) {
		rfc2544_set_progress_callback(g_ctx, progress_callback);
	}

	/* Print test info */
	const char *test_names[] = {"Throughput", "Latency", "Frame Loss", "Back-to-Back"};
	printf("RFC 2544 Test Master v%d.%d.%d\n", RFC2544_VERSION_MAJOR, RFC2544_VERSION_MINOR,
	       RFC2544_VERSION_PATCH);
	printf("Interface: %s\n", interface);
	printf("Test: %s\n", test_names[config.test_type]);
	if (config.frame_size > 0) {
		printf("Frame size: %u bytes\n", config.frame_size);
	} else {
		printf("Frame sizes: 64, 128, 256, 512, 1024, 1280, 1518%s\n",
		       config.include_jumbo ? ", 9000" : "");
	}
	printf("Trial duration: %u seconds\n", config.trial_duration_sec);
	printf("\nPress Ctrl-C to cancel\n\n");

	/* Run test */
	int ret = rfc2544_run(g_ctx);

	printf("\n");

	/* Print results */
	if (rfc2544_get_state(g_ctx) == STATE_COMPLETED) {
		rfc2544_print_results(g_ctx);
	}

	/* Cleanup */
	rfc2544_cleanup(g_ctx);

	return (ret < 0) ? 1 : 0;
}
