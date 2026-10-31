CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -ldl

VERSION := $(shell cat VERSION 2>/dev/null || echo dev)
CFLAGS += -DNVFC_VERSION=\"$(VERSION)\"

SRC := src/daemon.c src/config.c src/log.c src/nvml_dl.c
BUILD_DIR := build
BINARY := $(BUILD_DIR)/nvidia-fan-controld

# NVML is loaded at runtime (dlopen), so no NVIDIA dev headers are needed.
# USE_MOCK_NVML=1 builds the daemon against the mock library for tests.
ifeq ($(USE_MOCK_NVML),1)
MOCK_LIB := mock_nvml/libnvidia-ml.so
MOCK_COPY := $(BUILD_DIR)/mock_nvml/libnvidia-ml.so
CFLAGS += -DNVFC_MOCK_NVML -DNVFC_MOCK_NVML_PATH=\"$(abspath $(MOCK_COPY))\"
MOCK_DEPS := $(MOCK_COPY)

$(MOCK_LIB): mock_nvml/libnvidia-ml.c mock_nvml/nvml.h
	$(MAKE) -C mock_nvml

$(MOCK_COPY): $(MOCK_LIB)
	mkdir -p $(BUILD_DIR)/mock_nvml
	cp $(MOCK_LIB) $(MOCK_COPY)
else
MOCK_DEPS :=
endif

.PHONY: build
build: $(BINARY)

$(BINARY): $(SRC) src/config.h src/log.h src/nvml_dl.h VERSION $(MOCK_DEPS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

.PHONY: test
test:
	$(MAKE) USE_MOCK_NVML=1 build
	USE_MOCK_NVML=1 ./tests/run_tests.sh

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f mock_nvml/libnvidia-ml.so tests/*.log tests/*.tmp
