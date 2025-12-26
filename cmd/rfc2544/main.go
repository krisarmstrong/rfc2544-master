// RFC2544 Test Master - v2 Go Control Plane
//
// Network benchmark testing per RFC 2544:
// - Section 26.1: Throughput (binary search)
// - Section 26.2: Latency measurement
// - Section 26.3: Frame loss rate
// - Section 26.4: Back-to-back frames
package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/krisarmstrong/rfc2544-master/pkg/config"
	"github.com/krisarmstrong/rfc2544-master/pkg/tui"
	"github.com/krisarmstrong/rfc2544-master/pkg/web"
	"github.com/spf13/cobra"
)

var (
	version   = "2.0.0"
	cfgFile   string
	iface     string
	testType  string
	frameSize uint32
	webAddr   string
	useTUI    bool
	verbose   bool
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

Examples:
  # Run throughput test on eth0
  rfc2544 -i eth0 -t throughput

  # Run all tests with TUI
  rfc2544 -i eth0 --tui

  # Run with Web UI
  rfc2544 -i eth0 --web :8080

  # Use config file
  rfc2544 -c config.yaml`,
		Run: runMain,
	}

	// Flags
	rootCmd.Flags().StringVarP(&cfgFile, "config", "c", "", "Config file (YAML)")
	rootCmd.Flags().StringVarP(&iface, "interface", "i", "", "Network interface")
	rootCmd.Flags().StringVarP(&testType, "test", "t", "throughput", "Test type: throughput, latency, frame_loss, back_to_back")
	rootCmd.Flags().Uint32VarP(&frameSize, "frame-size", "s", 0, "Frame size (0 = all standard sizes)")
	rootCmd.Flags().StringVar(&webAddr, "web", "", "Enable Web UI on address (e.g., :8080)")
	rootCmd.Flags().BoolVar(&useTUI, "tui", false, "Enable terminal UI")
	rootCmd.Flags().BoolVarP(&verbose, "verbose", "v", false, "Verbose output")

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

	// Set up callbacks
	app.OnStart = func() {
		app.LogInfo("Starting %s test on %s", cfg.TestType, cfg.Interface)
		app.UpdateStats(tui.Stats{
			TestType:  tui.TestType(cfg.TestType),
			FrameSize: cfg.FrameSize,
			State:     "Running",
			StartTime: time.Now(),
		})
		// TODO: Integrate with dataplane
	}

	app.OnStop = func() {
		app.LogInfo("Stopping test...")
	}

	app.OnCancel = func() {
		app.LogWarn("Test cancelled")
	}

	app.OnQuit = func() {
		app.LogInfo("Shutting down...")
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

func runWebOnly(cfg *config.Config, sigCh chan os.Signal) {
	srv := web.New(cfg.WebUI.Address)

	srv.OnStart = func(webCfg web.Config) error {
		log.Printf("[main] Starting test: %+v", webCfg)
		// TODO: Integrate with dataplane
		return nil
	}

	srv.OnStop = func() error {
		log.Printf("[main] Stopping test")
		return nil
	}

	srv.OnCancel = func() {
		log.Printf("[main] Cancelling test")
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

	// Handle cancel
	cancelled := false
	go func() {
		<-sigCh
		cancelled = true
		fmt.Println("\nCancelling...")
	}()

	// Run tests (placeholder - would integrate with C dataplane)
	for _, fs := range frameSizes {
		if cancelled {
			break
		}

		fmt.Printf("Testing %d byte frames...\n", fs)

		switch cfg.TestType {
		case config.TestThroughput:
			fmt.Printf("  Running throughput test (binary search)...\n")
			// TODO: dataplane.RunThroughputTest()

		case config.TestLatency:
			fmt.Printf("  Running latency test...\n")
			// TODO: dataplane.RunLatencyTest()

		case config.TestFrameLoss:
			fmt.Printf("  Running frame loss test...\n")
			// TODO: dataplane.RunFrameLossTest()

		case config.TestBackToBack:
			fmt.Printf("  Running back-to-back test...\n")
			// TODO: dataplane.RunBackToBackTest()
		}
	}

	if cancelled {
		fmt.Println("Test cancelled")
		os.Exit(1)
	}

	fmt.Println("\nTest complete")
}
