import React, { useState, useEffect, useCallback } from 'react'
import {
  LineChart, Line, BarChart, Bar, XAxis, YAxis, CartesianGrid,
  Tooltip, Legend, ResponsiveContainer
} from 'recharts'

const styles = {
  container: {
    minHeight: '100vh',
    padding: '20px',
    background: 'linear-gradient(135deg, #0a0a1a 0%, #1a1a2e 100%)',
  },
  header: {
    display: 'flex',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: '20px',
    padding: '15px 20px',
    background: '#16213e',
    borderRadius: '8px',
  },
  title: {
    fontSize: '24px',
    fontWeight: 'bold',
    color: '#00ff88',
  },
  version: {
    color: '#888',
    fontSize: '14px',
  },
  grid: {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))',
    gap: '20px',
    marginBottom: '20px',
  },
  card: {
    background: '#16213e',
    borderRadius: '8px',
    padding: '20px',
    boxShadow: '0 4px 6px rgba(0,0,0,0.3)',
  },
  cardTitle: {
    fontSize: '16px',
    fontWeight: '600',
    color: '#4da6ff',
    marginBottom: '15px',
    borderBottom: '1px solid #2a2a4e',
    paddingBottom: '10px',
  },
  stat: {
    display: 'flex',
    justifyContent: 'space-between',
    padding: '8px 0',
    borderBottom: '1px solid #2a2a4e',
  },
  statLabel: {
    color: '#888',
  },
  statValue: {
    color: '#fff',
    fontFamily: 'monospace',
  },
  button: {
    padding: '10px 20px',
    borderRadius: '6px',
    border: 'none',
    cursor: 'pointer',
    fontWeight: '600',
    marginRight: '10px',
    transition: 'all 0.2s',
  },
  buttonStart: {
    background: '#00ff88',
    color: '#000',
  },
  buttonStop: {
    background: '#ff4444',
    color: '#fff',
  },
  input: {
    background: '#0a0a1a',
    border: '1px solid #3a3a5e',
    borderRadius: '4px',
    padding: '8px 12px',
    color: '#fff',
    marginBottom: '10px',
    width: '100%',
  },
  select: {
    background: '#0a0a1a',
    border: '1px solid #3a3a5e',
    borderRadius: '4px',
    padding: '8px 12px',
    color: '#fff',
    marginBottom: '10px',
    width: '100%',
  },
  progressBar: {
    height: '20px',
    background: '#0a0a1a',
    borderRadius: '10px',
    overflow: 'hidden',
    marginTop: '10px',
  },
  progressFill: {
    height: '100%',
    background: 'linear-gradient(90deg, #00ff88, #4da6ff)',
    transition: 'width 0.3s',
  },
  table: {
    width: '100%',
    borderCollapse: 'collapse',
    marginTop: '10px',
  },
  th: {
    textAlign: 'left',
    padding: '10px',
    background: '#0a0a1a',
    color: '#4da6ff',
    borderBottom: '2px solid #3a3a5e',
  },
  td: {
    padding: '10px',
    borderBottom: '1px solid #2a2a4e',
    fontFamily: 'monospace',
  },
}

function App() {
  const [stats, setStats] = useState({
    test_type: 'idle',
    state: 'Idle',
    progress: 0,
    tx_packets: 0,
    rx_packets: 0,
    tx_rate_mbps: 0,
    rx_rate_mbps: 0,
    latency_avg_ns: 0,
    loss_pct: 0,
  })

  const [results, setResults] = useState([])
  const [latencyHistory, setLatencyHistory] = useState([])

  const [config, setConfig] = useState({
    interface: 'eth0',
    test_type: 'throughput',
    frame_size: 1518,
    trial_duration_sec: 60,
  })

  const [isRunning, setIsRunning] = useState(false)

  // Fetch stats periodically
  useEffect(() => {
    const interval = setInterval(async () => {
      try {
        const res = await fetch('/api/stats')
        const data = await res.json()
        setStats(data)

        // Update latency history for chart
        if (data.latency_avg_ns > 0) {
          setLatencyHistory(prev => {
            const next = [...prev, {
              time: new Date().toLocaleTimeString(),
              avg: data.latency_avg_ns / 1000,
              min: data.latency_min_ns / 1000,
              max: data.latency_max_ns / 1000,
            }]
            return next.slice(-30) // Keep last 30 points
          })
        }
      } catch (err) {
        console.error('Failed to fetch stats:', err)
      }
    }, 1000)

    return () => clearInterval(interval)
  }, [])

  // Fetch results
  useEffect(() => {
    const interval = setInterval(async () => {
      try {
        const res = await fetch('/api/results')
        const data = await res.json()
        setResults(data)
      } catch (err) {
        console.error('Failed to fetch results:', err)
      }
    }, 2000)

    return () => clearInterval(interval)
  }, [])

  const handleStart = async () => {
    try {
      const res = await fetch('/api/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config),
      })
      if (res.ok) {
        setIsRunning(true)
        setLatencyHistory([])
      }
    } catch (err) {
      console.error('Failed to start:', err)
    }
  }

  const handleStop = async () => {
    try {
      await fetch('/api/stop', { method: 'POST' })
      setIsRunning(false)
    } catch (err) {
      console.error('Failed to stop:', err)
    }
  }

  const formatBytes = (bytes) => {
    if (bytes >= 1e9) return (bytes / 1e9).toFixed(2) + ' GB'
    if (bytes >= 1e6) return (bytes / 1e6).toFixed(2) + ' MB'
    if (bytes >= 1e3) return (bytes / 1e3).toFixed(2) + ' KB'
    return bytes + ' B'
  }

  const formatLatency = (ns) => {
    if (ns >= 1e6) return (ns / 1e6).toFixed(2) + ' ms'
    if (ns >= 1e3) return (ns / 1e3).toFixed(2) + ' us'
    return ns.toFixed(0) + ' ns'
  }

  return (
    <div style={styles.container}>
      {/* Header */}
      <div style={styles.header}>
        <div>
          <span style={styles.title}>RFC2544 Test Master</span>
          <span style={styles.version}> v2.0.0</span>
        </div>
        <div>
          <button
            style={{ ...styles.button, ...styles.buttonStart }}
            onClick={handleStart}
            disabled={isRunning}
          >
            Start Test
          </button>
          <button
            style={{ ...styles.button, ...styles.buttonStop }}
            onClick={handleStop}
            disabled={!isRunning}
          >
            Stop Test
          </button>
        </div>
      </div>

      {/* Config and Status */}
      <div style={styles.grid}>
        {/* Configuration */}
        <div style={styles.card}>
          <div style={styles.cardTitle}>Configuration</div>
          <label style={{ color: '#888', fontSize: '12px' }}>Interface</label>
          <input
            style={styles.input}
            value={config.interface}
            onChange={e => setConfig({ ...config, interface: e.target.value })}
            placeholder="eth0"
          />
          <label style={{ color: '#888', fontSize: '12px' }}>Test Type</label>
          <select
            style={styles.select}
            value={config.test_type}
            onChange={e => setConfig({ ...config, test_type: e.target.value })}
          >
            <option value="throughput">Throughput (26.1)</option>
            <option value="latency">Latency (26.2)</option>
            <option value="frame_loss">Frame Loss (26.3)</option>
            <option value="back_to_back">Back-to-Back (26.4)</option>
          </select>
          <label style={{ color: '#888', fontSize: '12px' }}>Frame Size</label>
          <select
            style={styles.select}
            value={config.frame_size}
            onChange={e => setConfig({ ...config, frame_size: parseInt(e.target.value) })}
          >
            <option value="0">All Standard Sizes</option>
            <option value="64">64 bytes</option>
            <option value="128">128 bytes</option>
            <option value="256">256 bytes</option>
            <option value="512">512 bytes</option>
            <option value="1024">1024 bytes</option>
            <option value="1280">1280 bytes</option>
            <option value="1518">1518 bytes</option>
            <option value="9000">9000 bytes (Jumbo)</option>
          </select>
        </div>

        {/* Live Stats */}
        <div style={styles.card}>
          <div style={styles.cardTitle}>Live Statistics</div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>State</span>
            <span style={{ ...styles.statValue, color: isRunning ? '#00ff88' : '#888' }}>
              {stats.state || 'Idle'}
            </span>
          </div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>TX Packets</span>
            <span style={styles.statValue}>{stats.tx_packets?.toLocaleString()}</span>
          </div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>RX Packets</span>
            <span style={styles.statValue}>{stats.rx_packets?.toLocaleString()}</span>
          </div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>TX Rate</span>
            <span style={styles.statValue}>{stats.tx_rate_mbps?.toFixed(2)} Mbps</span>
          </div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>Latency (avg)</span>
            <span style={styles.statValue}>{formatLatency(stats.latency_avg_ns || 0)}</span>
          </div>
          <div style={styles.stat}>
            <span style={styles.statLabel}>Loss</span>
            <span style={{ ...styles.statValue, color: stats.loss_pct > 0 ? '#ff4444' : '#00ff88' }}>
              {stats.loss_pct?.toFixed(4)}%
            </span>
          </div>
          <div style={styles.progressBar}>
            <div style={{ ...styles.progressFill, width: `${stats.progress || 0}%` }} />
          </div>
          <div style={{ textAlign: 'center', marginTop: '5px', color: '#888' }}>
            {(stats.progress || 0).toFixed(1)}%
          </div>
        </div>
      </div>

      {/* Charts */}
      <div style={styles.grid}>
        {/* Latency Chart */}
        <div style={styles.card}>
          <div style={styles.cardTitle}>Latency Over Time (us)</div>
          <ResponsiveContainer width="100%" height={200}>
            <LineChart data={latencyHistory}>
              <CartesianGrid strokeDasharray="3 3" stroke="#2a2a4e" />
              <XAxis dataKey="time" stroke="#888" fontSize={10} />
              <YAxis stroke="#888" fontSize={10} />
              <Tooltip
                contentStyle={{ background: '#16213e', border: '1px solid #3a3a5e' }}
              />
              <Legend />
              <Line type="monotone" dataKey="avg" stroke="#00ff88" dot={false} name="Avg" />
              <Line type="monotone" dataKey="min" stroke="#4da6ff" dot={false} name="Min" />
              <Line type="monotone" dataKey="max" stroke="#ff4444" dot={false} name="Max" />
            </LineChart>
          </ResponsiveContainer>
        </div>

        {/* Throughput by Frame Size */}
        <div style={styles.card}>
          <div style={styles.cardTitle}>Throughput by Frame Size</div>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={results}>
              <CartesianGrid strokeDasharray="3 3" stroke="#2a2a4e" />
              <XAxis dataKey="frame_size" stroke="#888" fontSize={10} />
              <YAxis stroke="#888" fontSize={10} />
              <Tooltip
                contentStyle={{ background: '#16213e', border: '1px solid #3a3a5e' }}
              />
              <Bar dataKey="max_rate_mbps" fill="#00ff88" name="Mbps" />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </div>

      {/* Results Table */}
      <div style={styles.card}>
        <div style={styles.cardTitle}>Test Results</div>
        <table style={styles.table}>
          <thead>
            <tr>
              <th style={styles.th}>Frame Size</th>
              <th style={styles.th}>Max Rate %</th>
              <th style={styles.th}>Throughput</th>
              <th style={styles.th}>Loss %</th>
              <th style={styles.th}>Latency Avg</th>
              <th style={styles.th}>Latency P99</th>
            </tr>
          </thead>
          <tbody>
            {results.map((r, i) => (
              <tr key={i}>
                <td style={styles.td}>{r.frame_size} bytes</td>
                <td style={styles.td}>{r.max_rate_pct?.toFixed(2)}%</td>
                <td style={styles.td}>{r.max_rate_mbps?.toFixed(2)} Mbps</td>
                <td style={{ ...styles.td, color: r.loss_pct > 0 ? '#ff4444' : '#00ff88' }}>
                  {r.loss_pct?.toFixed(4)}%
                </td>
                <td style={styles.td}>{formatLatency(r.latency_avg_ns || 0)}</td>
                <td style={styles.td}>{formatLatency(r.latency_p99_ns || 0)}</td>
              </tr>
            ))}
            {results.length === 0 && (
              <tr>
                <td style={{ ...styles.td, textAlign: 'center', color: '#888' }} colSpan={6}>
                  No results yet. Start a test to see results.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

export default App
