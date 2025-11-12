#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvml.h"

static char MOCK_DIR[512] = "/tmp/nvml-mock";
static int initialized = 0;
static int default_speed_called = 0;

static void path_join(char *out, const char *dir, const char *name) {
    snprintf(out, 512, "%s/%s", dir, name);
}

static void ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0777);
    }
}

static int read_int_file(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    int v = fallback;
    if (fscanf(f, "%d", &v) != 1) { fclose(f); return fallback; }
    fclose(f);
    return v;
}

static void append_log(const char *fname, const char *fmt, ...) {
    char p[512]; path_join(p, MOCK_DIR, fname);
    FILE *f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

nvmlReturn_t nvmlInit(void) {
    const char *d = getenv("NVML_MOCK_DIR");
    if (d && *d) snprintf(MOCK_DIR, sizeof(MOCK_DIR), "%s", d);
    ensure_dir(MOCK_DIR);
    initialized = 1;
    append_log("fan_speed.log", "[init]\n");
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlShutdown(void) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    initialized = 0;
    append_log("fan_speed.log", "[shutdown]\n");
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    if (!device) return NVML_ERROR_UNKNOWN;
    // One fake device is enough for tests
    if (index != 0) return NVML_ERROR_UNKNOWN;
    *device = (nvmlDevice_t)0x1;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t device, unsigned int *fanCount) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    if (!fanCount) return NVML_ERROR_UNKNOWN;
    char p[512]; path_join(p, MOCK_DIR, "fans");
    int fans = read_int_file(p, 1);
    if (fans < 1) fans = 1;
    *fanCount = (unsigned int)fans;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, unsigned int sensorType, unsigned int *temp) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    if (!temp) return NVML_ERROR_UNKNOWN;

    char p[512]; path_join(p, MOCK_DIR, "temperature");
    int t = read_int_file(p, -999);

    // If no control file exists, synthesize a gentle sawtooth between 35–85°C
    static int synth = 35;
    static int dir = +1;
    if (t == -999) {
        synth += dir * 3;
        if (synth >= 85) { synth = 85; dir = -1; }
        if (synth <= 35) { synth = 35; dir = +1; }
        t = synth;
    }

    *temp = (unsigned int)t;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetFanSpeed_v2(nvmlDevice_t device, unsigned int fanIndex, unsigned int speed) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    append_log("fan_speed.log", "fanIndex=%u speed=%u\n", fanIndex, speed);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetDefaultFanSpeed_v2(nvmlDevice_t device, unsigned int fanIndex) {
    if (!initialized) return NVML_ERROR_UNINITIALIZED;
    default_speed_called = 1;
    append_log("fan_speed.log", "reset_to_auto fanIndex=%u\n", fanIndex);
    return NVML_SUCCESS;
}
