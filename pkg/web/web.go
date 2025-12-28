// Package web provides a web server and API for RFC2544 Test Master
package web

import (
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"sync"
	"time"
)

// Stats for API responses
type Stats struct {
	TestType    string  `json:"test_type"`
	FrameSize   uint32  `json:"frame_size"`
	State       string  `json:"state"`
	Progress    float64 `json:"progress"`
	Iteration   int     `json:"iteration"`
	MaxIter     int     `json:"max_iter"`
	TxPackets   uint64  `json:"tx_packets"`
	TxBytes     uint64  `json:"tx_bytes"`
	RxPackets   uint64  `json:"rx_packets"`
	RxBytes     uint64  `json:"rx_bytes"`
	TxRate      float64 `json:"tx_rate_mbps"`
	RxRate      float64 `json:"rx_rate_mbps"`
	TxPPS       float64 `json:"tx_pps"`
	RxPPS       float64 `json:"rx_pps"`
	OfferedRate float64 `json:"offered_rate_pct"`
	LossPct     float64 `json:"loss_pct"`
	LatencyMin  float64 `json:"latency_min_ns"`
	LatencyMax  float64 `json:"latency_max_ns"`
	LatencyAvg  float64 `json:"latency_avg_ns"`
	LatencyP99  float64 `json:"latency_p99_ns"`
	Uptime      float64 `json:"uptime_sec"`
	Timestamp   int64   `json:"timestamp"`
}

// Result for completed test
type Result struct {
	FrameSize    uint32  `json:"frame_size"`
	MaxRatePct   float64 `json:"max_rate_pct"`
	MaxRateMbps  float64 `json:"max_rate_mbps"`
	MaxRatePps   float64 `json:"max_rate_pps"`
	LossPct      float64 `json:"loss_pct"`
	LatencyAvgNs float64 `json:"latency_avg_ns"`
	LatencyMinNs float64 `json:"latency_min_ns"`
	LatencyMaxNs float64 `json:"latency_max_ns"`
	LatencyP99Ns float64 `json:"latency_p99_ns"`
	Timestamp    int64   `json:"timestamp"`
}

// Status constants for test state
const (
	StatusIdle     = "idle"
	StatusRunning  = "running"
	StatusComplete = "complete"
	StatusError    = "error"
	StatusCancelled = "cancelled"
)

// Config for test execution
type Config struct {
	Interface      string        `json:"interface"`
	TestType       int           `json:"test_type"`
	FrameSize      uint32        `json:"frame_size"`
	IncludeJumbo   bool          `json:"include_jumbo"`
	TrialDuration  time.Duration `json:"trial_duration"`
	LineRateMbps   uint64        `json:"line_rate_mbps"`
	HWTimestamp    bool          `json:"hw_timestamp"`
	InitialRatePct float64       `json:"initial_rate_pct"`
	ResolutionPct  float64       `json:"resolution_pct"`

	// Y.1564 specific configuration
	Y1564 *Y1564Config `json:"y1564,omitempty"`
}

// TestResult for generic test results
type TestResult struct {
	TestType  string                 `json:"test_type"`
	FrameSize uint32                 `json:"frame_size"`
	Data      map[string]interface{} `json:"data"`
	Timestamp int64                  `json:"timestamp"`
}

// Y1564Config for Y.1564 test configuration
type Y1564Config struct {
	Services        []Y1564Service `json:"services"`
	ConfigSteps     []float64      `json:"config_steps"`
	StepDurationSec int            `json:"step_duration_sec"`
	PerfDurationMin int            `json:"perf_duration_min"`
	RunConfigTest   bool           `json:"run_config_test"`
	RunPerfTest     bool           `json:"run_perf_test"`
}

// Y1564Service for Y.1564 service definition
type Y1564Service struct {
	ServiceID   uint32    `json:"service_id"`
	ServiceName string    `json:"service_name"`
	FrameSize   uint32    `json:"frame_size"`
	CoS         uint8     `json:"cos"`
	Enabled     bool      `json:"enabled"`
	SLA         Y1564SLA  `json:"sla"`
}

// Y1564SLA for Y.1564 SLA parameters
type Y1564SLA struct {
	CIRMbps         float64 `json:"cir_mbps"`
	EIRMbps         float64 `json:"eir_mbps"`
	CBSBytes        uint32  `json:"cbs_bytes"`
	EBSBytes        uint32  `json:"ebs_bytes"`
	FDThresholdMs   float64 `json:"fd_threshold_ms"`
	FDVThresholdMs  float64 `json:"fdv_threshold_ms"`
	FLRThresholdPct float64 `json:"flr_threshold_pct"`
}

// Y1564StepResult for Y.1564 step test results
type Y1564StepResult struct {
	Step            uint32  `json:"step"`
	OfferedRatePct  float64 `json:"offered_rate_pct"`
	AchievedRateMbps float64 `json:"achieved_rate_mbps"`
	FramesTx        uint64  `json:"frames_tx"`
	FramesRx        uint64  `json:"frames_rx"`
	FLRPct          float64 `json:"flr_pct"`
	FDAvgMs         float64 `json:"fd_avg_ms"`
	FDMinMs         float64 `json:"fd_min_ms"`
	FDMaxMs         float64 `json:"fd_max_ms"`
	FDVMs           float64 `json:"fdv_ms"`
	FLRPass         bool    `json:"flr_pass"`
	FDPass          bool    `json:"fd_pass"`
	FDVPass         bool    `json:"fdv_pass"`
	StepPass        bool    `json:"step_pass"`
}

// Y1564ConfigResult for Y.1564 configuration test results
type Y1564ConfigResult struct {
	ServiceID   uint32            `json:"service_id"`
	ServiceName string            `json:"service_name"`
	Steps       []Y1564StepResult `json:"steps"`
	ServicePass bool              `json:"service_pass"`
}

// Y1564PerfResult for Y.1564 performance test results
type Y1564PerfResult struct {
	ServiceID   uint32  `json:"service_id"`
	ServiceName string  `json:"service_name"`
	DurationSec uint32  `json:"duration_sec"`
	FramesTx    uint64  `json:"frames_tx"`
	FramesRx    uint64  `json:"frames_rx"`
	FLRPct      float64 `json:"flr_pct"`
	FDAvgMs     float64 `json:"fd_avg_ms"`
	FDMinMs     float64 `json:"fd_min_ms"`
	FDMaxMs     float64 `json:"fd_max_ms"`
	FDVMs       float64 `json:"fdv_ms"`
	FLRPass     bool    `json:"flr_pass"`
	FDPass      bool    `json:"fd_pass"`
	FDVPass     bool    `json:"fdv_pass"`
	ServicePass bool    `json:"service_pass"`
}

// Server represents the web server
type Server struct {
	addr    string
	mux     *http.ServeMux
	server  *http.Server
	mu      sync.RWMutex
	stats   Stats
	results []Result
	testResults []TestResult
	config  Config
	status  string
	statusMsg string
	progress float64

	// Embedded UI (optional)
	uiFS fs.FS

	// Callbacks
	OnStart  func(cfg Config) error
	OnStop   func() error
	OnCancel func()
}

// Option for server configuration
type Option func(*Server)

// WithUI sets the embedded UI filesystem
func WithUI(uiFS embed.FS, subdir string) Option {
	return func(s *Server) {
		sub, err := fs.Sub(uiFS, subdir)
		if err == nil {
			s.uiFS = sub
		}
	}
}

// New creates a new web server
func New(addr string, opts ...Option) *Server {
	s := &Server{
		addr:    addr,
		mux:     http.NewServeMux(),
		results: make([]Result, 0),
	}

	for _, opt := range opts {
		opt(s)
	}

	s.setupRoutes()
	return s
}

func (s *Server) setupRoutes() {
	// API routes
	s.mux.HandleFunc("/api/stats", s.handleStats)
	s.mux.HandleFunc("/api/results", s.handleResults)
	s.mux.HandleFunc("/api/config", s.handleConfig)
	s.mux.HandleFunc("/api/start", s.handleStart)
	s.mux.HandleFunc("/api/stop", s.handleStop)
	s.mux.HandleFunc("/api/cancel", s.handleCancel)
	s.mux.HandleFunc("/api/health", s.handleHealth)

	// Static UI (if embedded)
	if s.uiFS != nil {
		s.mux.Handle("/", http.FileServer(http.FS(s.uiFS)))
	} else {
		s.mux.HandleFunc("/", s.handleRoot)
	}
}

func (s *Server) handleRoot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html")
	fmt.Fprintf(w, `<!DOCTYPE html>
<html>
<head>
    <title>RFC2544 Test Master</title>
    <style>
        body { font-family: system-ui, sans-serif; background: #1a1a2e; color: #eee; margin: 40px; }
        h1 { color: #0f0; }
        h2 { color: #4da6ff; }
        h3 { color: #8f8; margin-top: 20px; }
        .card { background: #16213e; padding: 20px; border-radius: 8px; margin: 10px 0; }
        pre { background: #0f0f23; padding: 10px; border-radius: 4px; overflow-x: auto; font-size: 13px; }
        a { color: #4da6ff; }
        ul { margin: 10px 0; }
        li { margin: 5px 0; }
    </style>
</head>
<body>
    <h1>RFC2544 Test Master</h1>
    <div class="card">
        <h2>API Endpoints</h2>
        <ul>
            <li><a href="/api/stats">GET /api/stats</a> - Current statistics</li>
            <li><a href="/api/results">GET /api/results</a> - Test results</li>
            <li><a href="/api/config">GET /api/config</a> - Current configuration</li>
            <li>POST /api/start - Start test</li>
            <li>POST /api/stop - Stop test</li>
            <li>POST /api/cancel - Cancel test</li>
            <li><a href="/api/health">GET /api/health</a> - Health check</li>
        </ul>
    </div>
    <div class="card">
        <h2>RFC 2544 Tests</h2>
        <h3>Throughput Test</h3>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"throughput","frame_size":1518}'</pre>
    </div>
    <div class="card">
        <h2>ITU-T Y.1564 (EtherSAM) Tests</h2>
        <p>Y.1564 tests services against SLA parameters (CIR, FD, FDV, FLR)</p>
        <h3>Test Types</h3>
        <ul>
            <li><b>y1564_config</b> - Service Configuration Test (step test at 25%%, 50%%, 75%%, 100%% CIR)</li>
            <li><b>y1564_perf</b> - Service Performance Test (sustained traffic at CIR)</li>
            <li><b>y1564</b> - Full test (both config and perf phases)</li>
        </ul>
        <h3>Single Service Y.1564 Test</h3>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{
    "interface": "eth0",
    "test_type": "y1564",
    "y1564": {
      "services": [{
        "service_id": 1,
        "service_name": "Voice",
        "frame_size": 128,
        "cos": 46,
        "enabled": true,
        "sla": {
          "cir_mbps": 10,
          "eir_mbps": 0,
          "fd_threshold_ms": 10,
          "fdv_threshold_ms": 5,
          "flr_threshold_pct": 0.01
        }
      }],
      "config_steps": [25, 50, 75, 100],
      "step_duration_sec": 60,
      "perf_duration_min": 15,
      "run_config_test": true,
      "run_perf_test": true
    }
  }'</pre>
        <h3>Multi-Service Y.1564 Test</h3>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{
    "interface": "eth0",
    "test_type": "y1564",
    "y1564": {
      "services": [
        {"service_id": 1, "service_name": "Voice", "frame_size": 128, "cos": 46, "enabled": true,
         "sla": {"cir_mbps": 10, "fd_threshold_ms": 10, "fdv_threshold_ms": 5, "flr_threshold_pct": 0.01}},
        {"service_id": 2, "service_name": "Video", "frame_size": 1518, "cos": 34, "enabled": true,
         "sla": {"cir_mbps": 100, "eir_mbps": 50, "fd_threshold_ms": 50, "fdv_threshold_ms": 30, "flr_threshold_pct": 0.1}},
        {"service_id": 3, "service_name": "Data", "frame_size": 1518, "cos": 0, "enabled": true,
         "sla": {"cir_mbps": 500, "eir_mbps": 200, "fd_threshold_ms": 100, "fdv_threshold_ms": 50, "flr_threshold_pct": 0.5}}
      ],
      "perf_duration_min": 15
    }
  }'</pre>
    </div>
    <div class="card">
        <h2>RFC 2889 LAN Switch Benchmarking</h2>
        <h3>Test Types</h3>
        <ul>
            <li><b>rfc2889_forwarding</b> - Forwarding rate measurement</li>
            <li><b>rfc2889_caching</b> - Address caching capacity</li>
            <li><b>rfc2889_learning</b> - Address learning rate</li>
            <li><b>rfc2889_broadcast</b> - Broadcast forwarding</li>
            <li><b>rfc2889_congestion</b> - Congestion control</li>
        </ul>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"rfc2889_forwarding"}'</pre>
    </div>
    <div class="card">
        <h2>RFC 6349 TCP Throughput Testing</h2>
        <h3>Test Types</h3>
        <ul>
            <li><b>rfc6349_throughput</b> - TCP throughput measurement</li>
            <li><b>rfc6349_path</b> - Path analysis (RTT, bottleneck BW)</li>
        </ul>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"rfc6349_throughput"}'</pre>
    </div>
    <div class="card">
        <h2>ITU-T Y.1731 Ethernet OAM</h2>
        <h3>Test Types</h3>
        <ul>
            <li><b>y1731_delay</b> - Delay measurement (DMM/DMR)</li>
            <li><b>y1731_loss</b> - Loss measurement (LMM/LMR)</li>
            <li><b>y1731_slm</b> - Synthetic loss measurement</li>
            <li><b>y1731_loopback</b> - Loopback test (LBM/LBR)</li>
        </ul>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"y1731_delay"}'</pre>
    </div>
    <div class="card">
        <h2>MEF Service Activation</h2>
        <h3>Test Types</h3>
        <ul>
            <li><b>mef_config</b> - Configuration test (step)</li>
            <li><b>mef_perf</b> - Performance test (sustained)</li>
            <li><b>mef</b> - Full MEF test suite</li>
        </ul>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"mef"}'</pre>
    </div>
    <div class="card">
        <h2>IEEE 802.1Qbv TSN Testing</h2>
        <h3>Test Types</h3>
        <ul>
            <li><b>tsn_timing</b> - Gate timing accuracy</li>
            <li><b>tsn_isolation</b> - Traffic class isolation</li>
            <li><b>tsn_latency</b> - Scheduled latency</li>
            <li><b>tsn</b> - Full TSN test suite</li>
        </ul>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"tsn"}'</pre>
    </div>
</body>
</html>`, s.addr, s.addr, s.addr, s.addr, s.addr, s.addr, s.addr, s.addr)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":    "ok",
		"timestamp": time.Now().Unix(),
		"version":   "2.0.0",
	})
}

func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	s.mu.RLock()
	stats := s.stats
	s.mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(stats)
}

func (s *Server) handleResults(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	s.mu.RLock()
	results := make([]Result, len(s.results))
	copy(results, s.results)
	s.mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(results)
}

func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request) {
	s.mu.RLock()
	config := s.config
	s.mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(config)
}

func (s *Server) handleStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var cfg Config
	if err := json.NewDecoder(r.Body).Decode(&cfg); err != nil {
		http.Error(w, fmt.Sprintf("Invalid config: %v", err), http.StatusBadRequest)
		return
	}

	s.mu.Lock()
	s.config = cfg
	s.results = s.results[:0] // Clear previous results
	s.mu.Unlock()

	if s.OnStart != nil {
		if err := s.OnStart(cfg); err != nil {
			http.Error(w, fmt.Sprintf("Start failed: %v", err), http.StatusInternalServerError)
			return
		}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "started"})
}

func (s *Server) handleStop(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if s.OnStop != nil {
		if err := s.OnStop(); err != nil {
			http.Error(w, fmt.Sprintf("Stop failed: %v", err), http.StatusInternalServerError)
			return
		}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "stopped"})
}

func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if s.OnCancel != nil {
		s.OnCancel()
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "cancelled"})
}

// UpdateStats updates the current statistics
func (s *Server) UpdateStats(stats Stats) {
	s.mu.Lock()
	s.stats = stats
	s.mu.Unlock()
}

// AddResult adds a test result (legacy)
func (s *Server) AddLegacyResult(result Result) {
	s.mu.Lock()
	s.results = append(s.results, result)
	s.mu.Unlock()
}

// AddResult adds a generic test result
func (s *Server) AddResult(result TestResult) {
	result.Timestamp = time.Now().Unix()
	s.mu.Lock()
	s.testResults = append(s.testResults, result)
	s.mu.Unlock()
}

// UpdateStatus updates the test status
func (s *Server) UpdateStatus(status, message string, progress float64) {
	s.mu.Lock()
	s.status = status
	s.statusMsg = message
	s.progress = progress
	s.stats.State = status
	s.stats.Progress = progress
	s.mu.Unlock()
}

// ClearResults clears all results
func (s *Server) ClearResults() {
	s.mu.Lock()
	s.results = s.results[:0]
	s.testResults = s.testResults[:0]
	s.mu.Unlock()
}

// Start begins serving HTTP requests
func (s *Server) Start() error {
	s.server = &http.Server{
		Addr:         s.addr,
		Handler:      s.mux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
	}

	log.Printf("[web] Starting server on %s", s.addr)
	return s.server.ListenAndServe()
}

// Stop gracefully shuts down the server
func (s *Server) Stop() error {
	if s.server != nil {
		return s.server.Close()
	}
	return nil
}
