CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=

SRC := src/daemon.c src/config.c src/log.c
BUILD_DIR := build
BINARY := $(BUILD_DIR)/nvidia_fan_controlV2d

ifeq ($(USE_MOCK_NVML),1)
MOCK_DIR := mock_nvml
MOCK_LIB := $(MOCK_DIR)/libnvidia-ml.so
MOCK_COPY := $(BUILD_DIR)/mock_nvml/libnvidia-ml.so

CFLAGS += -I$(MOCK_DIR)
LDFLAGS += -L$(BUILD_DIR)/mock_nvml -Wl,-rpath,'$$ORIGIN/mock_nvml'
LDLIBS += -lnvidia-ml

$(MOCK_LIB):
	$(MAKE) -C $(MOCK_DIR)

$(MOCK_COPY): $(MOCK_LIB) | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/mock_nvml
	cp $(MOCK_LIB) $(MOCK_COPY)

MOCK_DEPS := $(MOCK_COPY)
else
LDLIBS += -lnvidia-ml
MOCK_DEPS :=
endif

.PHONY: build
build: $(BINARY)

$(BINARY): $(SRC) $(MOCK_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: test
test:
	$(MAKE) USE_MOCK_NVML=1 build
	USE_MOCK_NVML=1 ./tests/run_tests.sh

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f tests/*.log tests/*.tmp
