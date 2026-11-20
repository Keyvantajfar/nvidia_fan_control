#ifndef NVFC_CONFIG_H
#define NVFC_CONFIG_H

#include <stddef.h>

#define NVFC_MAX_POINTS 7

/* Config file schema this build understands. Files without a [nvfc] section are
 * schema 1 (pre-profiles) and are read as a single "custom1" profile. */
#define NVFC_CONFIG_VERSION 2

#define NVFC_PROFILE_ID_MAX 16
#define NVFC_PROFILE_NAME_MAX 32

typedef struct {
    char profile[NVFC_PROFILE_ID_MAX];       /* active profile id: quiet, balanced, performance, custom1, custom2 */
    char profile_name[NVFC_PROFILE_NAME_MAX];/* its display name */
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
    int smooth_ramp;          /* ramp-down level: 0 slow, 1 balanced, 2 quick */
    int smooth_ramp_custom;   /* 1 if hold/step/interval were overridden in the file */
    int smooth_decay_tau;     /* seconds: how slowly the filter follows a falling temperature */
    int smooth_down_hold;     /* seconds a lower target must persist before the first drop */
    int smooth_down_step;     /* max % the fan drops per ramp-down write */
    int smooth_down_interval; /* seconds between successive ramp-down writes */
    int smooth_emergency_temp;/* at/above this temperature smoothing is bypassed */
} NvfcConfig;

#define NVFC_RAMP_SLOW 0
#define NVFC_RAMP_BALANCED 1
#define NVFC_RAMP_QUICK 2

const char *nvfc_ramp_name(int level);

void nvfc_set_defaults(NvfcConfig *cfg);
int  nvfc_load_config(const char *path, NvfcConfig *cfg);  // 0 on success

#endif  // NVFC_CONFIG_H