# RFC 2544 Test Master

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-green.svg)]()
[![Linux](https://img.shields.io/badge/platform-Linux-orange.svg)]()

A high-performance RFC 2544 network benchmark test generator for Linux. This is the "master" end of network testing - it generates test traffic that is reflected by a [reflector-native](https://github.com/krisarmstrong/reflector-native) endpoint.

## RFC 2544 Tests

This tool implements the four core tests from [RFC 2544: Benchmarking Methodology for Network Interconnect Devices](https://www.rfc-editor.org/rfc/rfc2544):

| Test | Section | Description |
|------|---------|-------------|
| **Throughput** | 26.1 | Binary search to find maximum rate with 0% frame loss |
| **Latency** | 26.2 | Round-trip time at various load levels |
| **Frame Loss** | 26.3 | Frame loss percentage vs. offered load |
| **Back-to-Back** | 26.4 | Maximum burst length with 0% loss |

### Standard Frame Sizes

Per RFC 2544 Section 9.1:
- 64, 128, 256, 512, 1024, 1280, 1518 bytes
- Optional: 9000 bytes (jumbo frames)

## Architecture

```
┌─────────────────────┐                    ┌─────────────────────┐
│   RFC2544 Master    │                    │     Reflector       │
│  (This Project)     │                    │  (reflector-native) │
│                     │                    │                     │
│  ┌───────────────┐  │   Test Traffic     │  ┌───────────────┐  │
│  │ Packet        │──┼───────────────────►│  │ Packet        │  │
│  │ Generator     │  │                    │  │ Reflector     │  │
│  └───────────────┘  │   Reflected        │  └───────────────┘  │
│  ┌───────────────┐  │◄───────────────────┼──│               │  │
│  │ Packet        │  │                    │  │               │  │
│  │ Analyzer      │  │                    │  │               │  │
│  └───────────────┘  │                    │  └───────────────┘  │
│                     │                    │                     │
└─────────────────────┘                    └─────────────────────┘
        DUT A                                      DUT B
```

## Quick Start

### Prerequisites

- Linux (kernel 5.4+ recommended for AF_XDP)
- Root privileges (for raw sockets)
- [reflector-native](https://github.com/krisarmstrong/reflector-native) running on the remote end

### Build

```bash
git clone https://github.com/krisarmstrong/rfc2544-master.git
cd rfc2544-master
make
```

### Run Tests

```bash
# Throughput test (default)
sudo ./rfc2544-linux eth0 -t throughput

# Latency test with 1518 byte frames
sudo ./rfc2544-linux eth0 -t latency -s 1518

# Frame loss test with JSON output
sudo ./rfc2544-linux eth0 -t loss --json

# Back-to-back test including jumbo frames
sudo ./rfc2544-linux eth0 -t burst --jumbo
```

## Usage

```
RFC 2544 Network Benchmark Test Master v1.0.0

Usage: rfc2544-linux <interface> [options]

Test Selection:
  -t, --test TYPE     Test type: throughput, latency, loss, burst
                        throughput = RFC2544.26.1 (default)
                        latency    = RFC2544.26.2
                        loss       = RFC2544.26.3
                        burst      = RFC2544.26.4 (back-to-back)

Frame Size Options:
  -s, --size SIZE     Specific frame size (default: all standard)
  --jumbo             Include 9000 byte jumbo frames
  Standard sizes: 64, 128, 256, 512, 1024, 1280, 1518

Timing Options:
  -d, --duration SEC  Trial duration in seconds (default: 60)
  --warmup SEC        Warmup period in seconds (default: 2)

Throughput Test Options:
  --resolution PCT    Binary search resolution % (default: 0.1)
  --max-iter N        Max binary search iterations (default: 20)
  --loss-tolerance    Acceptable frame loss % (default: 0.0)

Output Options:
  -v, --verbose       Enable verbose logging
  --json              Output results in JSON format
  --csv               Output results in CSV format
```

## Packet Signature

RFC2544 test packets use a custom 7-byte signature for identification:

```
Offset  Size    Field
------  ----    -----
0       7       Signature ("RFC2544")
7       4       Sequence number
11      8       TX timestamp (nanoseconds)
19      4       Stream ID
23      1       Flags
24      N       Padding
```

The reflector must be configured to accept RFC2544 signatures:

```bash
# On the reflector side
sudo ./reflector-linux eth0 --sig rfc2544
# Or accept all signatures (ITO + RFC2544 + Y.1564)
sudo ./reflector-linux eth0 --sig all
```

## Performance

| Platform | Expected Rate | Use Case |
|----------|--------------|----------|
| AF_PACKET | ~100 Mbps | Testing, development |
| AF_XDP | ~40 Gbps | Production (10G-40G) |
| DPDK | 100+ Gbps | Line-rate (100G+) |

## Example Output

```
RFC 2544 Test Master v1.0.0
Interface: eth0
Test: Throughput
Frame sizes: 64, 128, 256, 512, 1024, 1280, 1518
Trial duration: 60 seconds

=================================================================
RFC 2544 Test Results
=================================================================
Interface: eth0
Line rate: 10.00 Gbps

Throughput Test Results (Section 26.1)
-----------------------------------------------------------------
Frame            Rate         Rate            Rate  Iterations
Size             (%)        (Mbps)           (pps)
-----------------------------------------------------------------
64             99.50%      9950.00       14880952          12
128            99.80%      9980.00        8445946          10
256            99.90%      9990.00        4424779          11
512            99.95%      9995.00        2232143          10
1024           99.98%      9998.00        1119403           9
1280           99.98%      9998.00         895255           9
1518           99.99%      9999.00         755458           8

=================================================================
```

## Roadmap

- [ ] AF_XDP platform (high-performance)
- [ ] DPDK platform (line-rate)
- [ ] Hardware timestamping
- [ ] TUI dashboard
- [ ] Web UI
- [ ] Y.1564 (EtherSAM) support
- [ ] Multi-stream testing
- [ ] Report generation (PDF/HTML)

## Related Projects

- [reflector-native](https://github.com/krisarmstrong/reflector-native) - Network packet reflector (the other end)
- [iperf3](https://github.com/esnet/iperf) - Network bandwidth measurement
- [T-Rex](https://github.com/cisco-system-traffic-generator/trex-core) - Stateful traffic generator

## License

MIT License - see [LICENSE](LICENSE) for details.

## Contributing

Contributions welcome! Please read the contributing guidelines first.
