# Changelog

All notable changes to RFC 2544 Test Master will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial project structure
- Core RFC 2544 API and types
- AF_PACKET platform implementation (Linux)
- CLI interface with all RFC 2544 test options
- Throughput test framework (Section 26.1)
- Latency test framework (Section 26.2)
- Frame loss test framework (Section 26.3)
- Back-to-back test framework (Section 26.4)
- Custom RFC2544 packet signature
- JSON and CSV output formats

### Planned
- AF_XDP platform for high-performance testing
- DPDK platform for line-rate testing
- Hardware timestamping support
- TUI dashboard (same pattern as reflector-native)
- Web UI dashboard
- Y.1564 (EtherSAM) test support
- Multi-stream testing
- Report generation (PDF/HTML)

## [1.0.0] - TBD

Initial release with core RFC 2544 test functionality.
