Name:           rfc2544-master
Version:        2.0.0
Release:        1%{?dist}
Summary:        RFC2544 Network Benchmark Test Master

License:        MIT
URL:            https://github.com/krisarmstrong/rfc2544-master
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  libcap-devel
%if 0%{?fedora} || 0%{?rhel} >= 8
BuildRequires:  libxdp-devel
BuildRequires:  libbpf-devel
%endif

Requires:       libcap
%if 0%{?fedora} || 0%{?rhel} >= 8
Recommends:     libxdp
Recommends:     libbpf
%endif

%description
High-performance network benchmark testing per RFC 2544:
- Throughput testing with binary search (Section 26.1)
- Latency measurement at various loads (Section 26.2)
- Frame loss rate testing (Section 26.3)
- Back-to-back burst capacity testing (Section 26.4)

Also supports:
- ITU-T Y.1564 EtherSAM service testing
- ITU-T Y.1731 OAM performance monitoring
- RFC 2889 LAN switch testing
- RFC 6349 TCP throughput testing
- MEF carrier ethernet testing
- TSN time-sensitive networking

Features:
- AF_XDP for 10-40G performance
- Optional DPDK support for 100G line-rate
- Web UI for remote monitoring
- JSON/CSV output formats
- Systemd service with non-root operation via capabilities

%prep
%autosetup

%build
make linux %{?_smp_mflags}

%install
# Install binary
install -D -m 755 rfc2544-linux %{buildroot}%{_bindir}/rfc2544

# Install systemd service
install -D -m 644 scripts/service/rfc2544.service %{buildroot}%{_unitdir}/rfc2544.service

# Install environment file
install -D -m 644 packaging/debian/environment %{buildroot}%{_sysconfdir}/rfc2544/environment

# Create directories
install -d -m 755 %{buildroot}%{_localstatedir}/log/rfc2544
install -d -m 755 %{buildroot}%{_localstatedir}/lib/rfc2544

%pre
# Create group if it doesn't exist
getent group rfc2544 >/dev/null || groupadd -r rfc2544

# Create user if it doesn't exist
getent passwd rfc2544 >/dev/null || \
    useradd -r -g rfc2544 -d /var/lib/rfc2544 -s /sbin/nologin \
    -c "RFC2544 Test Master daemon" rfc2544

exit 0

%post
# Set file capabilities for non-root operation
setcap 'cap_net_raw,cap_net_admin,cap_sys_admin,cap_ipc_lock+ep' %{_bindir}/rfc2544 || true

# Set ownership
chown rfc2544:rfc2544 %{_localstatedir}/log/rfc2544
chown rfc2544:rfc2544 %{_localstatedir}/lib/rfc2544
chown root:rfc2544 %{_sysconfdir}/rfc2544
chmod 750 %{_sysconfdir}/rfc2544

%systemd_post rfc2544.service

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  RFC2544 Test Master installed successfully!              ║"
echo "╠═══════════════════════════════════════════════════════════╣"
echo "║  Start web UI:   sudo systemctl start rfc2544             ║"
echo "║  Enable at boot: sudo systemctl enable rfc2544            ║"
echo "║  View logs:      journalctl -u rfc2544 -f                 ║"
echo "║  Web UI:         http://localhost:8080                    ║"
echo "║                                                           ║"
echo "║  CLI usage:      rfc2544 <interface> --test <type>        ║"
echo "║  Test types:     throughput, latency, loss, burst         ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

%preun
%systemd_preun rfc2544.service

%postun
%systemd_postun_with_restart rfc2544.service

if [ $1 -eq 0 ]; then
    userdel rfc2544 2>/dev/null || true
    groupdel rfc2544 2>/dev/null || true
fi

%files
%license LICENSE
%doc README.md
%{_bindir}/rfc2544
%{_unitdir}/rfc2544.service
%dir %attr(750,root,rfc2544) %{_sysconfdir}/rfc2544
%config(noreplace) %{_sysconfdir}/rfc2544/environment
%dir %attr(755,rfc2544,rfc2544) %{_localstatedir}/log/rfc2544
%dir %attr(755,rfc2544,rfc2544) %{_localstatedir}/lib/rfc2544

%changelog
* Fri Dec 27 2024 Kris Armstrong <kris.armstrong@me.com> - 2.0.0-1
- Initial 2.0.0 release
- RFC 2544 throughput, latency, frame loss, back-to-back tests
- ITU-T Y.1564 EtherSAM service testing
- ITU-T Y.1731 OAM performance monitoring
- RFC 2889 LAN switch testing
- RFC 6349 TCP throughput testing
- AF_XDP and AF_PACKET support
- Systemd service with capability-based non-root operation
- Web UI for remote monitoring
- JSON/CSV output formats
