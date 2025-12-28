// Package config provides YAML configuration support for RFC2544 Test Master
package config

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// ============================================================================
// DefaultConfig Tests
// ============================================================================

func TestDefaultConfig(t *testing.T) {
	cfg := DefaultConfig()

	if cfg == nil {
		t.Fatal("DefaultConfig() returned nil")
	}

	// Test default test type
	if cfg.TestType != TestThroughput {
		t.Errorf("Expected TestType=%s, got %s", TestThroughput, cfg.TestType)
	}

	// Test default durations
	if cfg.TrialDuration != 60*time.Second {
		t.Errorf("Expected TrialDuration=60s, got %v", cfg.TrialDuration)
	}

	if cfg.WarmupPeriod != 2*time.Second {
		t.Errorf("Expected WarmupPeriod=2s, got %v", cfg.WarmupPeriod)
	}
}

func TestDefaultConfigThroughput(t *testing.T) {
	cfg := DefaultConfig()

	if cfg.Throughput.InitialRatePct != 100.0 {
		t.Errorf("Expected InitialRatePct=100.0, got %f", cfg.Throughput.InitialRatePct)
	}

	if cfg.Throughput.ResolutionPct != 0.1 {
		t.Errorf("Expected ResolutionPct=0.1, got %f", cfg.Throughput.ResolutionPct)
	}

	if cfg.Throughput.MaxIterations != 20 {
		t.Errorf("Expected MaxIterations=20, got %d", cfg.Throughput.MaxIterations)
	}

	if cfg.Throughput.AcceptableLoss != 0.0 {
		t.Errorf("Expected AcceptableLoss=0.0, got %f", cfg.Throughput.AcceptableLoss)
	}
}

func TestDefaultConfigLatency(t *testing.T) {
	cfg := DefaultConfig()

	if cfg.Latency.Samples != 1000 {
		t.Errorf("Expected Samples=1000, got %d", cfg.Latency.Samples)
	}

	if len(cfg.Latency.LoadLevels) != 10 {
		t.Errorf("Expected 10 load levels, got %d", len(cfg.Latency.LoadLevels))
	}
}

func TestDefaultConfigFrameLoss(t *testing.T) {
	cfg := DefaultConfig()

	if cfg.FrameLoss.StartPct != 100.0 {
		t.Errorf("Expected StartPct=100.0, got %f", cfg.FrameLoss.StartPct)
	}

	if cfg.FrameLoss.EndPct != 10.0 {
		t.Errorf("Expected EndPct=10.0, got %f", cfg.FrameLoss.EndPct)
	}

	if cfg.FrameLoss.StepPct != 10.0 {
		t.Errorf("Expected StepPct=10.0, got %f", cfg.FrameLoss.StepPct)
	}
}

func TestDefaultConfigBackToBack(t *testing.T) {
	cfg := DefaultConfig()

	if cfg.BackToBack.InitialBurst != 1000 {
		t.Errorf("Expected InitialBurst=1000, got %d", cfg.BackToBack.InitialBurst)
	}

	if cfg.BackToBack.Trials != 50 {
		t.Errorf("Expected Trials=50, got %d", cfg.BackToBack.Trials)
	}
}

// ============================================================================
// Protocol Default Config Tests
// ============================================================================

func TestDefaultY1564Config(t *testing.T) {
	cfg := DefaultY1564Config()

	if len(cfg.ConfigSteps) != 4 {
		t.Errorf("Expected 4 config steps, got %d", len(cfg.ConfigSteps))
	}

	expectedSteps := []float64{25, 50, 75, 100}
	for i, step := range cfg.ConfigSteps {
		if step != expectedSteps[i] {
			t.Errorf("Step %d: expected %f, got %f", i, expectedSteps[i], step)
		}
	}

	if cfg.StepDuration != 60*time.Second {
		t.Errorf("Expected StepDuration=60s, got %v", cfg.StepDuration)
	}

	if cfg.PerfDuration != 15*time.Minute {
		t.Errorf("Expected PerfDuration=15m, got %v", cfg.PerfDuration)
	}
}

func TestDefaultY1564SLA(t *testing.T) {
	sla := DefaultY1564SLA()

	if sla.CIRMbps != 100.0 {
		t.Errorf("Expected CIRMbps=100.0, got %f", sla.CIRMbps)
	}

	if sla.FDThresholdMs != 10.0 {
		t.Errorf("Expected FDThresholdMs=10.0, got %f", sla.FDThresholdMs)
	}

	if sla.FDVThresholdMs != 5.0 {
		t.Errorf("Expected FDVThresholdMs=5.0, got %f", sla.FDVThresholdMs)
	}

	if sla.FLRThresholdPct != 0.01 {
		t.Errorf("Expected FLRThresholdPct=0.01, got %f", sla.FLRThresholdPct)
	}
}

func TestDefaultRFC2889Config(t *testing.T) {
	cfg := DefaultRFC2889Config()

	if cfg.PortCount != 2 {
		t.Errorf("Expected PortCount=2, got %d", cfg.PortCount)
	}

	if cfg.AddressCount != 8192 {
		t.Errorf("Expected AddressCount=8192, got %d", cfg.AddressCount)
	}
}

func TestDefaultRFC6349Config(t *testing.T) {
	cfg := DefaultRFC6349Config()

	if cfg.MSS != 1460 {
		t.Errorf("Expected MSS=1460, got %d", cfg.MSS)
	}

	if cfg.RWND != 65535 {
		t.Errorf("Expected RWND=65535, got %d", cfg.RWND)
	}
}

func TestDefaultY1731Config(t *testing.T) {
	cfg := DefaultY1731Config()

	if cfg.MEPID != 1 {
		t.Errorf("Expected MEPID=1, got %d", cfg.MEPID)
	}

	if cfg.MEGLevel != 4 {
		t.Errorf("Expected MEGLevel=4, got %d", cfg.MEGLevel)
	}

	if cfg.MEGID != "DEFAULT-MEG" {
		t.Errorf("Expected MEGID='DEFAULT-MEG', got '%s'", cfg.MEGID)
	}
}

func TestDefaultMEFConfig(t *testing.T) {
	cfg := DefaultMEFConfig()

	if cfg.CIRMbps != 100.0 {
		t.Errorf("Expected CIRMbps=100.0, got %f", cfg.CIRMbps)
	}

	if cfg.CBSBytes != 12000 {
		t.Errorf("Expected CBSBytes=12000, got %d", cfg.CBSBytes)
	}
}

func TestDefaultTSNConfig(t *testing.T) {
	cfg := DefaultTSNConfig()

	if cfg.NumClasses != 8 {
		t.Errorf("Expected NumClasses=8, got %d", cfg.NumClasses)
	}

	if cfg.CycleTimeNs != 1000000 {
		t.Errorf("Expected CycleTimeNs=1000000, got %d", cfg.CycleTimeNs)
	}
}

// ============================================================================
// Validation Tests
// ============================================================================

func TestValidateNoInterface(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = ""

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for missing interface")
	}
}

func TestValidateValidConfig(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"

	err := cfg.Validate()
	if err != nil {
		t.Errorf("Unexpected error: %v", err)
	}
}

func TestValidateInvalidTestType(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = "invalid_test"

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for invalid test type")
	}
}

func TestValidateInvalidFrameSize(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.FrameSize = 100 // Not a standard size

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for invalid frame size")
	}
}

func TestValidateValidFrameSizes(t *testing.T) {
	validSizes := []uint32{0, 64, 128, 256, 512, 1024, 1280, 1518, 9000}

	for _, size := range validSizes {
		cfg := DefaultConfig()
		cfg.Interface = "eth0"
		cfg.FrameSize = size

		err := cfg.Validate()
		if err != nil {
			t.Errorf("Frame size %d should be valid, got error: %v", size, err)
		}
	}
}

func TestValidateInvalidResolution(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.Throughput.ResolutionPct = 0.0

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for zero resolution")
	}

	cfg.Throughput.ResolutionPct = 15.0
	err = cfg.Validate()
	if err == nil {
		t.Error("Expected error for resolution > 10")
	}
}

func TestValidateInvalidFrameLoss(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.FrameLoss.StartPct = 10.0
	cfg.FrameLoss.EndPct = 100.0 // Start < End is invalid

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for start < end in frame loss")
	}
}

func TestValidateY1564NoServices(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestY1564Full
	cfg.Y1564.Services = []Y1564Service{} // No services

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for Y.1564 without services")
	}
}

func TestValidateY1564ZeroCIR(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestY1564Full
	cfg.Y1564.Services = []Y1564Service{
		{ServiceID: 1, Enabled: true, SLA: Y1564SLA{CIRMbps: 0}},
	}

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for Y.1564 with zero CIR")
	}
}

func TestValidateRFC2889InsufficientPorts(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestRFC2889Forwarding
	cfg.RFC2889.PortCount = 1 // Need at least 2

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for RFC 2889 with < 2 ports")
	}
}

func TestValidateRFC6349ZeroMSS(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestRFC6349Throughput
	cfg.RFC6349.MSS = 0

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for RFC 6349 with zero MSS")
	}
}

func TestValidateY1731ZeroMEPID(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestY1731Delay
	cfg.Y1731.MEPID = 0

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for Y.1731 with zero MEP ID")
	}
}

func TestValidateMEFZeroCIR(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestMEFFull
	cfg.MEF.CIRMbps = 0

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for MEF with zero CIR")
	}
}

func TestValidateTSNZeroCycleTime(t *testing.T) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestTSNFull
	cfg.TSN.CycleTimeNs = 0

	err := cfg.Validate()
	if err == nil {
		t.Error("Expected error for TSN with zero cycle time")
	}
}

// ============================================================================
// StandardFrameSizes Tests
// ============================================================================

func TestStandardFrameSizes(t *testing.T) {
	sizes := StandardFrameSizes(false)

	expected := []uint32{64, 128, 256, 512, 1024, 1280, 1518}
	if len(sizes) != len(expected) {
		t.Errorf("Expected %d sizes, got %d", len(expected), len(sizes))
	}

	for i, size := range sizes {
		if size != expected[i] {
			t.Errorf("Size %d: expected %d, got %d", i, expected[i], size)
		}
	}
}

func TestStandardFrameSizesWithJumbo(t *testing.T) {
	sizes := StandardFrameSizes(true)

	expected := []uint32{64, 128, 256, 512, 1024, 1280, 1518, 9000}
	if len(sizes) != len(expected) {
		t.Errorf("Expected %d sizes, got %d", len(expected), len(sizes))
	}

	// Check last one is jumbo
	if sizes[len(sizes)-1] != 9000 {
		t.Errorf("Expected last size to be 9000, got %d", sizes[len(sizes)-1])
	}
}

// ============================================================================
// Load/Save Tests
// ============================================================================

func TestSaveAndLoad(t *testing.T) {
	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "rfc2544-test-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	configPath := filepath.Join(tmpDir, "test-config.yaml")

	// Create config
	cfg := DefaultConfig()
	cfg.Interface = "eth0"
	cfg.TestType = TestLatency
	cfg.FrameSize = 1518

	// Save
	err = cfg.Save(configPath)
	if err != nil {
		t.Fatalf("Failed to save config: %v", err)
	}

	// Load
	loaded, err := Load(configPath)
	if err != nil {
		t.Fatalf("Failed to load config: %v", err)
	}

	// Verify
	if loaded.Interface != cfg.Interface {
		t.Errorf("Interface: expected %s, got %s", cfg.Interface, loaded.Interface)
	}

	if loaded.TestType != cfg.TestType {
		t.Errorf("TestType: expected %s, got %s", cfg.TestType, loaded.TestType)
	}

	if loaded.FrameSize != cfg.FrameSize {
		t.Errorf("FrameSize: expected %d, got %d", cfg.FrameSize, loaded.FrameSize)
	}
}

func TestLoadNonexistent(t *testing.T) {
	_, err := Load("/nonexistent/path/config.yaml")
	if err == nil {
		t.Error("Expected error for nonexistent file")
	}
}

func TestLoadInvalidYAML(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "rfc2544-test-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	configPath := filepath.Join(tmpDir, "invalid.yaml")

	// Write invalid YAML
	err = os.WriteFile(configPath, []byte("{{{{invalid yaml"), 0644)
	if err != nil {
		t.Fatalf("Failed to write file: %v", err)
	}

	_, err = Load(configPath)
	if err == nil {
		t.Error("Expected error for invalid YAML")
	}
}

func TestLoadInvalidConfig(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "rfc2544-test-*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	configPath := filepath.Join(tmpDir, "invalid-config.yaml")

	// Write config missing interface
	err = os.WriteFile(configPath, []byte("test_type: throughput\n"), 0644)
	if err != nil {
		t.Fatalf("Failed to write file: %v", err)
	}

	_, err = Load(configPath)
	if err == nil {
		t.Error("Expected validation error")
	}
}

// ============================================================================
// Test Type Tests
// ============================================================================

func TestTestTypeConstants(t *testing.T) {
	// Verify test type string values
	testTypes := map[TestType]string{
		TestThroughput:        "throughput",
		TestLatency:           "latency",
		TestFrameLoss:         "frame_loss",
		TestBackToBack:        "back_to_back",
		TestY1564Full:         "y1564",
		TestRFC2889Forwarding: "rfc2889_forwarding",
		TestRFC6349Throughput: "rfc6349_throughput",
		TestY1731Delay:        "y1731_delay",
		TestMEFFull:           "mef",
		TestTSNFull:           "tsn",
	}

	for tt, expected := range testTypes {
		if string(tt) != expected {
			t.Errorf("TestType %v: expected string '%s', got '%s'", tt, expected, string(tt))
		}
	}
}

// ============================================================================
// Output Format Tests
// ============================================================================

func TestOutputFormatConstants(t *testing.T) {
	formats := map[OutputFormat]string{
		FormatText: "text",
		FormatJSON: "json",
		FormatCSV:  "csv",
	}

	for fmt, expected := range formats {
		if string(fmt) != expected {
			t.Errorf("OutputFormat %v: expected '%s', got '%s'", fmt, expected, string(fmt))
		}
	}
}

// ============================================================================
// Benchmark Tests
// ============================================================================

func BenchmarkDefaultConfig(b *testing.B) {
	for i := 0; i < b.N; i++ {
		_ = DefaultConfig()
	}
}

func BenchmarkValidate(b *testing.B) {
	cfg := DefaultConfig()
	cfg.Interface = "eth0"

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = cfg.Validate()
	}
}
