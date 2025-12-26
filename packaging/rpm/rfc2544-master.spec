Name:           rfc2544-master
Version:        2.0.0
Release:        1%{?dist}
Summary:        RFC2544 Network Benchmark Test Master

License:        MIT
URL:            https://github.com/krisarmstrong/rfc2544-master
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc make
Requires:       glibc >= 2.17

%description
High-performance network benchmark testing per RFC 2544:
- Throughput testing with binary search (Section 26.1)
- Latency measurement at various loads (Section 26.2)
- Frame loss rate testing (Section 26.3)
- Back-to-back burst capacity testing (Section 26.4)

Features:
- AF_XDP for 10-40G performance
- DPDK support for 100G line-rate
- Terminal UI (TUI) for interactive testing
- Web UI for remote monitoring
- JSON/CSV output formats

%prep
%autosetup

%build
make %{?_smp_mflags}

%install
install -D -m 755 rfc2544-linux %{buildroot}%{_bindir}/rfc2544
install -D -m 644 scripts/service/rfc2544.service %{buildroot}%{_unitdir}/rfc2544.service

%files
%license LICENSE
%doc README.md
%{_bindir}/rfc2544
%{_unitdir}/rfc2544.service

%post
%systemd_post rfc2544.service

%preun
%systemd_preun rfc2544.service

%postun
%systemd_postun_with_restart rfc2544.service

%changelog
* Wed Dec 25 2024 Kris Armstrong <kris@example.com> - 2.0.0-1
- Initial release with Go control plane, TUI, and Web UI
