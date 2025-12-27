// Package config provides YAML configuration support for RFC2544 Test Master
package config

import (
	"fmt"
	"os"
	"time"

	"gopkg.in/yaml.v3"
)

// TestType represents the RFC 2544 test types
type TestType string

const (
	TestThroughput      TestType = "throughput"       // Section 26.1
	TestLatency         TestType = "latency"          // Section 26.2
	TestFrameLoss       TestType = "frame_loss"       // Section 26.3
	TestBackToBack      TestType = "back_to_back"     // Section 26.4
	TestSystemRecovery  TestType = "system_recovery"  // Section 26.5
	TestReset           TestType = "reset"            // Section 26.6
	TestY1564Config     TestType = "y1564_config"     // ITU-T Y.1564 Service Configuration Test
	TestY1564Perf       TestType = "y1564_perf"       // ITU-T Y.1564 Service Performance Test
	TestY1564Full       TestType = "y1564"            // ITU-T Y.1564 Full Test (Config + Perf)
)

// OutputFormat for results
type OutputFormat string

const (
	FormatText OutputFormat = "text"
	FormatJSON OutputFormat = "json"
	FormatCSV  OutputFormat = "csv"
)

// Config represents the full configuration
type Config struct {
	// Interface settings
	Interface    string `yaml:"interface"`
	LineRateMbps uint64 `yaml:"line_rate_mbps"` // 0 = auto-detect
	AutoDetect   bool   `yaml:"auto_detect_nic"`

	// Test selection
	TestType     TestType `yaml:"test_type"`
	FrameSize    uint32   `yaml:"frame_size"`     // 0 = all standard sizes
	IncludeJumbo bool     `yaml:"include_jumbo"`  // Include 9000 byte frames

	// Timing
	TrialDuration time.Duration `yaml:"trial_duration"` // Default: 60s
	WarmupPeriod  time.Duration `yaml:"warmup_period"`  // Default: 2s

	// Throughput test (Section 26.1)
	Throughput ThroughputConfig `yaml:"throughput"`

	// Latency test (Section 26.2)
	Latency LatencyConfig `yaml:"latency"`

	// Frame loss test (Section 26.3)
	FrameLoss FrameLossConfig `yaml:"frame_loss"`

	// Back-to-back test (Section 26.4)
	BackToBack BackToBackConfig `yaml:"back_to_back"`

	// Features
	HWTimestamp    bool `yaml:"hw_timestamp"`
	MeasureLatency bool `yaml:"measure_latency"`

	// Output
	OutputFormat OutputFormat `yaml:"output_format"`
	Verbose      bool         `yaml:"verbose"`

	// Platform
	UseDPDK  bool   `yaml:"use_dpdk"`
	DPDKArgs string `yaml:"dpdk_args"`

	// Rate control
	UsePacing bool   `yaml:"use_pacing"`
	BatchSize uint32 `yaml:"batch_size"`

	// Web UI
	WebUI    WebUIConfig `yaml:"web_ui"`

	// ITU-T Y.1564 (EtherSAM) configuration
	Y1564 Y1564Config `yaml:"y1564"`
}

// ThroughputConfig for binary search throughput test
type ThroughputConfig struct {
	InitialRatePct float64 `yaml:"initial_rate_pct"` // Default: 100
	ResolutionPct  float64 `yaml:"resolution_pct"`   // Default: 0.1
	MaxIterations  uint32  `yaml:"max_iterations"`   // Default: 20
	AcceptableLoss float64 `yaml:"acceptable_loss"`  // Default: 0.0
}

// LatencyConfig for latency test
type LatencyConfig struct {
	Samples    uint32    `yaml:"samples"`     // Number of samples per trial
	LoadLevels []float64 `yaml:"load_levels"` // Load levels to test (% of throughput)
}

// FrameLossConfig for frame loss test
type FrameLossConfig struct {
	StartPct float64 `yaml:"start_pct"` // Starting offered load %
	EndPct   float64 `yaml:"end_pct"`   // Ending offered load %
	StepPct  float64 `yaml:"step_pct"`  // Step size
}

// BackToBackConfig for burst capacity test
type BackToBackConfig struct {
	InitialBurst uint64 `yaml:"initial_burst"` // Starting burst size
	Trials       uint32 `yaml:"trials"`        // Trials per burst size
}

// WebUIConfig for web interface
type WebUIConfig struct {
	Enabled bool   `yaml:"enabled"`
	Address string `yaml:"address"` // e.g., ":8080"
}

// Y1564SLA defines SLA parameters for Y.1564 testing
type Y1564SLA struct {
	CIRMbps         float64 `yaml:"cir_mbps"`          // Committed Information Rate
	EIRMbps         float64 `yaml:"eir_mbps"`          // Excess Information Rate
	CBSBytes        uint32  `yaml:"cbs_bytes"`         // Committed Burst Size
	EBSBytes        uint32  `yaml:"ebs_bytes"`         // Excess Burst Size
	FDThresholdMs   float64 `yaml:"fd_threshold_ms"`   // Frame Delay threshold (ms)
	FDVThresholdMs  float64 `yaml:"fdv_threshold_ms"`  // Frame Delay Variation threshold (ms)
	FLRThresholdPct float64 `yaml:"flr_threshold_pct"` // Frame Loss Ratio threshold (%)
}

// Y1564Service defines a service for Y.1564 testing
type Y1564Service struct {
	ServiceID   uint32   `yaml:"service_id"`
	ServiceName string   `yaml:"service_name"`
	SLA         Y1564SLA `yaml:"sla"`
	FrameSize   uint32   `yaml:"frame_size"`
	CoS         uint8    `yaml:"cos"` // Class of Service (DSCP value)
	Enabled     bool     `yaml:"enabled"`
}

// Y1564Config for ITU-T Y.1564 testing
type Y1564Config struct {
	Services        []Y1564Service `yaml:"services"`
	ConfigSteps     []float64      `yaml:"config_steps"`      // Step percentages (default: 25, 50, 75, 100)
	StepDuration    time.Duration  `yaml:"step_duration"`     // Duration per step (default: 60s)
	PerfDuration    time.Duration  `yaml:"perf_duration"`     // Performance test duration (default: 15m)
	RunConfigTest   bool           `yaml:"run_config_test"`   // Run configuration test
	RunPerfTest     bool           `yaml:"run_perf_test"`     // Run performance test
}

// DefaultY1564SLA returns default SLA parameters
func DefaultY1564SLA() Y1564SLA {
	return Y1564SLA{
		CIRMbps:         100.0,
		EIRMbps:         0.0,
		CBSBytes:        12000,
		EBSBytes:        0,
		FDThresholdMs:   10.0,
		FDVThresholdMs:  5.0,
		FLRThresholdPct: 0.01,
	}
}

// DefaultY1564Config returns default Y.1564 configuration
func DefaultY1564Config() Y1564Config {
	return Y1564Config{
		Services:      []Y1564Service{},
		ConfigSteps:   []float64{25, 50, 75, 100},
		StepDuration:  60 * time.Second,
		PerfDuration:  15 * time.Minute,
		RunConfigTest: true,
		RunPerfTest:   true,
	}
}

// DefaultConfig returns a configuration with RFC 2544 recommended defaults
func DefaultConfig() *Config {
	return &Config{
		AutoDetect:     true,
		TestType:       TestThroughput,
		FrameSize:      0, // All standard sizes
		IncludeJumbo:   false,
		TrialDuration:  60 * time.Second,
		WarmupPeriod:   2 * time.Second,

		Throughput: ThroughputConfig{
			InitialRatePct: 100.0,
			ResolutionPct:  0.1,
			MaxIterations:  20,
			AcceptableLoss: 0.0,
		},

		Latency: LatencyConfig{
			Samples:    1000,
			LoadLevels: []float64{10, 20, 30, 40, 50, 60, 70, 80, 90, 100},
		},

		FrameLoss: FrameLossConfig{
			StartPct: 100.0,
			EndPct:   10.0,
			StepPct:  10.0,
		},

		BackToBack: BackToBackConfig{
			InitialBurst: 1000,
			Trials:       50,
		},

		HWTimestamp:    true,
		MeasureLatency: true,
		OutputFormat:   FormatText,
		Verbose:        false,
		UseDPDK:        false,
		UsePacing:      true,
		BatchSize:      32,

		WebUI: WebUIConfig{
			Enabled: false,
			Address: ":8080",
		},

		Y1564: DefaultY1564Config(),
	}
}

// Load reads configuration from a YAML file
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	cfg := DefaultConfig()
	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	if err := cfg.Validate(); err != nil {
		return nil, fmt.Errorf("validate config: %w", err)
	}

	return cfg, nil
}

// Save writes configuration to a YAML file
func (c *Config) Save(path string) error {
	data, err := yaml.Marshal(c)
	if err != nil {
		return fmt.Errorf("marshal config: %w", err)
	}

	if err := os.WriteFile(path, data, 0644); err != nil {
		return fmt.Errorf("write config: %w", err)
	}

	return nil
}

// Validate checks configuration for errors
func (c *Config) Validate() error {
	if c.Interface == "" {
		return fmt.Errorf("interface is required")
	}

	// Validate test type
	switch c.TestType {
	case TestThroughput, TestLatency, TestFrameLoss, TestBackToBack:
		// Valid RFC 2544 test types
	case TestY1564Config, TestY1564Perf, TestY1564Full:
		// Valid Y.1564 test types - validate Y.1564 config
		if len(c.Y1564.Services) == 0 {
			return fmt.Errorf("Y.1564 test requires at least one service configured")
		}
		for i, svc := range c.Y1564.Services {
			if svc.Enabled && svc.SLA.CIRMbps <= 0 {
				return fmt.Errorf("service %d: CIR must be > 0", i+1)
			}
		}
	default:
		return fmt.Errorf("invalid test type: %s", c.TestType)
	}

	// Validate frame size
	validSizes := map[uint32]bool{
		0: true, 64: true, 128: true, 256: true, 512: true,
		1024: true, 1280: true, 1518: true, 9000: true,
	}
	if !validSizes[c.FrameSize] {
		return fmt.Errorf("invalid frame size: %d", c.FrameSize)
	}

	// Validate throughput config
	if c.Throughput.ResolutionPct <= 0 || c.Throughput.ResolutionPct > 10 {
		return fmt.Errorf("resolution must be between 0 and 10%%")
	}

	// Validate frame loss config
	if c.FrameLoss.StartPct < c.FrameLoss.EndPct {
		return fmt.Errorf("frame loss start must be >= end")
	}

	return nil
}

// StandardFrameSizes returns the RFC 2544 standard frame sizes
func StandardFrameSizes(includeJumbo bool) []uint32 {
	sizes := []uint32{64, 128, 256, 512, 1024, 1280, 1518}
	if includeJumbo {
		sizes = append(sizes, 9000)
	}
	return sizes
}
