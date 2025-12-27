// RFC2544 Test Master - v2 Go Control Plane
//
// Network benchmark testing per RFC 2544:
// - Section 26.1: Throughput (binary search)
// - Section 26.2: Latency measurement
// - Section 26.3: Frame loss rate
// - Section 26.4: Back-to-back frames
package main

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/krisarmstrong/rfc2544-master/pkg/config"
	"github.com/krisarmstrong/rfc2544-master/pkg/dataplane"
	"github.com/krisarmstrong/rfc2544-master/pkg/tui"
	"github.com/krisarmstrong/rfc2544-master/pkg/web"
	"github.com/spf13/cobra"
)

var (
	version      = "2.0.0"
	cfgFile      string
	iface        string
	testType     string
	frameSize    uint32
	webAddr      string
	useTUI       bool
	verbose      bool
	outputFormat string
	outputFile   string

	// Y.1564 specific options
	y1564CIR         float64
	y1564FD          float64
	y1564FDV         float64
	y1564FLR         float64
	y1564PerfMinutes uint32

	// System Recovery test options
	recoveryOverloadSec uint32
	recoveryThroughput  float64

	// Reset test options (informational only - manual reset required)
)

func main() {
	rootCmd := &cobra.Command{
		Use:   "rfc2544",
		Short: "RFC2544 Test Master - Network benchmark testing",
		Long: `RFC2544 Test Master v2

Network benchmark testing per RFC 2544:
  - Throughput: Binary search for max rate with 0% loss
  - Latency: Round-trip time at various loads
  - Frame Loss: Loss percentage vs offered load
  - Back-to-Back: Burst capacity testing

ITU-T Y.1564 (EtherSAM) testing:
  - y1564_config: Service Configuration Test (step test)
  - y1564_perf: Service Performance Test (sustained)
  - y1564: Full Y.1564 test (both config and perf)

Examples:
  # Run throughput test on eth0
  rfc2544 -i eth0 -t throughput

  # Run all tests with TUI
  rfc2544 -i eth0 --tui

  # Run with Web UI
  rfc2544 -i eth0 --web :8080

  # Run Y.1564 test with quick settings
  rfc2544 -i eth0 -t y1564 --cir 100 --fd 10 --fdv 5 --flr 0.01

  # Use config file
  rfc2544 -c config.yaml`,
		Run: runMain,
	}

	// Flags
	rootCmd.Flags().StringVarP(&cfgFile, "config", "c", "", "Config file (YAML)")
	rootCmd.Flags().StringVarP(&iface, "interface", "i", "", "Network interface")
	rootCmd.Flags().StringVarP(&testType, "test", "t", "throughput", "Test type: throughput, latency, frame_loss, back_to_back, system_recovery, reset, y1564_config, y1564_perf, y1564")
	rootCmd.Flags().Uint32VarP(&frameSize, "frame-size", "s", 0, "Frame size (0 = all standard sizes)")
	rootCmd.Flags().StringVar(&webAddr, "web", "", "Enable Web UI on address (e.g., :8080)")
	rootCmd.Flags().BoolVar(&useTUI, "tui", false, "Enable terminal UI")
	rootCmd.Flags().BoolVarP(&verbose, "verbose", "v", false, "Verbose output")
	rootCmd.Flags().StringVarP(&outputFormat, "output", "o", "text", "Output format: text, json, csv")
	rootCmd.Flags().StringVar(&outputFile, "output-file", "", "Output file (default: stdout)")

	// Y.1564 specific flags
	rootCmd.Flags().Float64Var(&y1564CIR, "cir", 100.0, "Y.1564: Committed Information Rate (Mbps)")
	rootCmd.Flags().Float64Var(&y1564FD, "fd", 10.0, "Y.1564: Frame Delay threshold (ms)")
	rootCmd.Flags().Float64Var(&y1564FDV, "fdv", 5.0, "Y.1564: Frame Delay Variation threshold (ms)")
	rootCmd.Flags().Float64Var(&y1564FLR, "flr", 0.01, "Y.1564: Frame Loss Ratio threshold (%)")
	rootCmd.Flags().Uint32Var(&y1564PerfMinutes, "perf-duration", 15, "Y.1564: Performance test duration (minutes)")

	// System Recovery test flags (Section 26.5)
	rootCmd.Flags().Uint32Var(&recoveryOverloadSec, "overload-sec", 60, "System Recovery: Overload duration in seconds")
	rootCmd.Flags().Float64Var(&recoveryThroughput, "recovery-throughput", 0, "System Recovery: Throughput % to use (0 = auto-detect)")

	// Version command
	rootCmd.AddCommand(&cobra.Command{
		Use:   "version",
		Short: "Print version",
		Run: func(cmd *cobra.Command, args []string) {
			fmt.Printf("RFC2544 Test Master v%s\n", version)
		},
	})

	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func runMain(cmd *cobra.Command, args []string) {
	// Load config
	var cfg *config.Config
	var err error

	if cfgFile != "" {
		cfg, err = config.Load(cfgFile)
		if err != nil {
			log.Fatalf("Failed to load config: %v", err)
		}
	} else {
		cfg = config.DefaultConfig()
	}

	// Override with CLI flags
	if iface != "" {
		cfg.Interface = iface
	}
	if testType != "" {
		cfg.TestType = config.TestType(testType)
	}
	if frameSize != 0 {
		cfg.FrameSize = frameSize
	}
	if webAddr != "" {
		cfg.WebUI.Enabled = true
		cfg.WebUI.Address = webAddr
	}
	cfg.Verbose = verbose

	// Apply Y.1564 CLI options if running Y.1564 test
	if cfg.TestType == config.TestY1564Config || cfg.TestType == config.TestY1564Perf || cfg.TestType == config.TestY1564Full {
		// Create a default service from CLI options
		defaultSvc := config.Y1564Service{
			ServiceID:   1,
			ServiceName: "CLI Service",
			FrameSize:   512,
			CoS:         0,
			Enabled:     true,
			SLA: config.Y1564SLA{
				CIRMbps:         y1564CIR,
				EIRMbps:         0,
				CBSBytes:        12000,
				EBSBytes:        0,
				FDThresholdMs:   y1564FD,
				FDVThresholdMs:  y1564FDV,
				FLRThresholdPct: y1564FLR,
			},
		}
		if frameSize != 0 {
			defaultSvc.FrameSize = frameSize
		}
		cfg.Y1564.Services = []config.Y1564Service{defaultSvc}
		cfg.Y1564.PerfDuration = time.Duration(y1564PerfMinutes) * time.Minute
	}

	// Validate
	if cfg.Interface == "" && !cfg.WebUI.Enabled {
		log.Fatal("Interface is required. Use -i <interface> or --web for API mode")
	}

	// Signal handling
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	// Mode selection
	if useTUI {
		runTUI(cfg, sigCh)
	} else if cfg.WebUI.Enabled {
		runWebOnly(cfg, sigCh)
	} else {
		runCLI(cfg, sigCh)
	}
}

func runTUI(cfg *config.Config, sigCh chan os.Signal) {
	app := tui.New()

	// Dataplane context (initialized on start)
	var dpCtx *dataplane.Context
	var cancelTest atomic.Bool

	// Set up callbacks
	app.OnStart = func() {
		app.LogInfo("Starting %s test on %s", cfg.TestType, cfg.Interface)
		app.UpdateStats(tui.Stats{
			TestType:  tui.TestType(cfg.TestType),
			FrameSize: cfg.FrameSize,
			State:     "Running",
			StartTime: time.Now(),
		})

		// Initialize dataplane
		dpCfg := dataplane.Config{
			Interface:      cfg.Interface,
			LineRate:       cfg.LineRateMbps * 1000000,
			AutoDetect:     cfg.AutoDetect,
			TestType:       dataplane.TestType(getTestTypeInt(cfg.TestType)),
			FrameSize:      cfg.FrameSize,
			IncludeJumbo:   cfg.IncludeJumbo,
			TrialDuration:  cfg.TrialDuration,
			WarmupPeriod:   cfg.WarmupPeriod,
			InitialRatePct: cfg.Throughput.InitialRatePct,
			ResolutionPct:  cfg.Throughput.ResolutionPct,
			MaxIterations:  cfg.Throughput.MaxIterations,
			AcceptableLoss: cfg.Throughput.AcceptableLoss,
			HWTimestamp:    cfg.HWTimestamp,
			MeasureLatency: cfg.MeasureLatency,
		}

		var err error
		dpCtx, err = dataplane.New(dpCfg)
		if err != nil {
			app.LogError("Failed to init dataplane: %v", err)
			app.UpdateStats(tui.Stats{State: "Error"})
			return
		}

		// Run tests in background
		go runTUITests(app, dpCtx, cfg, &cancelTest)
	}

	app.OnStop = func() {
		app.LogInfo("Stopping test...")
		cancelTest.Store(true)
		if dpCtx != nil {
			dpCtx.Cancel()
		}
	}

	app.OnCancel = func() {
		app.LogWarn("Test cancelled")
		cancelTest.Store(true)
		if dpCtx != nil {
			dpCtx.Cancel()
		}
	}

	app.OnQuit = func() {
		app.LogInfo("Shutting down...")
		if dpCtx != nil {
			dpCtx.Close()
		}
	}

	// Start with welcome message
	go func() {
		time.Sleep(100 * time.Millisecond)
		app.LogInfo("RFC2544 Test Master v%s", version)
		app.LogInfo("Interface: %s", cfg.Interface)
		app.LogInfo("Test type: %s", cfg.TestType)
		if cfg.FrameSize == 0 {
			app.LogInfo("Frame sizes: All standard (64-1518)")
		} else {
			app.LogInfo("Frame size: %d bytes", cfg.FrameSize)
		}
		app.Log("Press F1 to start, F10 to quit")
	}()

	// Handle signals
	go func() {
		<-sigCh
		app.Stop()
	}()

	if err := app.Run(); err != nil {
		log.Fatalf("TUI error: %v", err)
	}
}

func runTUITests(app *tui.App, ctx *dataplane.Context, cfg *config.Config, cancelled *atomic.Bool) {
	defer func() {
		app.UpdateStats(tui.Stats{State: "Complete"})
		ctx.Close()
	}()

	frameSizes := []uint32{cfg.FrameSize}
	if cfg.FrameSize == 0 {
		frameSizes = config.StandardFrameSizes(cfg.IncludeJumbo)
	}

	for _, fs := range frameSizes {
		if cancelled.Load() {
			return
		}

		ctx.SetFrameSize(fs)
		app.LogInfo("Testing %d byte frames...", fs)
		app.UpdateStats(tui.Stats{
			FrameSize: fs,
			State:     "Running",
		})

		switch cfg.TestType {
		case config.TestThroughput:
			app.LogInfo("Running throughput test...")
			result, err := ctx.RunThroughputTest()
			if err != nil {
				app.LogError("Throughput error: %v", err)
				continue
			}
			app.UpdateStats(tui.Stats{
				FrameSize:  fs,
				TxRate:     result.MaxRateMbps,
				TxPackets:  0,
				RxPackets:  0,
				LossPct:    0,
				LatencyAvg: result.Latency.AvgNs,
				State:      "Complete",
			})
			app.LogInfo("Max rate: %.2f Mbps (%.2f%%)", result.MaxRateMbps, result.MaxRatePct)

		case config.TestLatency:
			app.LogInfo("Running latency test...")
			results, err := ctx.RunLatencyTest(cfg.Latency.LoadLevels)
			if err != nil {
				app.LogError("Latency error: %v", err)
				continue
			}
			for _, r := range results {
				app.LogInfo("Load %.0f%%: avg=%.2fus min=%.2fus max=%.2fus",
					r.LoadPct, r.Latency.AvgNs/1000, r.Latency.MinNs/1000, r.Latency.MaxNs/1000)
			}

		case config.TestFrameLoss:
			app.LogInfo("Running frame loss test...")
			results, err := ctx.RunFrameLossTest(cfg.FrameLoss.StartPct, cfg.FrameLoss.EndPct, cfg.FrameLoss.StepPct)
			if err != nil {
				app.LogError("Frame loss error: %v", err)
				continue
			}
			for _, r := range results {
				app.LogInfo("Load %.0f%%: loss=%.4f%% (tx=%d rx=%d)",
					r.OfferedPct, r.LossPct, r.FramesTx, r.FramesRx)
			}

		case config.TestBackToBack:
			app.LogInfo("Running back-to-back test...")
			result, err := ctx.RunBackToBackTest(cfg.BackToBack.InitialBurst, cfg.BackToBack.Trials)
			if err != nil {
				app.LogError("Back-to-back error: %v", err)
				continue
			}
			app.LogInfo("Max burst: %d frames (%.2f us)", result.MaxBurstFrames, float64(result.BurstDurationUs))

		case config.TestY1564Config, config.TestY1564Perf, config.TestY1564Full:
			runTUIY1564Tests(app, ctx, cfg, cancelled)
		}
	}

	app.LogInfo("Test complete")
}

func runTUIY1564Tests(app *tui.App, ctx *dataplane.Context, cfg *config.Config, cancelled *atomic.Bool) {
	for _, svc := range cfg.Y1564.Services {
		if cancelled.Load() || !svc.Enabled {
			continue
		}

		app.LogInfo("Service %d: %s (CIR: %.2f Mbps)", svc.ServiceID, svc.ServiceName, svc.SLA.CIRMbps)

		dpSvc := &dataplane.Y1564Service{
			ServiceID:   svc.ServiceID,
			ServiceName: svc.ServiceName,
			FrameSize:   svc.FrameSize,
			CoS:         svc.CoS,
			Enabled:     svc.Enabled,
			SLA: dataplane.Y1564SLA{
				CIRMbps:         svc.SLA.CIRMbps,
				EIRMbps:         svc.SLA.EIRMbps,
				CBSBytes:        svc.SLA.CBSBytes,
				EBSBytes:        svc.SLA.EBSBytes,
				FDThresholdMs:   svc.SLA.FDThresholdMs,
				FDVThresholdMs:  svc.SLA.FDVThresholdMs,
				FLRThresholdPct: svc.SLA.FLRThresholdPct,
			},
		}

		// Config test
		if cfg.TestType == config.TestY1564Config || cfg.TestType == config.TestY1564Full {
			app.LogInfo("Running Configuration Test...")
			result, err := ctx.RunY1564ConfigTest(dpSvc)
			if err != nil {
				app.LogError("Config test error: %v", err)
			} else {
				passStr := "PASS"
				if !result.ServicePass {
					passStr = "FAIL"
				}
				app.LogInfo("Config Test: %s", passStr)
				for _, step := range result.Steps {
					app.LogInfo("  Step %d: FLR=%.4f%% FD=%.2fms FDV=%.2fms",
						step.Step, step.FLRPct, step.FDAvgMs, step.FDVMs)
				}
			}
		}

		// Perf test
		if cfg.TestType == config.TestY1564Perf || cfg.TestType == config.TestY1564Full {
			durationSec := uint32(cfg.Y1564.PerfDuration.Seconds())
			app.LogInfo("Running Performance Test (%d min)...", durationSec/60)
			result, err := ctx.RunY1564PerfTest(dpSvc, durationSec)
			if err != nil {
				app.LogError("Perf test error: %v", err)
			} else {
				passStr := "PASS"
				if !result.ServicePass {
					passStr = "FAIL"
				}
				app.LogInfo("Perf Test: %s (FLR=%.4f%% FD=%.2fms FDV=%.2fms)",
					passStr, result.FLRPct, result.FDAvgMs, result.FDVMs)
			}
		}
	}
}

// Active test context for web mode
var (
	webDpCtx    *dataplane.Context
	webDpMu     sync.Mutex
	webTestDone chan struct{}
)

func runWebOnly(cfg *config.Config, sigCh chan os.Signal) {
	srv := web.New(cfg.WebUI.Address)

	srv.OnStart = func(webCfg web.Config) error {
		log.Printf("[main] Starting test: %+v", webCfg)

		// Convert web config to dataplane config
		dpCfg := dataplane.Config{
			Interface:      webCfg.Interface,
			LineRate:       webCfg.LineRateMbps * 1000000,
			AutoDetect:     true,
			TestType:       dataplane.TestType(webCfg.TestType),
			FrameSize:      webCfg.FrameSize,
			IncludeJumbo:   webCfg.IncludeJumbo,
			TrialDuration:  webCfg.TrialDuration,
			WarmupPeriod:   2 * time.Second,
			InitialRatePct: 100.0,
			ResolutionPct:  0.1,
			MaxIterations:  20,
			AcceptableLoss: 0.0,
			HWTimestamp:    webCfg.HWTimestamp,
			MeasureLatency: true,
		}

		var err error
		webDpMu.Lock()
		webDpCtx, err = dataplane.New(dpCfg)
		if err != nil {
			webDpMu.Unlock()
			return fmt.Errorf("init dataplane: %w", err)
		}
		webTestDone = make(chan struct{})
		webDpMu.Unlock()

		// Run test in background
		go runWebTest(srv, webCfg)

		return nil
	}

	srv.OnStop = func() error {
		log.Printf("[main] Stopping test")
		webDpMu.Lock()
		if webDpCtx != nil {
			webDpCtx.Cancel()
			webDpMu.Unlock()
			<-webTestDone // Wait for test to finish
			webDpMu.Lock()
			webDpCtx.Close()
			webDpCtx = nil
		}
		webDpMu.Unlock()
		return nil
	}

	srv.OnCancel = func() {
		log.Printf("[main] Cancelling test")
		webDpMu.Lock()
		if webDpCtx != nil {
			webDpCtx.Cancel()
		}
		webDpMu.Unlock()
	}

	// Handle signals
	go func() {
		<-sigCh
		log.Println("[main] Shutting down...")
		srv.Stop()
	}()

	log.Printf("RFC2544 Test Master v%s", version)
	log.Printf("Web UI: http://localhost%s", cfg.WebUI.Address)

	if err := srv.Start(); err != nil {
		log.Fatalf("Web server error: %v", err)
	}
}

func runWebTest(srv *web.Server, webCfg web.Config) {
	defer func() {
		close(webTestDone)
		srv.UpdateStatus(web.StatusComplete, "Test complete", 100)
	}()

	webDpMu.Lock()
	ctx := webDpCtx
	webDpMu.Unlock()
	if ctx == nil {
		return
	}

	frameSizes := []uint32{webCfg.FrameSize}
	if webCfg.FrameSize == 0 {
		frameSizes = config.StandardFrameSizes(webCfg.IncludeJumbo)
	}

	totalSteps := len(frameSizes)
	currentStep := 0

	for _, fs := range frameSizes {
		ctx.SetFrameSize(fs)
		pct := float64(currentStep) / float64(totalSteps) * 100
		srv.UpdateStatus(web.StatusRunning, fmt.Sprintf("Testing %d byte frames", fs), pct)

		switch dataplane.TestType(webCfg.TestType) {
		case dataplane.TestThroughput:
			result, err := ctx.RunThroughputTest()
			if err != nil {
				srv.UpdateStatus(web.StatusError, fmt.Sprintf("Error: %v", err), pct)
				return
			}
			srv.AddResult(web.TestResult{
				TestType:  "throughput",
				FrameSize: fs,
				Data: map[string]interface{}{
					"max_rate_pct":  result.MaxRatePct,
					"max_rate_mbps": result.MaxRateMbps,
					"max_rate_pps":  result.MaxRatePPS,
					"iterations":    result.Iterations,
					"latency_avg":   result.Latency.AvgNs,
					"latency_min":   result.Latency.MinNs,
					"latency_max":   result.Latency.MaxNs,
				},
			})

		case dataplane.TestLatency:
			// Default load levels
			loadLevels := []float64{10, 20, 30, 40, 50, 60, 70, 80, 90, 100}
			results, err := ctx.RunLatencyTest(loadLevels)
			if err != nil {
				srv.UpdateStatus(web.StatusError, fmt.Sprintf("Error: %v", err), pct)
				return
			}
			for _, r := range results {
				srv.AddResult(web.TestResult{
					TestType:  "latency",
					FrameSize: fs,
					Data: map[string]interface{}{
						"load_pct":    r.LoadPct,
						"latency_avg": r.Latency.AvgNs,
						"latency_min": r.Latency.MinNs,
						"latency_max": r.Latency.MaxNs,
						"jitter":      r.Latency.JitterNs,
					},
				})
			}

		case dataplane.TestFrameLoss:
			results, err := ctx.RunFrameLossTest(100, 10, 10)
			if err != nil {
				srv.UpdateStatus(web.StatusError, fmt.Sprintf("Error: %v", err), pct)
				return
			}
			for _, r := range results {
				srv.AddResult(web.TestResult{
					TestType:  "frame_loss",
					FrameSize: fs,
					Data: map[string]interface{}{
						"offered_pct": r.OfferedPct,
						"frames_tx":   r.FramesTx,
						"frames_rx":   r.FramesRx,
						"loss_pct":    r.LossPct,
					},
				})
			}

		case dataplane.TestBackToBack:
			result, err := ctx.RunBackToBackTest(1000, 50)
			if err != nil {
				srv.UpdateStatus(web.StatusError, fmt.Sprintf("Error: %v", err), pct)
				return
			}
			srv.AddResult(web.TestResult{
				TestType:  "back_to_back",
				FrameSize: fs,
				Data: map[string]interface{}{
					"max_burst":    result.MaxBurstFrames,
					"duration_us":  result.BurstDurationUs,
					"trials":       result.Trials,
				},
			})
		}

		currentStep++
	}
}

func runCLI(cfg *config.Config, sigCh chan os.Signal) {
	fmt.Printf("RFC2544 Test Master v%s\n", version)
	fmt.Printf("Interface: %s\n", cfg.Interface)
	fmt.Printf("Test: %s\n", cfg.TestType)
	fmt.Println()

	// Get frame sizes to test
	frameSizes := []uint32{cfg.FrameSize}
	if cfg.FrameSize == 0 {
		frameSizes = config.StandardFrameSizes(cfg.IncludeJumbo)
	}

	fmt.Printf("Testing frame sizes: %v\n", frameSizes)
	fmt.Printf("Trial duration: %v\n", cfg.TrialDuration)
	fmt.Println()

	// Initialize dataplane context
	dpCfg := dataplane.Config{
		Interface:      cfg.Interface,
		LineRate:       cfg.LineRateMbps * 1000000, // Convert to bps
		AutoDetect:     cfg.AutoDetect,
		TestType:       dataplane.TestType(int(getTestTypeInt(cfg.TestType))),
		FrameSize:      cfg.FrameSize,
		IncludeJumbo:   cfg.IncludeJumbo,
		TrialDuration:  cfg.TrialDuration,
		WarmupPeriod:   cfg.WarmupPeriod,
		InitialRatePct: cfg.Throughput.InitialRatePct,
		ResolutionPct:  cfg.Throughput.ResolutionPct,
		MaxIterations:  cfg.Throughput.MaxIterations,
		AcceptableLoss: cfg.Throughput.AcceptableLoss,
		HWTimestamp:    cfg.HWTimestamp,
		MeasureLatency: cfg.MeasureLatency,
	}

	ctx, err := dataplane.New(dpCfg)
	if err != nil {
		log.Fatalf("Failed to initialize dataplane: %v", err)
	}
	defer ctx.Close()

	// Handle cancel
	var cancelled atomic.Bool
	go func() {
		<-sigCh
		cancelled.Store(true)
		fmt.Println("\nCancelling...")
		ctx.Cancel()
	}()

	// Results storage
	var allResults []interface{}

	// Run tests
	for _, fs := range frameSizes {
		if cancelled.Load() {
			break
		}

		fmt.Printf("\nTesting %d byte frames...\n", fs)
		ctx.SetFrameSize(fs)

		switch cfg.TestType {
		case config.TestThroughput:
			fmt.Printf("  Running throughput test (binary search)...\n")
			result, err := ctx.RunThroughputTest()
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printThroughputResult(result, fs)
			allResults = append(allResults, result)

		case config.TestLatency:
			fmt.Printf("  Running latency test...\n")
			results, err := ctx.RunLatencyTest(cfg.Latency.LoadLevels)
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printLatencyResults(results, fs)
			allResults = append(allResults, results)

		case config.TestFrameLoss:
			fmt.Printf("  Running frame loss test...\n")
			results, err := ctx.RunFrameLossTest(cfg.FrameLoss.StartPct, cfg.FrameLoss.EndPct, cfg.FrameLoss.StepPct)
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printFrameLossResults(results, fs)
			allResults = append(allResults, results)

		case config.TestBackToBack:
			fmt.Printf("  Running back-to-back test...\n")
			result, err := ctx.RunBackToBackTest(cfg.BackToBack.InitialBurst, cfg.BackToBack.Trials)
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printBackToBackResult(result, fs)
			allResults = append(allResults, result)

		case config.TestSystemRecovery:
			fmt.Printf("  Running system recovery test (Section 26.5)...\n")
			// Use provided throughput or default to 100%
			throughputPct := recoveryThroughput
			if throughputPct == 0 {
				throughputPct = 100.0
			}
			result, err := ctx.RunSystemRecoveryTest(throughputPct, recoveryOverloadSec)
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printRecoveryResult(result, fs)
			allResults = append(allResults, result)

		case config.TestReset:
			fmt.Printf("  Running reset test (Section 26.6)...\n")
			fmt.Printf("  NOTE: This test requires manual device reset trigger\n")
			result, err := ctx.RunResetTest()
			if err != nil {
				log.Printf("  Error: %v", err)
				continue
			}
			printResetResult(result, fs)
			allResults = append(allResults, result)

		case config.TestY1564Config, config.TestY1564Perf, config.TestY1564Full:
			runY1564Tests(ctx, cfg, &allResults, &cancelled)
		}
	}

	if cancelled.Load() {
		fmt.Println("\nTest cancelled")
		os.Exit(1)
	}

	// Output results in requested format
	if err := outputResults(allResults, cfg.TestType); err != nil {
		log.Printf("Error writing results: %v", err)
	}

	fmt.Println("\nTest complete")
}

func runY1564Tests(ctx *dataplane.Context, cfg *config.Config, allResults *[]interface{}, cancelled *atomic.Bool) {
	for _, svc := range cfg.Y1564.Services {
		if cancelled.Load() || !svc.Enabled {
			continue
		}

		fmt.Printf("\n  Service %d: %s (CIR: %.2f Mbps)\n", svc.ServiceID, svc.ServiceName, svc.SLA.CIRMbps)

		dpSvc := &dataplane.Y1564Service{
			ServiceID:   svc.ServiceID,
			ServiceName: svc.ServiceName,
			FrameSize:   svc.FrameSize,
			CoS:         svc.CoS,
			Enabled:     svc.Enabled,
			SLA: dataplane.Y1564SLA{
				CIRMbps:         svc.SLA.CIRMbps,
				EIRMbps:         svc.SLA.EIRMbps,
				CBSBytes:        svc.SLA.CBSBytes,
				EBSBytes:        svc.SLA.EBSBytes,
				FDThresholdMs:   svc.SLA.FDThresholdMs,
				FDVThresholdMs:  svc.SLA.FDVThresholdMs,
				FLRThresholdPct: svc.SLA.FLRThresholdPct,
			},
		}

		// Run Configuration Test
		if cfg.TestType == config.TestY1564Config || cfg.TestType == config.TestY1564Full {
			fmt.Printf("    Running Configuration Test (step test)...\n")
			configResult, err := ctx.RunY1564ConfigTest(dpSvc)
			if err != nil {
				log.Printf("    Config test error: %v", err)
			} else {
				printY1564ConfigResult(configResult, &svc)
				*allResults = append(*allResults, configResult)
			}
		}

		// Run Performance Test
		if cfg.TestType == config.TestY1564Perf || cfg.TestType == config.TestY1564Full {
			durationSec := uint32(cfg.Y1564.PerfDuration.Seconds())
			fmt.Printf("    Running Performance Test (%d minutes)...\n", durationSec/60)
			perfResult, err := ctx.RunY1564PerfTest(dpSvc, durationSec)
			if err != nil {
				log.Printf("    Perf test error: %v", err)
			} else {
				printY1564PerfResult(perfResult, &svc)
				*allResults = append(*allResults, perfResult)
			}
		}
	}
}

func printThroughputResult(r *dataplane.ThroughputResultCLI, frameSize uint32) {
	fmt.Printf("  Results for %d bytes:\n", frameSize)
	fmt.Printf("    Max Rate: %.2f%% (%.2f Mbps, %.0f pps)\n", r.MaxRatePct, r.MaxRateMbps, r.MaxRatePPS)
	fmt.Printf("    Iterations: %d\n", r.Iterations)
	if r.Latency.Count > 0 {
		fmt.Printf("    Latency: min=%.2fus avg=%.2fus max=%.2fus\n",
			r.Latency.MinNs/1000, r.Latency.AvgNs/1000, r.Latency.MaxNs/1000)
	}
}

func printLatencyResults(results []dataplane.LatencyResultCLI, frameSize uint32) {
	fmt.Printf("  Latency results for %d bytes:\n", frameSize)
	fmt.Printf("    %8s %12s %12s %12s %12s\n", "Load%", "Min(us)", "Avg(us)", "Max(us)", "Jitter(us)")
	for _, r := range results {
		fmt.Printf("    %8.1f %12.2f %12.2f %12.2f %12.2f\n",
			r.LoadPct, r.Latency.MinNs/1000, r.Latency.AvgNs/1000, r.Latency.MaxNs/1000, r.Latency.JitterNs/1000)
	}
}

func printFrameLossResults(results []dataplane.FrameLossResultCLI, frameSize uint32) {
	fmt.Printf("  Frame loss results for %d bytes:\n", frameSize)
	fmt.Printf("    %8s %12s %12s %12s\n", "Load%", "TX", "RX", "Loss%")
	for _, r := range results {
		fmt.Printf("    %8.1f %12d %12d %12.4f\n", r.OfferedPct, r.FramesTx, r.FramesRx, r.LossPct)
	}
}

func printBackToBackResult(r *dataplane.BackToBackResultCLI, frameSize uint32) {
	fmt.Printf("  Back-to-back results for %d bytes:\n", frameSize)
	fmt.Printf("    Max Burst: %d frames\n", r.MaxBurstFrames)
	fmt.Printf("    Burst Duration: %.2f us\n", float64(r.BurstDurationUs))
	fmt.Printf("    Trials: %d\n", r.Trials)
}

func printRecoveryResult(r *dataplane.RecoveryResultCLI, frameSize uint32) {
	fmt.Printf("  System Recovery results for %d bytes:\n", frameSize)
	fmt.Printf("    Overload Rate: %.1f%% for %d seconds\n", r.OverloadRatePct, r.OverloadSec)
	fmt.Printf("    Recovery Rate: %.1f%%\n", r.RecoveryRatePct)
	if r.RecoveryTimeMs >= 0 {
		fmt.Printf("    Recovery Time: %.2f ms\n", r.RecoveryTimeMs)
	} else {
		fmt.Printf("    Recovery Time: DID NOT RECOVER\n")
	}
	fmt.Printf("    Frames Lost: %d\n", r.FramesLost)
	fmt.Printf("    Trials: %d\n", r.Trials)
}

func printResetResult(r *dataplane.ResetResultCLI, frameSize uint32) {
	fmt.Printf("  Reset test results for %d bytes:\n", frameSize)
	if r.ResetTimeMs >= 0 {
		fmt.Printf("    Reset Time: %.2f ms\n", r.ResetTimeMs)
	} else {
		fmt.Printf("    Reset Time: NOT DETECTED OR DID NOT RECOVER\n")
	}
	fmt.Printf("    Frames Lost: %d\n", r.FramesLost)
	fmt.Printf("    Trials: %d\n", r.Trials)
	fmt.Printf("    Manual Reset: %t\n", r.ManualReset)
}

func printY1564ConfigResult(r *dataplane.Y1564ConfigResult, svc *config.Y1564Service) {
	passStr := "PASS"
	if !r.ServicePass {
		passStr = "FAIL"
	}
	fmt.Printf("    Configuration Test: %s\n", passStr)
	fmt.Printf("      %8s %10s %10s %10s %10s %8s\n", "Step", "Rate%", "FLR%", "FD(ms)", "FDV(ms)", "Result")
	for _, step := range r.Steps {
		stepPass := "PASS"
		if !step.StepPass {
			stepPass = "FAIL"
		}
		fmt.Printf("      %8d %10.1f %10.4f %10.2f %10.2f %8s\n",
			step.Step, step.OfferedRatePct, step.FLRPct, step.FDAvgMs, step.FDVMs, stepPass)
	}
}

func printY1564PerfResult(r *dataplane.Y1564PerfResult, svc *config.Y1564Service) {
	passStr := "PASS"
	if !r.ServicePass {
		passStr = "FAIL"
	}
	fmt.Printf("    Performance Test (%ds): %s\n", r.DurationSec, passStr)
	fmt.Printf("      Frames: TX=%d RX=%d\n", r.FramesTx, r.FramesRx)
	fmt.Printf("      FLR: %.4f%% (threshold: %.4f%%) - %s\n", r.FLRPct, svc.SLA.FLRThresholdPct, passFailStr(r.FLRPass))
	fmt.Printf("      FD:  %.2f ms (threshold: %.2f ms) - %s\n", r.FDAvgMs, svc.SLA.FDThresholdMs, passFailStr(r.FDPass))
	fmt.Printf("      FDV: %.2f ms (threshold: %.2f ms) - %s\n", r.FDVMs, svc.SLA.FDVThresholdMs, passFailStr(r.FDVPass))
}

func passFailStr(pass bool) string {
	if pass {
		return "PASS"
	}
	return "FAIL"
}

func outputResults(results []interface{}, testType config.TestType) error {
	if len(results) == 0 {
		return nil
	}

	var output *os.File
	var err error

	if outputFile != "" {
		output, err = os.Create(outputFile)
		if err != nil {
			return fmt.Errorf("create output file: %w", err)
		}
		defer output.Close()
	} else {
		output = os.Stdout
	}

	switch outputFormat {
	case "json":
		return outputJSON(output, results)
	case "csv":
		return outputCSV(output, results, testType)
	default:
		// Text output already printed
		return nil
	}
}

func outputJSON(w *os.File, results []interface{}) error {
	encoder := json.NewEncoder(w)
	encoder.SetIndent("", "  ")
	return encoder.Encode(results)
}

func outputCSV(w *os.File, results []interface{}, testType config.TestType) error {
	writer := csv.NewWriter(w)
	defer writer.Flush()

	switch testType {
	case config.TestThroughput:
		writer.Write([]string{"FrameSize", "MaxRatePct", "MaxRateMbps", "MaxRatePPS", "Iterations", "LatencyMinUs", "LatencyAvgUs", "LatencyMaxUs"})
		for _, r := range results {
			if tr, ok := r.(*dataplane.ThroughputResultCLI); ok {
				writer.Write([]string{
					fmt.Sprintf("%d", tr.FrameSize),
					fmt.Sprintf("%.4f", tr.MaxRatePct),
					fmt.Sprintf("%.4f", tr.MaxRateMbps),
					fmt.Sprintf("%.0f", tr.MaxRatePPS),
					fmt.Sprintf("%d", tr.Iterations),
					fmt.Sprintf("%.2f", tr.Latency.MinNs/1000),
					fmt.Sprintf("%.2f", tr.Latency.AvgNs/1000),
					fmt.Sprintf("%.2f", tr.Latency.MaxNs/1000),
				})
			}
		}

	case config.TestLatency:
		writer.Write([]string{"FrameSize", "LoadPct", "MinUs", "AvgUs", "MaxUs", "JitterUs", "P50Us", "P95Us", "P99Us"})
		for _, r := range results {
			if lrs, ok := r.([]dataplane.LatencyResultCLI); ok {
				for _, lr := range lrs {
					writer.Write([]string{
						fmt.Sprintf("%d", lr.FrameSize),
						fmt.Sprintf("%.1f", lr.LoadPct),
						fmt.Sprintf("%.2f", lr.Latency.MinNs/1000),
						fmt.Sprintf("%.2f", lr.Latency.AvgNs/1000),
						fmt.Sprintf("%.2f", lr.Latency.MaxNs/1000),
						fmt.Sprintf("%.2f", lr.Latency.JitterNs/1000),
						fmt.Sprintf("%.2f", lr.Latency.P50Ns/1000),
						fmt.Sprintf("%.2f", lr.Latency.P95Ns/1000),
						fmt.Sprintf("%.2f", lr.Latency.P99Ns/1000),
					})
				}
			}
		}

	case config.TestFrameLoss:
		writer.Write([]string{"FrameSize", "OfferedPct", "FramesTx", "FramesRx", "LossPct"})
		for _, r := range results {
			if flrs, ok := r.([]dataplane.FrameLossResultCLI); ok {
				for _, fl := range flrs {
					writer.Write([]string{
						fmt.Sprintf("%d", fl.FrameSize),
						fmt.Sprintf("%.1f", fl.OfferedPct),
						fmt.Sprintf("%d", fl.FramesTx),
						fmt.Sprintf("%d", fl.FramesRx),
						fmt.Sprintf("%.4f", fl.LossPct),
					})
				}
			}
		}

	case config.TestBackToBack:
		writer.Write([]string{"FrameSize", "MaxBurstFrames", "BurstDurationUs", "Trials"})
		for _, r := range results {
			if br, ok := r.(*dataplane.BackToBackResultCLI); ok {
				writer.Write([]string{
					fmt.Sprintf("%d", br.FrameSize),
					fmt.Sprintf("%d", br.MaxBurstFrames),
					fmt.Sprintf("%d", br.BurstDurationUs),
					fmt.Sprintf("%d", br.Trials),
				})
			}
		}

	case config.TestSystemRecovery:
		writer.Write([]string{"FrameSize", "OverloadRatePct", "RecoveryRatePct", "OverloadSec", "RecoveryTimeMs", "FramesLost", "Trials"})
		for _, r := range results {
			if rr, ok := r.(*dataplane.RecoveryResultCLI); ok {
				writer.Write([]string{
					fmt.Sprintf("%d", rr.FrameSize),
					fmt.Sprintf("%.1f", rr.OverloadRatePct),
					fmt.Sprintf("%.1f", rr.RecoveryRatePct),
					fmt.Sprintf("%d", rr.OverloadSec),
					fmt.Sprintf("%.2f", rr.RecoveryTimeMs),
					fmt.Sprintf("%d", rr.FramesLost),
					fmt.Sprintf("%d", rr.Trials),
				})
			}
		}

	case config.TestReset:
		writer.Write([]string{"FrameSize", "ResetTimeMs", "FramesLost", "Trials", "ManualReset"})
		for _, r := range results {
			if rr, ok := r.(*dataplane.ResetResultCLI); ok {
				writer.Write([]string{
					fmt.Sprintf("%d", rr.FrameSize),
					fmt.Sprintf("%.2f", rr.ResetTimeMs),
					fmt.Sprintf("%d", rr.FramesLost),
					fmt.Sprintf("%d", rr.Trials),
					fmt.Sprintf("%t", rr.ManualReset),
				})
			}
		}

	case config.TestY1564Config, config.TestY1564Perf, config.TestY1564Full:
		writer.Write([]string{"ServiceID", "TestPhase", "Step", "FLRPct", "FDMs", "FDVMs", "Pass"})
		for _, r := range results {
			if cr, ok := r.(*dataplane.Y1564ConfigResult); ok {
				for _, step := range cr.Steps {
					writer.Write([]string{
						fmt.Sprintf("%d", cr.ServiceID),
						"Config",
						fmt.Sprintf("%d", step.Step),
						fmt.Sprintf("%.4f", step.FLRPct),
						fmt.Sprintf("%.2f", step.FDAvgMs),
						fmt.Sprintf("%.2f", step.FDVMs),
						fmt.Sprintf("%t", step.StepPass),
					})
				}
			}
			if pr, ok := r.(*dataplane.Y1564PerfResult); ok {
				writer.Write([]string{
					fmt.Sprintf("%d", pr.ServiceID),
					"Perf",
					"-",
					fmt.Sprintf("%.4f", pr.FLRPct),
					fmt.Sprintf("%.2f", pr.FDAvgMs),
					fmt.Sprintf("%.2f", pr.FDVMs),
					fmt.Sprintf("%t", pr.ServicePass),
				})
			}
		}
	}

	return nil
}

// getTestTypeInt converts config.TestType to int
func getTestTypeInt(t config.TestType) int {
	switch t {
	case config.TestThroughput:
		return 0
	case config.TestLatency:
		return 1
	case config.TestFrameLoss:
		return 2
	case config.TestBackToBack:
		return 3
	case config.TestSystemRecovery:
		return 4
	case config.TestReset:
		return 5
	case config.TestY1564Config:
		return 6
	case config.TestY1564Perf:
		return 7
	case config.TestY1564Full:
		return 8
	default:
		return 0
	}
}
