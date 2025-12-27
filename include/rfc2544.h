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
	TEST_THROUGHPUT = 0,      /* RFC2544.26.1 - Binary search for max throughput */
	TEST_LATENCY = 1,         /* RFC2544.26.2 - Round-trip latency */
	TEST_FRAME_LOSS = 2,      /* RFC2544.26.3 - Frame loss rate */
	TEST_BACK_TO_BACK = 3,    /* RFC2544.26.4 - Burst capacity */
	TEST_SYSTEM_RECOVERY = 4, /* RFC2544.26.5 - System recovery time */
	TEST_RESET = 5,           /* RFC2544.26.6 - Reset time (informational) */
	TEST_Y1564_CONFIG = 6,    /* ITU-T Y.1564 - Service Configuration Test */
	TEST_Y1564_PERF = 7,      /* ITU-T Y.1564 - Service Performance Test */
	TEST_Y1564_FULL = 8,      /* ITU-T Y.1564 - Both Config and Perf Tests */
	TEST_COUNT = 9
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

/* System recovery test result (Section 26.5) */
typedef struct {
	uint32_t frame_size;        /* Frame size tested */
	double overload_rate_pct;   /* Overload rate (typically 110% of throughput) */
	double recovery_rate_pct;   /* Recovery rate (typically 50% of throughput) */
	uint32_t overload_sec;      /* Duration of overload in seconds */
	double recovery_time_ms;    /* Time to recover from overload (milliseconds) */
	uint64_t frames_lost;       /* Frames lost during recovery period */
	uint32_t trials;            /* Number of trials performed */
} recovery_result_t;

/* Reset test result (Section 26.6) */
typedef struct {
	uint32_t frame_size;        /* Frame size tested */
	double reset_time_ms;       /* Time for device to resume forwarding (ms) */
	uint64_t frames_lost;       /* Frames lost during reset */
	uint32_t trials;            /* Number of trials performed */
	bool manual_reset;          /* True if reset was triggered manually */
} reset_result_t;

/* ============================================================================
 * ITU-T Y.1564 (EtherSAM) Types
 * ============================================================================
 *
 * Y.1564 tests services against SLA parameters rather than raw throughput.
 * Two test phases:
 * 1. Service Configuration Test - validates SLA at 25%, 50%, 75%, 100% of CIR
 * 2. Service Performance Test - long-duration validation (15+ minutes)
 *
 * Supports up to 8 services tested simultaneously.
 */

/* Y.1564 Signature - 7 bytes, space-padded */
#define Y1564_SIGNATURE "Y.1564 "
#define Y1564_SIG_LEN 7
#define Y1564_MAX_SERVICES 8
#define Y1564_CONFIG_STEPS 4

/* Y.1564 Service SLA Configuration */
typedef struct {
	double cir_mbps;          /* Committed Information Rate (Mbps) */
	double eir_mbps;          /* Excess Information Rate (Mbps, 0 = none) */
	uint32_t cbs_bytes;       /* Committed Burst Size (bytes) */
	uint32_t ebs_bytes;       /* Excess Burst Size (bytes) */
	double fd_threshold_ms;   /* Frame Delay threshold (milliseconds) */
	double fdv_threshold_ms;  /* Frame Delay Variation threshold (ms) */
	double flr_threshold_pct; /* Frame Loss Ratio threshold (%) */
} y1564_sla_t;

/* Y.1564 Service Configuration */
typedef struct {
	uint32_t service_id;      /* Service identifier (1-8) */
	char service_name[32];    /* Human-readable name */
	y1564_sla_t sla;          /* SLA parameters */
	uint32_t frame_size;      /* Test frame size */
	uint8_t cos;              /* Class of Service (DSCP value) */
	bool enabled;             /* Service enabled for test */
} y1564_service_t;

/* Y.1564 Step Result (one step of Config test) */
typedef struct {
	uint32_t step;             /* Step number (1-4) */
	double offered_rate_pct;   /* % of CIR (25, 50, 75, 100) */
	double achieved_rate_mbps; /* Actual rate achieved */
	uint64_t frames_tx;        /* Frames transmitted */
	uint64_t frames_rx;        /* Frames received */
	double flr_pct;            /* Frame Loss Ratio (%) */
	double fd_avg_ms;          /* Average Frame Delay (ms) */
	double fd_min_ms;          /* Minimum Frame Delay (ms) */
	double fd_max_ms;          /* Maximum Frame Delay (ms) */
	double fdv_ms;             /* Frame Delay Variation (ms) */
	bool flr_pass;             /* FLR within threshold */
	bool fd_pass;              /* FD within threshold */
	bool fdv_pass;             /* FDV within threshold */
	bool step_pass;            /* Overall step pass/fail */
} y1564_step_result_t;

/* Y.1564 Service Configuration Test Result */
typedef struct {
	uint32_t service_id;                        /* Service ID */
	char service_name[32];                      /* Service name */
	y1564_step_result_t steps[Y1564_CONFIG_STEPS]; /* 25%, 50%, 75%, 100% */
	bool service_pass;                          /* All steps passed */
} y1564_config_result_t;

/* Y.1564 Service Performance Test Result */
typedef struct {
	uint32_t service_id;       /* Service ID */
	char service_name[32];     /* Service name */
	uint32_t duration_sec;     /* Test duration (seconds) */
	uint64_t frames_tx;        /* Frames transmitted */
	uint64_t frames_rx;        /* Frames received */
	double flr_pct;            /* Frame Loss Ratio (%) */
	double fd_avg_ms;          /* Average Frame Delay (ms) */
	double fd_min_ms;          /* Minimum Frame Delay (ms) */
	double fd_max_ms;          /* Maximum Frame Delay (ms) */
	double fdv_ms;             /* Frame Delay Variation (ms) */
	bool flr_pass;             /* FLR within threshold */
	bool fd_pass;              /* FD within threshold */
	bool fdv_pass;             /* FDV within threshold */
	bool service_pass;         /* Overall service pass/fail */
} y1564_perf_result_t;

/* Y.1564 Test Configuration */
typedef struct {
	y1564_service_t services[Y1564_MAX_SERVICES]; /* Service configurations */
	uint32_t service_count;                        /* Number of services (1-8) */
	double config_steps[Y1564_CONFIG_STEPS];       /* Step percentages (default: 25,50,75,100) */
	uint32_t step_duration_sec;                    /* Duration per step (default: 60s) */
	uint32_t perf_duration_sec;                    /* Performance test duration (default: 900s) */
	bool run_config_test;                          /* Run configuration test */
	bool run_perf_test;                            /* Run performance test */
} y1564_config_t;

/* ============================================================================
 * IMIX (Internet Mix) Types
 * ============================================================================
 *
 * IMIX profiles simulate realistic Internet traffic patterns using weighted
 * distributions of frame sizes instead of fixed sizes.
 */

/* IMIX profile types */
typedef enum {
	IMIX_NONE = 0,           /* Use fixed frame sizes */
	IMIX_SIMPLE = 1,         /* 7:4:1 ratio of 64:570:1518 bytes */
	IMIX_CISCO = 2,          /* Cisco standard: 7x64, 4x594, 1x1518 */
	IMIX_TOLLY = 3,          /* Tolly Group profile */
	IMIX_IPSEC = 4,          /* IPSec-heavy traffic profile */
	IMIX_CUSTOM = 5          /* User-defined distribution */
} imix_profile_t;

/* IMIX frame distribution entry */
typedef struct {
	uint32_t frame_size;     /* Frame size in bytes */
	double weight;           /* Weight (percentage or ratio) */
} imix_entry_t;

#define IMIX_MAX_ENTRIES 16

/* IMIX configuration */
typedef struct {
	imix_profile_t profile;              /* Profile type */
	uint32_t entry_count;                /* Number of entries in custom profile */
	imix_entry_t entries[IMIX_MAX_ENTRIES]; /* Custom frame size distribution */
} imix_config_t;

/* IMIX result (aggregate of all frame sizes) */
typedef struct {
	double avg_frame_size;               /* Weighted average frame size */
	double throughput_mbps;              /* Achieved throughput */
	double frame_rate_fps;               /* Achieved frame rate */
	uint64_t total_frames_tx;            /* Total frames transmitted */
	uint64_t total_frames_rx;            /* Total frames received */
	double loss_pct;                     /* Overall frame loss */
	double latency_avg_ms;               /* Average latency */
	double latency_min_ms;               /* Minimum latency */
	double latency_max_ms;               /* Maximum latency */
	double jitter_ms;                    /* Jitter (FDV) */
} imix_result_t;

/* ============================================================================
 * Bidirectional Testing Types
 * ============================================================================
 */

/* Bidirectional test mode */
typedef enum {
	BIDIR_NONE = 0,          /* Unidirectional (default) */
	BIDIR_SYMMETRIC = 1,     /* Same rate both directions */
	BIDIR_ASYMMETRIC = 2     /* Different rates per direction */
} bidir_mode_t;

/* Bidirectional result */
typedef struct {
	throughput_result_t tx_result;  /* TX direction results */
	throughput_result_t rx_result;  /* RX direction results */
	double aggregate_mbps;          /* Combined throughput */
} bidir_result_t;

/* ============================================================================
 * Multi-Port Testing Types
 * ============================================================================
 */

#define MAX_TEST_PORTS 8

/* Port configuration */
typedef struct {
	char interface[64];      /* Interface name */
	uint8_t src_mac[6];      /* Source MAC */
	uint8_t dst_mac[6];      /* Destination MAC */
	uint32_t src_ip;         /* Source IP */
	uint32_t dst_ip;         /* Destination IP */
	uint16_t src_port;       /* Source UDP port */
	uint16_t dst_port;       /* Destination UDP port */
	double rate_pct;         /* Rate percentage of line rate */
	bool enabled;            /* Port enabled */
} port_config_t;

/* Multi-port configuration */
typedef struct {
	uint32_t port_count;               /* Number of ports */
	port_config_t ports[MAX_TEST_PORTS]; /* Port configurations */
	bool aggregate_results;            /* Aggregate or per-port results */
} multiport_config_t;

/* ============================================================================
 * IPv6 Testing Types (RFC 5180)
 * ============================================================================
 */

/* IPv6 test mode */
typedef enum {
	IP_MODE_V4 = 0,          /* IPv4 only (default) */
	IP_MODE_V6 = 1,          /* IPv6 only */
	IP_MODE_DUAL = 2         /* Dual-stack (both) */
} ip_mode_t;

/* IPv6 configuration */
typedef struct {
	uint8_t src_addr[16];    /* Source IPv6 address */
	uint8_t dst_addr[16];    /* Destination IPv6 address */
	uint8_t traffic_class;   /* Traffic class (DSCP) */
	uint32_t flow_label;     /* Flow label */
	uint8_t hop_limit;       /* Hop limit (TTL equivalent) */
} ipv6_config_t;

/* ============================================================================
 * Y.1564 Color-Aware Metering Types
 * ============================================================================
 */

/* MEF color marking */
typedef enum {
	COLOR_GREEN = 0,         /* Within CIR */
	COLOR_YELLOW = 1,        /* Between CIR and CIR+EIR */
	COLOR_RED = 2            /* Above CIR+EIR (drop) */
} traffic_color_t;

/* Color-aware metering result */
typedef struct {
	uint64_t green_frames;   /* Frames within CIR */
	uint64_t yellow_frames;  /* Frames in EIR */
	uint64_t red_frames;     /* Frames dropped (above EIR) */
	double green_pct;        /* Percentage green */
	double yellow_pct;       /* Percentage yellow */
	double red_pct;          /* Percentage red (dropped) */
} color_result_t;

/* CBS/EBS burst validation result */
typedef struct {
	bool cbs_valid;          /* CBS test passed */
	bool ebs_valid;          /* EBS test passed */
	uint32_t measured_cbs;   /* Measured Committed Burst Size */
	uint32_t measured_ebs;   /* Measured Excess Burst Size */
	uint32_t expected_cbs;   /* Expected CBS from SLA */
	uint32_t expected_ebs;   /* Expected EBS from SLA */
} y1564_burst_result_t;

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
	bool measure_latency;     /* Enable latency measurement during tests */

	/* Output */
	stats_format_t output_format;
	bool verbose;

	/* Rate control */
	bool use_pacing;          /* Enable software pacing */
	uint32_t batch_size;      /* TX batch size */

	/* Platform selection */
	bool use_dpdk;            /* Use DPDK for packet I/O */
	char *dpdk_args;          /* DPDK EAL arguments */

	/* IMIX configuration */
	imix_config_t imix;       /* IMIX traffic profile */

	/* Bidirectional testing */
	bidir_mode_t bidir_mode;  /* Bidirectional test mode */
	double reverse_rate_pct;  /* Reverse direction rate (for asymmetric) */

	/* Multi-port testing */
	multiport_config_t multiport; /* Multi-port configuration */

	/* IPv6 testing (RFC 5180) */
	ip_mode_t ip_mode;        /* IP version mode */
	ipv6_config_t ipv6;       /* IPv6 configuration */

	/* Y.1564 color-aware metering */
	bool color_aware;         /* Enable color-aware metering */
	bool validate_burst;      /* Validate CBS/EBS burst sizes */

	/* Y.1564 configuration */
	y1564_config_t y1564;     /* Y.1564 test parameters */
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

/**
 * Run system recovery test (Section 26.5)
 * Measures time to recover from overload condition
 * @param ctx Test context
 * @param frame_size Frame size to test
 * @param throughput_pct Known throughput rate from throughput test
 * @param overload_sec Duration of overload condition in seconds
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_system_recovery_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                 double throughput_pct, uint32_t overload_sec,
                                 recovery_result_t *result);

/**
 * Run reset test (Section 26.6) - Informational
 * Measures time for device to resume forwarding after reset
 * NOTE: This test requires external reset trigger (manual or automated)
 * @param ctx Test context
 * @param frame_size Frame size to test
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_reset_test(rfc2544_ctx_t *ctx, uint32_t frame_size, reset_result_t *result);

/* ============================================================================
 * ITU-T Y.1564 Test Functions
 * ============================================================================ */

/**
 * Run Y.1564 Service Configuration Test
 * Tests service at 25%, 50%, 75%, 100% of CIR
 * @param ctx Test context
 * @param service Service configuration with SLA parameters
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int y1564_config_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                      y1564_config_result_t *result);

/**
 * Run Y.1564 Service Performance Test
 * Long-duration test at CIR (default 15 minutes)
 * @param ctx Test context
 * @param service Service configuration with SLA parameters
 * @param duration_sec Test duration in seconds
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int y1564_perf_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                    uint32_t duration_sec, y1564_perf_result_t *result);

/**
 * Run Y.1564 Multi-Service Test
 * Tests multiple services simultaneously (up to 8)
 * @param ctx Test context
 * @param services Array of service configurations
 * @param service_count Number of services (1-8)
 * @param config_results Array of config results (caller allocates, size = service_count)
 * @param perf_results Array of perf results (caller allocates, size = service_count)
 * @return 0 on success, negative on error
 */
int y1564_multi_service_test(rfc2544_ctx_t *ctx, const y1564_service_t *services,
                             uint32_t service_count,
                             y1564_config_result_t *config_results,
                             y1564_perf_result_t *perf_results);

/**
 * Get default Y.1564 configuration
 * @param config Configuration to populate with defaults
 */
void y1564_default_config(y1564_config_t *config);

/**
 * Get default Y.1564 SLA (typical voice service)
 * @param sla SLA structure to populate
 */
void y1564_default_sla(y1564_sla_t *sla);

/**
 * Print Y.1564 test results
 * @param config_results Array of config test results
 * @param perf_results Array of perf test results
 * @param service_count Number of services
 * @param format Output format (TEXT, JSON, CSV)
 */
void y1564_print_results(const y1564_config_result_t *config_results,
                         const y1564_perf_result_t *perf_results,
                         uint32_t service_count, stats_format_t format);

/* ============================================================================
 * IMIX Test Functions
 * ============================================================================ */

/**
 * Get predefined IMIX profile configuration
 * @param profile Profile type (IMIX_SIMPLE, IMIX_CISCO, etc.)
 * @param config Configuration to populate
 */
void imix_get_profile(imix_profile_t profile, imix_config_t *config);

/**
 * Run IMIX throughput test
 * @param ctx Test context
 * @param imix_config IMIX profile configuration
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_imix_throughput(rfc2544_ctx_t *ctx, const imix_config_t *imix_config,
                            imix_result_t *result);

/**
 * Calculate weighted average frame size for IMIX profile
 * @param config IMIX configuration
 * @return Weighted average frame size in bytes
 */
double imix_avg_frame_size(const imix_config_t *config);

/* ============================================================================
 * Bidirectional Test Functions
 * ============================================================================ */

/**
 * Run bidirectional throughput test
 * @param ctx Test context
 * @param mode Bidirectional mode
 * @param reverse_rate Reverse direction rate (for asymmetric mode)
 * @param result Result structure (caller allocates)
 * @return 0 on success, negative on error
 */
int rfc2544_bidir_throughput(rfc2544_ctx_t *ctx, bidir_mode_t mode,
                             double reverse_rate, bidir_result_t *result);

/* ============================================================================
 * Multi-Port Test Functions
 * ============================================================================ */

/**
 * Initialize multi-port test context
 * @param ctx Test context
 * @param config Multi-port configuration
 * @return 0 on success, negative on error
 */
int rfc2544_multiport_init(rfc2544_ctx_t *ctx, const multiport_config_t *config);

/**
 * Run multi-port throughput test
 * @param ctx Test context
 * @param results Array of results (one per port)
 * @return 0 on success, negative on error
 */
int rfc2544_multiport_throughput(rfc2544_ctx_t *ctx, throughput_result_t *results);

/* ============================================================================
 * IPv6 Test Functions (RFC 5180)
 * ============================================================================ */

/**
 * Configure IPv6 test parameters
 * @param ctx Test context
 * @param config IPv6 configuration
 * @return 0 on success, negative on error
 */
int rfc2544_ipv6_configure(rfc2544_ctx_t *ctx, const ipv6_config_t *config);

/**
 * Parse IPv6 address from string
 * @param str IPv6 address string (e.g., "2001:db8::1")
 * @param addr Output address buffer (16 bytes)
 * @return 0 on success, negative on error
 */
int rfc2544_parse_ipv6(const char *str, uint8_t addr[16]);

/* ============================================================================
 * Y.1564 Color-Aware Metering Functions
 * ============================================================================ */

/**
 * Run color-aware metering test
 * @param ctx Test context
 * @param service Service configuration
 * @param result Color result structure
 * @return 0 on success, negative on error
 */
int y1564_color_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                     color_result_t *result);

/**
 * Validate CBS/EBS burst sizes
 * @param ctx Test context
 * @param service Service configuration
 * @param result Burst validation result
 * @return 0 on success, negative on error
 */
int y1564_burst_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                     y1564_burst_result_t *result);

/* ============================================================================
 * Interface Auto-Detection Functions
 * ============================================================================ */

/**
 * NIC capabilities structure
 */
typedef struct {
	char name[64];           /* Interface name */
	uint64_t link_speed;     /* Link speed in bps */
	bool supports_hw_ts;     /* Hardware timestamping support */
	bool supports_xdp;       /* XDP/AF_XDP support */
	bool is_up;              /* Interface is up */
	uint32_t mtu;            /* Maximum transmission unit */
	uint8_t mac[6];          /* MAC address */
} nic_info_t;

/**
 * Detect NIC capabilities
 * @param interface Interface name
 * @param info NIC info structure to populate
 * @return 0 on success, negative on error
 */
int rfc2544_detect_nic(const char *interface, nic_info_t *info);

/**
 * List available network interfaces suitable for testing
 * @param interfaces Array to populate (caller allocates)
 * @param max_count Maximum interfaces to return
 * @return Number of interfaces found, negative on error
 */
int rfc2544_list_interfaces(nic_info_t *interfaces, uint32_t max_count);

/**
 * Recommend best interface for testing
 * @param info Recommended interface info
 * @return 0 on success, negative on error
 */
int rfc2544_recommend_interface(nic_info_t *info);

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

/* ============================================================================
 * Y.1564 Packet Structure
 * ============================================================================
 *
 * Y.1564 packets use the same structure as RFC2544, with a different signature.
 * The reflector already supports "Y.1564 " signature (7 bytes, space-padded).
 *
 * Packet layout (after Ethernet + IP + UDP headers):
 *
 * Offset  Size    Field
 * ------  ----    -----
 * 0       7       Signature ("Y.1564 ")
 * 7       4       Sequence number (uint32_t, network order)
 * 11      8       TX timestamp (uint64_t nanoseconds, network order)
 * 19      4       Service ID (uint32_t, 1-8 for multi-service)
 * 23      1       Flags (bit 0: request timestamp, bit 1: is response)
 * 24      N       Padding to reach frame size
 *
 * DSCP is set in the IP header ToS field for CoS marking.
 */

#define Y1564_PAYLOAD_OFFSET 0
#define Y1564_SEQNUM_OFFSET 7
#define Y1564_TIMESTAMP_OFFSET 11
#define Y1564_SERVICEID_OFFSET 19
#define Y1564_FLAGS_OFFSET 23
#define Y1564_PADDING_OFFSET 24

#define Y1564_FLAG_REQ_TIMESTAMP 0x01
#define Y1564_FLAG_IS_RESPONSE 0x02

#define Y1564_MIN_PAYLOAD 24
#define Y1564_MIN_FRAME 64

/* Calculate payload size for Y.1564 */
#define Y1564_PAYLOAD_SIZE(frame_size) ((frame_size) - 14 - 20 - 8 - 4)

/* ============================================================================
 * RFC 2889 - LAN Switch Benchmarking Types
 * ============================================================================
 *
 * RFC 2889 defines methodologies for benchmarking LAN switching devices:
 * - Forwarding Rate: Maximum rate at which frames are forwarded
 * - Address Caching Capacity: Maximum MAC addresses in forwarding table
 * - Address Learning Rate: Rate at which new addresses are learned
 * - Broadcast Forwarding: Broadcast frame handling performance
 * - Congestion Control: Behavior under congested conditions
 */

#define RFC2889_SIGNATURE "RFC2889"
#define RFC2889_SIG_LEN 7
#define RFC2889_MAX_PORTS 64
#define RFC2889_MAX_MAC_ENTRIES 1000000

/* RFC 2889 test types */
typedef enum {
	RFC2889_FORWARDING_RATE = 0,      /* Section 5.1 - Forwarding rate */
	RFC2889_ADDRESS_CACHING = 1,      /* Section 5.2 - Address caching */
	RFC2889_ADDRESS_LEARNING = 2,     /* Section 5.3 - Address learning */
	RFC2889_BROADCAST_FORWARDING = 3, /* Section 5.4 - Broadcast forwarding */
	RFC2889_BROADCAST_LATENCY = 4,    /* Section 5.5 - Broadcast latency */
	RFC2889_CONGESTION_CONTROL = 5,   /* Section 5.6 - Congestion control */
	RFC2889_FORWARD_PRESSURE = 6,     /* Section 5.7 - Forward pressure */
	RFC2889_ERROR_FILTERING = 7,      /* Section 5.8 - Error frame filtering */
	RFC2889_TEST_COUNT = 8
} rfc2889_test_type_t;

/* Traffic distribution patterns */
typedef enum {
	TRAFFIC_FULLY_MESHED = 0,    /* All ports to all ports */
	TRAFFIC_PARTIALLY_MESHED = 1, /* Subset of port pairs */
	TRAFFIC_PAIR_WISE = 2,        /* Port N to port N+1 */
	TRAFFIC_ONE_TO_MANY = 3,      /* Single source, multiple destinations */
	TRAFFIC_MANY_TO_ONE = 4       /* Multiple sources, single destination */
} traffic_pattern_t;

/* Forwarding rate result (Section 5.1) */
typedef struct {
	uint32_t frame_size;           /* Frame size tested */
	uint32_t port_count;           /* Number of ports */
	traffic_pattern_t pattern;     /* Traffic pattern used */
	double max_rate_pct;           /* Maximum forwarding rate (% of line rate) */
	double max_rate_fps;           /* Maximum forwarding rate (frames/sec) */
	double aggregate_rate_mbps;    /* Aggregate throughput across all ports */
	uint64_t frames_tx;            /* Total frames transmitted */
	uint64_t frames_rx;            /* Total frames received */
	double loss_pct;               /* Frame loss percentage */
} rfc2889_fwd_result_t;

/* Address caching result (Section 5.2) */
typedef struct {
	uint32_t frame_size;           /* Frame size used */
	uint32_t addresses_tested;     /* Number of unique addresses tested */
	uint32_t addresses_cached;     /* Number successfully cached */
	uint32_t cache_capacity;       /* Measured cache capacity */
	double learning_time_ms;       /* Time to learn all addresses */
	double overflow_loss_pct;      /* Loss when cache exceeded */
} rfc2889_cache_result_t;

/* Address learning result (Section 5.3) */
typedef struct {
	uint32_t frame_size;           /* Frame size used */
	double learning_rate_fps;      /* Addresses learned per second */
	uint32_t addresses_learned;    /* Total addresses learned */
	double learning_time_ms;       /* Total learning time */
	uint32_t verification_frames;  /* Frames used to verify learning */
	double verification_loss_pct;  /* Loss during verification */
} rfc2889_learning_result_t;

/* Broadcast forwarding result (Section 5.4) */
typedef struct {
	uint32_t frame_size;           /* Frame size tested */
	uint32_t ingress_ports;        /* Number of ingress ports */
	uint32_t egress_ports;         /* Number of egress ports */
	double broadcast_rate_fps;     /* Maximum broadcast rate */
	double broadcast_rate_mbps;    /* Broadcast throughput */
	uint64_t frames_tx;            /* Broadcast frames sent */
	uint64_t frames_rx;            /* Broadcast frames received */
	double replication_factor;     /* Actual vs expected copies */
} rfc2889_broadcast_result_t;

/* Congestion control result (Section 5.6) */
typedef struct {
	uint32_t frame_size;           /* Frame size tested */
	double overload_rate_pct;      /* Offered load (% of capacity) */
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	uint64_t frames_dropped;       /* Frames dropped due to congestion */
	double head_of_line_blocking;  /* HOL blocking percentage */
	bool backpressure_observed;    /* Backpressure/pause frames seen */
	uint64_t pause_frames_rx;      /* 802.3x pause frames received */
} rfc2889_congestion_result_t;

/* RFC 2889 port configuration */
typedef struct {
	char interface[64];            /* Interface name */
	uint8_t mac_base[6];           /* Base MAC for generated addresses */
	uint32_t mac_count;            /* Number of unique MACs to use */
	bool is_ingress;               /* Port is traffic source */
	bool is_egress;                /* Port is traffic destination */
} rfc2889_port_t;

/* RFC 2889 test configuration */
typedef struct {
	rfc2889_test_type_t test_type; /* Test to run */
	traffic_pattern_t pattern;     /* Traffic pattern */
	uint32_t port_count;           /* Number of ports */
	rfc2889_port_t ports[RFC2889_MAX_PORTS]; /* Port configurations */
	uint32_t frame_size;           /* Frame size (0 = all standard) */
	uint32_t trial_duration_sec;   /* Duration per trial */
	uint32_t warmup_sec;           /* Warmup period */
	uint32_t address_count;        /* For address caching tests */
	double acceptable_loss_pct;    /* Acceptable frame loss */
} rfc2889_config_t;

/* ============================================================================
 * RFC 6349 - TCP Throughput Testing Types
 * ============================================================================
 *
 * RFC 6349 Framework for TCP Throughput Testing:
 * - Measures actual TCP throughput vs theoretical maximum
 * - Accounts for RTT, loss, and buffer sizes
 * - Uses Bandwidth-Delay Product (BDP) for analysis
 */

#define RFC6349_SIGNATURE "RFC6349"
#define RFC6349_SIG_LEN 7

/* TCP test methodology */
typedef enum {
	TCP_THROUGHPUT = 0,            /* Basic throughput test */
	TCP_SINGLE_STREAM = 0,         /* Single TCP connection (alias) */
	TCP_MULTI_STREAM = 1,          /* Multiple parallel connections */
	TCP_BIDIRECTIONAL = 2          /* Simultaneous send/receive */
} tcp_test_mode_t;

/* TCP throughput result */
typedef struct {
	/* Throughput */
	double achieved_rate_mbps;     /* Achieved TCP throughput */
	double theoretical_rate_mbps;  /* Theoretical maximum (BDP-based) */

	/* RTT metrics */
	double rtt_min_ms;             /* Minimum RTT */
	double rtt_avg_ms;             /* Average RTT */
	double rtt_max_ms;             /* Maximum RTT */

	/* BDP analysis */
	uint64_t bdp_bytes;            /* Bandwidth-Delay Product */
	uint32_t rwnd_used;            /* Receive window used */

	/* Transfer stats */
	uint64_t bytes_transferred;    /* Total bytes transferred */
	uint64_t retransmissions;      /* TCP retransmissions */
	uint32_t test_duration_ms;     /* Test duration (ms) */

	/* Efficiency metrics */
	double tcp_efficiency;         /* TCP efficiency (%) */
	double buffer_delay_pct;       /* Buffer delay (%) */
	double transfer_time_ratio;    /* Actual/ideal time ratio */

	/* Pass/fail */
	bool passed;                   /* Test passed */
} rfc6349_result_t;

/* TCP path characteristics */
typedef struct {
	uint32_t path_mtu;             /* Path MTU */
	uint32_t mss;                  /* Maximum Segment Size */
	double rtt_min_ms;             /* Minimum RTT */
	double rtt_avg_ms;             /* Average RTT */
	double rtt_max_ms;             /* Maximum RTT */
	uint64_t bdp_bytes;            /* Bandwidth-Delay Product */
	uint32_t ideal_rwnd;           /* Ideal receive window */
	double bottleneck_bw_mbps;     /* Bottleneck bandwidth */
} tcp_path_info_t;

/* RFC 6349 test configuration */
typedef struct {
	double target_rate_mbps;       /* Target rate (Mbps) */
	double min_rtt_ms;             /* Minimum RTT (ms) */
	double max_rtt_ms;             /* Maximum RTT (ms) */
	uint32_t rwnd_size;            /* Receive window size (0 = auto) */
	uint32_t test_duration_sec;    /* Test duration */
	uint32_t parallel_streams;     /* Number of parallel streams */
	uint32_t mss;                  /* Maximum Segment Size */
	tcp_test_mode_t mode;          /* Test mode */
} rfc6349_config_t;

/* ============================================================================
 * ITU-T Y.1731 - Ethernet OAM Performance Monitoring Types
 * ============================================================================
 *
 * Y.1731 defines OAM functions for Carrier Ethernet networks:
 * - Continuity Check (CC) - Fault detection
 * - Loopback (LB) - Connectivity verification
 * - Delay Measurement (DM) - One-way and two-way delay
 * - Loss Measurement (LM) - Frame loss ratio
 * - Synthetic Loss Measurement (SLM) - Proactive loss monitoring
 */

#define Y1731_SIGNATURE "Y.1731 "
#define Y1731_SIG_LEN 7

/* Y.1731 OAM PDU types (OpCodes) */
typedef enum {
	Y1731_CCM = 1,                 /* Continuity Check Message */
	Y1731_LBR = 2,                 /* Loopback Reply */
	Y1731_LBM = 3,                 /* Loopback Message */
	Y1731_LTR = 4,                 /* Linktrace Reply */
	Y1731_LTM = 5,                 /* Linktrace Message */
	Y1731_AIS = 33,                /* Alarm Indication Signal */
	Y1731_LCK = 35,                /* Locked Signal */
	Y1731_TST = 37,                /* Test Signal */
	Y1731_APS = 39,                /* Automatic Protection Switching */
	Y1731_MCC = 41,                /* Maintenance Communication Channel */
	Y1731_LMR = 42,                /* Loss Measurement Reply */
	Y1731_LMM = 43,                /* Loss Measurement Message */
	Y1731_1DM = 45,                /* One-way Delay Measurement */
	Y1731_DMR = 46,                /* Delay Measurement Reply */
	Y1731_DMM = 47,                /* Delay Measurement Message */
	Y1731_EXR = 48,                /* Experimental OAM Reply */
	Y1731_EXM = 49,                /* Experimental OAM Message */
	Y1731_VSR = 50,                /* Vendor Specific Reply */
	Y1731_VSM = 51,                /* Vendor Specific Message */
	Y1731_SLR = 54,                /* Synthetic Loss Reply */
	Y1731_SLM = 55                 /* Synthetic Loss Message */
} y1731_opcode_t;

/* MEG (Maintenance Entity Group) level */
typedef enum {
	MEG_LEVEL_CUSTOMER = 0,        /* Customer level */
	MEG_LEVEL_1 = 1,
	MEG_LEVEL_2 = 2,
	MEG_LEVEL_PROVIDER = 3,        /* Provider level */
	MEG_LEVEL_4 = 4,
	MEG_LEVEL_5 = 5,
	MEG_LEVEL_6 = 6,
	MEG_LEVEL_OPERATOR = 7         /* Operator level */
} meg_level_t;

/* CCM interval */
typedef enum {
	CCM_INVALID = 0,
	CCM_3_33MS = 1,                /* 3.33ms - for protection switching */
	CCM_10MS = 2,                  /* 10ms */
	CCM_100MS = 3,                 /* 100ms */
	CCM_1S = 4,                    /* 1 second */
	CCM_10S = 5,                   /* 10 seconds */
	CCM_1MIN = 6,                  /* 1 minute */
	CCM_10MIN = 7                  /* 10 minutes */
} ccm_interval_t;

/* Delay measurement result */
typedef struct {
	uint32_t frames_sent;          /* Frames sent */
	uint32_t frames_received;      /* Frames received */
	uint32_t frames_lost;          /* Frames lost */
	double delay_min_us;           /* Minimum delay (microseconds) */
	double delay_avg_us;           /* Average delay */
	double delay_max_us;           /* Maximum delay */
	double delay_variation_us;     /* Delay variation (jitter) */
} y1731_delay_result_t;

/* Loss measurement result */
typedef struct {
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	uint64_t near_end_loss;        /* Near-end frame loss count */
	uint64_t far_end_loss;         /* Far-end frame loss count */
	double near_end_loss_ratio;    /* Near-end frame loss ratio */
	double far_end_loss_ratio;     /* Far-end frame loss ratio */
	double availability_pct;       /* Service availability */
} y1731_loss_result_t;

/* Loopback result */
typedef struct {
	uint64_t lbm_sent;             /* LBM frames sent */
	uint64_t lbr_received;         /* LBR frames received */
	double rtt_min_ms;             /* Minimum RTT (ms) */
	double rtt_avg_ms;             /* Average RTT (ms) */
	double rtt_max_ms;             /* Maximum RTT (ms) */
} y1731_loopback_result_t;

/* CCM result */
typedef struct {
	ccm_interval_t interval;       /* CCM interval */
	uint64_t ccm_sent;             /* CCMs transmitted */
	uint64_t ccm_received;         /* CCMs received */
	uint64_t ccm_errors;           /* CCM errors (sequence, etc.) */
	bool rdi_received;             /* Remote Defect Indication received */
	bool connectivity_ok;          /* MEP reachable */
	double uptime_pct;             /* Percentage of time connected */
} y1731_ccm_result_t;

/* Y.1731 MEP (Maintenance End Point) configuration */
typedef struct {
	uint32_t mep_id;               /* MEP identifier (1-8191) */
	meg_level_t meg_level;         /* MEG level (0-7) */
	char meg_id[32];               /* MEG identifier string */
	ccm_interval_t ccm_interval;   /* CCM transmission interval */
	uint8_t priority;              /* 802.1p priority (0-7) */
	bool enabled;                  /* MEP enabled */
} y1731_mep_config_t;

/* Y.1731 test configuration */
typedef struct {
	y1731_mep_config_t mep;        /* MEP configuration */
	y1731_opcode_t test_type;      /* OAM function to test */
	uint32_t duration_sec;         /* Test duration */
	uint32_t measurement_interval_ms; /* Measurement interval */
	uint32_t frame_size;           /* Test frame size */
	bool priority_tagged;          /* Use priority tag */
	uint8_t priority;              /* 802.1p priority (0-7) */
} y1731_config_t;

/* Y.1731 session state */
typedef enum {
	Y1731_STATE_INIT = 0,
	Y1731_STATE_RUNNING = 1,
	Y1731_STATE_STOPPED = 2,
	Y1731_STATE_ERROR = 3
} y1731_state_t;

/* Y.1731 session context */
typedef struct {
	y1731_mep_config_t local_mep;  /* Local MEP configuration */
	y1731_mep_config_t remote_mep; /* Remote MEP configuration */
	y1731_state_t state;           /* Session state */
	uint64_t ccm_tx_count;         /* CCMs transmitted */
	uint64_t ccm_rx_count;         /* CCMs received */
	bool rdi_received;             /* RDI flag received */
	uint64_t last_ccm_time;        /* Last CCM timestamp */
} y1731_session_t;

/* Y.1731 session status */
typedef struct {
	y1731_state_t state;           /* Current state */
	uint64_t ccm_tx_count;         /* CCMs transmitted */
	uint64_t ccm_rx_count;         /* CCMs received */
	bool rdi_received;             /* RDI received */
	uint16_t local_mep_id;         /* Local MEP ID */
	uint16_t remote_mep_id;        /* Remote MEP ID */
	bool connectivity_ok;          /* Connectivity status */
} y1731_session_status_t;

/* ============================================================================
 * MEF 48/49 - Carrier Ethernet Performance Testing Types
 * ============================================================================
 *
 * MEF 48: Carrier Ethernet Functional Testing Specification
 * MEF 49: Service Activation Testing Methodology
 *
 * Defines Service OAM (SOAM) and SLA validation for CE 2.0 services.
 */

#define MEF_SIGNATURE "MEF48 "
#define MEF_SIG_LEN 7

/* MEF service types */
typedef enum {
	MEF_EPL = 0,                   /* Ethernet Private Line (point-to-point) */
	MEF_EVPL = 1,                  /* Ethernet Virtual Private Line */
	MEF_EP_LAN = 2,                /* Ethernet Private LAN (multipoint) */
	MEF_EVP_LAN = 3,               /* Ethernet Virtual Private LAN */
	MEF_EP_TREE = 4,               /* Ethernet Private Tree */
	MEF_EVP_TREE = 5               /* Ethernet Virtual Private Tree */
} mef_service_type_t;

/* MEF CoS (Class of Service) */
typedef enum {
	MEF_COS_BEST_EFFORT = 0,       /* Best effort */
	MEF_COS_LOW = 1,               /* Low priority */
	MEF_COS_MEDIUM = 2,            /* Medium priority */
	MEF_COS_HIGH = 3,              /* High priority */
	MEF_COS_CRITICAL = 4,          /* Critical/realtime */
	MEF_COS_HIGH_PRIORITY = 3      /* Alias for high priority */
} mef_cos_t;

/* MEF performance tier */
typedef enum {
	MEF_TIER_STANDARD = 0,         /* Standard performance */
	MEF_TIER_PREMIUM = 1,          /* Premium performance */
	MEF_TIER_MISSION_CRITICAL = 2  /* Mission critical */
} mef_perf_tier_t;

/* MEF bandwidth profile is defined below with the full mef_config_t */

/* MEF SLA parameters */
typedef struct {
	double fd_threshold_us;        /* Frame Delay threshold (microseconds) */
	double fdv_threshold_us;       /* Frame Delay Variation threshold */
	double flr_threshold_pct;      /* Frame Loss Ratio threshold */
	double availability_pct;       /* Availability threshold */
	uint32_t mttr_minutes;         /* Mean Time To Repair */
	uint32_t mtbf_hours;           /* Mean Time Between Failures */
} mef_sla_t;

/* MEF step result (for configuration test) */
typedef struct {
	uint32_t step_pct;             /* Step percentage (25, 50, 75, 100) */
	uint32_t offered_rate_kbps;    /* Offered rate */
	uint32_t achieved_rate_kbps;   /* Achieved rate */
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	double fd_us;                  /* Frame delay (microseconds) */
	double fd_min_us;              /* Min frame delay */
	double fd_max_us;              /* Max frame delay */
	double fdv_us;                 /* Frame delay variation */
	double flr_pct;                /* Frame loss ratio */
	bool passed;                   /* Step passed */
} mef_step_result_t;

/* MEF bandwidth profile (simplified for config) */
typedef struct {
	uint32_t cir_kbps;             /* Committed Information Rate */
	uint32_t cbs_bytes;            /* Committed Burst Size */
	uint32_t eir_kbps;             /* Excess Information Rate */
	uint32_t ebs_bytes;            /* Excess Burst Size */
	bool color_mode;               /* Color-aware mode */
	bool coupling_flag;            /* Coupling flag */
} mef_bandwidth_profile_t;

/* MEF test configuration (simplified) */
typedef struct {
	mef_service_type_t service_type;
	mef_cos_t cos;
	char service_id[32];
	mef_bandwidth_profile_t bw_profile;
	mef_sla_t sla;
	uint32_t config_test_duration_sec;
	uint32_t perf_test_duration_min;
	uint32_t frame_sizes[7];
	uint32_t num_frame_sizes;
} mef_config_t;

/* MEF SLA compliance report */
typedef struct {
	double fd_threshold_us;
	double fdv_threshold_us;
	double flr_threshold_pct;
	double avail_threshold_pct;
	double fd_measured_us;
	double fdv_measured_us;
	double flr_measured_pct;
	double avail_measured_pct;
	double fd_margin_us;
	double fdv_margin_us;
	double flr_margin_pct;
	double avail_margin_pct;
	bool fd_compliant;
	bool fdv_compliant;
	bool flr_compliant;
	bool avail_compliant;
	bool overall_compliant;
} mef_sla_report_t;

/* MEF service configuration test result */
typedef struct {
	char service_id[32];           /* Service identifier */
	mef_step_result_t steps[4];    /* Step results (25%, 50%, 75%, 100%) */
	uint32_t num_steps;            /* Number of steps */
	bool overall_passed;           /* All steps passed */
} mef_config_result_t;

/* MEF performance test result */
typedef struct {
	char service_id[32];           /* Service identifier */
	uint32_t duration_sec;         /* Test duration */
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */

	/* Throughput */
	uint32_t throughput_kbps;      /* Achieved throughput */

	/* Delay */
	double fd_min_us;              /* Minimum frame delay (us) */
	double fd_avg_us;              /* Average frame delay (us) */
	double fd_max_us;              /* Maximum frame delay (us) */
	double fdv_us;                 /* Frame delay variation (us) */

	/* Loss */
	double flr_pct;                /* Frame loss ratio */

	/* Availability */
	double availability_pct;       /* Service availability */

	/* Pass/fail */
	bool fd_passed;
	bool fdv_passed;
	bool flr_passed;
	bool avail_passed;
	bool overall_passed;           /* Met all SLA requirements */
} mef_perf_result_t;

/* ============================================================================
 * IEEE 802.1Qbv - Time-Sensitive Networking (TSN) Types
 * ============================================================================
 *
 * 802.1Qbv defines time-aware shaping for deterministic Ethernet:
 * - Gate Control List (GCL) for scheduled transmission
 * - Time-aware queuing for bounded latency
 * - Integration with IEEE 802.1AS time synchronization
 */

#define TSN_SIGNATURE "802Qbv"
#define TSN_SIG_LEN 7
#define TSN_MAX_GATES 8
#define TSN_MAX_GCL_ENTRIES 256

/* Traffic class priority (0-7) */
typedef uint8_t tsn_priority_t;

/* Gate state */
typedef enum {
	GATE_CLOSED = 0,               /* Gate closed - no transmission */
	GATE_OPEN = 1                  /* Gate open - transmission allowed */
} gate_state_t;

/* Gate Control List entry */
typedef struct {
	uint8_t gate_states;           /* Bit mask of gate states (8 gates) */
	uint32_t time_interval_ns;     /* Duration of this entry (nanoseconds) */
} gcl_entry_t;

/* Gate Control List */
typedef struct {
	uint32_t entry_count;          /* Number of GCL entries */
	gcl_entry_t entries[TSN_MAX_GCL_ENTRIES];
	uint64_t base_time_ns;         /* Base time (PTP timestamp) */
	uint32_t cycle_time_ns;        /* Cycle time (sum of all intervals) */
	uint32_t cycle_time_extension_ns; /* Extension for overrun */
} gate_control_list_t;

/* TSN stream identification */
typedef struct {
	uint8_t dst_mac[6];            /* Destination MAC */
	uint16_t vlan_id;              /* VLAN ID */
	uint8_t priority;              /* 802.1p priority */
	uint32_t stream_id;            /* Stream identifier */
} tsn_stream_id_t;

/* TSN stream reservation */
typedef struct {
	tsn_stream_id_t stream;        /* Stream identification */
	double bandwidth_mbps;         /* Reserved bandwidth */
	uint32_t max_frame_size;       /* Maximum frame size */
	uint32_t max_interval_frames;  /* Max frames per interval */
	uint32_t interval_ns;          /* Transmission interval */
	uint32_t max_latency_ns;       /* Maximum allowed latency */
} tsn_reservation_t;

/* TSN timing result */
typedef struct {
	double latency_min_ns;         /* Minimum latency */
	double latency_avg_ns;         /* Average latency */
	double latency_max_ns;         /* Maximum latency */
	double jitter_ns;              /* Latency variation */
	bool deadline_met;             /* All frames met deadline */
	uint64_t frames_on_time;       /* Frames within deadline */
	uint64_t frames_late;          /* Frames that missed deadline */
	double on_time_pct;            /* Percentage on-time */
} tsn_timing_result_t;

/* TSN gate test result */
typedef struct {
	uint8_t gate_id;               /* Gate/priority tested */
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	uint64_t frames_blocked;       /* Frames blocked by gate */
	double gate_efficiency_pct;    /* Percentage transmitted in open window */
	double guard_band_violation_pct; /* Frames in guard band */
	tsn_timing_result_t timing;    /* Timing results */
} tsn_gate_result_t;

/* TSN stream result */
typedef struct {
	tsn_stream_id_t stream;        /* Stream tested */
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	double throughput_mbps;        /* Achieved throughput */
	double loss_pct;               /* Frame loss */
	tsn_timing_result_t timing;    /* Timing results */
	bool reservation_met;          /* Met bandwidth reservation */
	bool deadline_met;             /* Met latency deadline */
} tsn_stream_result_t;

/* TSN synchronization result */
typedef struct {
	double offset_ns;              /* Time offset from master */
	double offset_max_ns;          /* Maximum observed offset */
	double path_delay_ns;          /* Path delay */
	double freq_offset_ppb;        /* Frequency offset (ppb) */
	bool sync_locked;              /* PTP synchronized */
	uint32_t sync_steps;           /* Steps to lock */
} tsn_sync_result_t;

/* TSN test configuration */
typedef struct {
	/* Gate Control */
	gate_control_list_t gcl;       /* Gate control list */
	bool verify_gcl;               /* Verify GCL timing */

	/* Stream configuration */
	uint32_t stream_count;         /* Number of streams */
	tsn_reservation_t streams[8];  /* Stream reservations */

	/* Test parameters */
	uint32_t duration_sec;         /* Test duration */
	uint32_t warmup_sec;           /* Warmup period */
	uint32_t frame_size;           /* Test frame size */

	/* Timing requirements */
	uint32_t max_latency_ns;       /* Maximum acceptable latency */
	uint32_t max_jitter_ns;        /* Maximum acceptable jitter */

	/* PTP/802.1AS sync */
	bool require_ptp_sync;         /* Require PTP synchronization */
	uint32_t max_sync_offset_ns;   /* Maximum sync offset */
	bool ptp_enabled;              /* PTP enabled */
	bool preemption_enabled;       /* Frame preemption */
	uint32_t num_traffic_classes;  /* Number of traffic classes */
	uint64_t base_time_ns;         /* Base time for GCL */
	uint32_t cycle_time_ns;        /* Cycle time */
} tsn_config_t;

/* TSN gate timing test result */
typedef struct {
	uint32_t cycles_tested;        /* Number of cycles tested */
	uint32_t timing_errors;        /* Number of timing errors */
	double max_gate_deviation_ns;  /* Maximum gate deviation */
	double avg_gate_deviation_ns;  /* Average gate deviation */
	bool gate_timing_passed;       /* Gate timing passed */
} tsn_timing_result_t_v2;

/* TSN per-class result */
typedef struct {
	uint64_t frames_tx;            /* Frames transmitted */
	uint64_t frames_rx;            /* Frames received */
	uint64_t frames_interfered;    /* Frames that interfered */
	double isolation_pct;          /* Isolation percentage */
	double latency_avg_ns;         /* Average latency */
	double latency_max_ns;         /* Maximum latency */
	bool passed;                   /* Class test passed */
} tsn_class_result_t;

/* TSN traffic class isolation test result */
typedef struct {
	uint32_t num_classes;          /* Number of classes tested */
	tsn_class_result_t class_results[8]; /* Per-class results */
	bool overall_passed;           /* Overall test passed */
} tsn_isolation_result_t;

/* TSN scheduled latency result */
typedef struct {
	uint32_t traffic_class;        /* Traffic class tested */
	uint32_t samples;              /* Number of samples */
	double latency_min_ns;         /* Minimum latency */
	double latency_avg_ns;         /* Average latency */
	double latency_max_ns;         /* Maximum latency */
	double latency_99_ns;          /* 99th percentile */
	double latency_999_ns;         /* 99.9th percentile */
	double jitter_ns;              /* Jitter */
	bool latency_passed;           /* Latency passed */
	bool jitter_passed;            /* Jitter passed */
	bool overall_passed;           /* Overall passed */
} tsn_latency_result_t;

/* TSN PTP synchronization result */
typedef struct {
	uint32_t samples;              /* Number of samples */
	double offset_avg_ns;          /* Average offset */
	double offset_max_ns;          /* Maximum offset */
	double offset_stddev_ns;       /* Offset std deviation */
	bool sync_achieved;            /* PTP sync achieved */
} tsn_ptp_result_t;

/* TSN full test result */
typedef struct {
	tsn_timing_result_t_v2 timing_result;      /* Gate timing results */
	tsn_isolation_result_t isolation_result;   /* Isolation results */
	tsn_latency_result_t latency_results[8];   /* Per-class latency */
	tsn_ptp_result_t ptp_result;               /* PTP sync results */
	bool overall_passed;                       /* Overall test passed */
} tsn_full_result_t;

/* ============================================================================
 * Extended Test Type Enumeration
 * ============================================================================ */

/* Extended test types (beyond basic RFC 2544) */
typedef enum {
	/* RFC 2544 basic tests (0-8 already defined) */

	/* RFC 2889 LAN Switch tests */
	TEST_RFC2889_FORWARDING = 10,
	TEST_RFC2889_CACHING = 11,
	TEST_RFC2889_LEARNING = 12,
	TEST_RFC2889_BROADCAST = 13,
	TEST_RFC2889_CONGESTION = 14,

	/* RFC 6349 TCP tests */
	TEST_RFC6349_THROUGHPUT = 20,
	TEST_RFC6349_PATH = 21,

	/* ITU-T Y.1731 OAM tests */
	TEST_Y1731_CCM = 30,
	TEST_Y1731_LOOPBACK = 31,
	TEST_Y1731_DELAY = 32,
	TEST_Y1731_LOSS = 33,
	TEST_Y1731_SLM = 34,

	/* MEF 48/49 tests */
	TEST_MEF_CONFIG = 40,
	TEST_MEF_PERF = 41,
	TEST_MEF_FULL = 42,

	/* IEEE 802.1Qbv TSN tests */
	TEST_TSN_TIMING = 50,
	TEST_TSN_GATE = 51,
	TEST_TSN_STREAM = 52,
	TEST_TSN_SYNC = 53,

	TEST_TYPE_MAX = 100
} extended_test_type_t;

/* ============================================================================
 * RFC 2889 API Functions
 * ============================================================================ */

/**
 * Run RFC 2889 forwarding rate test
 */
int rfc2889_forwarding_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                            rfc2889_fwd_result_t *result);

/**
 * Run RFC 2889 address caching test
 */
int rfc2889_caching_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                         rfc2889_cache_result_t *result);

/**
 * Run RFC 2889 address learning test
 */
int rfc2889_learning_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                          rfc2889_learning_result_t *result);

/**
 * Run RFC 2889 broadcast forwarding test
 */
int rfc2889_broadcast_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                           rfc2889_broadcast_result_t *result);

/**
 * Run RFC 2889 congestion control test
 */
int rfc2889_congestion_test(rfc2544_ctx_t *ctx, const rfc2889_config_t *config,
                            rfc2889_congestion_result_t *result);

/**
 * Get default RFC 2889 configuration
 */
void rfc2889_default_config(rfc2889_config_t *config);

/**
 * Print RFC 2889 results
 */
void rfc2889_print_results(const void *result, rfc2889_test_type_t type,
                           stats_format_t format);

/* ============================================================================
 * RFC 6349 API Functions
 * ============================================================================ */

/**
 * Run RFC 6349 TCP throughput test
 */
int rfc6349_throughput_test(rfc2544_ctx_t *ctx, const rfc6349_config_t *config,
                            rfc6349_result_t *result);

/**
 * Run RFC 6349 path characterization
 */
int rfc6349_path_test(rfc2544_ctx_t *ctx, const rfc6349_config_t *config,
                      tcp_path_info_t *path_info);

/**
 * Calculate theoretical TCP throughput
 */
double rfc6349_theoretical_throughput(double bandwidth_mbps, double rtt_ms,
                                      double loss_pct, uint32_t mss);

/**
 * Get default RFC 6349 configuration
 */
void rfc6349_default_config(rfc6349_config_t *config);

/**
 * Print RFC 6349 results
 */
void rfc6349_print_results(const rfc6349_result_t *result, stats_format_t format);

/* ============================================================================
 * Y.1731 API Functions
 * ============================================================================ */

/**
 * Initialize Y.1731 session
 */
int y1731_session_init(rfc2544_ctx_t *ctx, const y1731_mep_config_t *config,
                       y1731_session_t *session);

/**
 * Run Y.1731 delay measurement test
 */
int y1731_delay_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                            uint32_t count, uint32_t interval_ms,
                            y1731_delay_result_t *result);

/**
 * Run Y.1731 loss measurement test
 */
int y1731_loss_measurement(rfc2544_ctx_t *ctx, y1731_session_t *session,
                           uint32_t duration_sec, y1731_loss_result_t *result);

/**
 * Run Y.1731 synthetic loss measurement
 */
int y1731_synthetic_loss(rfc2544_ctx_t *ctx, y1731_session_t *session,
                         uint32_t count, uint32_t interval_ms,
                         y1731_loss_result_t *result);

/**
 * Run Y.1731 loopback test
 */
int y1731_loopback(rfc2544_ctx_t *ctx, y1731_session_t *session,
                   const uint8_t *target_mac, uint32_t count,
                   y1731_loopback_result_t *result);

/**
 * Start Y.1731 CCM transmission
 */
int y1731_start_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session);

/**
 * Stop Y.1731 CCM transmission
 */
int y1731_stop_ccm(rfc2544_ctx_t *ctx, y1731_session_t *session);

/**
 * Get Y.1731 session status
 */
int y1731_get_status(y1731_session_t *session, y1731_session_status_t *status);

/**
 * Get default Y.1731 MEP configuration
 */
void y1731_default_mep_config(y1731_mep_config_t *config);

/**
 * Print Y.1731 delay results
 */
void y1731_print_delay_results(const y1731_delay_result_t *result);

/**
 * Print Y.1731 loss results
 */
void y1731_print_loss_results(const y1731_loss_result_t *result);

/* ============================================================================
 * MEF 48/49 API Functions
 * ============================================================================ */

/**
 * Run MEF service configuration test
 */
int mef_config_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                    mef_config_result_t *result);

/**
 * Run MEF service performance test
 */
int mef_perf_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                  mef_perf_result_t *result);

/**
 * Run full MEF 48/49 test suite
 */
int mef_full_test(rfc2544_ctx_t *ctx, const mef_config_t *config,
                  mef_config_result_t *config_result,
                  mef_perf_result_t *perf_result);

/**
 * Validate service against MEF SLA
 */
int mef_validate_sla(const mef_perf_result_t *result, const mef_sla_t *sla,
                     mef_sla_report_t *report);

/**
 * Get default MEF configuration
 */
void mef_default_config(mef_config_t *config);

/**
 * Get default MEF bandwidth profile
 */
void mef_default_bandwidth_profile(mef_bandwidth_profile_t *profile);

/**
 * Get default MEF SLA configuration
 */
void mef_default_sla(mef_sla_t *sla);

/**
 * Print MEF configuration test results
 */
void mef_print_config_results(const mef_config_result_t *result);

/**
 * Print MEF performance test results
 */
void mef_print_perf_results(const mef_perf_result_t *result);

/**
 * Print MEF results
 */
void mef_print_results(const mef_config_result_t *config_result,
                       const mef_perf_result_t *perf_result,
                       stats_format_t format);

/* ============================================================================
 * IEEE 802.1Qbv TSN API Functions
 * ============================================================================ */

/**
 * Run TSN gate timing test
 */
int tsn_gate_timing_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                         tsn_timing_result_t_v2 *result);

/**
 * Run TSN traffic class isolation test
 */
int tsn_isolation_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                       tsn_isolation_result_t *result);

/**
 * Run TSN scheduled latency test
 */
int tsn_scheduled_latency_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                               uint32_t traffic_class,
                               tsn_latency_result_t *result);

/**
 * Run TSN PTP synchronization test
 */
int tsn_ptp_sync_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                      tsn_ptp_result_t *result);

/**
 * Run full TSN test suite
 */
int tsn_full_test(rfc2544_ctx_t *ctx, const tsn_config_t *config,
                  tsn_full_result_t *result);

/**
 * Get default TSN configuration
 */
void tsn_default_config(tsn_config_t *config);

/**
 * Create exclusive GCL for traffic classes
 */
int tsn_create_exclusive_gcl(gate_control_list_t *gcl, uint32_t num_classes,
                             uint32_t cycle_time_ns);

/**
 * Create priority-based GCL
 */
int tsn_create_priority_gcl(gate_control_list_t *gcl, uint32_t cycle_time_ns,
                            uint32_t high_prio_time_pct);

/**
 * Verify GCL configuration
 */
int tsn_verify_gcl(const gate_control_list_t *gcl);

/**
 * Print TSN timing results
 */
void tsn_print_timing_results(const tsn_timing_result_t_v2 *result);

/**
 * Print TSN isolation results
 */
void tsn_print_isolation_results(const tsn_isolation_result_t *result);

/**
 * Print TSN latency results
 */
void tsn_print_latency_results(const tsn_latency_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* RFC2544_H */
