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

// Config for test execution
type Config struct {
	Interface      string  `json:"interface"`
	TestType       string  `json:"test_type"`
	FrameSize      uint32  `json:"frame_size"`
	IncludeJumbo   bool    `json:"include_jumbo"`
	TrialDuration  int     `json:"trial_duration_sec"`
	InitialRatePct float64 `json:"initial_rate_pct"`
	ResolutionPct  float64 `json:"resolution_pct"`
}

// Server represents the web server
type Server struct {
	addr    string
	mux     *http.ServeMux
	server  *http.Server
	mu      sync.RWMutex
	stats   Stats
	results []Result
	config  Config

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
        .card { background: #16213e; padding: 20px; border-radius: 8px; margin: 10px 0; }
        pre { background: #0f0f23; padding: 10px; border-radius: 4px; overflow-x: auto; }
        a { color: #4da6ff; }
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
        <h2>Start Test</h2>
        <pre>curl -X POST http://localhost%s/api/start \
  -H "Content-Type: application/json" \
  -d '{"interface":"eth0","test_type":"throughput","frame_size":1518}'</pre>
    </div>
</body>
</html>`, s.addr)
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

// AddResult adds a test result
func (s *Server) AddResult(result Result) {
	s.mu.Lock()
	s.results = append(s.results, result)
	s.mu.Unlock()
}

// ClearResults clears all results
func (s *Server) ClearResults() {
	s.mu.Lock()
	s.results = s.results[:0]
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
