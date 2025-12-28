# RFC 2544 Test Master Makefile
#
# Targets:
#   make           - Build for current platform
#   make linux     - Build for Linux (AF_XDP + AF_PACKET)
#   make clean     - Clean build artifacts
#   make test      - Run tests
#   make install   - Install to /usr/local/bin

# Version (from git tag or fallback)
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "v1.0.0-dev")
COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# Compiler settings
CC := gcc
CFLAGS := -Wall -Wextra -O3 -march=native -pthread
CFLAGS += -fno-strict-aliasing -fomit-frame-pointer
CFLAGS += -funroll-loops -finline-functions -ftree-vectorize -flto
CFLAGS += -Iinclude
LDFLAGS := -pthread -flto -lm

# Platform detection
UNAME := $(shell uname -s)

# Common sources
COMMON_SRCS := src/dataplane/common/core.c \
               src/dataplane/common/main.c \
               src/dataplane/common/packet.c \
               src/dataplane/common/pacing.c \
               src/dataplane/common/y1564.c \
               src/dataplane/common/imix.c \
               src/dataplane/common/bidir.c \
               src/dataplane/common/ipv6.c \
               src/dataplane/common/nic_detect.c \
               src/dataplane/common/color.c \
               src/dataplane/common/multiport.c \
               src/dataplane/common/rfc2889.c \
               src/dataplane/common/rfc6349.c \
               src/dataplane/common/y1731.c \
               src/dataplane/common/mef.c \
               src/dataplane/common/tsn.c

# Platform-specific sources
ifeq ($(UNAME),Linux)
    # Check for AF_XDP support
    HAS_XDP := $(shell grep -q 'if_xdp.h' /usr/include/linux/if_xdp.h 2>/dev/null && echo 1 || echo 0)
    HAS_XDP := $(shell test -f /usr/include/linux/if_xdp.h && echo 1 || echo 0)

    PLATFORM_SRCS := src/dataplane/linux_packet/packet_platform.c

    ifeq ($(HAS_XDP),1)
        PLATFORM_SRCS += src/dataplane/linux_xdp/xdp_platform.c
        CFLAGS += -DHAVE_AF_XDP=1
        LDFLAGS += -lxdp -lbpf
        $(info Building with AF_XDP support)
    else
        $(info Building without AF_XDP (fallback to AF_PACKET))
    endif

    # Check for DPDK
    HAS_DPDK := $(shell pkg-config --exists libdpdk 2>/dev/null && echo 1 || echo 0)
    ifeq ($(HAS_DPDK),1)
        PLATFORM_SRCS += src/dataplane/linux_dpdk/dpdk_platform.c
        CFLAGS += $(shell pkg-config --cflags libdpdk) -DHAVE_DPDK=1
        LDFLAGS += $(shell pkg-config --libs libdpdk)
        $(info Building with DPDK support)
    else
        $(info Building without DPDK)
    endif

    TARGET := rfc2544-linux
else ifeq ($(UNAME),Darwin)
    $(error RFC2544 Test Master requires Linux for packet generation)
else
    $(error Unsupported platform: $(UNAME))
endif

SRCS := $(COMMON_SRCS) $(PLATFORM_SRCS)
OBJS := $(SRCS:.c=.o)

# Generate version header
include/version_generated.h: FORCE
	@echo "/* Auto-generated version file */" > $@
	@echo "#ifndef VERSION_GENERATED_H" >> $@
	@echo "#define VERSION_GENERATED_H" >> $@
	@echo "#define VERSION_STRING \"$(VERSION)\"" >> $@
	@echo "#define VERSION_COMMIT \"$(COMMIT)\"" >> $@
	@echo "#endif" >> $@
	@echo "Generated version: $(VERSION) ($(COMMIT))"

FORCE:

# Default target
all: include/version_generated.h $(TARGET)
	@echo "Build complete: $(TARGET)"

# Link target
$(TARGET): $(OBJS)
	@echo "Linking $@..."
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Compile sources
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Linux target
linux: all

# Clean
clean:
	@echo "Cleaning..."
	rm -f $(OBJS)
	rm -f include/version_generated.h
	rm -f rfc2544-linux
	@echo "Clean complete"

# Install
install: all
	install -m 755 $(TARGET) /usr/local/bin/rfc2544

# Uninstall
uninstall:
	rm -f /usr/local/bin/rfc2544

# ============================================================================
# Testing
# ============================================================================

# Test directories
TEST_C_DIR := tests/c
TEST_C_SRCS := $(wildcard $(TEST_C_DIR)/test_*.c)
TEST_C_BINS := $(TEST_C_SRCS:.c=)

# Sources for tests (common + platform, minus main.c)
TEST_SRCS := $(filter-out src/dataplane/common/main.c,$(COMMON_SRCS)) $(PLATFORM_SRCS)

# Test compiler flags (less optimization for debugging)
TEST_CFLAGS := -Wall -Wextra -O0 -g -pthread -Iinclude -I$(TEST_C_DIR) $(filter -D%,$(CFLAGS))
TEST_LDFLAGS := -pthread -lm $(LDFLAGS)

# Build C test executables
$(TEST_C_DIR)/test_%: $(TEST_C_DIR)/test_%.c $(TEST_SRCS)
	@echo "Building C test: $@..."
	$(CC) $(TEST_CFLAGS) -o $@ $< $(TEST_SRCS) $(TEST_LDFLAGS)

# Build all C tests
c-test-build: $(TEST_C_BINS)
	@echo "C test binaries built"

# Run all C tests
c-test: c-test-build
	@echo "=== Running C Unit Tests ==="
	@passed=0; failed=0; total=0; \
	for test in $(TEST_C_BINS); do \
		echo ""; \
		echo ">>> Running $$test..."; \
		if $$test; then \
			passed=$$((passed + 1)); \
		else \
			failed=$$((failed + 1)); \
		fi; \
		total=$$((total + 1)); \
	done; \
	echo ""; \
	echo "=== C Test Summary ==="; \
	echo "Total: $$total  Passed: $$passed  Failed: $$failed"; \
	if [ $$failed -gt 0 ]; then exit 1; fi

# Run Go tests with coverage
go-test:
	@echo "=== Running Go Unit Tests ==="
	go test -v -race ./pkg/...

# Run Go tests with coverage report
go-test-coverage:
	@echo "=== Running Go Tests with Coverage ==="
	go test -v -race -coverprofile=coverage.out -covermode=atomic ./pkg/...
	go tool cover -func=coverage.out
	@echo ""
	@echo "HTML coverage report: go tool cover -html=coverage.out"

# Run Go tests with HTML coverage report
go-test-coverage-html: go-test-coverage
	go tool cover -html=coverage.out -o coverage.html
	@echo "Coverage report written to coverage.html"

# Run all tests (C + Go)
test: c-test go-test
	@echo ""
	@echo "=== All Tests Complete ==="

# Run all tests with coverage
test-coverage: c-test go-test-coverage
	@echo ""
	@echo "=== All Tests with Coverage Complete ==="

# Clean test artifacts
test-clean:
	rm -f $(TEST_C_BINS)
	rm -f coverage.out coverage.html

# Format code
format:
	clang-format -i src/**/*.c include/*.h

# Static analysis
lint:
	cppcheck --enable=all --suppress=missingIncludeSystem src/ include/

# ============================================================================
# Go Control Plane (v2)
# ============================================================================

# Build Go control plane with TUI
go-build:
	@echo "Building Go control plane..."
	cd cmd/rfc2544 && go build -o ../../rfc2544-v2 .
	@echo "Built: rfc2544-v2"

# Build with embedded web UI
go-build-ui: ui-build
	@echo "Building Go control plane with embedded UI..."
	cd cmd/rfc2544 && go build -tags embed_ui -o ../../rfc2544-v2 .

# Run Go tests
go-test:
	go test ./pkg/...

# Build React Web UI
ui-build:
	@echo "Building React Web UI..."
	cd ui && npm install && npm run build
	@echo "Web UI built"

# Development server for UI
ui-dev:
	cd ui && npm run dev

# Full v2 build (C dataplane + Go control plane + UI)
v2: all go-build-ui
	@echo "RFC 2544 Test Master v2 built successfully"

# ============================================================================
# Packaging
# ============================================================================

# Version for packaging
PKG_VERSION := $(shell git describe --tags --always 2>/dev/null | sed 's/^v//' || echo "2.0.0")

# Build Debian package (requires debuild or dpkg-deb)
deb: linux
	@echo "Building Debian package..."
	@if command -v dpkg-buildpackage >/dev/null 2>&1; then \
		mkdir -p debian && cp -r packaging/debian/* debian/; \
		dpkg-buildpackage -us -uc -b; \
		rm -rf debian; \
		echo "✅ Debian package built"; \
	else \
		echo "Building simplified .deb package..."; \
		mkdir -p build/deb/rfc2544-master/DEBIAN; \
		mkdir -p build/deb/rfc2544-master/usr/bin; \
		mkdir -p build/deb/rfc2544-master/lib/systemd/system; \
		mkdir -p build/deb/rfc2544-master/etc/rfc2544; \
		cp packaging/debian/control build/deb/rfc2544-master/DEBIAN/; \
		cp packaging/debian/postinst build/deb/rfc2544-master/DEBIAN/; \
		cp packaging/debian/prerm build/deb/rfc2544-master/DEBIAN/; \
		cp packaging/debian/postrm build/deb/rfc2544-master/DEBIAN/; \
		chmod 755 build/deb/rfc2544-master/DEBIAN/postinst build/deb/rfc2544-master/DEBIAN/prerm build/deb/rfc2544-master/DEBIAN/postrm; \
		sed -i "s/^Version:.*/Version: $(PKG_VERSION)/" build/deb/rfc2544-master/DEBIAN/control 2>/dev/null || \
			sed -i '' "s/^Version:.*/Version: $(PKG_VERSION)/" build/deb/rfc2544-master/DEBIAN/control; \
		cp $(TARGET) build/deb/rfc2544-master/usr/bin/rfc2544; \
		cp scripts/service/rfc2544.service build/deb/rfc2544-master/lib/systemd/system/; \
		cp packaging/debian/environment build/deb/rfc2544-master/etc/rfc2544/; \
		dpkg-deb --build build/deb/rfc2544-master; \
		mv build/deb/rfc2544-master.deb rfc2544-master_$(PKG_VERSION)_amd64.deb; \
		rm -rf build/deb; \
		echo "✅ Built: rfc2544-master_$(PKG_VERSION)_amd64.deb"; \
	fi

# Build RPM package (requires rpmbuild)
rpm: linux
	@echo "Building RPM package..."
	@if command -v rpmbuild >/dev/null 2>&1; then \
		mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}; \
		tar czf ~/rpmbuild/SOURCES/rfc2544-master-$(PKG_VERSION).tar.gz \
			--transform="s|^|rfc2544-master-$(PKG_VERSION)/|" \
			--exclude='.git*' --exclude='*.o' --exclude='rfc2544-*' .; \
		sed "s/^Version:.*/Version:        $(PKG_VERSION)/" packaging/rpm/rfc2544-master.spec > ~/rpmbuild/SPECS/rfc2544-master.spec; \
		rpmbuild -bb ~/rpmbuild/SPECS/rfc2544-master.spec; \
		find ~/rpmbuild/RPMS -name "rfc2544*.rpm" -exec cp {} . \;; \
		echo "✅ RPM package built"; \
	else \
		echo "❌ rpmbuild not found. Install with: sudo dnf install rpm-build"; \
		exit 1; \
	fi

# Smoke tests (requires root for veth)
smoke-test: linux
	@echo "Running smoke tests..."
	@if [ "$$(id -u)" != "0" ]; then \
		echo "Smoke tests require root. Run: sudo make smoke-test"; \
		exit 1; \
	fi
	@./tests/smoke/run_smoke_tests.sh

# Build all packages
packages: deb rpm
	@echo "✅ All packages built"

.PHONY: all linux clean install uninstall test format lint FORCE
.PHONY: go-build go-build-ui go-test go-test-coverage go-test-coverage-html ui-build ui-dev v2 deb rpm
.PHONY: c-test c-test-build test-coverage test-clean smoke-test packages
