#ifndef NVFC_CONFIG_H
#define NVFC_CONFIG_H

#include <stddef.h>

#define NVFC_MAX_POINTS 7

typedef struct {
    int temps[NVFC_MAX_POINTS];
    int speeds[NVFC_MAX_POINTS];
    int num_points;
    int gpu_index;
    int sleep_low;            /* poll interval while cool (also the backoff floor) */
    int sleep_high;           /* poll interval at/above high_temp_threshold */
    int sleep_max;            /* longest poll interval when cool and stable */
    int high_temp_threshold;

    /* Smart Smooth */
    int smooth_enabled;       /* 1 = interpolate + filter, 0 = classic stepped curve */
    int smooth_deadband;      /* ignore speed changes smaller than this (%) */
    int smooth_down_hold;     /* seconds a lower target must persist before ramping down */
    int smooth_down_step;     /* max % the fan drops per ramp-down write */
    int smooth_emergency_temp;/* at/above this temperature smoothing is bypassed */
} NvfcConfig;

void nvfc_set_defaults(NvfcConfig *cfg);
int  nvfc_load_config(const char *path, NvfcConfig *cfg);  // 0 on success

#endif  // NVFC_CONFIG_H
