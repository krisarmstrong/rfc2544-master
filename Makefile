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
               src/dataplane/common/main.c

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
        CFLAGS += $(shell pkg-config --cflags libdpdk) -DHAVE_DPDK=1
        LDFLAGS += $(shell pkg-config --libs libdpdk)
        $(info Building with DPDK support)
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

# Test (placeholder)
test: all
	@echo "Running tests..."
	./$(TARGET) --help

# Format code
format:
	clang-format -i src/**/*.c include/*.h

# Static analysis
lint:
	cppcheck --enable=all --suppress=missingIncludeSystem src/ include/

.PHONY: all linux clean install uninstall test format lint FORCE
