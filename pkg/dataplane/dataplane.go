// Package dataplane provides CGO bindings to the C dataplane library
package dataplane

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../.. -lrfc2544 -lpthread -lm
#cgo linux LDFLAGS: -lxdp -lbpf

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Forward declarations for C types
typedef struct rfc2544_ctx rfc2544_ctx_t;

// Test types
typedef enum {
    TEST_THROUGHPUT = 0,
    TEST_LATENCY = 1,
    TEST_FRAME_LOSS = 2,
    TEST_BACK_TO_BACK = 3,
    TEST_SYSTEM_RECOVERY = 4,
    TEST_RESET = 5,
    TEST_Y1564_CONFIG = 6,
    TEST_Y1564_PERF = 7,
    TEST_Y1564_FULL = 8
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

// System recovery result (Section 26.5)
typedef struct {
    uint32_t frame_size;
    double overload_rate_pct;
    double recovery_rate_pct;
    uint32_t overload_sec;
    double recovery_time_ms;
    uint64_t frames_lost;
    uint32_t trials;
} recovery_result_t;

// Reset result (Section 26.6)
typedef struct {
    uint32_t frame_size;
    double reset_time_ms;
    uint64_t frames_lost;
    uint32_t trials;
    bool manual_reset;
} reset_result_t;

// Y.1564 SLA parameters
typedef struct {
    double cir_mbps;
    double eir_mbps;
    uint32_t cbs_bytes;
    uint32_t ebs_bytes;
    double fd_threshold_ms;
    double fdv_threshold_ms;
    double flr_threshold_pct;
} y1564_sla_t;

// Y.1564 Service configuration
typedef struct {
    uint32_t service_id;
    char service_name[32];
    y1564_sla_t sla;
    uint32_t frame_size;
    uint8_t cos;
    bool enabled;
} y1564_service_t;

// Y.1564 Step result
typedef struct {
    uint32_t step;
    double offered_rate_pct;
    double achieved_rate_mbps;
    uint64_t frames_tx;
    uint64_t frames_rx;
    double flr_pct;
    double fd_avg_ms;
    double fd_min_ms;
    double fd_max_ms;
    double fdv_ms;
    bool flr_pass;
    bool fd_pass;
    bool fdv_pass;
    bool step_pass;
} y1564_step_result_t;

// Y.1564 Configuration test result
typedef struct {
    uint32_t service_id;
    y1564_step_result_t steps[4];
    bool service_pass;
} y1564_config_result_t;

// Y.1564 Performance test result
typedef struct {
    uint32_t service_id;
    uint32_t duration_sec;
    uint64_t frames_tx;
    uint64_t frames_rx;
    double flr_pct;
    double fd_avg_ms;
    double fd_min_ms;
    double fd_max_ms;
    double fdv_ms;
    bool flr_pass;
    bool fd_pass;
    bool fdv_pass;
    bool service_pass;
} y1564_perf_result_t;

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
extern int rfc2544_system_recovery_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                                        double throughput_pct, uint32_t overload_sec,
                                        recovery_result_t *result);
extern int rfc2544_reset_test(rfc2544_ctx_t *ctx, uint32_t frame_size,
                              reset_result_t *result);

extern uint64_t rfc2544_get_line_rate(const char *interface);
extern uint64_t rfc2544_calc_pps(uint64_t line_rate, uint32_t frame_size);
extern void rfc2544_default_config(rfc2544_config_t *config);

// Y.1564 functions
extern int y1564_config_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                             y1564_config_result_t *result);
extern int y1564_perf_test(rfc2544_ctx_t *ctx, const y1564_service_t *service,
                           uint32_t duration_sec, y1564_perf_result_t *result);
extern int y1564_multi_service_test(rfc2544_ctx_t *ctx, const y1564_service_t *services,
                                    uint32_t service_count, y1564_config_result_t *config_results,
                                    y1564_perf_result_t *perf_results);
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
	TestSystemRecovery
	TestReset
	TestY1564Config
	TestY1564Perf
	TestY1564Full
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

// RecoveryResult from RFC 2544 Section 26.5 System Recovery test
type RecoveryResult struct {
	FrameSize       uint32
	OverloadRatePct float64
	RecoveryRatePct float64
	OverloadSec     uint32
	RecoveryTimeMs  float64
	FramesLost      uint64
	Trials          uint32
}

// ResetResult from RFC 2544 Section 26.6 Reset test
type ResetResult struct {
	FrameSize    uint32
	ResetTimeMs  float64
	FramesLost   uint64
	Trials       uint32
	ManualReset  bool
}

// Y1564SLA contains SLA parameters for Y.1564 testing
type Y1564SLA struct {
	CIRMbps         float64
	EIRMbps         float64
	CBSBytes        uint32
	EBSBytes        uint32
	FDThresholdMs   float64
	FDVThresholdMs  float64
	FLRThresholdPct float64
}

// Y1564Service represents a service configuration for Y.1564 testing
type Y1564Service struct {
	ServiceID   uint32
	ServiceName string
	SLA         Y1564SLA
	FrameSize   uint32
	CoS         uint8
	Enabled     bool
}

// Y1564StepResult from a Y.1564 configuration test step
type Y1564StepResult struct {
	Step            uint32
	OfferedRatePct  float64
	AchievedRateMbps float64
	FramesTx        uint64
	FramesRx        uint64
	FLRPct          float64
	FDAvgMs         float64
	FDMinMs         float64
	FDMaxMs         float64
	FDVMs           float64
	FLRPass         bool
	FDPass          bool
	FDVPass         bool
	StepPass        bool
}

// Y1564ConfigResult from Y.1564 service configuration test
type Y1564ConfigResult struct {
	ServiceID   uint32
	Steps       [4]Y1564StepResult
	ServicePass bool
}

// Y1564PerfResult from Y.1564 service performance test
type Y1564PerfResult struct {
	ServiceID   uint32
	DurationSec uint32
	FramesTx    uint64
	FramesRx    uint64
	FLRPct      float64
	FDAvgMs     float64
	FDMinMs     float64
	FDMaxMs     float64
	FDVMs       float64
	FLRPass     bool
	FDPass      bool
	FDVPass     bool
	ServicePass bool
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
	ctx       *C.rfc2544_ctx_t
	mu        sync.Mutex
	stats     Stats
	config    Config
	frameSize uint32
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
	C.strncpy(&ccfg._interface[0], cIface, 63)

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

	var dpdkArgsPtr *C.char
	if cfg.DPDKArgs != "" {
		dpdkArgsPtr = C.CString(cfg.DPDKArgs)
		ccfg.dpdk_args = dpdkArgsPtr
	}

	ret := C.rfc2544_configure(c.ctx, &ccfg)

	// Free DPDK args string after configure copies it
	if dpdkArgsPtr != nil {
		C.free(unsafe.Pointer(dpdkArgsPtr))
	}

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

// runThroughputTestOld executes RFC 2544 Section 26.1 throughput test (deprecated, use RunThroughputTest)
func (c *Context) runThroughputTestOld(frameSize uint32) ([]ThroughputResult, error) {
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

// runLatencyTestOld executes RFC 2544 Section 26.2 latency test (deprecated)
func (c *Context) runLatencyTestOld(frameSize uint32, loadPct float64) (*LatencyResult, error) {
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

// runFrameLossTestOld executes RFC 2544 Section 26.3 frame loss test (deprecated)
func (c *Context) runFrameLossTestOld(frameSize uint32) ([]FrameLossPoint, error) {
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

// runBackToBackTestOld executes RFC 2544 Section 26.4 burst test (deprecated)
func (c *Context) runBackToBackTestOld(frameSize uint32) (*BurstResult, error) {
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

// RunY1564ConfigTest executes ITU-T Y.1564 Service Configuration Test
func (c *Context) RunY1564ConfigTest(service *Y1564Service) (*Y1564ConfigResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	// Convert Go service to C service
	var cService C.y1564_service_t
	cService.service_id = C.uint32_t(service.ServiceID)
	cService.sla.cir_mbps = C.double(service.SLA.CIRMbps)
	cService.sla.eir_mbps = C.double(service.SLA.EIRMbps)
	cService.sla.cbs_bytes = C.uint32_t(service.SLA.CBSBytes)
	cService.sla.ebs_bytes = C.uint32_t(service.SLA.EBSBytes)
	cService.sla.fd_threshold_ms = C.double(service.SLA.FDThresholdMs)
	cService.sla.fdv_threshold_ms = C.double(service.SLA.FDVThresholdMs)
	cService.sla.flr_threshold_pct = C.double(service.SLA.FLRThresholdPct)
	cService.frame_size = C.uint32_t(service.FrameSize)
	cService.cos = C.uint8_t(service.CoS)
	cService.enabled = C.bool(service.Enabled)

	// Copy service name (ensure null-termination)
	nameBytes := []byte(service.ServiceName)
	for i := 0; i < len(nameBytes) && i < 31; i++ {
		cService.service_name[i] = C.char(nameBytes[i])
	}
	cService.service_name[31] = 0 // Ensure null-termination

	var cResult C.y1564_config_result_t
	ret := C.y1564_config_test(c.ctx, &cService, &cResult)
	if ret < 0 {
		return nil, fmt.Errorf("Y.1564 config test failed: %d", ret)
	}

	result := &Y1564ConfigResult{
		ServiceID:   uint32(cResult.service_id),
		ServicePass: bool(cResult.service_pass),
	}

	for i := 0; i < 4; i++ {
		result.Steps[i] = Y1564StepResult{
			Step:            uint32(cResult.steps[i].step),
			OfferedRatePct:  float64(cResult.steps[i].offered_rate_pct),
			AchievedRateMbps: float64(cResult.steps[i].achieved_rate_mbps),
			FramesTx:        uint64(cResult.steps[i].frames_tx),
			FramesRx:        uint64(cResult.steps[i].frames_rx),
			FLRPct:          float64(cResult.steps[i].flr_pct),
			FDAvgMs:         float64(cResult.steps[i].fd_avg_ms),
			FDMinMs:         float64(cResult.steps[i].fd_min_ms),
			FDMaxMs:         float64(cResult.steps[i].fd_max_ms),
			FDVMs:           float64(cResult.steps[i].fdv_ms),
			FLRPass:         bool(cResult.steps[i].flr_pass),
			FDPass:          bool(cResult.steps[i].fd_pass),
			FDVPass:         bool(cResult.steps[i].fdv_pass),
			StepPass:        bool(cResult.steps[i].step_pass),
		}
	}

	return result, nil
}

// RunY1564PerfTest executes ITU-T Y.1564 Service Performance Test
func (c *Context) RunY1564PerfTest(service *Y1564Service, durationSec uint32) (*Y1564PerfResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	// Convert Go service to C service
	var cService C.y1564_service_t
	cService.service_id = C.uint32_t(service.ServiceID)
	cService.sla.cir_mbps = C.double(service.SLA.CIRMbps)
	cService.sla.eir_mbps = C.double(service.SLA.EIRMbps)
	cService.sla.cbs_bytes = C.uint32_t(service.SLA.CBSBytes)
	cService.sla.ebs_bytes = C.uint32_t(service.SLA.EBSBytes)
	cService.sla.fd_threshold_ms = C.double(service.SLA.FDThresholdMs)
	cService.sla.fdv_threshold_ms = C.double(service.SLA.FDVThresholdMs)
	cService.sla.flr_threshold_pct = C.double(service.SLA.FLRThresholdPct)
	cService.frame_size = C.uint32_t(service.FrameSize)
	cService.cos = C.uint8_t(service.CoS)
	cService.enabled = C.bool(service.Enabled)

	// Copy service name (ensure null-termination)
	nameBytes := []byte(service.ServiceName)
	for i := 0; i < len(nameBytes) && i < 31; i++ {
		cService.service_name[i] = C.char(nameBytes[i])
	}
	cService.service_name[31] = 0 // Ensure null-termination

	var cResult C.y1564_perf_result_t
	ret := C.y1564_perf_test(c.ctx, &cService, C.uint32_t(durationSec), &cResult)
	if ret < 0 {
		return nil, fmt.Errorf("Y.1564 perf test failed: %d", ret)
	}

	return &Y1564PerfResult{
		ServiceID:   uint32(cResult.service_id),
		DurationSec: uint32(cResult.duration_sec),
		FramesTx:    uint64(cResult.frames_tx),
		FramesRx:    uint64(cResult.frames_rx),
		FLRPct:      float64(cResult.flr_pct),
		FDAvgMs:     float64(cResult.fd_avg_ms),
		FDMinMs:     float64(cResult.fd_min_ms),
		FDMaxMs:     float64(cResult.fd_max_ms),
		FDVMs:       float64(cResult.fdv_ms),
		FLRPass:     bool(cResult.flr_pass),
		FDPass:      bool(cResult.fd_pass),
		FDVPass:     bool(cResult.fdv_pass),
		ServicePass: bool(cResult.service_pass),
	}, nil
}

// =============================================================================
// Wrapper types and functions for CLI integration
// =============================================================================

// ThroughputResult wraps the throughput test result for CLI
type ThroughputResultCLI struct {
	FrameSize   uint32
	MaxRatePct  float64
	MaxRateMbps float64
	MaxRatePPS  float64
	Iterations  uint32
	Latency     LatencyStats
}

// LatencyResultCLI wraps the latency test result for CLI
type LatencyResultCLI struct {
	FrameSize uint32
	LoadPct   float64
	Latency   LatencyStats
}

// FrameLossResultCLI wraps the frame loss test result for CLI
type FrameLossResultCLI struct {
	FrameSize  uint32
	OfferedPct float64
	FramesTx   uint64
	FramesRx   uint64
	LossPct    float64
}

// BackToBackResultCLI wraps the back-to-back test result for CLI
type BackToBackResultCLI struct {
	FrameSize       uint32
	MaxBurstFrames  uint64
	BurstDurationUs uint64
	Trials          uint32
}

// RecoveryResultCLI wraps the system recovery test result for CLI
type RecoveryResultCLI struct {
	FrameSize       uint32
	OverloadRatePct float64
	RecoveryRatePct float64
	OverloadSec     uint32
	RecoveryTimeMs  float64
	FramesLost      uint64
	Trials          uint32
}

// ResetResultCLI wraps the reset test result for CLI
type ResetResultCLI struct {
	FrameSize   uint32
	ResetTimeMs float64
	FramesLost  uint64
	Trials      uint32
	ManualReset bool
}

// New creates a new RFC2544 context with configuration
func New(cfg Config) (*Context, error) {
	ctx, err := NewContext(cfg.Interface)
	if err != nil {
		return nil, err
	}

	if err := ctx.Configure(&cfg); err != nil {
		ctx.Close()
		return nil, err
	}

	// Store config in context for later use
	ctx.config = cfg

	return ctx, nil
}

// SetFrameSize sets the frame size for subsequent tests
func (c *Context) SetFrameSize(frameSize uint32) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.frameSize = frameSize
}

// RunThroughputTestCLI runs throughput test and returns CLI-friendly result
func (c *Context) RunThroughputTest() (*ThroughputResultCLI, error) {
	results, err := c.runThroughputTestInternal(c.frameSize)
	if err != nil {
		return nil, err
	}
	if len(results) == 0 {
		return nil, fmt.Errorf("no results")
	}

	r := results[0]
	return &ThroughputResultCLI{
		FrameSize:   r.FrameSize,
		MaxRatePct:  r.MaxRatePct,
		MaxRateMbps: r.MaxRateMbps,
		MaxRatePPS:  r.MaxRatePps,
		Iterations:  r.Iterations,
		Latency:     r.Latency,
	}, nil
}

// RunLatencyTestCLI runs latency test at multiple load levels
func (c *Context) RunLatencyTest(loadLevels []float64) ([]LatencyResultCLI, error) {
	var results []LatencyResultCLI

	for _, load := range loadLevels {
		result, err := c.runLatencyTestInternal(c.frameSize, load)
		if err != nil {
			continue
		}
		results = append(results, LatencyResultCLI{
			FrameSize: c.frameSize,
			LoadPct:   load,
			Latency:   result.Latency,
		})
	}

	if len(results) == 0 {
		return nil, fmt.Errorf("no latency results")
	}

	return results, nil
}

// RunFrameLossTestCLI runs frame loss test with stepped load
func (c *Context) RunFrameLossTest(startPct, endPct, stepPct float64) ([]FrameLossResultCLI, error) {
	results, err := c.runFrameLossTestInternal(c.frameSize)
	if err != nil {
		return nil, err
	}

	var cliResults []FrameLossResultCLI
	for _, r := range results {
		cliResults = append(cliResults, FrameLossResultCLI{
			FrameSize:  c.frameSize,
			OfferedPct: r.OfferedRatePct,
			FramesTx:   r.FramesSent,
			FramesRx:   r.FramesRecv,
			LossPct:    r.LossPct,
		})
	}

	return cliResults, nil
}

// RunBackToBackTestCLI runs back-to-back burst test
func (c *Context) RunBackToBackTest(initialBurst uint64, trials uint32) (*BackToBackResultCLI, error) {
	result, err := c.runBackToBackTestInternal(c.frameSize)
	if err != nil {
		return nil, err
	}

	return &BackToBackResultCLI{
		FrameSize:       c.frameSize,
		MaxBurstFrames:  result.MaxBurst,
		BurstDurationUs: uint64(result.BurstDuration),
		Trials:          result.Trials,
	}, nil
}

// RunSystemRecoveryTest runs RFC 2544 Section 26.5 System Recovery test
func (c *Context) RunSystemRecoveryTest(throughputPct float64, overloadSec uint32) (*RecoveryResultCLI, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result C.recovery_result_t

	ret := C.rfc2544_system_recovery_test(c.ctx, C.uint32_t(c.frameSize),
		C.double(throughputPct), C.uint32_t(overloadSec), &result)
	if ret < 0 {
		return nil, fmt.Errorf("system recovery test failed: %d", ret)
	}

	return &RecoveryResultCLI{
		FrameSize:       uint32(result.frame_size),
		OverloadRatePct: float64(result.overload_rate_pct),
		RecoveryRatePct: float64(result.recovery_rate_pct),
		OverloadSec:     uint32(result.overload_sec),
		RecoveryTimeMs:  float64(result.recovery_time_ms),
		FramesLost:      uint64(result.frames_lost),
		Trials:          uint32(result.trials),
	}, nil
}

// RunResetTest runs RFC 2544 Section 26.6 Reset test
func (c *Context) RunResetTest() (*ResetResultCLI, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result C.reset_result_t

	ret := C.rfc2544_reset_test(c.ctx, C.uint32_t(c.frameSize), &result)
	if ret < 0 {
		return nil, fmt.Errorf("reset test failed: %d", ret)
	}

	return &ResetResultCLI{
		FrameSize:   uint32(result.frame_size),
		ResetTimeMs: float64(result.reset_time_ms),
		FramesLost:  uint64(result.frames_lost),
		Trials:      uint32(result.trials),
		ManualReset: bool(result.manual_reset),
	}, nil
}

// Internal wrappers for the existing methods
func (c *Context) runThroughputTestInternal(frameSize uint32) ([]ThroughputResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	maxResults := 8
	results := make([]C.throughput_result_t, maxResults)
	var count C.uint32_t

	ret := C.rfc2544_throughput_test(c.ctx, C.uint32_t(frameSize), &results[0], &count)
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

func (c *Context) runLatencyTestInternal(frameSize uint32, loadPct float64) (*LatencyResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	var result C.latency_result_t
	ret := C.rfc2544_latency_test(c.ctx, C.uint32_t(frameSize), C.double(loadPct), &result)
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

func (c *Context) runFrameLossTestInternal(frameSize uint32) ([]FrameLossPoint, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	maxResults := 20
	results := make([]C.frame_loss_point_t, maxResults)
	var count C.uint32_t

	ret := C.rfc2544_frame_loss_test(c.ctx, C.uint32_t(frameSize), &results[0], &count)
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

func (c *Context) runBackToBackTestInternal(frameSize uint32) (*BurstResult, error) {
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
