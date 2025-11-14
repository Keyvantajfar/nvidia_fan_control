#ifndef NVFC_CONFIG_H
#define NVFC_CONFIG_H

#include <stddef.h>

#define NVFC_MAX_POINTS 7

typedef struct {
    int temps[NVFC_MAX_POINTS];
    int speeds[NVFC_MAX_POINTS];
    int num_points;
    int gpu_index;
    int sleep_low;
    int sleep_high;
    int high_temp_threshold;
} NvfcConfig;

void nvfc_set_defaults(NvfcConfig *cfg);
int  nvfc_load_config(const char *path, NvfcConfig *cfg);  // 0 on success

#endif  // NVFC_CONFIG_H
