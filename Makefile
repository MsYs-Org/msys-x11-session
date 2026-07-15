CC ?= cc
PYTHON ?= $(if $(wildcard /opt/msys-dev/.runtime/python/bin/python3),/opt/msys-dev/.runtime/python/bin/python3,python3)
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11 -ldl -lpthread -lm
# Source checkouts use the sibling SDK. The remote development synchronizer
# builds this repository in an isolated atomic staging directory, where the
# already-synced SDK lives at /opt/msys-dev/msys-sdk. Packages still carry one
# fully-linked ELF and have no runtime SDK path dependency.
SDK_ROOT ?= $(firstword $(wildcard ../msys-sdk /opt/msys-dev/msys-sdk))
SDK_INCLUDE := $(SDK_ROOT)/include
SDK_SOURCE := $(SDK_ROOT)/src/mipc.c

BIN_DIR := bin
TARGET := $(BIN_DIR)/msys-x11-policy
TEST_TARGET := $(BIN_DIR)/test-policy-logic
LAYOUT_TEST_TARGET := $(BIN_DIR)/test-layout
AGENT_TEST_TARGET := $(BIN_DIR)/test-native-agent
PACKAGE_ID := org.msys.x11.session
PACKAGE_VERSION := 0.2.8
PACKAGE_ARCHIVE := dist/$(PACKAGE_ID)-$(PACKAGE_VERSION).tar.gz

.PHONY: all native-test python-test test strict integration-test publisher-test \
	aarch64-build rss-probe package package-test clean

all: $(TARGET)

$(TARGET): src/msys_x11_policy.c src/msys_x11_agent.c \
		src/msys_display_session_native.c src/msys_layout.c \
		src/msys_layout.h src/msys_x11_agent.h src/msys_x11_policy_api.h \
		$(SDK_SOURCE) $(SDK_INCLUDE)/msys/mipc.h
	mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INCLUDE) $(CFLAGS) \
		src/msys_x11_policy.c src/msys_x11_agent.c \
		src/msys_display_session_native.c src/msys_layout.c $(SDK_SOURCE) \
		$(LDFLAGS) $(LDLIBS) -o $@

$(TEST_TARGET): tests/test_policy_logic.c src/msys_x11_policy.c \
		src/msys_x11_agent.c src/msys_display_session_native.c \
		src/msys_layout.c src/msys_layout.h $(SDK_SOURCE) \
		$(SDK_INCLUDE)/msys/mipc.h
	mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INCLUDE) $(CFLAGS) tests/test_policy_logic.c \
		src/msys_x11_agent.c src/msys_display_session_native.c \
		src/msys_layout.c $(SDK_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@

$(LAYOUT_TEST_TARGET): tests/test_layout.c src/msys_layout.c src/msys_layout.h
	mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_layout.c src/msys_layout.c $(LDFLAGS) -o $@

$(AGENT_TEST_TARGET): tests/test_native_agent.c \
		tests/native_agent_policy_stub.c src/msys_x11_agent.c \
		src/msys_x11_agent.h src/msys_x11_policy_api.h \
		$(SDK_SOURCE) $(SDK_INCLUDE)/msys/mipc.h
	mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) -I$(SDK_INCLUDE) -Isrc $(CFLAGS) \
		tests/test_native_agent.c tests/native_agent_policy_stub.c \
		src/msys_x11_agent.c $(SDK_SOURCE) $(LDFLAGS) -lpthread -o $@

native-test: $(TEST_TARGET) $(LAYOUT_TEST_TARGET) $(AGENT_TEST_TARGET)
	$(TEST_TARGET)
	$(LAYOUT_TEST_TARGET)
	$(AGENT_TEST_TARGET)

python-test:
	env -u PYTHONPATH $(PYTHON) -m unittest discover -s tests -v

test: native-test python-test

strict:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -Werror" all native-test

integration-test: $(TARGET)
	tests/test_x11_runtime.sh

publisher-test: $(TARGET)
	sh scripts/test_native_publisher.sh

aarch64-build:
	sh scripts/build_aarch64_j1.sh

rss-probe: $(TARGET)
	sh scripts/probe_native_rss.sh :98

package: $(TARGET)
	$(PYTHON) scripts/build_package.py --root . --output $(PACKAGE_ARCHIVE)

package-test: package
	@tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT INT TERM; \
	tar -xzf $(PACKAGE_ARCHIVE) -C "$$tmp"; \
	env -u PYTHONPATH $(PYTHON) -I "$$tmp/scripts/msys_window_policy_entry.py" --check-import; \
	test -x "$$tmp/bin/msys-x11-policy"; \
	test -f "$$tmp/manifest.json"

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(LAYOUT_TEST_TARGET) $(AGENT_TEST_TARGET)
	rm -rf build
