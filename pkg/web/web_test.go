// Package web tests for RFC2544 Test Master web server and API
package web

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// ============================================================================
// Server Creation Tests
// ============================================================================

func TestNew(t *testing.T) {
	s := New(":8080")
	if s == nil {
		t.Fatal("New() returned nil")
	}
	if s.addr != ":8080" {
		t.Errorf("Expected addr=:8080, got %s", s.addr)
	}
	if s.mux == nil {
		t.Error("Expected mux to be initialized")
	}
	if s.results == nil {
		t.Error("Expected results slice to be initialized")
	}
}

func TestNewWithDifferentPorts(t *testing.T) {
	tests := []struct {
		addr string
	}{
		{":8080"},
		{":9090"},
		{"localhost:3000"},
		{"0.0.0.0:80"},
	}

	for _, tc := range tests {
		s := New(tc.addr)
		if s.addr != tc.addr {
			t.Errorf("Expected addr=%s, got %s", tc.addr, s.addr)
		}
	}
}

// ============================================================================
// Health Endpoint Tests
// ============================================================================

func TestHandleHealth(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/health", nil)
	w := httptest.NewRecorder()

	s.handleHealth(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	var resp map[string]interface{}
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if resp["status"] != "ok" {
		t.Errorf("Expected status=ok, got %v", resp["status"])
	}
	if resp["version"] != "2.0.0" {
		t.Errorf("Expected version=2.0.0, got %v", resp["version"])
	}
	if _, ok := resp["timestamp"]; !ok {
		t.Error("Expected timestamp field in response")
	}
}

func TestHandleHealthContentType(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/health", nil)
	w := httptest.NewRecorder()

	s.handleHealth(w, req)

	contentType := w.Header().Get("Content-Type")
	if contentType != "application/json" {
		t.Errorf("Expected Content-Type=application/json, got %s", contentType)
	}
}

// ============================================================================
// Stats Endpoint Tests
// ============================================================================

func TestHandleStats(t *testing.T) {
	s := New(":8080")

	// Set some stats
	s.UpdateStats(Stats{
		TestType:   "throughput",
		FrameSize:  1518,
		State:      StatusRunning,
		Progress:   50.0,
		TxPackets:  1000000,
		RxPackets:  999000,
		LossPct:    0.1,
	})

	req := httptest.NewRequest(http.MethodGet, "/api/stats", nil)
	w := httptest.NewRecorder()

	s.handleStats(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	var stats Stats
	if err := json.NewDecoder(w.Body).Decode(&stats); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if stats.TestType != "throughput" {
		t.Errorf("Expected TestType=throughput, got %s", stats.TestType)
	}
	if stats.FrameSize != 1518 {
		t.Errorf("Expected FrameSize=1518, got %d", stats.FrameSize)
	}
	if stats.State != StatusRunning {
		t.Errorf("Expected State=%s, got %s", StatusRunning, stats.State)
	}
	if stats.Progress != 50.0 {
		t.Errorf("Expected Progress=50.0, got %f", stats.Progress)
	}
}

func TestHandleStatsMethodNotAllowed(t *testing.T) {
	s := New(":8080")

	methods := []string{http.MethodPost, http.MethodPut, http.MethodDelete}
	for _, method := range methods {
		req := httptest.NewRequest(method, "/api/stats", nil)
		w := httptest.NewRecorder()

		s.handleStats(w, req)

		if w.Code != http.StatusMethodNotAllowed {
			t.Errorf("Method %s: Expected status 405, got %d", method, w.Code)
		}
	}
}

// ============================================================================
// Results Endpoint Tests
// ============================================================================

func TestHandleResultsEmpty(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/results", nil)
	w := httptest.NewRecorder()

	s.handleResults(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	var results []Result
	if err := json.NewDecoder(w.Body).Decode(&results); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if len(results) != 0 {
		t.Errorf("Expected empty results, got %d", len(results))
	}
}

func TestHandleResultsWithData(t *testing.T) {
	s := New(":8080")

	// Add some results
	s.AddLegacyResult(Result{
		FrameSize:   64,
		MaxRatePct:  99.5,
		MaxRateMbps: 995.0,
		LossPct:     0.0,
	})
	s.AddLegacyResult(Result{
		FrameSize:   1518,
		MaxRatePct:  100.0,
		MaxRateMbps: 1000.0,
		LossPct:     0.0,
	})

	req := httptest.NewRequest(http.MethodGet, "/api/results", nil)
	w := httptest.NewRecorder()

	s.handleResults(w, req)

	var results []Result
	if err := json.NewDecoder(w.Body).Decode(&results); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if len(results) != 2 {
		t.Errorf("Expected 2 results, got %d", len(results))
	}
	if results[0].FrameSize != 64 {
		t.Errorf("Expected first result FrameSize=64, got %d", results[0].FrameSize)
	}
	if results[1].FrameSize != 1518 {
		t.Errorf("Expected second result FrameSize=1518, got %d", results[1].FrameSize)
	}
}

func TestHandleResultsMethodNotAllowed(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodPost, "/api/results", nil)
	w := httptest.NewRecorder()

	s.handleResults(w, req)

	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("Expected status 405, got %d", w.Code)
	}
}

// ============================================================================
// Config Endpoint Tests
// ============================================================================

func TestHandleConfig(t *testing.T) {
	s := New(":8080")

	// Set config
	s.mu.Lock()
	s.config = Config{
		Interface:      "eth0",
		TestType:       0, // throughput
		FrameSize:      1518,
		LineRateMbps:   10000,
		InitialRatePct: 100.0,
		ResolutionPct:  0.1,
	}
	s.mu.Unlock()

	req := httptest.NewRequest(http.MethodGet, "/api/config", nil)
	w := httptest.NewRecorder()

	s.handleConfig(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	var cfg Config
	if err := json.NewDecoder(w.Body).Decode(&cfg); err != nil {
		t.Fatalf("Failed to decode response: %v", err)
	}

	if cfg.Interface != "eth0" {
		t.Errorf("Expected Interface=eth0, got %s", cfg.Interface)
	}
	if cfg.FrameSize != 1518 {
		t.Errorf("Expected FrameSize=1518, got %d", cfg.FrameSize)
	}
}

// ============================================================================
// Start Endpoint Tests
// ============================================================================

func TestHandleStartSuccess(t *testing.T) {
	s := New(":8080")

	var startCalled bool
	var receivedConfig Config
	s.OnStart = func(cfg Config) error {
		startCalled = true
		receivedConfig = cfg
		return nil
	}

	body := `{"interface":"eth0","test_type":0,"frame_size":1518}`
	req := httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	if !startCalled {
		t.Error("OnStart callback was not called")
	}

	if receivedConfig.Interface != "eth0" {
		t.Errorf("Expected Interface=eth0, got %s", receivedConfig.Interface)
	}
}

func TestHandleStartInvalidJSON(t *testing.T) {
	s := New(":8080")

	body := `{invalid json`
	req := httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(body))
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	if w.Code != http.StatusBadRequest {
		t.Errorf("Expected status 400, got %d", w.Code)
	}
}

func TestHandleStartMethodNotAllowed(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/start", nil)
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("Expected status 405, got %d", w.Code)
	}
}

func TestHandleStartClearsResults(t *testing.T) {
	s := New(":8080")

	// Add some previous results
	s.AddLegacyResult(Result{FrameSize: 64})
	s.AddLegacyResult(Result{FrameSize: 128})

	s.OnStart = func(cfg Config) error { return nil }

	body := `{"interface":"eth0"}`
	req := httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(body))
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	s.mu.RLock()
	resultCount := len(s.results)
	s.mu.RUnlock()

	if resultCount != 0 {
		t.Errorf("Expected results to be cleared, got %d results", resultCount)
	}
}

// ============================================================================
// Stop Endpoint Tests
// ============================================================================

func TestHandleStopSuccess(t *testing.T) {
	s := New(":8080")

	var stopCalled bool
	s.OnStop = func() error {
		stopCalled = true
		return nil
	}

	req := httptest.NewRequest(http.MethodPost, "/api/stop", nil)
	w := httptest.NewRecorder()

	s.handleStop(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	if !stopCalled {
		t.Error("OnStop callback was not called")
	}
}

func TestHandleStopMethodNotAllowed(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/stop", nil)
	w := httptest.NewRecorder()

	s.handleStop(w, req)

	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("Expected status 405, got %d", w.Code)
	}
}

// ============================================================================
// Cancel Endpoint Tests
// ============================================================================

func TestHandleCancelSuccess(t *testing.T) {
	s := New(":8080")

	var cancelCalled bool
	s.OnCancel = func() {
		cancelCalled = true
	}

	req := httptest.NewRequest(http.MethodPost, "/api/cancel", nil)
	w := httptest.NewRecorder()

	s.handleCancel(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	if !cancelCalled {
		t.Error("OnCancel callback was not called")
	}
}

func TestHandleCancelMethodNotAllowed(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/cancel", nil)
	w := httptest.NewRecorder()

	s.handleCancel(w, req)

	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("Expected status 405, got %d", w.Code)
	}
}

// ============================================================================
// Root/Index Endpoint Tests
// ============================================================================

func TestHandleRootHTML(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	w := httptest.NewRecorder()

	s.handleRoot(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}

	contentType := w.Header().Get("Content-Type")
	if contentType != "text/html" {
		t.Errorf("Expected Content-Type=text/html, got %s", contentType)
	}

	body := w.Body.String()
	if !strings.Contains(body, "RFC2544 Test Master") {
		t.Error("Expected HTML to contain 'RFC2544 Test Master'")
	}
	if !strings.Contains(body, "/api/stats") {
		t.Error("Expected HTML to contain API endpoint documentation")
	}
}

func TestHandleRootY1564Documentation(t *testing.T) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	w := httptest.NewRecorder()

	s.handleRoot(w, req)

	body := w.Body.String()

	// Check Y.1564 documentation is present
	if !strings.Contains(body, "Y.1564") {
		t.Error("Expected HTML to contain Y.1564 documentation")
	}
	if !strings.Contains(body, "EtherSAM") {
		t.Error("Expected HTML to contain EtherSAM reference")
	}
}

// ============================================================================
// UpdateStats Tests
// ============================================================================

func TestUpdateStats(t *testing.T) {
	s := New(":8080")

	stats := Stats{
		TestType:   "latency",
		FrameSize:  512,
		State:      StatusRunning,
		Progress:   75.0,
		TxPackets:  5000000,
		RxPackets:  4999000,
		LatencyAvg: 1500.0,
	}

	s.UpdateStats(stats)

	s.mu.RLock()
	result := s.stats
	s.mu.RUnlock()

	if result.TestType != "latency" {
		t.Errorf("Expected TestType=latency, got %s", result.TestType)
	}
	if result.Progress != 75.0 {
		t.Errorf("Expected Progress=75.0, got %f", result.Progress)
	}
}

func TestUpdateStatsConcurrent(t *testing.T) {
	s := New(":8080")
	var wg sync.WaitGroup

	// Concurrent updates
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			s.UpdateStats(Stats{
				Progress: float64(idx),
			})
		}(i)
	}

	// Concurrent reads
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s.mu.RLock()
			_ = s.stats.Progress
			s.mu.RUnlock()
		}()
	}

	wg.Wait()
	// Test passes if no race condition panics
}

// ============================================================================
// AddResult Tests
// ============================================================================

func TestAddLegacyResult(t *testing.T) {
	s := New(":8080")

	s.AddLegacyResult(Result{FrameSize: 64, MaxRatePct: 99.0})
	s.AddLegacyResult(Result{FrameSize: 128, MaxRatePct: 99.5})
	s.AddLegacyResult(Result{FrameSize: 256, MaxRatePct: 100.0})

	s.mu.RLock()
	count := len(s.results)
	s.mu.RUnlock()

	if count != 3 {
		t.Errorf("Expected 3 results, got %d", count)
	}
}

func TestAddResult(t *testing.T) {
	s := New(":8080")

	s.AddResult(TestResult{
		TestType:  "y1564",
		FrameSize: 1518,
		Data: map[string]interface{}{
			"service_id": 1,
			"pass":       true,
		},
	})

	s.mu.RLock()
	count := len(s.testResults)
	result := s.testResults[0]
	s.mu.RUnlock()

	if count != 1 {
		t.Errorf("Expected 1 test result, got %d", count)
	}
	if result.TestType != "y1564" {
		t.Errorf("Expected TestType=y1564, got %s", result.TestType)
	}
	if result.Timestamp == 0 {
		t.Error("Expected Timestamp to be set")
	}
}

// ============================================================================
// UpdateStatus Tests
// ============================================================================

func TestUpdateStatus(t *testing.T) {
	s := New(":8080")

	s.UpdateStatus(StatusRunning, "Testing frame size 1518", 25.0)

	s.mu.RLock()
	status := s.status
	msg := s.statusMsg
	progress := s.progress
	statsState := s.stats.State
	statsProgress := s.stats.Progress
	s.mu.RUnlock()

	if status != StatusRunning {
		t.Errorf("Expected status=%s, got %s", StatusRunning, status)
	}
	if msg != "Testing frame size 1518" {
		t.Errorf("Expected message='Testing frame size 1518', got '%s'", msg)
	}
	if progress != 25.0 {
		t.Errorf("Expected progress=25.0, got %f", progress)
	}
	if statsState != StatusRunning {
		t.Errorf("Expected stats.State=%s, got %s", StatusRunning, statsState)
	}
	if statsProgress != 25.0 {
		t.Errorf("Expected stats.Progress=25.0, got %f", statsProgress)
	}
}

func TestUpdateStatusAllStates(t *testing.T) {
	s := New(":8080")

	states := []string{StatusIdle, StatusRunning, StatusComplete, StatusError, StatusCancelled}
	for _, state := range states {
		s.UpdateStatus(state, "", 0.0)

		s.mu.RLock()
		got := s.status
		s.mu.RUnlock()

		if got != state {
			t.Errorf("Expected status=%s, got %s", state, got)
		}
	}
}

// ============================================================================
// ClearResults Tests
// ============================================================================

func TestClearResults(t *testing.T) {
	s := New(":8080")

	// Add some results
	s.AddLegacyResult(Result{FrameSize: 64})
	s.AddLegacyResult(Result{FrameSize: 128})
	s.AddResult(TestResult{TestType: "y1564"})

	s.ClearResults()

	s.mu.RLock()
	legacyCount := len(s.results)
	testCount := len(s.testResults)
	s.mu.RUnlock()

	if legacyCount != 0 {
		t.Errorf("Expected 0 legacy results, got %d", legacyCount)
	}
	if testCount != 0 {
		t.Errorf("Expected 0 test results, got %d", testCount)
	}
}

// ============================================================================
// Status Constants Tests
// ============================================================================

func TestStatusConstants(t *testing.T) {
	if StatusIdle != "idle" {
		t.Errorf("Expected StatusIdle='idle', got '%s'", StatusIdle)
	}
	if StatusRunning != "running" {
		t.Errorf("Expected StatusRunning='running', got '%s'", StatusRunning)
	}
	if StatusComplete != "complete" {
		t.Errorf("Expected StatusComplete='complete', got '%s'", StatusComplete)
	}
	if StatusError != "error" {
		t.Errorf("Expected StatusError='error', got '%s'", StatusError)
	}
	if StatusCancelled != "cancelled" {
		t.Errorf("Expected StatusCancelled='cancelled', got '%s'", StatusCancelled)
	}
}

// ============================================================================
// Y.1564 Configuration Tests
// ============================================================================

func TestY1564ConfigSerialization(t *testing.T) {
	cfg := Y1564Config{
		Services: []Y1564Service{
			{
				ServiceID:   1,
				ServiceName: "Voice",
				FrameSize:   128,
				CoS:         46,
				Enabled:     true,
				SLA: Y1564SLA{
					CIRMbps:         10.0,
					EIRMbps:         0.0,
					FDThresholdMs:   10.0,
					FDVThresholdMs:  5.0,
					FLRThresholdPct: 0.01,
				},
			},
		},
		ConfigSteps:     []float64{25, 50, 75, 100},
		StepDurationSec: 60,
		PerfDurationMin: 15,
		RunConfigTest:   true,
		RunPerfTest:     true,
	}

	// Serialize to JSON
	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("Failed to marshal Y1564Config: %v", err)
	}

	// Deserialize back
	var decoded Y1564Config
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Failed to unmarshal Y1564Config: %v", err)
	}

	if len(decoded.Services) != 1 {
		t.Errorf("Expected 1 service, got %d", len(decoded.Services))
	}
	if decoded.Services[0].ServiceName != "Voice" {
		t.Errorf("Expected ServiceName='Voice', got '%s'", decoded.Services[0].ServiceName)
	}
	if decoded.Services[0].SLA.CIRMbps != 10.0 {
		t.Errorf("Expected CIRMbps=10.0, got %f", decoded.Services[0].SLA.CIRMbps)
	}
}

func TestY1564StepResultSerialization(t *testing.T) {
	result := Y1564StepResult{
		Step:            1,
		OfferedRatePct:  25.0,
		AchievedRateMbps: 2.5,
		FramesTx:        100000,
		FramesRx:        99990,
		FLRPct:          0.01,
		FDAvgMs:         5.0,
		FDMinMs:         1.0,
		FDMaxMs:         10.0,
		FDVMs:           9.0,
		FLRPass:         true,
		FDPass:          true,
		FDVPass:         false,
		StepPass:        false,
	}

	data, err := json.Marshal(result)
	if err != nil {
		t.Fatalf("Failed to marshal Y1564StepResult: %v", err)
	}

	var decoded Y1564StepResult
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Failed to unmarshal Y1564StepResult: %v", err)
	}

	if decoded.Step != 1 {
		t.Errorf("Expected Step=1, got %d", decoded.Step)
	}
	if decoded.StepPass != false {
		t.Error("Expected StepPass=false")
	}
}

// ============================================================================
// Integration Tests
// ============================================================================

func TestFullAPIWorkflow(t *testing.T) {
	s := New(":8080")

	// Setup callbacks
	var testStarted, testStopped bool
	s.OnStart = func(cfg Config) error {
		testStarted = true
		return nil
	}
	s.OnStop = func() error {
		testStopped = true
		return nil
	}

	// 1. Check health
	req := httptest.NewRequest(http.MethodGet, "/api/health", nil)
	w := httptest.NewRecorder()
	s.handleHealth(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("Health check failed: %d", w.Code)
	}

	// 2. Start test
	startBody := `{"interface":"eth0","test_type":0,"frame_size":1518}`
	req = httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(startBody))
	w = httptest.NewRecorder()
	s.handleStart(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("Start failed: %d", w.Code)
	}
	if !testStarted {
		t.Error("OnStart not called")
	}

	// 3. Update stats during test
	s.UpdateStats(Stats{
		TestType:  "throughput",
		FrameSize: 1518,
		State:     StatusRunning,
		Progress:  50.0,
	})

	// 4. Check stats
	req = httptest.NewRequest(http.MethodGet, "/api/stats", nil)
	w = httptest.NewRecorder()
	s.handleStats(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("Stats check failed: %d", w.Code)
	}

	// 5. Add result
	s.AddLegacyResult(Result{
		FrameSize:   1518,
		MaxRatePct:  100.0,
		MaxRateMbps: 1000.0,
	})

	// 6. Stop test
	req = httptest.NewRequest(http.MethodPost, "/api/stop", nil)
	w = httptest.NewRecorder()
	s.handleStop(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("Stop failed: %d", w.Code)
	}
	if !testStopped {
		t.Error("OnStop not called")
	}

	// 7. Check results
	req = httptest.NewRequest(http.MethodGet, "/api/results", nil)
	w = httptest.NewRecorder()
	s.handleResults(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("Results check failed: %d", w.Code)
	}

	var results []Result
	json.NewDecoder(w.Body).Decode(&results)
	if len(results) != 1 {
		t.Errorf("Expected 1 result, got %d", len(results))
	}
}

// ============================================================================
// Benchmarks
// ============================================================================

func BenchmarkHandleStats(b *testing.B) {
	s := New(":8080")
	s.UpdateStats(Stats{
		TestType:  "throughput",
		FrameSize: 1518,
		TxPackets: 1000000,
		RxPackets: 999000,
	})

	req := httptest.NewRequest(http.MethodGet, "/api/stats", nil)

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		w := httptest.NewRecorder()
		s.handleStats(w, req)
	}
}

func BenchmarkHandleHealth(b *testing.B) {
	s := New(":8080")
	req := httptest.NewRequest(http.MethodGet, "/api/health", nil)

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		w := httptest.NewRecorder()
		s.handleHealth(w, req)
	}
}

func BenchmarkUpdateStats(b *testing.B) {
	s := New(":8080")
	stats := Stats{
		TestType:  "throughput",
		TxPackets: 1000000,
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.UpdateStats(stats)
	}
}

func BenchmarkAddResult(b *testing.B) {
	s := New(":8080")
	result := Result{
		FrameSize:   1518,
		MaxRatePct:  100.0,
		MaxRateMbps: 1000.0,
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.AddLegacyResult(result)
	}
}

func BenchmarkHandleStartDecode(b *testing.B) {
	s := New(":8080")
	s.OnStart = func(cfg Config) error { return nil }

	body := `{"interface":"eth0","test_type":0,"frame_size":1518,"line_rate_mbps":10000}`

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		req := httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(body))
		w := httptest.NewRecorder()
		s.handleStart(w, req)
	}
}

func BenchmarkConcurrentStatsAccess(b *testing.B) {
	s := New(":8080")

	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			s.UpdateStats(Stats{Progress: 50.0})
			s.mu.RLock()
			_ = s.stats.Progress
			s.mu.RUnlock()
		}
	})
}

// ============================================================================
// Edge Cases
// ============================================================================

func TestHandleStartEmptyBody(t *testing.T) {
	s := New(":8080")
	s.OnStart = func(cfg Config) error { return nil }

	req := httptest.NewRequest(http.MethodPost, "/api/start", bytes.NewReader([]byte{}))
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	// Empty body should fail to decode
	if w.Code != http.StatusBadRequest {
		t.Errorf("Expected status 400, got %d", w.Code)
	}
}

func TestHandleStartNoCallback(t *testing.T) {
	s := New(":8080")
	// Don't set OnStart callback

	body := `{"interface":"eth0"}`
	req := httptest.NewRequest(http.MethodPost, "/api/start", strings.NewReader(body))
	w := httptest.NewRecorder()

	s.handleStart(w, req)

	// Should succeed even without callback
	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}
}

func TestHandleStopNoCallback(t *testing.T) {
	s := New(":8080")
	// Don't set OnStop callback

	req := httptest.NewRequest(http.MethodPost, "/api/stop", nil)
	w := httptest.NewRecorder()

	s.handleStop(w, req)

	// Should succeed even without callback
	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}
}

func TestHandleCancelNoCallback(t *testing.T) {
	s := New(":8080")
	// Don't set OnCancel callback

	req := httptest.NewRequest(http.MethodPost, "/api/cancel", nil)
	w := httptest.NewRecorder()

	s.handleCancel(w, req)

	// Should succeed even without callback
	if w.Code != http.StatusOK {
		t.Errorf("Expected status 200, got %d", w.Code)
	}
}

func TestServerStopNilServer(t *testing.T) {
	s := New(":8080")
	// s.server is nil until Start() is called

	err := s.Stop()
	if err != nil {
		t.Errorf("Expected no error when stopping nil server, got %v", err)
	}
}

// ============================================================================
// Type Serialization Tests
// ============================================================================

func TestStatsSerialization(t *testing.T) {
	stats := Stats{
		TestType:    "throughput",
		FrameSize:   1518,
		State:       StatusRunning,
		Progress:    50.0,
		Iteration:   5,
		MaxIter:     10,
		TxPackets:   1000000,
		TxBytes:     1518000000,
		RxPackets:   999000,
		RxBytes:     1516482000,
		TxRate:      1000.0,
		RxRate:      999.0,
		TxPPS:       812744.0,
		RxPPS:       811931.0,
		OfferedRate: 100.0,
		LossPct:     0.1,
		LatencyMin:  500.0,
		LatencyMax:  5000.0,
		LatencyAvg:  1500.0,
		LatencyP99:  4500.0,
		Uptime:      30.5,
		Timestamp:   time.Now().Unix(),
	}

	data, err := json.Marshal(stats)
	if err != nil {
		t.Fatalf("Failed to marshal Stats: %v", err)
	}

	var decoded Stats
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Failed to unmarshal Stats: %v", err)
	}

	if decoded.TestType != stats.TestType {
		t.Errorf("TestType mismatch: expected %s, got %s", stats.TestType, decoded.TestType)
	}
	if decoded.TxPackets != stats.TxPackets {
		t.Errorf("TxPackets mismatch: expected %d, got %d", stats.TxPackets, decoded.TxPackets)
	}
}

func TestResultSerialization(t *testing.T) {
	result := Result{
		FrameSize:    1518,
		MaxRatePct:   99.5,
		MaxRateMbps:  995.0,
		MaxRatePps:   654321.0,
		LossPct:      0.0,
		LatencyAvgNs: 1500.0,
		LatencyMinNs: 500.0,
		LatencyMaxNs: 5000.0,
		LatencyP99Ns: 4500.0,
		Timestamp:    time.Now().Unix(),
	}

	data, err := json.Marshal(result)
	if err != nil {
		t.Fatalf("Failed to marshal Result: %v", err)
	}

	var decoded Result
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Failed to unmarshal Result: %v", err)
	}

	if decoded.FrameSize != result.FrameSize {
		t.Errorf("FrameSize mismatch: expected %d, got %d", result.FrameSize, decoded.FrameSize)
	}
	if decoded.MaxRatePct != result.MaxRatePct {
		t.Errorf("MaxRatePct mismatch: expected %f, got %f", result.MaxRatePct, decoded.MaxRatePct)
	}
}

func TestConfigSerialization(t *testing.T) {
	cfg := Config{
		Interface:      "eth0",
		TestType:       0,
		FrameSize:      1518,
		IncludeJumbo:   true,
		TrialDuration:  60 * time.Second,
		LineRateMbps:   10000,
		HWTimestamp:    true,
		InitialRatePct: 100.0,
		ResolutionPct:  0.1,
		Y1564: &Y1564Config{
			Services: []Y1564Service{
				{ServiceID: 1, ServiceName: "Test", Enabled: true},
			},
		},
	}

	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("Failed to marshal Config: %v", err)
	}

	var decoded Config
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("Failed to unmarshal Config: %v", err)
	}

	if decoded.Interface != cfg.Interface {
		t.Errorf("Interface mismatch: expected %s, got %s", cfg.Interface, decoded.Interface)
	}
	if decoded.Y1564 == nil {
		t.Error("Expected Y1564 config to be present")
	}
}
