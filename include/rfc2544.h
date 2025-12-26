/*
 * rfc2544.h - RFC 2544 Network Benchmark Test Master API
 *
 * RFC 2544: Benchmarking Methodology for Network Interconnect Devices
 * https://www.rfc-editor.org/rfc/rfc2544
 *
 * This implementation supports:
 * - Section 26.1: Throughput (binary search for max rate with 0% loss)
 * - Section 26.2: Latency (round-trip time at various loads)
 * - Section 26.3: Frame Loss Rate (loss percentage vs offered load)
 * - Section 26.4: Back-to-Back Frames (burst capacity)
 *
 * Standard frame sizes: 64, 128, 256, 512, 1024, 1280, 1518 bytes
 * Optional: 9000 bytes (jumbo frames)
 */

#ifndef RFC2544_H
#define RFC2544_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Version */
#define RFC2544_VERSION_MAJOR 1
#define RFC2544_VERSION_MINOR 0
#define RFC2544_VERSION_PATCH 0

/* Signature for custom RFC2544 packets - 7 bytes like ITO */
#define RFC2544_SIGNATURE "RFC2544"
#define RFC2544_SIG_LEN 7

/* Standard RFC 2544 frame sizes (Section 9.1) */
typedef enum {
	FRAME_SIZE_64 = 64,
	FRAME_SIZE_128 = 128,
	FRAME_SIZE_256 = 256,
	FRAME_SIZE_512 = 512,
	FRAME_SIZE_1024 = 1024,
	FRAME_SIZE_1280 = 1280,
	FRAME_SIZE_1518 = 1518,
	FRAME_SIZE_9000 = 9000 /* Jumbo (optional) */
} frame_size_t;

/* Standard frame sizes array */
#define RFC2544_FRAME_SIZES                                                                        \
	{64, 128, 256, 512, 1024, 1280, 1518}
#define RFC2544_FRAME_SIZE_COUNT 7

/* Test types */
typedef enum {
	TEST_THROUGHPUT = 0,   /* RFC2544.26.1 - Binary search for max throughput */
	TEST_LATENCY = 1,      /* RFC2544.26.2 - Round-trip latency */
	TEST_FRAME_LOSS = 2,   /* RFC2544.26.3 - Frame loss rate */
	TEST_BACK_TO_BACK = 3, /* RFC2544.26.4 - Burst capacity */
	TEST_COUNT = 4
} test_type_t;

/* Test state */
typedef enum {
	STATE_IDLE = 0,
	STATE_RUNNING = 1,
	STATE_COMPLETED = 2,
	STATE_FAILED = 3,
	STATE_CANCELLED = 4
} test_state_t;

/* Log levels */
typedef enum { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 } log_level_t;

/* Stats output format */
typedef enum {
	STATS_FORMAT_TEXT = 0,
	STATS_FORMAT_JSON = 1,
	STATS_FORMAT_CSV = 2
} stats_format_t;

/* Latency statistics */
typedef struct {
	uint64_t count;   /* Number of measurements */
	double min_ns;    /* Minimum latency in nanoseconds */
	double max_ns;    /* Maximum latency in nanoseconds */
	double avg_ns;    /* Average latency in nanoseconds */
	double jitter_ns; /* Jitter (variation) in nanoseconds */
	double p50_ns;    /* 50th percentile */
	double p95_ns;    /* 95th percentile */
	double p99_ns;    /* 99th percentile */
} latency_stats_t;

/* Frame loss result for a single load level */
typedef struct {
	double offered_rate_pct; /* Offered load as % of line rate */
	double actual_rate_mbps; /* Actual offered rate in Mbps */
	uint64_t frames_sent;    /* Frames transmitted */
	uint64_t frames_recv;    /* Frames received */
	double loss_pct;         /* Frame loss percentage */
} frame_loss_point_t;

/* Throughput test result for a single frame size */
typedef struct {
	uint32_t frame_size;     /* Frame size tested */
	double max_rate_pct;     /* Maximum throughput as % of line rate */
	double max_rate_mbps;    /* Maximum throughput in Mbps */
	double max_rate_pps;     /* Maximum throughput in packets/sec */
	uint64_t frames_tested;  /* Total frames transmitted */
	uint32_t iterations;     /* Binary search iterations */
	latency_stats_t latency; /* Latency at max throughput */
} throughput_result_t;

/* Latency test result for a single load level */
typedef struct {
	uint32_t frame_size;     /* Frame size tested */
	double offered_rate_pct; /* Offered load as % of line rate */
	latency_stats_t latency; /* Latency statistics */
} latency_result_t;

/* Back-to-back test result */
typedef struct {
	uint32_t frame_size;   /* Frame size tested */
	uint64_t max_burst;    /* Maximum burst length with 0% loss */
	double burst_duration; /* Burst duration in microseconds */
	uint32_t trials;       /* Number of trials performed */
} burst_result_t;

/* Test configuration */
typedef struct {
	/* Interface */
	char interface[64];   /* Network interface name */
	uint64_t line_rate;   /* Line rate in bits/sec (e.g., 10e9 for 10G) */
	bool auto_detect_nic; /* Auto-detect NIC capabilities */

	/* Test parameters */
	test_type_t test_type;       /* Test to run */
	uint32_t frame_size;         /* Specific frame size (0 = all standard sizes) */
	bool include_jumbo;          /* Include 9000 byte jumbo frames */
	uint32_t trial_duration_sec; /* Duration per trial (default: 60s) */
	uint32_t warmup_sec;         /* Warmup period (default: 2s) */

	/* Throughput test specific */
	double initial_rate_pct;  /* Starting rate % (default: 100) */
	double resolution_pct;    /* Binary search resolution (default: 0.1%) */
	uint32_t max_iterations;  /* Max binary search iterations (default: 20) */
	double acceptable_loss;   /* Acceptable frame loss (default: 0.0%) */

	/* Latency test specific */
	uint32_t latency_samples;    /* Number of latency samples per trial */
	double latency_load_pct[10]; /* Load levels to test (default: 10,20,..,100) */
	uint32_t latency_load_count; /* Number of load levels */

	/* Frame loss specific */
	double loss_start_pct;    /* Starting offered load % */
	double loss_end_pct;      /* Ending offered load % */
	double loss_step_pct;     /* Step size for offered load */

	/* Back-to-back specific */
	uint64_t initial_burst;   /* Starting burst size */
	uint32_t burst_trials;    /* Trials per burst size (default: 50) */

	/* Hardware timestamping */
	bool hw_timestamp;        /* Use hardware timestamping if available */

	/* Output */
	stats_format_t output_format;
	bool verbose;

	/* Rate control */
	bool use_pacing;          /* Enable software pacing */
	uint32_t batch_size;      /* TX batch size */
} rfc2544_config_t;

/* Test context */
typedef struct rfc2544_ctx rfc2544_ctx_t;

/* Test progress callback */
typedef void (*progress_callback_t)(const rfc2544_ctx_t *ctx, const char *message, double pct);

/* ============================================================================
 * Core API
 * ============================================================================ */

/**
 * Initialize RFC2544 test context
 * @param ctx Pointer to context to initialize
 * @param interface Network interface name
 * @return 0 on success, negative on error
 */
int rfc2544_init(rfc2544_ctx_t **ctx, const char *interface);

/**
 * Configure test parameters
 * @param ctx Test context
 * @param config Configuration to apply
 * @return 0 on success, negative on error
 */
int rfc2544_configure(rfc2544_ctx_t *ctx, const rfc2544_config_t *config);

/**
 * Set progress callback
 * @param ctx Test context
 * @param callback Progress callback function
 */
void rfc2544_set_progress_callback(rfc2544_ctx_t *ctx, progress_callback_t callback);

/**
 * Run configured test
 * @param ctx Test context
 * @return 0 on success, negative on error
 */
int rfc2544_run(rfc2544_ctx_t *ctx);

/**
 * Cancel running test
 * @param ctx Test context
 */
void rfc2544_cancel(rfc2544_ctx_t *ctx);

/**
 * Get current test state
 * @param ctx Test context
 * @return Current test state
 */
test_state_t rfc2544_get_state(const rfc2544_ctx_t *ctx);

/**
 * Clean up and free context
 * @param ctx Test context
 */
void rfc2544_cleanup(rfc2544_ctx_t *ctx);

/* ============================================================================
 * Individual Test Functions
 * ============================================================================ */

/**
 * Run throughput test (Section 26.1)
 * Binary search to find maximum rate with zero frame loss
 * @param ctx Test context
 * @param frame_size Frame size to test (0 = all standard sizes)
 * @param result Result structure (caller allocates)
 * @param result_count Number of results (1 if specific size, 7 if all)
 * @return 0 on success, negative on error
 */
int rfc2544_throughput_test(rfc2544_ctx_t *ctx, uint32_t frame_size, throughput_result_t *result,
                            uint32_t *result_count);

/**
 * Run latency test (Section 26.2)
 * Measure round-trip latency at specified load levels
 * @param ctx Test context
 * @param frame_size Frame size to test
 * @param load_pct Load level as % of throughput (from throughput test)
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_latency_test(rfc2544_ctx_t *ctx, uint32_t frame_size, double load_pct,
                         latency_result_t *result);

/**
 * Run frame loss test (Section 26.3)
 * Measure frame loss at various offered loads
 * @param ctx Test context
 * @param frame_size Frame size to test
 * @param results Array of results (caller allocates)
 * @param result_count Number of load levels tested
 * @return 0 on success, negative on error
 */
int rfc2544_frame_loss_test(rfc2544_ctx_t *ctx, uint32_t frame_size, frame_loss_point_t *results,
                            uint32_t *result_count);

/**
 * Run back-to-back test (Section 26.4)
 * Find maximum burst length with zero frame loss
 * @param ctx Test context
 * @param frame_size Frame size to test
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_back_to_back_test(rfc2544_ctx_t *ctx, uint32_t frame_size, burst_result_t *result);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Set log level
 * @param level Log level
 */
void rfc2544_set_log_level(log_level_t level);

/**
 * Get line rate for interface
 * @param interface Network interface name
 * @return Line rate in bits/sec, 0 on error
 */
uint64_t rfc2544_get_line_rate(const char *interface);

/**
 * Calculate theoretical max packet rate
 * @param line_rate Line rate in bits/sec
 * @param frame_size Frame size in bytes
 * @return Packets per second
 */
uint64_t rfc2544_calc_pps(uint64_t line_rate, uint32_t frame_size);

/**
 * Get default configuration
 * @param config Configuration to populate
 */
void rfc2544_default_config(rfc2544_config_t *config);

/**
 * Print results in configured format
 * @param ctx Test context
 */
void rfc2544_print_results(const rfc2544_ctx_t *ctx);

/* ============================================================================
 * Packet Structure
 * ============================================================================
 *
 * RFC2544 test packets use a custom signature for identification.
 * The packet format allows the reflector to identify and reflect these
 * packets while maintaining sequence numbers and timestamps.
 *
 * Packet layout (after Ethernet + IP + UDP headers):
 *
 * Offset  Size    Field
 * ------  ----    -----
 * 0       7       Signature ("RFC2544")
 * 7       4       Sequence number (uint32_t, network order)
 * 11      8       TX timestamp (uint64_t nanoseconds, network order)
 * 19      4       Stream ID (uint32_t, for multi-stream tests)
 * 23      1       Flags (bit 0: request timestamp, bit 1: is response)
 * 24      N       Padding to reach frame size
 *
 * Total payload: 24 bytes minimum + padding
 * Minimum frame: 64 bytes (14 ETH + 20 IP + 8 UDP + 22 payload)
 */

#define RFC2544_PAYLOAD_OFFSET 0
#define RFC2544_SEQNUM_OFFSET 7
#define RFC2544_TIMESTAMP_OFFSET 11
#define RFC2544_STREAMID_OFFSET 19
#define RFC2544_FLAGS_OFFSET 23
#define RFC2544_PADDING_OFFSET 24

#define RFC2544_FLAG_REQ_TIMESTAMP 0x01
#define RFC2544_FLAG_IS_RESPONSE 0x02

#define RFC2544_MIN_PAYLOAD 24
#define RFC2544_MIN_FRAME 64

/* Calculate payload size for a given frame size */
#define RFC2544_PAYLOAD_SIZE(frame_size) ((frame_size) - 14 - 20 - 8 - 4)
/* 14=ETH, 20=IP, 8=UDP, 4=FCS */

#ifdef __cplusplus
}
#endif

#endif /* RFC2544_H */
