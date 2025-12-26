// Package dataplane provides CGO bindings to the C dataplane library
package dataplane

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../.. -lrfc2544 -lpthread -lm

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Forward declarations for C types
typedef struct rfc2544_ctx rfc2544_ctx_t;

// Test types
typedef enum {
    TEST_THROUGHPUT = 0,
    TEST_LATENCY = 1,
    TEST_FRAME_LOSS = 2,
    TEST_BACK_TO_BACK = 3
} test_type_t;

// Test state
typedef enum {
    STATE_IDLE = 0,
    STATE_RUNNING = 1,
    STATE_COMPLETED = 2,
    STATE_FAILED = 3,
    STATE_CANCELLED = 4
} test_state_t;

// Stats format
typedef enum {
    STATS_FORMAT_TEXT = 0,
    STATS_FORMAT_JSON = 1,
    STATS_FORMAT_CSV = 2
} stats_format_t;

// Latency stats
typedef struct {
    uint64_t count;
    double min_ns;
    double max_ns;
    double avg_ns;
    double jitter_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
} latency_stats_t;

// Throughput result
typedef struct {
    uint32_t frame_size;
    double max_rate_pct;
    double max_rate_mbps;
    double max_rate_pps;
    uint64_t frames_tested;
    uint32_t iterations;
    latency_stats_t latency;
} throughput_result_t;

// Frame loss point
typedef struct {
    double offered_rate_pct;
    double actual_rate_mbps;
    uint64_t frames_sent;
    uint64_t frames_recv;
    double loss_pct;
} frame_loss_point_t;

// Latency result
typedef struct {
    uint32_t frame_size;
    double offered_rate_pct;
    latency_stats_t latency;
} latency_result_t;

// Burst result
typedef struct {
    uint32_t frame_size;
    uint64_t max_burst;
    double burst_duration;
    uint32_t trials;
} burst_result_t;

// Config structure
typedef struct {
    char interface[64];
    uint64_t line_rate;
    bool auto_detect_nic;

    test_type_t test_type;
    uint32_t frame_size;
    bool include_jumbo;
    uint32_t trial_duration_sec;
    uint32_t warmup_sec;

    double initial_rate_pct;
    double resolution_pct;
    uint32_t max_iterations;
    double acceptable_loss;

    uint32_t latency_samples;
    double latency_load_pct[10];
    uint32_t latency_load_count;

    double loss_start_pct;
    double loss_end_pct;
    double loss_step_pct;

    uint64_t initial_burst;
    uint32_t burst_trials;

    bool hw_timestamp;
    bool measure_latency;

    stats_format_t output_format;
    bool verbose;

    bool use_pacing;
    uint32_t batch_size;

    bool use_dpdk;
    char *dpdk_args;
} rfc2544_config_t;

// External C functions
extern int rfc2544_init(rfc2544_ctx_t **ctx, const char *interface);
extern int rfc2544_configure(rfc2544_ctx_t *ctx, const rfc2544_config_t *config);
extern int rfc2544_run(rfc2544_ctx_t *ctx);
extern void rfc2544_cancel(rfc2544_ctx_t *ctx);
extern test_state_t rfc2544_get_state(const rfc2544_ctx_t *ctx);
extern void rfc2544_cleanup(rfc2544_ctx_t *ctx);

extern int rfc2544_throughput_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                   throughput_result_t *result, uint32_t *result_count);
extern int rfc2544_latency_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                double load_pct, latency_result_t *result);
extern int rfc2544_frame_loss_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                   frame_loss_point_t *results, uint32_t *result_count);
extern int rfc2544_back_to_back_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                     burst_result_t *result);

extern uint64_t rfc2544_get_line_rate(const char *interface);
extern uint64_t rfc2544_calc_pps(uint64_t line_rate, uint32_t frame_size);
extern void rfc2544_default_config(rfc2544_config_t *config);
*/
import "C"
import (
	"fmt"
	"sync"
	"time"
	"unsafe"
)

// TestType mirrors C test_type_t
type TestType int

const (
	TestThroughput TestType = iota
	TestLatency
	TestFrameLoss
	TestBackToBack
)

// TestState mirrors C test_state_t
type TestState int

const (
	StateIdle TestState = iota
	StateRunning
	StateCompleted
	StateFailed
	StateCancelled
)

// LatencyStats contains latency measurements
type LatencyStats struct {
	Count    uint64
	MinNs    float64
	MaxNs    float64
	AvgNs    float64
	JitterNs float64
	P50Ns    float64
	P95Ns    float64
	P99Ns    float64
}

// ThroughputResult from binary search test
type ThroughputResult struct {
	FrameSize    uint32
	MaxRatePct   float64
	MaxRateMbps  float64
	MaxRatePps   float64
	FramesTested uint64
	Iterations   uint32
	Latency      LatencyStats
}

// FrameLossPoint for a single load level
type FrameLossPoint struct {
	OfferedRatePct float64
	ActualRateMbps float64
	FramesSent     uint64
	FramesRecv     uint64
	LossPct        float64
}

// LatencyResult from latency test
type LatencyResult struct {
	FrameSize      uint32
	OfferedRatePct float64
	Latency        LatencyStats
}

// BurstResult from back-to-back test
type BurstResult struct {
	FrameSize     uint32
	MaxBurst      uint64
	BurstDuration float64
	Trials        uint32
}

// Config for RFC2544 tests
type Config struct {
	Interface      string
	LineRate       uint64
	AutoDetect     bool
	TestType       TestType
	FrameSize      uint32
	IncludeJumbo   bool
	TrialDuration  time.Duration
	WarmupPeriod   time.Duration
	InitialRatePct float64
	ResolutionPct  float64
	MaxIterations  uint32
	AcceptableLoss float64
	HWTimestamp    bool
	MeasureLatency bool
	UsePacing      bool
	BatchSize      uint32
	UseDPDK        bool
	DPDKArgs       string
}

// Context wraps the C rfc2544_ctx_t
type Context struct {
	ctx   *C.rfc2544_ctx_t
	mu    sync.Mutex
	stats Stats
}

// Stats for real-time monitoring
type Stats struct {
	TxPackets   uint64
	TxBytes     uint64
	RxPackets   uint64
	RxBytes     uint64
	CurrentRate float64
	Progress    float64
	Timestamp   time.Time
}

// NewContext creates a new RFC2544 test context
func NewContext(iface string) (*Context, error) {
	cIface := C.CString(iface)
	defer C.free(unsafe.Pointer(cIface))

	var cctx *C.rfc2544_ctx_t
	ret := C.rfc2544_init(&cctx, cIface)
	if ret < 0 {
		return nil, fmt.Errorf("init failed: %d", ret)
	}

	return &Context{ctx: cctx}, nil
}

// Configure applies test configuration
func (c *Context) Configure(cfg *Config) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	var ccfg C.rfc2544_config_t
	C.rfc2544_default_config(&ccfg)

	// Copy interface name
	cIface := C.CString(cfg.Interface)
	defer C.free(unsafe.Pointer(cIface))
	C.strncpy(&ccfg.interface[0], cIface, 63)

	ccfg.line_rate = C.uint64_t(cfg.LineRate)
	ccfg.auto_detect_nic = C.bool(cfg.AutoDetect)
	ccfg.test_type = C.test_type_t(cfg.TestType)
	ccfg.frame_size = C.uint32_t(cfg.FrameSize)
	ccfg.include_jumbo = C.bool(cfg.IncludeJumbo)
	ccfg.trial_duration_sec = C.uint32_t(cfg.TrialDuration.Seconds())
	ccfg.warmup_sec = C.uint32_t(cfg.WarmupPeriod.Seconds())
	ccfg.initial_rate_pct = C.double(cfg.InitialRatePct)
	ccfg.resolution_pct = C.double(cfg.ResolutionPct)
	ccfg.max_iterations = C.uint32_t(cfg.MaxIterations)
	ccfg.acceptable_loss = C.double(cfg.AcceptableLoss)
	ccfg.hw_timestamp = C.bool(cfg.HWTimestamp)
	ccfg.measure_latency = C.bool(cfg.MeasureLatency)
	ccfg.use_pacing = C.bool(cfg.UsePacing)
	ccfg.batch_size = C.uint32_t(cfg.BatchSize)
	ccfg.use_dpdk = C.bool(cfg.UseDPDK)

	if cfg.DPDKArgs != "" {
		ccfg.dpdk_args = C.CString(cfg.DPDKArgs)
	}

	ret := C.rfc2544_configure(c.ctx, &ccfg)
	if ret < 0 {
		return fmt.Errorf("configure failed: %d", ret)
	}

	return nil
}

// Run starts the configured test
func (c *Context) Run() error {
	ret := C.rfc2544_run(c.ctx)
	if ret < 0 {
		return fmt.Errorf("run failed: %d", ret)
	}
	return nil
}

// Cancel stops a running test
func (c *Context) Cancel() {
	C.rfc2544_cancel(c.ctx)
}

// State returns the current test state
func (c *Context) State() TestState {
	return TestState(C.rfc2544_get_state(c.ctx))
}

// Close cleans up resources
func (c *Context) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.ctx != nil {
		C.rfc2544_cleanup(c.ctx)
		c.ctx = nil
	}
}

// RunThroughputTest executes RFC 2544 Section 26.1 throughput test
func (c *Context) RunThroughputTest(frameSize uint32) ([]ThroughputResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	maxResults := 8 // 7 standard + 1 jumbo
	results := make([]C.throughput_result_t, maxResults)
	var count C.uint32_t

	ret := C.rfc2544_throughput_test(c.ctx, C.uint32_t(frameSize),
		&results[0], &count)
	if ret < 0 {
		return nil, fmt.Errorf("throughput test failed: %d", ret)
	}

	goResults := make([]ThroughputResult, count)
	for i := 0; i < int(count); i++ {
		goResults[i] = ThroughputResult{
			FrameSize:    uint32(results[i].frame_size),
			MaxRatePct:   float64(results[i].max_rate_pct),
			MaxRateMbps:  float64(results[i].max_rate_mbps),
			MaxRatePps:   float64(results[i].max_rate_pps),
			FramesTested: uint64(results[i].frames_tested),
			Iterations:   uint32(results[i].iterations),
			Latency: LatencyStats{
				Count:    uint64(results[i].latency.count),
				MinNs:    float64(results[i].latency.min_ns),
				MaxNs:    float64(results[i].latency.max_ns),
				AvgNs:    float64(results[i].latency.avg_ns),
				JitterNs: float64(results[i].latency.jitter_ns),
				P50Ns:    float64(results[i].latency.p50_ns),
				P95Ns:    float64(results[i].latency.p95_ns),
				P99Ns:    float64(results[i].latency.p99_ns),
			},
		}
	}

	return goResults, nil
}

// RunLatencyTest executes RFC 2544 Section 26.2 latency test
func (c *Context) RunLatencyTest(frameSize uint32, loadPct float64) (*LatencyResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result C.latency_result_t
	ret := C.rfc2544_latency_test(c.ctx, C.uint32_t(frameSize),
		C.double(loadPct), &result)
	if ret < 0 {
		return nil, fmt.Errorf("latency test failed: %d", ret)
	}

	return &LatencyResult{
		FrameSize:      uint32(result.frame_size),
		OfferedRatePct: float64(result.offered_rate_pct),
		Latency: LatencyStats{
			Count:    uint64(result.latency.count),
			MinNs:    float64(result.latency.min_ns),
			MaxNs:    float64(result.latency.max_ns),
			AvgNs:    float64(result.latency.avg_ns),
			JitterNs: float64(result.latency.jitter_ns),
			P50Ns:    float64(result.latency.p50_ns),
			P95Ns:    float64(result.latency.p95_ns),
			P99Ns:    float64(result.latency.p99_ns),
		},
	}, nil
}

// RunFrameLossTest executes RFC 2544 Section 26.3 frame loss test
func (c *Context) RunFrameLossTest(frameSize uint32) ([]FrameLossPoint, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	maxResults := 20 // Up to 20 load levels
	results := make([]C.frame_loss_point_t, maxResults)
	var count C.uint32_t

	ret := C.rfc2544_frame_loss_test(c.ctx, C.uint32_t(frameSize),
		&results[0], &count)
	if ret < 0 {
		return nil, fmt.Errorf("frame loss test failed: %d", ret)
	}

	goResults := make([]FrameLossPoint, count)
	for i := 0; i < int(count); i++ {
		goResults[i] = FrameLossPoint{
			OfferedRatePct: float64(results[i].offered_rate_pct),
			ActualRateMbps: float64(results[i].actual_rate_mbps),
			FramesSent:     uint64(results[i].frames_sent),
			FramesRecv:     uint64(results[i].frames_recv),
			LossPct:        float64(results[i].loss_pct),
		}
	}

	return goResults, nil
}

// RunBackToBackTest executes RFC 2544 Section 26.4 burst test
func (c *Context) RunBackToBackTest(frameSize uint32) (*BurstResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result C.burst_result_t
	ret := C.rfc2544_back_to_back_test(c.ctx, C.uint32_t(frameSize), &result)
	if ret < 0 {
		return nil, fmt.Errorf("back-to-back test failed: %d", ret)
	}

	return &BurstResult{
		FrameSize:     uint32(result.frame_size),
		MaxBurst:      uint64(result.max_burst),
		BurstDuration: float64(result.burst_duration),
		Trials:        uint32(result.trials),
	}, nil
}

// GetLineRate returns the interface line rate in bits/sec
func GetLineRate(iface string) uint64 {
	cIface := C.CString(iface)
	defer C.free(unsafe.Pointer(cIface))
	return uint64(C.rfc2544_get_line_rate(cIface))
}

// CalcPPS calculates packets per second for given rate and frame size
func CalcPPS(lineRate uint64, frameSize uint32) uint64 {
	return uint64(C.rfc2544_calc_pps(C.uint64_t(lineRate), C.uint32_t(frameSize)))
}
