// Package tui provides a terminal user interface for RFC2544 Test Master
package tui

import (
	"fmt"
	"time"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

// TestType for display
type TestType string

const (
	TestThroughput TestType = "Throughput"
	TestLatency    TestType = "Latency"
	TestFrameLoss  TestType = "Frame Loss"
	TestBackToBack TestType = "Back-to-Back"
)

// Stats represents real-time test statistics
type Stats struct {
	// Current test info
	TestType    TestType
	FrameSize   uint32
	Progress    float64
	State       string
	Iteration   int
	MaxIter     int

	// Packet counters
	TxPackets uint64
	TxBytes   uint64
	RxPackets uint64
	RxBytes   uint64

	// Rates
	TxRate float64 // Mbps
	RxRate float64 // Mbps
	TxPPS  float64 // Packets per second
	RxPPS  float64 // Packets per second

	// Current trial
	OfferedRate float64 // % of line rate
	LossPct     float64

	// Latency (nanoseconds)
	LatencyMin float64
	LatencyMax float64
	LatencyAvg float64
	LatencyP99 float64

	// Uptime
	StartTime time.Time
	Duration  time.Duration
}

// Result represents a completed test result
type Result struct {
	FrameSize    uint32
	MaxRatePct   float64
	MaxRateMbps  float64
	LossPct      float64
	LatencyAvgNs float64
	Timestamp    time.Time
}

// App represents the TUI application
type App struct {
	app         *tview.Application
	pages       *tview.Pages
	statsView   *tview.Table
	resultsView *tview.Table
	logView     *tview.TextView
	progressBar *tview.TextView
	statusBar   *tview.TextView

	stats   Stats
	results []Result

	// Callbacks
	OnStart  func()
	OnStop   func()
	OnCancel func()
	OnQuit   func()
}

// New creates a new TUI application
func New() *App {
	a := &App{
		app:     tview.NewApplication(),
		pages:   tview.NewPages(),
		results: make([]Result, 0),
	}
	a.build()
	return a
}

func (a *App) build() {
	// Stats panel (left side)
	a.statsView = tview.NewTable().
		SetBorders(false).
		SetSelectable(false, false)
	a.statsView.SetTitle(" Statistics ").SetBorder(true)
	a.initStatsView()

	// Results panel (right side)
	a.resultsView = tview.NewTable().
		SetBorders(true).
		SetSelectable(true, false)
	a.resultsView.SetTitle(" Results ").SetBorder(true)
	a.initResultsView()

	// Progress bar
	a.progressBar = tview.NewTextView().
		SetDynamicColors(true).
		SetTextAlign(tview.AlignCenter)
	a.progressBar.SetTitle(" Progress ").SetBorder(true)
	a.updateProgressBar(0)

	// Log view (bottom)
	a.logView = tview.NewTextView().
		SetDynamicColors(true).
		SetScrollable(true).
		SetChangedFunc(func() {
			a.app.Draw()
		})
	a.logView.SetTitle(" Log ").SetBorder(true)

	// Status bar
	a.statusBar = tview.NewTextView().
		SetDynamicColors(true).
		SetTextAlign(tview.AlignCenter)
	a.statusBar.SetText("[yellow]RFC2544 Test Master[white] | [green]F1[white] Start | [red]F2[white] Stop | [blue]F10[white] Quit")

	// Layout
	topRow := tview.NewFlex().
		AddItem(a.statsView, 0, 1, false).
		AddItem(a.resultsView, 0, 2, false)

	mainFlex := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(topRow, 0, 3, false).
		AddItem(a.progressBar, 3, 0, false).
		AddItem(a.logView, 0, 1, false).
		AddItem(a.statusBar, 1, 0, false)

	a.pages.AddPage("main", mainFlex, true, true)

	// Key bindings
	a.app.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		switch event.Key() {
		case tcell.KeyF1:
			if a.OnStart != nil {
				go a.OnStart()
			}
			return nil
		case tcell.KeyF2:
			if a.OnStop != nil {
				go a.OnStop()
			}
			return nil
		case tcell.KeyF10, tcell.KeyEscape:
			if a.OnQuit != nil {
				a.OnQuit()
			}
			a.app.Stop()
			return nil
		case tcell.KeyCtrlC:
			if a.OnCancel != nil {
				a.OnCancel()
			}
			return nil
		}
		return event
	})

	a.app.SetRoot(a.pages, true)
}

func (a *App) initStatsView() {
	labels := []string{
		"Test Type:",
		"Frame Size:",
		"State:",
		"Progress:",
		"Iteration:",
		"",
		"TX Packets:",
		"TX Rate:",
		"RX Packets:",
		"RX Rate:",
		"",
		"Offered Rate:",
		"Frame Loss:",
		"",
		"Latency Min:",
		"Latency Avg:",
		"Latency Max:",
		"Latency P99:",
		"",
		"Duration:",
	}

	for i, label := range labels {
		a.statsView.SetCell(i, 0, tview.NewTableCell(label).
			SetTextColor(tcell.ColorYellow).
			SetAlign(tview.AlignRight))
		a.statsView.SetCell(i, 1, tview.NewTableCell("-").
			SetTextColor(tcell.ColorWhite).
			SetAlign(tview.AlignLeft))
	}
}

func (a *App) initResultsView() {
	headers := []string{"Frame Size", "Max Rate %", "Rate Mbps", "Loss %", "Latency Avg"}
	for i, h := range headers {
		a.resultsView.SetCell(0, i, tview.NewTableCell(h).
			SetTextColor(tcell.ColorYellow).
			SetAlign(tview.AlignCenter).
			SetSelectable(false))
	}
}

// UpdateStats updates the statistics display
func (a *App) UpdateStats(s Stats) {
	a.stats = s
	a.app.QueueUpdateDraw(func() {
		values := []string{
			string(s.TestType),
			fmt.Sprintf("%d bytes", s.FrameSize),
			s.State,
			fmt.Sprintf("%.1f%%", s.Progress),
			fmt.Sprintf("%d / %d", s.Iteration, s.MaxIter),
			"",
			fmt.Sprintf("%d", s.TxPackets),
			fmt.Sprintf("%.2f Mbps (%.0f pps)", s.TxRate, s.TxPPS),
			fmt.Sprintf("%d", s.RxPackets),
			fmt.Sprintf("%.2f Mbps (%.0f pps)", s.RxRate, s.RxPPS),
			"",
			fmt.Sprintf("%.2f%%", s.OfferedRate),
			fmt.Sprintf("%.4f%%", s.LossPct),
			"",
			fmt.Sprintf("%.2f us", s.LatencyMin/1000),
			fmt.Sprintf("%.2f us", s.LatencyAvg/1000),
			fmt.Sprintf("%.2f us", s.LatencyMax/1000),
			fmt.Sprintf("%.2f us", s.LatencyP99/1000),
			"",
			s.Duration.Round(time.Second).String(),
		}

		for i, v := range values {
			a.statsView.SetCell(i, 1, tview.NewTableCell(v).
				SetTextColor(tcell.ColorWhite).
				SetAlign(tview.AlignLeft))
		}

		a.updateProgressBar(s.Progress)
	})
}

// AddResult adds a test result to the results table
func (a *App) AddResult(r Result) {
	a.results = append(a.results, r)
	a.app.QueueUpdateDraw(func() {
		row := len(a.results)
		a.resultsView.SetCell(row, 0, tview.NewTableCell(fmt.Sprintf("%d", r.FrameSize)).
			SetAlign(tview.AlignCenter))
		a.resultsView.SetCell(row, 1, tview.NewTableCell(fmt.Sprintf("%.2f", r.MaxRatePct)).
			SetAlign(tview.AlignCenter))
		a.resultsView.SetCell(row, 2, tview.NewTableCell(fmt.Sprintf("%.2f", r.MaxRateMbps)).
			SetAlign(tview.AlignCenter))
		a.resultsView.SetCell(row, 3, tview.NewTableCell(fmt.Sprintf("%.4f", r.LossPct)).
			SetAlign(tview.AlignCenter))
		a.resultsView.SetCell(row, 4, tview.NewTableCell(fmt.Sprintf("%.2f us", r.LatencyAvgNs/1000)).
			SetAlign(tview.AlignCenter))
	})
}

// Log adds a message to the log view
func (a *App) Log(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	timestamp := time.Now().Format("15:04:05")
	a.app.QueueUpdateDraw(func() {
		fmt.Fprintf(a.logView, "[gray]%s[white] %s\n", timestamp, msg)
		a.logView.ScrollToEnd()
	})
}

// LogInfo logs an info message
func (a *App) LogInfo(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	timestamp := time.Now().Format("15:04:05")
	a.app.QueueUpdateDraw(func() {
		fmt.Fprintf(a.logView, "[gray]%s [green][INFO][white] %s\n", timestamp, msg)
		a.logView.ScrollToEnd()
	})
}

// LogWarn logs a warning message
func (a *App) LogWarn(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	timestamp := time.Now().Format("15:04:05")
	a.app.QueueUpdateDraw(func() {
		fmt.Fprintf(a.logView, "[gray]%s [yellow][WARN][white] %s\n", timestamp, msg)
		a.logView.ScrollToEnd()
	})
}

// LogError logs an error message
func (a *App) LogError(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	timestamp := time.Now().Format("15:04:05")
	a.app.QueueUpdateDraw(func() {
		fmt.Fprintf(a.logView, "[gray]%s [red][ERROR][white] %s\n", timestamp, msg)
		a.logView.ScrollToEnd()
	})
}

func (a *App) updateProgressBar(pct float64) {
	width := 50
	filled := int(pct / 100.0 * float64(width))
	if filled > width {
		filled = width
	}

	bar := ""
	for i := 0; i < width; i++ {
		if i < filled {
			bar += "[green]█"
		} else {
			bar += "[gray]░"
		}
	}
	a.progressBar.SetText(fmt.Sprintf("%s[white] %.1f%%", bar, pct))
}

// SetStatus updates the status bar
func (a *App) SetStatus(msg string) {
	a.app.QueueUpdateDraw(func() {
		a.statusBar.SetText(msg)
	})
}

// Run starts the TUI application
func (a *App) Run() error {
	return a.app.Run()
}

// Stop stops the TUI application
func (a *App) Stop() {
	a.app.Stop()
}

// ClearResults clears the results table
func (a *App) ClearResults() {
	a.results = a.results[:0]
	a.app.QueueUpdateDraw(func() {
		a.resultsView.Clear()
		a.initResultsView()
	})
}
